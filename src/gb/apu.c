/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The sound (Pan Docs, "Audio" and "Audio Registers"). Two square channels (the first with a sweep), a
// wave channel and a noise channel. The channels are brought up to date only when something needs
// them: a register access, a step of the frame sequencer, the frame's end. Each sample is the
// average of every channel's level over its 128 dots.

enum {
	SAMPLE_DOTS = 4194304 / GB_SAMPLE_RATE,
};

static const uint8_t duties[4] = { 0x01, 0x81, 0x87, 0x7E };

static uint8_t* reg(struct GB* gb, uint8_t address) {
	return &gb->apu.regs[address - 0x10];
}

// Dots per step of each channel's waveform.
static uint32_t squarePeriod(const struct GBChannel* ch) {
	return (2048 - ch->frequency) * 4;
}

static uint32_t wavePeriod(const struct GBChannel* ch) {
	return (2048 - ch->frequency) * 2;
}

static uint32_t noisePeriod(struct GB* gb) {
	uint8_t nr43 = *reg(gb, 0x22);
	uint32_t divisor = nr43 & 7 ? (nr43 & 7) * 16 : 8;
	return divisor << (nr43 >> 4);
}

static void squareLevel(struct GB* gb, int i) {
	struct GBChannel* ch = &gb->apu.square[i];
	uint8_t duty = duties[*reg(gb, i ? 0x16 : 0x11) >> 6];
	ch->level = ch->on && (duty >> (7 - ch->position) & 1) ? ch->volume : 0;
}

static void waveLevel(struct GB* gb) {
	struct GBChannel* ch = &gb->apu.wave;
	static const uint8_t shifts[4] = { 4, 0, 1, 2 };
	uint8_t sample = gb->apu.waveSample;
	sample = ch->position & 1 ? sample & 0xF : sample >> 4;
	ch->level = ch->on ? sample >> shifts[*reg(gb, 0x1C) >> 5 & 3] : 0;
}

static void noiseLevel(struct GB* gb) {
	struct GBChannel* ch = &gb->apu.noise;
	ch->level = ch->on && !(gb->apu.lfsr & 1) ? ch->volume : 0;
}

static void allLevels(struct GB* gb) {
	squareLevel(gb, 0);
	squareLevel(gb, 1);
	waveLevel(gb);
	noiseLevel(gb);
}

// Adds a stretch of dots at the channels' present levels to the sample being built.
static void accumulate(struct GB* gb, uint32_t dots) {
	struct GBApu* apu = &gb->apu;
	uint8_t nr51 = *reg(gb, 0x25);
	uint8_t nr50 = *reg(gb, 0x24);
	const struct GBChannel* channels[4] = { &apu->square[0], &apu->square[1], &apu->wave, &apu->noise };
	int left = 0, right = 0;
	for (int i = 0; i < 4; ++i) {
		if (!channels[i]->dac) {
			continue;
		}
		int analog = 2 * channels[i]->level - 15;
		if (nr51 >> (4 + i) & 1) {
			left += analog;
		}
		if (nr51 >> i & 1) {
			right += analog;
		}
	}
	apu->accumLeft += left * ((nr50 >> 4 & 7) + 1) * (int32_t) dots;
	apu->accumRight += right * ((nr50 & 7) + 1) * (int32_t) dots;
}

// Moves the running channels' waveforms through the steps due by end.
static void stepAll(struct GB* gb, uint32_t end) {
	struct GBApu* apu = &gb->apu;
	for (int i = 0; i < 2; ++i) {
		struct GBChannel* ch = &apu->square[i];
		if (!ch->on) {
			continue;
		}
		while (gbSince(end, ch->nextStep) >= 0) {
			ch->position = (ch->position + 1) & 7;
			ch->nextStep += squarePeriod(ch);
		}
	}
	struct GBChannel* wave = &apu->wave;
	if (wave->on) {
		while (gbSince(end, wave->nextStep) >= 0) {
			wave->position = (wave->position + 1) & 31;
			apu->waveSample = gb->io[0x30 + (wave->position >> 1)];
			apu->waveReadAt = wave->nextStep;
			wave->nextStep += wavePeriod(wave);
		}
	}
	struct GBChannel* noise = &apu->noise;
	if (noise->on) {
		bool narrow = *reg(gb, 0x22) & 8;
		uint32_t period = noisePeriod(gb);
		while (gbSince(end, noise->nextStep) >= 0) {
			uint16_t bit = (apu->lfsr ^ (apu->lfsr >> 1)) & 1;
			apu->lfsr = (apu->lfsr >> 1) | (bit << 14);
			if (narrow) {
				apu->lfsr = (apu->lfsr & ~0x40) | (bit << 6);
			}
			noise->nextStep += period;
		}
	}
	allLevels(gb);
}

