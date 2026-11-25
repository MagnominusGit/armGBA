/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The machine: loading, reset, the events and running a frame.

static void updateNext(struct GB* gb) {
	uint32_t best = gb->now + 0x40000000;
	for (int i = 0; i < GB_EVENTS; ++i) {
		if ((gb->eventsOn >> i & 1) && gbSince(gb->events[i], best) < 0) {
			best = gb->events[i];
		}
	}
	gb->nextEvent = best;
}

void gbSchedule(struct GB* gb, int event, uint32_t at) {
	gb->events[event] = at;
	gb->eventsOn |= 1u << event;
	updateNext(gb);
}

void gbUnschedule(struct GB* gb, int event) {
	gb->eventsOn &= ~(1u << event);
	updateNext(gb);
}

// Runs every event due by now, earliest first.
void gbRunEvents(struct GB* gb) {
	for (;;) {
		int due = -1;
		for (int i = 0; i < GB_EVENTS; ++i) {
			if ((gb->eventsOn >> i & 1) && gbSince(gb->now, gb->events[i]) >= 0 &&
			    (due < 0 || gbSince(gb->events[i], gb->events[due]) < 0)) {
				due = i;
			}
		}
		if (due < 0) {
			break;
		}
		gb->eventsOn &= ~(1u << due);
		switch (due) {
		case GB_EVENT_PPU:
			gbPpuEvent(gb);
			break;
		case GB_EVENT_TIMER:
			gbTimerEvent(gb);
			break;
		case GB_EVENT_SERIAL:
			gbSerialEvent(gb);
			break;
		case GB_EVENT_DMA:
			gbDmaSync(gb);
			break;
		case GB_EVENT_HDMA:
			gbHdmaEvent(gb);
			break;
		case GB_EVENT_SPEED:
			gbSpeedEvent(gb);
			break;
		default:
			break;
		}
	}
	updateNext(gb);
}

void gbRequest(struct GB* gb, uint8_t irq) {
	gb->ifl |= irq;
	gb->attention = true;
}

// The speed switch (SameBoy's stop() and GB_advance_cycles()): to double speed 6 cycles on, back to
// single at once; the PPU stands still for 5 cycles (1 to double with an interrupt pending), and unless
// an interrupt is pending the CPU halts for 0x20008 cycles counted at each speed, the divider running.
enum { SWITCH_CYCLES = 0x20008, SWITCH_TO_DOUBLE = 6 };

static void setSpeed(struct GB* gb, bool doubleSpeed) {
	gbApuSync(gb);
	gbTimerSync(gb);
	gb->doubleSpeed = doubleSpeed;
	gb->mcycle = doubleSpeed ? 2 : 4;
	gb->io[0x4D] = doubleSpeed ? 0x80 : 0;
}

void gbSpeedEvent(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	if (cpu->switching == 1) {
		setSpeed(gb, true);
		if (cpu->halted) {
			cpu->switching = 2;
			// 4 dots short of the count, where SameBoy's CPU goes on (measured with gbsync).
			gbSchedule(gb, GB_EVENT_SPEED, gb->now + (SWITCH_CYCLES - SWITCH_TO_DOUBLE) / 2 - 4);
		} else {
			cpu->switching = 0;
		}
		return;
	}
	if (cpu->switching == 2) {
		cpu->switching = 0;
		cpu->halted = false;
		gb->attention = true;
	}
}

// STOP: on the Color with a speed switch armed, switches speed; otherwise sleeps until a button.
void gbStop(struct GB* gb) {
	gb->attention = true;
	if (gb->cgbMode && (gb->io[0x4D] & 1)) {
		struct GBCpu* cpu = &gb->cpu;
		bool pending = gb->ie & gb->ifl & 0x1F;
		bool toDouble = !gb->doubleSpeed;
		gb->io[0x4D] &= 0x80;
		gbPpuFreeze(gb, pending ? (toDouble ? 1 : 0) : 5);
		if (!pending) {
			cpu->halted = true;
			cpu->justHalted = true;
		}
		if (toDouble) {
			cpu->switching = 1;
			gbSchedule(gb, GB_EVENT_SPEED, gb->now + SWITCH_TO_DOUBLE);
		} else {
			setSpeed(gb, false);
			if (!pending) {
				cpu->switching = 2;
				gbSchedule(gb, GB_EVENT_SPEED, gb->now + SWITCH_CYCLES);
			}
		}
		return;
	}
	gb->cpu.stopped = true;
}

// Work RAM as the original Game Boy's tends to come up: 256-byte blocks of mostly 0s and mostly 1s,
// alternating with address bit 8 and swapped by bit 11, with about one bit in fifty the other way.
// The stray bits come from a fixed seed, so every run starts alike.
static void powerOnRam(struct GB* gb) {
	uint32_t seed = 0x2F6E2B1;
	for (unsigned i = 0; i < GB_SIZE_WRAM; ++i) {
		uint8_t value = ((i >> 8) ^ (i >> 11)) & 1 ? 0xFF : 0x00;
		for (int bit = 0; bit < 8; ++bit) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			if (seed % 50 == 0) {
				value ^= 1 << bit;
			}
		}
		gb->wram[i] = value;
	}
}

