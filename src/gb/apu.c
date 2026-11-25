/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * The envelopes follow SameBoy (Copyright (c) Lior Halphon), under the Expat (MIT) license: see
 * LICENSES/SameBoy.txt. */
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

// Where a channel's output goes: the sums of the samples (bucket 0 is the sample being built), the
// bucket the time has reached, and when that bucket ends.
struct Spans {
	int32_t* sums;
	uint32_t bucket;
	uint32_t boundary;
};

// Adds v over the dots from t to end.
static inline void addSpan(struct Spans* spans, uint32_t t, uint32_t end, int32_t v) {
	while (gbSince(end, spans->boundary) > 0) {
		spans->sums[spans->bucket] += v * (int32_t) (spans->boundary - t);
		t = spans->boundary;
		++spans->bucket;
		spans->boundary += SAMPLE_DOTS;
	}
	spans->sums[spans->bucket] += v * (int32_t) (end - t);
}

// One channel's DAC output (-15 to 15, twice the level less 15) over time, from start to end, stepping
// its waveform as it goes. A channel whose DAC is off puts out nothing.
static void integrate(struct GB* gb, int index, uint32_t start, uint32_t end, int32_t* sums) {
	struct GBApu* apu = &gb->apu;
	struct Spans spans = { sums, (start - apu->sampleStart) / SAMPLE_DOTS, 0 };
	spans.boundary = apu->sampleStart + (spans.bucket + 1) * SAMPLE_DOTS;
	struct GBChannel* ch = index < 2 ? &apu->square[index] : index == 2 ? &apu->wave : &apu->noise;
	if (!ch->dac) {
		return;
	}
	if (!ch->on) {
		addSpan(&spans, start, end, -15);
		return;
	}
	uint32_t t = start;
	if (index < 2) {
		uint8_t duty = duties[*reg(gb, index ? 0x16 : 0x11) >> 6];
		uint32_t period = squarePeriod(ch);
		int32_t high = 2 * ch->volume - 15;
		for (;;) {
			int32_t v = duty >> (7 - ch->position) & 1 ? high : -15;
			if (gbSince(end, ch->nextStep) < 0) {
				addSpan(&spans, t, end, v);
				break;
			}
			addSpan(&spans, t, ch->nextStep, v);
			t = ch->nextStep;
			ch->position = (ch->position + 1) & 7;
			ch->nextStep += period;
		}
	} else if (index == 2) {
		static const uint8_t shifts[4] = { 4, 0, 1, 2 };
		unsigned shift = shifts[*reg(gb, 0x1C) >> 5 & 3];
		uint32_t period = wavePeriod(ch);
		for (;;) {
			uint8_t sample = ch->position & 1 ? apu->waveSample & 0xF : apu->waveSample >> 4;
			int32_t v = 2 * (sample >> shift) - 15;
			if (gbSince(end, ch->nextStep) < 0) {
				addSpan(&spans, t, end, v);
				break;
			}
			addSpan(&spans, t, ch->nextStep, v);
			t = ch->nextStep;
			ch->position = (ch->position + 1) & 31;
			apu->waveSample = gb->io[0x30 + (ch->position >> 1)];
			apu->waveReadAt = ch->nextStep;
			ch->nextStep += period;
		}
	} else {
		bool narrow = *reg(gb, 0x22) & 8;
		uint32_t period = noisePeriod(gb);
		int32_t high = 2 * ch->volume - 15;
		for (;;) {
			int32_t v = apu->lfsr & 1 ? -15 : high;
			if (gbSince(end, ch->nextStep) < 0) {
				addSpan(&spans, t, end, v);
				break;
			}
			addSpan(&spans, t, ch->nextStep, v);
			t = ch->nextStep;
			uint16_t bit = (apu->lfsr ^ (apu->lfsr >> 1)) & 1;
			apu->lfsr = (apu->lfsr >> 1) | (bit << 14);
			if (narrow) {
				apu->lfsr = (apu->lfsr & ~0x40) | (bit << 6);
			}
			ch->nextStep += period;
		}
	}
}