// The earliest step any running channel takes after from, or limit.
static uint32_t nextChange(struct GB* gb, uint32_t from, uint32_t limit) {
	struct GBApu* apu = &gb->apu;
	uint32_t best = limit;
	const struct GBChannel* channels[4] = { &apu->square[0], &apu->square[1], &apu->wave, &apu->noise };
	for (int i = 0; i < 4; ++i) {
		if (channels[i]->on && gbSince(channels[i]->nextStep, from) > 0 && gbSince(channels[i]->nextStep, best) < 0) {
			best = channels[i]->nextStep;
		}
	}
	return best;
}

// The sound from the last sync to end, sample by sample.
static void render(struct GB* gb, uint32_t end) {
	struct GBApu* apu = &gb->apu;
	uint32_t t = apu->lastSync;
	while (gbSince(end, t) > 0) {
		uint32_t sampleEnd = apu->sampleStart + SAMPLE_DOTS;
		uint32_t stop = gbSince(end, sampleEnd) < 0 ? end : sampleEnd;
		// Within the sample, from one change of level to the next.
		while (gbSince(stop, t) > 0) {
			uint32_t change = nextChange(gb, t, stop);
			accumulate(gb, change - t);
			t = change;
			stepAll(gb, t);
		}
		if (t == sampleEnd) {
			if (gb->audioCount + 2 <= sizeof(gb->audio) / sizeof(gb->audio[0])) {
				gb->audio[gb->audioCount++] = apu->accumLeft / (SAMPLE_DOTS * 2);
				gb->audio[gb->audioCount++] = apu->accumRight / (SAMPLE_DOTS * 2);
			}
			apu->accumLeft = 0;
			apu->accumRight = 0;
			apu->sampleStart = sampleEnd;
		}
	}
	apu->lastSync = end;
}

// The sweep's next frequency; past 2047, channel 1 stops.
static uint16_t sweepCalculate(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t nr10 = *reg(gb, 0x10);
	uint16_t delta = apu->sweepShadow >> (nr10 & 7);
	uint16_t next;
	if (nr10 & 8) {
		next = apu->sweepShadow - delta;
		apu->sweepNegated = true;
	} else {
		next = apu->sweepShadow + delta;
	}
	if (next > 2047) {
		apu->square[0].on = false;
	}
	return next;
}

static void clockLength(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* channels[4] = { &apu->square[0], &apu->square[1], &apu->wave, &apu->noise };
	for (int i = 0; i < 4; ++i) {
		struct GBChannel* ch = channels[i];
		if (ch->lengthOn && ch->length && !--ch->length) {
			ch->on = false;
		}
	}
}

static void clockSweep(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t nr10 = *reg(gb, 0x10);
	if (apu->sweepTimer && --apu->sweepTimer) {
		return;
	}
	uint8_t pace = nr10 >> 4 & 7;
	apu->sweepTimer = pace ? pace : 8;
	if (!apu->sweepOn || !pace) {
		return;
	}
	uint16_t next = sweepCalculate(gb);
	if (next <= 2047 && (nr10 & 7)) {
		apu->sweepShadow = next;
		apu->square[0].frequency = next;
		sweepCalculate(gb);
	}
}

static void clockEnvelope(struct GBChannel* ch, uint8_t nrx2) {
	uint8_t pace = nrx2 & 7;
	if (!pace || !ch->envelopeTimer || --ch->envelopeTimer) {
		return;
	}
	ch->envelopeTimer = pace;
	if (nrx2 & 8) {
		if (ch->volume < 15) {
			++ch->volume;
		}
	} else if (ch->volume) {
		--ch->volume;
	}
}

static void frameSequencer(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t step = apu->frameStep;
	if (!(step & 1)) {
		clockLength(gb);
	}
	if (step == 2 || step == 6) {
		clockSweep(gb);
	}
	if (step == 7) {
		clockEnvelope(&apu->square[0], *reg(gb, 0x12));
		clockEnvelope(&apu->square[1], *reg(gb, 0x17));
		clockEnvelope(&apu->noise, *reg(gb, 0x21));
	}
	apu->frameStep = (step + 1) & 7;
	allLevels(gb);
}

