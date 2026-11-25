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
		default:
			break;
		}
	}
	updateNext(gb);
}

void gbRequest(struct GB* gb, uint8_t irq) {
	gb->ifl |= irq;
}

// STOP: on the Color with a speed switch armed, switches speed; otherwise sleeps until a button.
void gbStop(struct GB* gb) {
	if (gb->cgbMode && (gb->io[0x4D] & 1)) {
		gbApuSync(gb);
		gbTimerSync(gb);
		gb->doubleSpeed = !gb->doubleSpeed;
		gb->mcycle = gb->doubleSpeed ? 2 : 4;
		gb->io[0x4D] = gb->doubleSpeed ? 0x80 : 0;
		gb->timer.counter = 0;
		return;
	}
	gb->cpu.stopped = true;
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
	gb->cgbMode = model == GB_MODEL_CGB && (rom[0x143] & 0x80);
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

bool gbLoad(struct GB* gb, const uint8_t* rom, size_t size, enum GBModel model) {
	if (size < 0x150) {
		return false;
	}
	const uint8_t* boot = gb->boot;
	size_t bootSize = gb->bootSize;
	memset(gb, 0, sizeof(*gb));
	gb->boot = boot;
	gb->bootSize = bootSize;
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
	}
	gbApuSync(gb);
}