// The state the boot ROM leaves (Pan Docs, "Power Up Sequence").
void gbReset(struct GB* gb) {
	const uint8_t* rom = gb->rom;
	size_t romSize = gb->romSize;
	void (*serialOut)(void*, uint8_t) = gb->serialOut;
	void* user = gb->user;
	uint8_t model = gb->model;
	struct GBCart cart = gb->cart;
	memset(gb, 0, offsetof(struct GB, rom));
	gb->rom = rom;
	gb->romSize = romSize;
	gb->serialOut = serialOut;
	gb->user = user;
	gb->model = model;
	gb->cart = cart;
	gbCartInit(gb);
	// The Color's boot ROM runs in Color mode and leaves it by KEY0; without one, the header decides.
	gb->cgbMode = model == GB_MODEL_CGB && (gb->boot || (rom[0x143] & 0x80));
	gbBlankFrame(gb);
	if (model == GB_MODEL_DMG) {
		powerOnRam(gb);
	}
	gb->mcycle = 4;
	struct GBCpu* cpu = &gb->cpu;
	gb->joypadSelect = 0;
	gb->wramBank = 1;
	if (gb->boot) {
		// Power on into the boot ROM: everything at 0 but the divider, which starts at 8 (as SameBoy
		// has it), and the LCD off.
		gb->bootMapped = true;
		gb->timer.counter = 8;
		gb->timer.reloadedAt = gb->now - 0x10000;
		gbMapRom(gb);
		gbApuReset(gb);
		gbPpuReset(gb);
		gb->ppu.lcdc = 0;
		gbUnschedule(gb, GB_EVENT_PPU);
		updateNext(gb);
		return;
	}
	if (model == GB_MODEL_CGB) {
		cpu->a = 0x11;
		cpu->f = 0x80;
		cpu->d = 0xFF;
		cpu->e = 0x56;
		cpu->l = 0x0D;
	} else {
		cpu->a = 0x01;
		cpu->f = 0xB0;
		cpu->c = 0x13;
		cpu->e = 0xD8;
		cpu->h = 0x01;
		cpu->l = 0x4D;
	}
	cpu->sp = 0xFFFE;
	cpu->pc = 0x100;
	gb->timer.counter = 0xABCC;
	gb->timer.reloadedAt = gb->now - 0x10000;
	gb->ifl = 0x01;
	gbMapRom(gb);
	gbApuReset(gb);
	gb->apu.power = true;
	gb->apu.regs[0x24 - 0x10] = 0x77;
	gb->apu.regs[0x25 - 0x10] = 0xF3;
	gbPpuReset(gb);
	updateNext(gb);
}

// The screen showing nothing, as with the LCD off.
void gbBlankFrame(struct GB* gb) {
	for (int y = 0; y < GB_HEIGHT; ++y) {
		for (int x = 0; x < GB_WIDTH; ++x) {
			gb->frame[y][x] = gb->model == GB_MODEL_CGB ? 0x7FFF : GB_SHADE_OFF;
			if (gb->video) {
				gb->video[y * GB_WIDTH + x] = gb->model == GB_MODEL_CGB && gb->videoTable ? gb->videoTable[0x7FFF] : gb->videoColors[4];
			}
		}
	}
}

// A state is the struct up to the cartridge's pointer: everything after it is set again on loading.
size_t gbStateSize(void) {
	return offsetof(struct GB, rom);
}

void gbSaveState(const struct GB* gb, void* data) {
	memcpy(data, gb, gbStateSize());
}

bool gbLoadState(struct GB* gb, const void* data, size_t size) {
	if (size < gbStateSize()) {
		return false;
	}
	memcpy(gb, data, gbStateSize());
	gbMapRom(gb);
	// The device's time goes on from now, not from when the state was saved.
	gb->cart.rtc.lastHost = gb->hostMicros;
	// A frontend's picture isn't in a state: blank while the screen shows nothing, else redrawn by
	// the next frame.
	if (gb->video && (!(gb->ppu.lcdc & 0x80) || gb->ppu.hiddenFrame)) {
		gbBlankFrame(gb);
	}
	return true;
}

bool gbLoad(struct GB* gb, const uint8_t* rom, size_t size, enum GBModel model) {
	if (size < 0x150) {
		return false;
	}
	const uint8_t* boot = gb->boot;
	size_t bootSize = gb->bootSize;
	uint32_t* video = gb->video;
	const uint32_t* videoTable = gb->videoTable;
	bool rtcRealTime = gb->rtcRealTime;
	unsigned rtcPercent = gb->rtcPercent ? gb->rtcPercent : 100;
	int64_t hostMicros = gb->hostMicros;
	uint32_t videoColors[5];
	memcpy(videoColors, gb->videoColors, sizeof(videoColors));
	memset(gb, 0, sizeof(*gb));
	gb->boot = boot;
	gb->bootSize = bootSize;
	gb->video = video;
	gb->videoTable = videoTable;
	gb->rtcRealTime = rtcRealTime;
	gb->rtcPercent = rtcPercent;
	gb->hostMicros = hostMicros;
	memcpy(gb->videoColors, videoColors, sizeof(videoColors));
	gb->rom = rom;
	gb->romSize = size;
	gb->model = model;
	if (!gbCartInit(gb)) {
		return false;
	}
	gbReset(gb);
	return true;
}

// Runs to the start of the next V-blank, or a frame's time with the LCD off.
void gbRunFrame(struct GB* gb) {
	uint32_t limit = gb->now + GB_FRAME_DOTS;
	gb->frameEnded = false;
	gb->audioCount = 0;
	while (!gb->frameEnded && gbSince(limit, gb->now) > 0) {
		gbCpuRun(gb, limit);
		// With the LCD on, a frame goes on to V-blank, however late it started.
		if (!gb->frameEnded && (gb->ppu.lcdc & 0x80) && gbSince(gb->now, limit) >= 0) {
			limit = gb->now + GB_FRAME_DOTS;
		}
	}
	gbApuSync(gb);
	// The clock counts in a time that wraps after some minutes: up to date every frame.
	if (gb->cart.hasRtc) {
		gbRtcSync(gb);
	}
}
