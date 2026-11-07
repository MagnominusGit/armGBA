/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "printer.h"

#include <string.h>

enum {
	STEP_MAGIC_0,
	STEP_MAGIC_1,
	STEP_COMMAND,
	STEP_COMPRESSION,
	STEP_LENGTH_0,
	STEP_LENGTH_1,
	STEP_DATA,
	STEP_CHECKSUM_0,
	STEP_CHECKSUM_1,
	STEP_ACK,
	STEP_STATUS,
};

enum {
	COMMAND_INIT = 0x1,
	COMMAND_PRINT = 0x2,
	COMMAND_DATA = 0x4,
	COMMAND_STATUS = 0xF,
};

enum {
	STATUS_CHECKSUM = 0x01,
	STATUS_PRINTING = 0x02,
	STATUS_FULL = 0x04,
	STATUS_UNPROCESSED = 0x08,
};

enum {
	SECOND = 2097152,
	TIMEOUT = SECOND / 10,
};

void printerReset(struct Printer* p) {
	memset(p, 0, sizeof(*p));
}

// A packet's data byte, staged until the checksum is in. Compressed data has runs of one byte (a
// count byte with bit 7 set, two more than its low bits, then the byte) and stretches sent as they
// are (one more than the count).
static void dataByte(struct Printer* p, uint8_t byte) {
	if (p->compressed && !p->run) {
		p->runRepeat = byte & 0x80;
		p->run = (byte & 0x7F) + (p->runRepeat ? 2 : 1);
		return;
	}
	int times = p->compressed && p->runRepeat ? p->run : 1;
	if (p->compressed) {
		p->run = p->runRepeat ? 0 : p->run - 1;
	}
	for (; times && p->got < sizeof(p->data); --times) {
		p->data[p->got++] = byte;
	}
}

// The image in tiles, 20 to a row, in the print's shades.
static void print(struct Printer* p, uint64_t now, PrinterOutput output) {
	int const rows = p->stored / 40;
	uint8_t const palette = p->settings[2];
	// Exposure is the print head's burning time: $00 a quarter lighter, $7F a quarter darker.
	int const exposure = 96 + (p->settings[3] & 0x7F) * 64 / 127;
	uint8_t shades[4];
	for (int i = 0; i < 4; ++i) {
		int const ink = ((palette >> (2 * i)) & 3) * 85 * exposure / 128;
		shades[i] = 255 - (ink > 255 ? 255 : ink);
	}
	static uint8_t pixels[PRINTER_IMAGE / 40 * 160];
	for (int y = 0; y < rows; ++y) {
		for (int x = 0; x < 160; ++x) {
			const uint8_t* tile = &p->image[(y / 8 * 20 + x / 8) * 16 + (y % 8) * 2];
			int const bit = 7 - x % 8;
			int const color = ((tile[0] >> bit) & 1) | (((tile[1] >> bit) & 1) << 1);
			pixels[y * 160 + x] = shades[color];
		}
	}
	if (output && (p->settings[0] || (p->settings[1] & 0xF))) {
		output(pixels, p->settings[0] ? rows : 0, p->settings[1] & 0xF, ++p->prints);
	}
	p->busyUntil = now + (uint64_t) rows * SECOND / 8;
	p->status = (p->status & ~STATUS_UNPROCESSED) | STATUS_PRINTING | STATUS_FULL;
	p->stored = 0;
	p->ended = false;
}

static void command(struct Printer* p, uint64_t now, PrinterOutput output) {
	switch (p->command) {
	case COMMAND_INIT:
		p->status = 0;
		p->stored = 0;
		p->ended = false;
		break;
	case COMMAND_DATA:
		if (!p->length) {
			p->ended = true;
			break;
		}
		if (p->got > PRINTER_IMAGE - p->stored) {
			p->got = PRINTER_IMAGE - p->stored;
		}
		memcpy(&p->image[p->stored], p->data, p->got);
		p->stored += p->got;
		p->status |= STATUS_UNPROCESSED;
		break;
	case COMMAND_PRINT:
		if (p->length == 4 && p->ended) {
			print(p, now, output);
		}
		break;
	default:
		break;
	}
}

uint8_t printerTransfer(struct Printer* p, uint8_t out, uint64_t now, PrinterOutput output) {
	uint8_t const reply = p->reply;
	p->reply = 0;
	if (p->step != STEP_MAGIC_0 && now - p->lastByte > TIMEOUT) {
		p->step = STEP_MAGIC_0;
	}
	p->lastByte = now;
	if (p->status & STATUS_PRINTING && now >= p->busyUntil) {
		p->status &= ~STATUS_PRINTING;
	}
	switch (p->step) {
	case STEP_MAGIC_0:
		p->step = out == 0x88 ? STEP_MAGIC_1 : STEP_MAGIC_0;
		break;
	case STEP_MAGIC_1:
		p->step = out == 0x33 ? STEP_COMMAND : STEP_MAGIC_0;
		break;
	case STEP_COMMAND:
		p->command = out;
		p->sum = out;
		p->step = STEP_COMPRESSION;
		break;
	case STEP_COMPRESSION:
		p->compressed = out & 1;
		p->sum += out;
		p->step = STEP_LENGTH_0;
		break;
	case STEP_LENGTH_0:
		p->length = out;
		p->sum += out;
		p->step = STEP_LENGTH_1;
		break;
	case STEP_LENGTH_1:
		p->length |= out << 8;
		p->sum += out;
		p->left = p->length;
		p->run = 0;
		p->got = 0;
		p->step = p->length ? STEP_DATA : STEP_CHECKSUM_0;
		break;
	case STEP_DATA:
		p->sum += out;
		if (p->command == COMMAND_PRINT) {
			if (p->length - p->left < 4) {
				p->settings[p->length - p->left] = out;
			}
		} else if (p->command == COMMAND_DATA) {
			dataByte(p, out);
		}
		if (!--p->left) {
			p->step = STEP_CHECKSUM_0;
		}
		break;
	case STEP_CHECKSUM_0:
		p->checksum = out;
		p->step = STEP_CHECKSUM_1;
		break;
	case STEP_CHECKSUM_1:
		p->checksum |= out << 8;
		p->status &= ~STATUS_CHECKSUM;
		if (p->checksum != p->sum) {
			p->status |= STATUS_CHECKSUM;
		}
		p->reply = 0x81;
		p->step = STEP_ACK;
		break;
	case STEP_ACK:
		if (!(p->status & STATUS_CHECKSUM)) {
			command(p, now, output);
		}
		p->reply = p->status;
		p->step = STEP_STATUS;
		break;
	case STEP_STATUS:
		p->step = STEP_MAGIC_0;
		break;
	}
	return reply;
}
