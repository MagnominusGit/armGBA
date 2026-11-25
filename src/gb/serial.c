/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

// The serial port (Pan Docs, "Serial Data Transfer"). The internal clock is a bit of the divider's
// counter: a bit shifts on each of its falling edges, bit 8 (8192 Hz), or bit 3 with the Color's fast
// clock. With no cable, 1s shift in; a transfer on the external clock never ends.

static unsigned clockBit(const struct GB* gb) {
	return gb->cgbMode && (gb->serial.sc & 2) ? 3 : 8;
}

void gbSerialWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	struct GBSerial* serial = &gb->serial;
	if (reg == 0x01) {
		serial->sb = value;
		return;
	}
	serial->sc = value & (gb->cgbMode ? 0x83 : 0x81);
	if ((value & 0x81) != 0x81) {
		gbUnschedule(gb, GB_EVENT_SERIAL);
		return;
	}
	if (gb->serialOut) {
		gb->serialOut(gb->user, serial->sb);
	}
	// The eighth falling edge of the clock bit from now.
	gbTimerSync(gb);
	unsigned shift = clockBit(gb) + 1;
	uint32_t c = gb->timer.counter;
	uint32_t edge = ((c >> shift) + 8) << shift;
	uint32_t per = gb->doubleSpeed ? 2 : 1;
	serial->bits = 8;
	gbSchedule(gb, GB_EVENT_SERIAL, gb->now + (edge - c + per - 1) / per);
}

void gbSerialEvent(struct GB* gb) {
	struct GBSerial* serial = &gb->serial;
	serial->sb = 0xFF;
	serial->bits = 0;
	serial->sc &= 0x7F;
	gbRequest(gb, GB_IRQ_SERIAL);
}
