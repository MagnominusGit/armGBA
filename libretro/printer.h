/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_PRINTER_H
#define ARMGBA_LIBRETRO_PRINTER_H

// The Game Boy Printer, as Pan Docs describes it. The Game Boy clocks each byte of a packet to it:
// $88 $33, a command, a compression flag, the data's length, the data, a checksum, then two $00 that
// the printer answers with $81 and its status.
// - $01 clears the image memory; $04 adds image data (tiles, 20 to a row), and an empty $04 ends it;
//   $02 prints it with four settings: sheets, margins (before in the high nibble, after in the
//   low), palette and exposure; $0F only asks for the status.
// - Status: bit 0 checksum error, 1 printing, 2 image data full, 3 unprocessed data.
// - Printing takes a second for each eight rows of pixels, as SameBoy has it. A packet left
//   unfinished for 100 ms is dropped.
//
// Times are in Game Boy sound samples (2^21 a second). A state of all zeros is a printer just
// switched on.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PRINTER_IMAGE = 0x2000 };

struct Printer {
	uint64_t lastByte;
	uint64_t busyUntil;
	uint32_t prints;
	uint16_t length;
	uint16_t left;
	uint16_t sum;
	uint16_t checksum;
	uint16_t stored;
	uint16_t got;
	uint8_t step;
	uint8_t command;
	uint8_t compressed;
	uint8_t run;
	uint8_t runRepeat;
	uint8_t status;
	uint8_t reply;
	uint8_t ended;
	uint8_t settings[4];
	uint8_t data[0x280];
	uint8_t image[PRINTER_IMAGE];
};

// A finished print: rows of 160 gray pixels, white 255. When the paper is cut after it, the
// printout so far is done. Each print has a number; a state loaded for a rollback or run-ahead
// makes the same print again, with the same number.
typedef void (*PrinterOutput)(const uint8_t* pixels, int rows, bool cut, uint32_t number);

void printerReset(struct Printer*);
// One byte the Game Boy sent at time now; returns the printer's byte for the same transfer.
uint8_t printerTransfer(struct Printer*, uint8_t out, uint64_t now, PrinterOutput output);

#ifdef __cplusplus
}
#endif

#endif
