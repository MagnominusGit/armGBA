/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
// gbrun rom [frames] [model]: runs a Game Boy ROM headless and prints what the serial port sent, the
// Mooneye result (Fibonacci registers after LD B,B, or all 0x42), and a hash of the last frame.
// env: SHOT=file.pgm saves the last frame (shades 0-3, or the Color's 15-bit values scaled).
#include "gb.h"
#include "../libretro/armgba-boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static char serialText[4096];
static size_t serialLength;

static void serialOut(void* user, uint8_t byte) {
	(void) user;
	if (serialLength + 1 < sizeof(serialText)) {
		serialText[serialLength++] = byte;
	}
}

static void saveShot(const struct GB* gb, const char* path) {
	FILE* o = fopen(path, "wb");
	if (gb->model == GB_MODEL_CGB) {
		// The Color's picture as 8-bit RGB (P6).
		fprintf(o, "P6\n%d %d\n255\n", GB_WIDTH, GB_HEIGHT);
		for (int y = 0; y < GB_HEIGHT; ++y) {
			for (int x = 0; x < GB_WIDTH; ++x) {
				uint16_t v = gb->frame[y][x];
				fputc((v & 31) * 255 / 31, o);
				fputc((v >> 5 & 31) * 255 / 31, o);
				fputc((v >> 10 & 31) * 255 / 31, o);
			}
		}
	} else {
		fprintf(o, "P5\n%d %d\n255\n", GB_WIDTH, GB_HEIGHT);
		for (int y = 0; y < GB_HEIGHT; ++y) {
			for (int x = 0; x < GB_WIDTH; ++x) {
				uint16_t v = gb->frame[y][x] == GB_SHADE_OFF ? 0 : gb->frame[y][x];
				fputc(255 - v * 85, o);
			}
		}
	}
	fclose(o);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s rom [frames] [dmg|cgb]\n", argv[0]);
		return 2;
	}
	FILE* f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	size_t padded = 0x8000;
	while (padded < (size_t) size) {
		padded <<= 1;
	}
	uint8_t* rom = calloc(1, padded);
	if (fread(rom, 1, size, f) != (size_t) size) {
		return 2;
	}
	fclose(f);
	int frames = argc > 2 ? atoi(argv[2]) : 600;
	enum GBModel model = argc > 3 && !strcmp(argv[3], "cgb") ? GB_MODEL_CGB : GB_MODEL_DMG;
	static struct GB gb;
	// armGBA's own boot ROMs, unless NOBOOT is set.
	if (!getenv("NOBOOT")) {
		gb.boot = model == GB_MODEL_CGB ? armgba_cgb_boot : getenv("POCKET") ? armgba_mgb_boot : armgba_dmg_boot;
		gb.bootSize = model == GB_MODEL_CGB ? sizeof(armgba_cgb_boot) : 256;
	}
	if (!gbLoad(&gb, rom, padded, model)) {
		fprintf(stderr, "can't load\n");
		return 2;
	}
	gb.serialOut = serialOut;
	// CHEAT=program: a cheat cartridge in front of the game (NOGAME=1: with nothing on top).
	static struct GBCheatCart cheat;
	if (getenv("CHEAT")) {
		FILE* c = fopen(getenv("CHEAT"), "rb");
		static uint8_t program[0x80000];
		size_t n = c ? fread(program, 1, sizeof(program), c) : 0;
		if (c) {
			fclose(c);
		}
		if (!gbCheatCartInit(&cheat, program, n, !getenv("NOGAME"))) {
			fprintf(stderr, "can't use the cheat cartridge\n");
			return 2;
		}
		gb.cheatCart = &cheat;
		gbReset(&gb);
	}
	// PRESS="frame:keys,...": holds keys (gb.keys bits) for 6 frames from each frame given.
	// SHOTS="frame,...": with SHOT, also saves those frames, as SHOT-frame.
	const char* press = getenv("PRESS");
	const char* result = "none";
	// MARK=n: times frames n to the end, and runs them all (no stopping at a result).
	int mark = getenv("MARK") ? atoi(getenv("MARK")) : -1;
	double marked = 0;
	uint64_t audioHash = 1469598103934665603ULL;
	int frame;
	for (frame = 0; frame < frames; ++frame) {
		if (frame == mark) {
			marked = now();
		}
		gb.keys = 0;
		for (const char* p = press; p && *p;) {
			int at = atoi(p), keys = (int) strtol(strchr(p, ':') + 1, NULL, 0);
			if (frame >= at && frame < at + 6) {
				gb.keys = keys;
			}
			p = strchr(p, ',');
			p = p ? p + 1 : NULL;
		}
		gbRunFrame(&gb);
		if (getenv("SHOTS") && getenv("SHOT")) {
			for (const char* p = getenv("SHOTS"); p && *p;) {
				if (atoi(p) == frame) {
					char path[512];
					snprintf(path, sizeof(path), "%s-%d.pgm", getenv("SHOT"), frame);
					saveShot(&gb, path);
				}
				p = strchr(p, ',');
				p = p ? p + 1 : NULL;
			}
		}
		for (uint32_t i = 0; i < gb.audioCount; ++i) {
			audioHash = (audioHash ^ (uint16_t) gb.audio[i]) * 1099511628211ULL;
		}
		if (mark >= 0) {
			continue;
		}
		const struct GBCpu* c = &gb.cpu;
		if (c->b == 3 && c->c == 5 && c->d == 8 && c->e == 13 && c->h == 21 && c->l == 34) {
			result = "mooneye-pass";
			break;
		}
		if (c->b == 0x42 && c->c == 0x42 && c->d == 0x42 && c->e == 0x42 && c->h == 0x42 && c->l == 0x42) {
			result = "mooneye-fail";
			break;
		}
		// Blargg's newer tests: a signature at $A001, then the status at $A000 and text from $A004.
		if (gb.sram[1] == 0xDE && gb.sram[2] == 0xB0 && gb.sram[3] == 0x61 && gb.sram[0] != 0x80) {
			result = gb.sram[0] ? "ram-fail" : "ram-pass";
			size_t n = strnlen((const char*) &gb.sram[4], 2000);
			memcpy(serialText, &gb.sram[4], n);
			serialLength = n;
			break;
		}
		// Mooneye's serial result: the Fibonacci numbers, or six 0x42.
		if (serialLength >= 6) {
			static const char pass[6] = { 3, 5, 8, 13, 21, 34 };
			if (!memcmp(serialText + serialLength - 6, pass, 6)) {
				result = "mooneye-pass";
				break;
			}
			if (!memcmp(serialText + serialLength - 6, "BBBBBB", 6)) {
				result = "mooneye-fail";
				break;
			}
		}
		if (strstr(serialText, "Passed") || strstr(serialText, "Failed")) {
			result = strstr(serialText, "Passed") ? "serial-pass" : "serial-fail";
			break;
		}
	}
	if (mark >= 0 && mark < frames) {
		printf("fps=%.1f\n", (frames - mark) / (now() - marked));
	}
	uint64_t hash = 1469598103934665603ULL;
	for (int y = 0; y < GB_HEIGHT; ++y) {
		for (int x = 0; x < GB_WIDTH; ++x) {
			hash = (hash ^ (gb.frame[y][x] == GB_SHADE_OFF ? 0 : gb.frame[y][x])) * 1099511628211ULL;
		}
	}
	if (getenv("SHOT")) {
		saveShot(&gb, getenv("SHOT"));
	}
	for (size_t i = 0; i < serialLength; ++i) {
		if (serialText[i] == '\n' || (serialText[i] >= 32 && serialText[i] < 127)) {
			continue;
		}
		serialText[i] = '.';
	}
	serialText[serialLength] = 0;
	printf("%s frames=%d hash=%016llx audio=%016llx pc=%04x\n%s%s", result, frame, (unsigned long long) hash, (unsigned long long) audioHash, gb.cpu.pc, serialText,
	       serialLength && serialText[serialLength - 1] != '\n' ? "\n" : "");
	return 0;
}
