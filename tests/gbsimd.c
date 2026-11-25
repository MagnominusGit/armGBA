/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
// gbsimd [rounds]: armGB's SIMD drawing (ppu-simd.c) against its plain loop, on random tiles and palettes.
#include "gb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t seed = 0x9E3779B97F4A7C15ULL;
static uint64_t next(void) {
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

int main(int argc, char** argv) {
	long rounds = argc > 1 ? atol(argv[1]) : 100000;
	static uint32_t palettes[8][4];
	for (long r = 0; r < rounds; ++r) {
		for (int p = 0; p < 8; ++p) {
			for (int i = 0; i < 4; ++i) {
				palettes[p][i] = (uint32_t) next();
			}
		}
		int count = 1 + (int) (next() % 21);
		uint64_t pixels[21];
		const uint32_t* colors[21];
		uint8_t front[21];
		for (int t = 0; t < count; ++t) {
			pixels[t] = next() & 0x0303030303030303ULL;
			colors[t] = palettes[next() & 7];
			front[t] = next() & 1 ? 4 : 0;
		}
		uint32_t lineA[21 * 8], lineB[21 * 8];
		uint8_t infoA[21 * 8], infoB[21 * 8];
		memset(lineA, 0xAA, sizeof(lineA));
		memset(lineB, 0xAA, sizeof(lineB));
		memset(infoA, 0x55, sizeof(infoA));
		memset(infoB, 0x55, sizeof(infoB));
		gbExpandTiles(lineA, infoA, pixels, colors, front, count);
		gbExpandTilesPlain(lineB, infoB, pixels, colors, front, count);
		if (memcmp(lineA, lineB, sizeof(lineA)) || memcmp(infoA, infoB, sizeof(infoA))) {
			printf("differs in round %ld (%d tiles)\n", r, count);
			return 1;
		}
	}
	for (long r = 0; r < rounds; ++r) {
		uint32_t lut[4];
		for (int i = 0; i < 4; ++i) {
			lut[i] = (uint32_t) next();
		}
		uint8_t index[GB_WIDTH + 16];
		for (int x = 0; x < GB_WIDTH + 16; ++x) {
			index[x] = next() & 3;
		}
		int count = 1 + (int) (next() % (GB_WIDTH + 16));
		int from = (int) (next() % 8);
		count = count + from > GB_WIDTH + 16 ? GB_WIDTH + 16 - from : count;
		uint32_t a[GB_WIDTH + 16], b[GB_WIDTH + 16];
		memset(a, 0xAA, sizeof(a));
		memset(b, 0xAA, sizeof(b));
		gbShadeLine(a, index + from, lut, count);
		gbShadeLinePlain(b, index + from, lut, count);
		if (memcmp(a, b, sizeof(a))) {
			printf("shades differ in round %ld\n", r);
			return 1;
		}
	}
	printf("same in %ld rounds\n", rounds);
	return 0;
}