// The sound from the last sync to end: each channel on its own over the whole stretch, then the
// samples mixed from their sums. A stretch is at most a frame or a frame sequencer step long.
static void render(struct GB* gb, uint32_t end) {
	struct GBApu* apu = &gb->apu;
	enum { MAX_SAMPLES = 1024 };
	while (gbSince(end, apu->lastSync) > 0) {
		uint32_t stop = end;
		if (gbSince(stop, apu->sampleStart + (MAX_SAMPLES - 1) * SAMPLE_DOTS) > 0) {
			stop = apu->sampleStart + (MAX_SAMPLES - 1) * SAMPLE_DOTS;
		}
		uint32_t complete = (stop - apu->sampleStart) / SAMPLE_DOTS;
		static int32_t sums[4][MAX_SAMPLES];
		for (int i = 0; i < 4; ++i) {
			memset(sums[i], 0, (complete + 1) * sizeof(int32_t));
			integrate(gb, i, apu->lastSync, stop, sums[i]);
		}
		uint8_t nr51 = *reg(gb, 0x25);
		uint8_t nr50 = *reg(gb, 0x24);
		int32_t leftVolume = (nr50 >> 4 & 7) + 1;
		int32_t rightVolume = (nr50 & 7) + 1;
		// Mixed as they stand now; the sample being built carries over what it has, mixed as it was. A
		// sample is the mean over its 128 dots, times 64: all four channels at full volume reach 30720.
		uint8_t muted = gb->channelsMuted;
		for (uint32_t k = 0; k <= complete; ++k) {
			int32_t left = 0, right = 0, heardLeft = 0, heardRight = 0;
			for (int i = 0; i < 4; ++i) {
				if (nr51 >> (4 + i) & 1) {
					left += sums[i][k];
					heardLeft += muted >> i & 1 ? 0 : sums[i][k];
				}
				if (nr51 >> i & 1) {
					right += sums[i][k];
					heardRight += muted >> i & 1 ? 0 : sums[i][k];
				}
			}
			left *= leftVolume;
			right *= rightVolume;
			heardLeft *= leftVolume;
			heardRight *= rightVolume;
			if (!k) {
				left += apu->partialLeft;
				right += apu->partialRight;
				heardLeft += apu->partialHeardLeft;
				heardRight += apu->partialHeardRight;
			}
			if (k == complete) {
				apu->partialLeft = left;
				apu->partialRight = right;
				apu->partialHeardLeft = heardLeft;
				apu->partialHeardRight = heardRight;
			} else if (gb->audioCount + 2 <= sizeof(gb->audio) / sizeof(gb->audio[0])) {
				gb->audioDacs[gb->audioCount / 2] = apu->square[0].dac || apu->square[1].dac || apu->wave.dac || apu->noise.dac;
				// Channel Selection: what's heard, beside the console's own.
				if (muted) {
					gb->heard[gb->audioCount] = heardLeft / 2;
					gb->heard[gb->audioCount + 1] = heardRight / 2;
				}
				gb->audio[gb->audioCount++] = left / 2;
				gb->audio[gb->audioCount++] = right / 2;
			}
		}
		apu->sampleStart += complete * SAMPLE_DOTS;
		apu->lastSync = stop;
	}
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

// The envelopes, as SameBoy (MIT) has them: a 3-bit countdown steps down on frame sequencer step 6
// while the envelope's clock is low. Half a step after it reaches 0 (the sequencer's bit rising), it
// reloads from NRx2 and, if that isn't 0, raises the clock, which the next step takes down, moving
// the volume. A clock raised with the volume at 15 going up, or 0 going down, locks the envelope until
// the channel restarts.
static void setEnvelopeClock(struct GBChannel* ch, bool value, bool up) {
	if (ch->envClock == value) {
		return;
	}
	ch->envClock = value;
	if (value) {
		ch->envShouldLock = (ch->volume == 0xF && up) || (ch->volume == 0 && !up);
	} else {
		ch->envLocked |= ch->envShouldLock;
	}
}

static void tickEnvelope(struct GBChannel* ch, uint8_t nrx2) {
	setEnvelopeClock(ch, false, false);
	if (ch->envLocked || !(nrx2 & 7)) {
		return;
	}
	ch->volume = (ch->volume + (nrx2 & 8 ? 1 : -1)) & 0xF;
}

// The envelopes' channels, each with its NRx2.
static int envelopes(struct GB* gb, struct GBChannel* chs[3], uint8_t nrx2[3]) {
	struct GBApu* apu = &gb->apu;
	chs[0] = &apu->square[0], chs[1] = &apu->square[1], chs[2] = &apu->noise;
	nrx2[0] = *reg(gb, 0x12), nrx2[1] = *reg(gb, 0x17), nrx2[2] = *reg(gb, 0x21);
	return 3;
}

// Half a step on: the sequencer's bit rising.
static void sequencerRise(struct GB* gb) {
	struct GBChannel* chs[3];
	uint8_t nrx2[3];
	for (int i = envelopes(gb, chs, nrx2); i--;) {
		if (chs[i]->on && chs[i]->envCountdown == 0) {
			chs[i]->envCountdown = nrx2[i] & 7;
			setEnvelopeClock(chs[i], chs[i]->envCountdown != 0, nrx2[i] & 8);
		}
	}
}

// NRx2 written while the channel plays ("zombie mode", SameBoy's): the volume can move or invert, by
// how the envelope's clock lines meet. The original Game Boy and the earlier Colors take the write as
// two, through all bits set.
static void envelopeWrite(struct GBChannel* ch, uint8_t value, uint8_t old) {
	if (ch->envClock) {
		ch->envCountdown = value & 7;
	}
	bool tick = (value & 7) && !(old & 7) && !ch->envLocked;
	bool invert = (value & 8) ^ (old & 8);
	if ((value & 0xF) == 8 && (old & 0xF) == 8 && !ch->envLocked) {
		tick = true;
	}
	if (invert) {
		if (value & 8) {
			if (!(old & 7) && !ch->envLocked) {
				ch->volume ^= 0xF;
			} else {
				ch->volume = (0xE - ch->volume) & 0xF;
			}
			tick = false;
		} else {
			ch->volume = (0x10 - ch->volume) & 0xF;
		}
	}
	if (tick) {
		ch->volume = (ch->volume + (value & 8 ? 1 : -1)) & 0xF;
	} else if (!(value & 7) && ch->envClock) {
		setEnvelopeClock(ch, false, false);
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
	struct GBChannel* chs[3];
	uint8_t nrx2[3];
	for (int i = envelopes(gb, chs, nrx2); i--;) {
		if (step == 6 && !chs[i]->envClock) {
			chs[i]->envCountdown = (chs[i]->envCountdown - 1) & 7;
		}
		if (chs[i]->envClock) {
			tickEnvelope(chs[i], nrx2[i]);
		}
	}
	apu->frameStep = (step + 1) & 7;
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
	// Each edge of the sequencer's bit: falling, a step; rising, the envelopes' half step.
	unsigned bit = sequencerBit(gb);
	for (;;) {
		uint32_t edge = ((c >> bit) + 1) << bit;
		uint32_t at = apu->lastSync + (edge - c + per - 1) / per;
		if (gbSince(at, gb->now) > 0) {
			break;
		}
		render(gb, at);
		if (edge >> bit & 1) {
			sequencerRise(gb);
		} else {
			frameSequencer(gb);
		}
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
	ch->envCountdown = nrx2 & 7;
	ch->envClock = ch->envLocked = false;
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
		if (!gbColor(gb) && ch->on && apu->waveReadAt == gb->now) {
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
}

void gbApuWrite(struct GB* gb, uint8_t address, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	if (address >= 0x30) {
		if (apu->wave.on) {
			// While it plays, the byte being read takes the write; on the original Game Boy, only
			// if it is read just then; on the Advance, never (Pan Docs).
			if (gb->model != GB_MODEL_AGB && (gbColor(gb) || (gbSince(gb->now, apu->waveReadAt) >= 2 && gbSince(gb->now, apu->waveReadAt) < 4))) {
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
			if (!gbColor(gb)) {
				apu->square[0].length = lengths[0];
				apu->square[1].length = lengths[1];
				apu->wave.length = lengths[2];
				apu->noise.length = lengths[3];
			}
				} else if (!apu->power && power) {
			apu->frameStep = 0;
		}
		apu->power = power;
		return;
	}
	bool lengthRegister = address == 0x11 || address == 0x16 || address == 0x1B || address == 0x20;
	if (!apu->power && (!lengthRegister || gbColor(gb))) {
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
		} else if (ch->on) {
			if (gb->model == GB_MODEL_DMG || gb->model == GB_MODEL_MGB) {
				envelopeWrite(ch, 0xFF, old);
				envelopeWrite(ch, value, 0xFF);
			} else {
				envelopeWrite(ch, value, old);
			}
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
}

uint8_t gbApuRead(struct GB* gb, uint8_t address) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	if (address >= 0x30) {
		if (apu->wave.on) {
			// While it plays, a read gets the byte the channel is reading, if it reads it just then;
			// on the Advance, FF (Pan Docs).
			if (gb->model == GB_MODEL_AGB) {
				return 0xFF;
			}
			if (gbColor(gb)) {
				// The Color: the byte being read, the step due this very dot not yet taken.
				unsigned position = apu->waveReadAt == gb->now ? (apu->wave.position - 1) & 31 : apu->wave.position;
				return gb->io[0x30 + (position >> 1)];
			}
			if (gbSince(gb->now, apu->waveReadAt) >= 2 && gbSince(gb->now, apu->waveReadAt) < 4) {
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

// PCM12 and PCM34 (the Color's): each channel's digital output now, 0 to 15, two to a byte.
uint8_t gbApuPcm(struct GB* gb, bool high) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	uint8_t levels[2];
	for (int i = 0; i < 2; ++i) {
		int index = high ? 2 + i : i;
		struct GBChannel* ch = index < 2 ? &apu->square[index] : index == 2 ? &apu->wave : &apu->noise;
		uint8_t level = 0;
		if (ch->on) {
			if (index < 2) {
				uint8_t duty = duties[*reg(gb, index ? 0x16 : 0x11) >> 6];
				level = duty >> (7 - ch->position) & 1 ? ch->volume : 0;
			} else if (index == 2) {
				static const uint8_t shifts[4] = { 4, 0, 1, 2 };
				uint8_t sample = ch->position & 1 ? apu->waveSample & 0xF : apu->waveSample >> 4;
				level = sample >> shifts[*reg(gb, 0x1C) >> 5 & 3];
			} else {
				level = apu->lfsr & 1 ? 0 : ch->volume;
			}
		}
		levels[i] = level;
	}
	return levels[0] | levels[1] << 4;
}

void gbApuReset(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	memset(apu, 0, sizeof(*apu));
	apu->lastSync = gb->now;
	apu->sampleStart = gb->now;
	apu->lfsr = 0x7FFF;
}