// The counter bit whose falling edges step the frame sequencer: 512 Hz at either speed.
static unsigned sequencerBit(const struct GB* gb) {
	return gb->doubleSpeed ? 13 : 12;
}

void gbApuSync(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	int32_t dots = gbSince(gb->now, apu->lastSync);
	if (dots <= 0) {
		return;
	}
	if (!apu->power) {
		render(gb, gb->now);
		return;
	}
	// The divider's counter at the last sync, from its value now.
	gbTimerSync(gb);
	uint32_t per = gb->doubleSpeed ? 2 : 1;
	uint32_t c = (uint16_t) (gb->timer.counter - (uint32_t) dots * per);
	unsigned shift = sequencerBit(gb) + 1;
	for (;;) {
		uint32_t edge = ((c >> shift) + 1) << shift;
		uint32_t at = apu->lastSync + (edge - c + per - 1) / per;
		if (gbSince(at, gb->now) > 0) {
			break;
		}
		render(gb, at);
		frameSequencer(gb);
		c = edge & 0xFFFF;
	}
	render(gb, gb->now);
}

// Writing DIV can make the frame sequencer's bit fall.
void gbApuDivReset(struct GB* gb) {
	gbApuSync(gb);
	if (gb->apu.power && gb->timer.counter >> sequencerBit(gb) & 1) {
		frameSequencer(gb);
	}
}

static struct GBChannel* channelOf(struct GB* gb, uint8_t address) {
	struct GBApu* apu = &gb->apu;
	if (address < 0x15) {
		return &apu->square[0];
	}
	if (address < 0x1A) {
		return &apu->square[1];
	}
	if (address < 0x1F) {
		return &apu->wave;
	}
	return &apu->noise;
}

// Enabling the length counter in a frame sequencer step that won't clock it clocks it once.
static void lengthEnable(struct GB* gb, struct GBChannel* ch, uint8_t value, uint16_t max) {
	bool was = ch->lengthOn;
	ch->lengthOn = value & 0x40;
	bool quiet = gb->apu.frameStep & 1;
	if (!was && ch->lengthOn && quiet && ch->length && !--ch->length && !(value & 0x80)) {
		ch->on = false;
	}
	if (value & 0x80 && !ch->length) {
		ch->length = max;
		if (ch->lengthOn && quiet) {
			--ch->length;
		}
	}
}

static void startEnvelope(struct GBChannel* ch, uint8_t nrx2) {
	ch->on = ch->dac;
	ch->volume = nrx2 >> 4;
	ch->envelopeTimer = nrx2 & 7;
}

static void writeControl(struct GB* gb, uint8_t address, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* ch = channelOf(gb, address);
	if (ch != &apu->noise) {
		ch->frequency = (ch->frequency & 0xFF) | (value & 7) << 8;
	}
	lengthEnable(gb, ch, value, ch == &apu->wave ? 256 : 64);
	if (!(value & 0x80)) {
		return;
	}
	if (ch == &apu->square[0] || ch == &apu->square[1]) {
		startEnvelope(ch, *reg(gb, address - 2));
		ch->nextStep = gb->now + squarePeriod(ch);
		if (ch == &apu->square[0]) {
			uint8_t nr10 = *reg(gb, 0x10);
			apu->sweepShadow = ch->frequency;
			apu->sweepTimer = nr10 >> 4 & 7 ? nr10 >> 4 & 7 : 8;
			apu->sweepOn = (nr10 & 0x70) || (nr10 & 7);
			apu->sweepNegated = false;
			if (nr10 & 7) {
				sweepCalculate(gb);
			}
		}
	} else if (ch == &apu->wave) {
		// On the original Game Boy, retriggered just as it reads a byte, the channel writes over
		// the start of wave RAM: byte 0 with that byte, or bytes 0-3 with its aligned four.
		if (gb->model != GB_MODEL_CGB && ch->on && apu->waveReadAt == gb->now) {
			unsigned next = ch->position >> 1;
			if (next < 4) {
				gb->io[0x30] = gb->io[0x30 + next];
			} else {
				memcpy(&gb->io[0x30], &gb->io[0x30 + (next & ~3)], 4);
			}
		}
		ch->on = ch->dac;
		ch->position = 0;
		// The first read comes 2 units of 2 dots after a period (SameBoy's sample countdown).
		ch->nextStep = gb->now + wavePeriod(ch) + 4;
	} else {
		startEnvelope(ch, *reg(gb, 0x21));
		apu->lfsr = 0x7FFF;
		ch->nextStep = gb->now + noisePeriod(gb);
	}
	allLevels(gb);
}

