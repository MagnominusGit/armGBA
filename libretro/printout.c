/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Game Boy Printer's paper. Prints without a cut after them run on as one strip, as a long
// printout does on the real paper; a cut ends the strip, and it is saved as a gray PNG named after
// the game and the time, in the save folder.

#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

enum {
	WIDTH = 160,
	MOST_ROWS = 8192,
	REMEMBERED = 16,
};

static uint8_t* paper;
static int paperRows;
// The last prints taken, by number, contents and when, so the ones a rollback or run-ahead makes
// again moments later are left out; the same print made again after a state is loaded by hand is
// kept.
static uint32_t takenNumber[REMEMBERED];
static uint32_t takenCrc[REMEMBERED];
static double takenAt[REMEMBERED];
static int taken;

static double seconds(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec + now.tv_nsec / 1e9;
}

static void chunk(FILE* f, const char* type, const uint8_t* data, uint32_t size) {
	uint8_t head[8] = { size >> 24, size >> 16, size >> 8, size, type[0], type[1], type[2], type[3] };
	fwrite(head, 1, 8, f);
	uLong crc = crc32(0, head + 4, 4);
	if (size) {
		fwrite(data, 1, size, f);
		crc = crc32(crc, data, size);
	}
	uint8_t tail[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
	fwrite(tail, 1, 4, f);
}

static bool writePng(const char* path, const uint8_t* pixels, int rows) {
	size_t const rawSize = (size_t) rows * (WIDTH + 1);
	uint8_t* raw = malloc(rawSize);
	uLongf packedSize = compressBound(rawSize);
	uint8_t* packed = malloc(packedSize);
	FILE* f = raw && packed ? fopen(path, "wb") : NULL;
	bool written = false;
	if (f) {
		for (int y = 0; y < rows; ++y) {
			raw[y * (WIDTH + 1)] = 0;
			memcpy(&raw[y * (WIDTH + 1) + 1], &pixels[y * WIDTH], WIDTH);
		}
		if (compress2(packed, &packedSize, raw, rawSize, 9) == Z_OK) {
			static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
			// 8-bit grayscale.
			uint8_t header[13] = { 0, 0, 0, WIDTH, rows >> 24, rows >> 16, rows >> 8, rows, 8, 0, 0, 0, 0 };
			fwrite(signature, 1, 8, f);
			chunk(f, "IHDR", header, 13);
			chunk(f, "IDAT", packed, (uint32_t) packedSize);
			chunk(f, "IEND", NULL, 0);
			written = !ferror(f);
		}
		written &= !fclose(f);
	}
	free(raw);
	free(packed);
	return written;
}

// The save folder, or the game's own; the game's name without its folder or extension.
static void printoutPath(char* out, size_t size) {
	const char* folder = NULL;
	environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &folder);
	const char* slash = strrchr(firstPath, '/');
	const char* name = slash ? slash + 1 : firstPath;
	const char* dot = strrchr(name, '.');
	int const nameLength = dot ? (int) (dot - name) : (int) strlen(name);
	char gameFolder[PATH_SIZE];
	if (!folder || !*folder) {
		snprintf(gameFolder, sizeof(gameFolder), "%.*s", slash ? (int) (slash - firstPath) : 1, slash ? firstPath : ".");
		folder = gameFolder;
	}
	char stamp[32];
	time_t const now = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	for (int n = 1; n < 100; ++n) {
		snprintf(out, size, n == 1 ? "%s/%.*s print %s.png" : "%s/%.*s print %s-%d.png", folder, nameLength, name, stamp, n);
		FILE* exists = fopen(out, "rb");
		if (!exists) {
			return;
		}
		fclose(exists);
	}
}

void printoutFinish(void) {
	if (paperRows) {
		char path[SAVE_PATH_SIZE];
		printoutPath(path, sizeof(path));
		if (writePng(path, paper, paperRows)) {
			const char* name = strrchr(path, '/');
			char message[256];
			snprintf(message, sizeof(message), "Printed: %.200s", name ? name + 1 : path);
			notify(message, 240);
		} else {
			showMessage("armGBA couldn't save the printout");
		}
	}
	free(paper);
	paper = NULL;
	paperRows = 0;
}

void printoutPage(const uint8_t* pixels, int rows, bool cut, uint32_t number) {
	uint32_t const crc = (uint32_t) crc32(number, pixels, (uInt) rows * WIDTH);
	double const now = seconds();
	for (int i = 0; i < taken && i < REMEMBERED; ++i) {
		if (takenNumber[i] == number && takenCrc[i] == crc && now - takenAt[i] < 10) {
			return;
		}
	}
	takenNumber[taken % REMEMBERED] = number;
	takenCrc[taken % REMEMBERED] = crc;
	takenAt[taken % REMEMBERED] = now;
	++taken;
	if (rows > MOST_ROWS - paperRows) {
		rows = MOST_ROWS - paperRows;
	}
	if (rows > 0) {
		uint8_t* grown = realloc(paper, (size_t) (paperRows + rows) * WIDTH);
		if (grown) {
			paper = grown;
			memcpy(&paper[paperRows * WIDTH], pixels, (size_t) rows * WIDTH);
			paperRows += rows;
		}
	}
	if (cut) {
		printoutFinish();
	}
}
