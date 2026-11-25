/* Copyright (c) 2026 Magnominus
 * SPDX-License-Identifier: MIT */
#include "gb.h"

// The serial port (Pan Docs, "Serial Data Transfer"). With no cable, a transfer on the internal clock
// shifts in 1s and ends after eight bits at 8192 Hz; one on the external clock never ends.

enum {
	BIT_DOTS = 512,
};

void gbSerialWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	struct GBSerial* serial = &gb->serial;
	if (reg == 0x01) {
		serial->sb = value;
		return;
	}
	serial->sc = value & 0x83;
	if ((value & 0x81) == 0x81) {
		if (gb->serialOut) {
			gb->serialOut(gb->user, serial->sb);
		}
		serial->bits = 8;
		gbSchedule(gb, GB_EVENT_SERIAL, gb->now + BIT_DOTS * 8);
	} else {
		gbUnschedule(gb, GB_EVENT_SERIAL);
	}
}

void gbSerialEvent(struct GB* gb) {
	struct GBSerial* serial = &gb->serial;
	serial->sb = 0xFF;
	serial->bits = 0;
	serial->sc &= 0x7F;
	gbRequest(gb, GB_IRQ_SERIAL);
}