void gbApuWrite(struct GB* gb, uint8_t address, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	if (address >= 0x30) {
		if (apu->wave.on) {
			// While it plays, the byte being read takes the write; on the original Game Boy, only
			// if it is read just then.
			if (gb->model == GB_MODEL_CGB || gbSince(gb->now, apu->waveReadAt) >= 2 && gbSince(gb->now, apu->waveReadAt) < 4) {
				gb->io[0x30 + (apu->wave.position >> 1)] = value;
			}
		} else {
			gb->io[address] = value;
		}
		return;
	}
	if (address == 0x26) {
		bool power = value & 0x80;
		if (apu->power && !power) {
			// Off: every register cleared, but the original Game Boy keeps the lengths.
			uint16_t lengths[4] = { apu->square[0].length, apu->square[1].length, apu->wave.length, apu->noise.length };
			memset(apu->regs, 0, 0x16);
			memset(apu->square, 0, sizeof(apu->square));
			memset(&apu->wave, 0, sizeof(apu->wave));
			memset(&apu->noise, 0, sizeof(apu->noise));
			apu->sweepOn = false;
			if (gb->model != GB_MODEL_CGB) {
				apu->square[0].length = lengths[0];
				apu->square[1].length = lengths[1];
				apu->wave.length = lengths[2];
				apu->noise.length = lengths[3];
			}
			allLevels(gb);
		} else if (!apu->power && power) {
			apu->frameStep = 0;
		}
		apu->power = power;
		return;
	}
	bool lengthRegister = address == 0x11 || address == 0x16 || address == 0x1B || address == 0x20;
	if (!apu->power && (!lengthRegister || gb->model == GB_MODEL_CGB)) {
		// Off, only the original Game Boy's length counters can be written.
		return;
	}
	uint8_t old = *reg(gb, address);
	struct GBChannel* ch = channelOf(gb, address);
	if (!apu->power) {
		ch->length = address == 0x1B ? 256 - value : 64 - (value & 0x3F);
		return;
	}
	*reg(gb, address) = value;
	switch (address) {
	case 0x10:
		// Back from a negated sweep that has calculated, channel 1 stops.
		if (apu->sweepNegated && (old & 8) && !(value & 8)) {
			apu->square[0].on = false;
		}
		break;
	case 0x11:
	case 0x16:
	case 0x20:
		ch->length = 64 - (value & 0x3F);
		break;
	case 0x1B:
		ch->length = 256 - value;
		break;
	case 0x12:
	case 0x17:
	case 0x21:
		ch->dac = (value & 0xF8) != 0;
		if (!ch->dac) {
			ch->on = false;
		}
		break;
	case 0x1A:
		ch->dac = value & 0x80;
		if (!ch->dac) {
			ch->on = false;
		}
		break;
	case 0x13:
	case 0x18:
	case 0x1D:
		ch->frequency = (ch->frequency & 0x700) | value;
		break;
	case 0x14:
	case 0x19:
	case 0x1E:
	case 0x23:
		writeControl(gb, address, value);
		break;
	default:
		break;
	}
	allLevels(gb);
}

uint8_t gbApuRead(struct GB* gb, uint8_t address) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	if (address >= 0x30) {
		if (apu->wave.on) {
			// While it plays, a read gets the byte the channel is reading, if it reads it just then.
			if (gb->model == GB_MODEL_CGB || gbSince(gb->now, apu->waveReadAt) >= 2 && gbSince(gb->now, apu->waveReadAt) < 4) {
				return gb->io[0x30 + (apu->wave.position >> 1)];
			}
			return 0xFF;
		}
		return gb->io[address];
	}
	if (address == 0x26) {
		return (apu->power ? 0x80 : 0) | (apu->square[0].on ? 1 : 0) | (apu->square[1].on ? 2 : 0) |
		       (apu->wave.on ? 4 : 0) | (apu->noise.on ? 8 : 0);
	}
	return *reg(gb, address);
}

void gbApuReset(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	memset(apu, 0, sizeof(*apu));
	apu->lastSync = gb->now;
	apu->sampleStart = gb->now;
	apu->lfsr = 0x7FFF;
}
