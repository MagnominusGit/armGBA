/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
// gbcmp core.so rom frames "frame:button[:hold],...": runs an original Game Boy game on armGB and, as a
// black box through libretro, on another core (armGBA's own, with its Gambatte core), from power-on
// with armGBA's boot ROM and the same buttons, and compares their pictures frame by frame as the four
// shades. A frame counts as matched if the other core shows the same picture within two frames of it.
// Prints one line: the cartridge type, frames matched, the first frame matched by neither, and how
// many pixels differ there.
#define _GNU_SOURCE
#include "gb.h"
#include "../libretro/armgba-boot.h"
#include "../libretro/libretro.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	MAX_FRAMES = 20000,
};

static uint16_t (*theirs)[GB_HEIGHT][GB_WIDTH];
// CGB=1: the Game Boy Color, its colours as 15-bit RGB.
static bool colorModel;
// Work RAM after each frame, from both cores.
// Work RAM: 8 KiB, or all eight 4 KiB banks of the Game Boy Color.
#define RAM_MAX 0x8000
static size_t ramBytes = 0x2000;
static uint8_t (*theirRam)[RAM_MAX];
// Cartridge RAM, the first 32 KiB, each frame: games keep variables there too.
#define SRAM_MAX 0x8000
static uint8_t (*theirSram)[SRAM_MAX];
static uint8_t (*ourSram)[SRAM_MAX];
static uint8_t (*ourRam)[RAM_MAX];
static int theirCount;
static unsigned pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned heldButtons;
static char systemDir[] = "/tmp/gbcmp-XXXXXX";

static const struct {
	const char* key;
	const char* value;
} options[] = {
	{ "armgba_gb_boot_rom", "open-source" },
	{ "armgba_gb_model", "Game Boy" },
	{ "armgba_gb_core", "gambatte" },
	{ "armgba_gb_screen", "grayscale" },
	{ "armgba_gb_contrast", "0" },
	{ "armgba_gb_color_correction", "disabled" },
	{ "armgba_lcd_ghosting", "disabled" },
	{ "armgba_lcd_grid", "disabled" },
	{ "armgba_threaded_video", "disabled" },
};

static bool environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = data;
		// The environment can set any option, over the ones below.
		v->value = getenv(v->key);
		if (v->value) {
			return true;
		}
		for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
			if (!strcmp(v->key, options[i].key)) {
				v->value = options[i].value;
			}
		}
		if (colorModel && !strcmp(v->key, "armgba_gb_model")) {
			v->value = "Game Boy Color";
		}
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixelFormat = *(const unsigned*) data;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		// SYSDIR: the other core's system folder (boot ROMs), else an empty one.
		*(const char**) data = getenv("SYSDIR") ? getenv("SYSDIR") : systemDir;
		return true;
	default:
		return false;
	}
}

// A grey to its shade: 0 white to 3 black, the nearest of the four.
static uint8_t shade(unsigned level) {
	static const unsigned levels[4] = { 0xFF, 0xA5, 0x52, 0x00 };
	int best = 0;
	for (int i = 1; i < 4; ++i) {
		if (abs((int) level - (int) levels[i]) < abs((int) level - (int) levels[best])) {
			best = i;
		}
	}
	return best;
}

static void video(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (!data || theirCount >= MAX_FRAMES) {
		return;
	}
	uint16_t(*out)[GB_WIDTH] = theirs[theirCount++];
	for (unsigned y = 0; y < GB_HEIGHT && y < height; ++y) {
		for (unsigned x = 0; x < GB_WIDTH && x < width; ++x) {
			uint32_t c = ((const uint32_t*) ((const uint8_t*) data + y * pitch))[x];
			if (colorModel) {
				out[y][x] = (c >> 19 & 0x1F) | (c >> 11 & 0x1F) << 5 | (c >> 3 & 0x1F) << 10;
			} else {
				out[y][x] = shade(c >> 8 & 0xFF);
			}
		}
	}
}

static void audioSample(int16_t l, int16_t r) {
	(void) l;
	(void) r;
}

// The other core's sound: its root mean square, from frame 600 on.
static double audioSquares;
static long audioCount;
static int currentFrame;

static long batchFrames;

static size_t audioBatch(const int16_t* data, size_t frames) {
	batchFrames += frames;
	if (currentFrame >= 600) {
		for (size_t i = 0; i < frames * 2; ++i) {
			audioSquares += (double) data[i] * data[i];
		}
		audioCount += frames * 2;
	}
	return frames;
}

static void inputPoll(void) {
}

// libretro's joypad buttons to the Game Boy's.
static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) index;
	if (port || device != RETRO_DEVICE_JOYPAD) {
		return 0;
	}
	static const unsigned keys[16] = {
		[RETRO_DEVICE_ID_JOYPAD_B] = GB_KEY_B, [RETRO_DEVICE_ID_JOYPAD_SELECT] = GB_KEY_SELECT,
		[RETRO_DEVICE_ID_JOYPAD_START] = GB_KEY_START, [RETRO_DEVICE_ID_JOYPAD_UP] = GB_KEY_UP,
		[RETRO_DEVICE_ID_JOYPAD_DOWN] = GB_KEY_DOWN, [RETRO_DEVICE_ID_JOYPAD_LEFT] = GB_KEY_LEFT,
		[RETRO_DEVICE_ID_JOYPAD_RIGHT] = GB_KEY_RIGHT, [RETRO_DEVICE_ID_JOYPAD_A] = GB_KEY_A,
	};
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
		int16_t mask = 0;
		for (unsigned i = 0; i < 16; ++i) {
			if (keys[i] && (heldButtons & keys[i])) {
				mask |= 1 << i;
			}
		}
		return mask;
	}
	return id < 16 && keys[id] && (heldButtons & keys[id]) ? 1 : 0;
}

// The buttons held in a frame, from "frame:button[:hold],...".
static unsigned buttonsAt(const char* script, int frame) {
	unsigned held = 0;
	char* copy = strdup(script);
	for (char* item = strtok(copy, ","); item; item = strtok(NULL, ",")) {
		int start = 0, hold = 1;
		char name[16] = "";
		if (sscanf(item, "%d:%15[^:]:%d", &start, name, &hold) < 2 || frame < start || frame >= start + hold) {
			continue;
		}
		static const struct {
			const char* name;
			unsigned key;
		} names[] = { { "a", GB_KEY_A }, { "b", GB_KEY_B }, { "select", GB_KEY_SELECT }, { "start", GB_KEY_START },
			          { "right", GB_KEY_RIGHT }, { "left", GB_KEY_LEFT }, { "up", GB_KEY_UP }, { "down", GB_KEY_DOWN } };
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
			if (!strcmp(name, names[i].name)) {
				held |= names[i].key;
			}
		}
	}
	free(copy);
	return held;
}

static uint64_t hashPicture(const uint16_t (*p)[GB_WIDTH]) {
	uint64_t hash = 1469598103934665603ULL;
	for (int y = 0; y < GB_HEIGHT; ++y) {
		for (int x = 0; x < GB_WIDTH; ++x) {
			hash = (hash ^ p[y][x]) * 1099511628211ULL;
		}
	}
	return hash;
}

int main(int argc, char** argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: %s core.so rom frames inputs\n", argv[0]);
		return 2;
	}
	int frames = atoi(argv[3]);
	if (frames > MAX_FRAMES) {
		frames = MAX_FRAMES;
	}
	const char* script = argv[4];
	FILE* f = fopen(argv[2], "rb");
	if (!f) {
		perror(argv[2]);
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
	if (!mkdtemp(systemDir)) {
		return 2;
	}
	colorModel = getenv("CGB") != NULL;
	ramBytes = colorModel ? 0x8000 : 0x2000;
	theirs = calloc(MAX_FRAMES, sizeof(*theirs));
	theirRam = calloc(frames, sizeof(*theirRam));
	theirSram = calloc(frames, sizeof(*theirSram));
	ourSram = calloc(frames, sizeof(*ourSram));
	ourRam = calloc(frames, sizeof(*ourRam));
	uint16_t(*ours)[GB_HEIGHT][GB_WIDTH] = calloc(frames, sizeof(*ours));

	// The other core, frame by frame.
	void* core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!core) {
		fprintf(stderr, "%s\n", dlerror());
		return 2;
	}
#define SYM(name) ((void (*)(void)) dlsym(core, #name))
	((void (*)(retro_environment_t)) SYM(retro_set_environment))(environment);
	((void (*)(retro_video_refresh_t)) SYM(retro_set_video_refresh))(video);
	((void (*)(retro_audio_sample_t)) SYM(retro_set_audio_sample))(audioSample);
	((void (*)(retro_audio_sample_batch_t)) SYM(retro_set_audio_sample_batch))(audioBatch);
	((void (*)(retro_input_poll_t)) SYM(retro_set_input_poll))(inputPoll);
	((void (*)(retro_input_state_t)) SYM(retro_set_input_state))(inputState);
	SYM(retro_init)();
	struct retro_game_info info = { argv[2], rom, (size_t) size, NULL };
	if (!((bool (*)(const struct retro_game_info*)) SYM(retro_load_game))(&info)) {
		fprintf(stderr, "the other core can't load it\n");
		return 2;
	}
	for (int i = 0; i < frames; ++i) {
		heldButtons = buttonsAt(script, i);
		currentFrame = i;
		int before = theirCount;
		long samplesBefore = batchFrames;
		SYM(retro_run)();
		if (getenv("TIMES") && i < atoi(getenv("TIMES"))) {
			fprintf(stderr, "theirs %d: %ld samples\n", i, batchFrames - samplesBefore);
		}
		const uint8_t* ram = ((void* (*)(unsigned)) dlsym(core, "retro_get_memory_data"))(RETRO_MEMORY_SYSTEM_RAM);
		size_t ramSize = ((size_t (*)(unsigned)) dlsym(core, "retro_get_memory_size"))(RETRO_MEMORY_SYSTEM_RAM);
		if (ram && ramSize >= ramBytes) {
			memcpy(theirRam[i], ram, ramBytes);
		}
		const uint8_t* sram = ((void* (*)(unsigned)) dlsym(core, "retro_get_memory_data"))(RETRO_MEMORY_SAVE_RAM);
		size_t sramSize = ((size_t (*)(unsigned)) dlsym(core, "retro_get_memory_size"))(RETRO_MEMORY_SAVE_RAM);
		if (sram) {
			memcpy(theirSram[i], sram, sramSize < SRAM_MAX ? sramSize : SRAM_MAX);
		}
		// A frame with no picture keeps the last.
		if (theirCount == before && theirCount && theirCount < MAX_FRAMES) {
			memcpy(theirs[theirCount], theirs[theirCount - 1], sizeof(theirs[0]));
			++theirCount;
		}
	}
	SYM(retro_unload_game)();
	SYM(retro_deinit)();

	// armGB.
	static struct GB gb;
	// NOBOOT=1: armGB without a boot ROM, as the other core runs Sachen's cartridges.
	if (!getenv("NOBOOT")) {
		gb.boot = colorModel ? armgba_cgb_boot : armgba_dmg_boot;
		gb.bootSize = colorModel ? sizeof(armgba_cgb_boot) : sizeof(armgba_dmg_boot);
	}
	if (!gbLoad(&gb, rom, padded, colorModel ? GB_MODEL_CGB : GB_MODEL_DMG)) {
		fprintf(stderr, "armGB can't load it\n");
		return 2;
	}
	// armgba_cpu_clock (as the other core's option): armGB's CPU Clock too, from when the boot ROM
	// hands over (the option's CPU Clock During Boot at 100%).
	unsigned quarters = getenv("armgba_cpu_clock") ? atoi(getenv("armgba_cpu_clock")) / 25 : 4;
	if (getenv("RAMOUT")) {
		FILE* o = fopen(getenv("RAMOUT"), "wb");
		fwrite(theirRam[0], 1, ramBytes, o);
		fclose(o);
	}
	// Work RAM starts as the other core's did at power-on (its first frame is the boot ROM's, which
	// leaves work RAM alone), so games that read it before writing it run alike. For this test only.
	memcpy(gb.wram, theirRam[0], ramBytes);
	for (int i = 0; i < frames; ++i) {
		if (!gb.bootMapped && gb.cpuQuarters != quarters) {
			gbSetCpuQuarters(&gb, quarters, false);
		}
		gb.keys = buttonsAt(script, i);
		uint32_t started = gb.now;
		gbRunFrame(&gb);
		if (getenv("TIMES") && i < atoi(getenv("TIMES"))) {
			fprintf(stderr, "ours %d: %u dots lcdc %02x frameCount %u double %d pc %04x line %u dot %d\n", i, gb.now - started, gb.ppu.lcdc, gb.frameCount, gb.doubleSpeed, gb.cpu.pc, gb.ppu.line, (int) (gb.now - gb.ppu.lineStart));
		}
		memcpy(ourRam[i], gb.wram, ramBytes);
		memcpy(ourSram[i], gb.sram, gb.cart.ramSize < SRAM_MAX ? gb.cart.ramSize : SRAM_MAX);
		for (int y = 0; y < GB_HEIGHT; ++y) {
			for (int x = 0; x < GB_WIDTH; ++x) {
				ours[i][y][x] = !colorModel && gb.frame[y][x] == GB_SHADE_OFF ? 0 : gb.frame[y][x];
			}
		}
	}

	// Matched: the other core shows the same picture within two frames.
	int n = frames < theirCount ? frames : theirCount;
	uint64_t* hashOurs = malloc(n * sizeof(uint64_t));
	uint64_t* hashTheirs = malloc(n * sizeof(uint64_t));
	for (int i = 0; i < n; ++i) {
		hashOurs[i] = hashPicture(ours[i]);
		hashTheirs[i] = hashPicture(theirs[i]);
	}
	// FROM=n: the first unmatched frame is looked for from frame n.
	int from = getenv("FROM") ? atoi(getenv("FROM")) : 0;
	// The other core's frames can run a frame or two longer around the LCD switching on or off, so
	// the offset between the two drifts: each match recentres the window on the frame it found.
	int matched = 0, exact = 0, first = -1, unmatched = 0, drift = 0, maxDrift = 0;
	for (int i = 0; i < n; ++i) {
		bool found = false;
		for (int k = 0; k <= 2 && !found; ++k) {
			for (int sign = 1; sign >= -1 && !found; sign -= 2) {
				int j = i + drift + k * sign;
				// No further than a second either way, so a screen held on one side doesn't draw it off.
				if (j >= 0 && j < n && abs(drift + k * sign) <= 60 && hashOurs[i] == hashTheirs[j]) {
					found = true;
					drift += k * sign;
				}
			}
		}
		maxDrift = abs(drift) > maxDrift ? abs(drift) : maxDrift;
		exact += hashOurs[i] == hashTheirs[i];
		if (found) {
			++matched;
		} else {
			++unmatched;
			if (first < 0 && i >= from) {
				first = i;
			}
		}
	}
	// AROUND=n: both cores' pictures for n frames either side of the first unmatched one.
	if (getenv("AROUND") && first >= 0) {
		int around = atoi(getenv("AROUND"));
		for (int i = first - around; i <= first + around; ++i) {
			if (i >= 0 && i < n) {
				fprintf(stderr, "%5d ours %04x theirs %04x%s\n", i, (unsigned) (hashOurs[i] & 0xFFFF), (unsigned) (hashTheirs[i] & 0xFFFF), i == first ? "  <" : "");
			}
		}
	}
	// HASHES=a: both cores' picture hashes from frame a, 100 frames.
	if (getenv("HASHES")) {
		for (int i = atoi(getenv("HASHES")); i < n && i < atoi(getenv("HASHES")) + 100; ++i) {
			fprintf(stderr, "%5d ours %04x theirs %04x\n", i, (unsigned) (hashOurs[i] & 0xFFFF), (unsigned) (hashTheirs[i] & 0xFFFF));
		}
	}
	int differing = 0;
	if (first >= 0) {
		for (int y = 0; y < GB_HEIGHT; ++y) {
			for (int x = 0; x < GB_WIDTH; ++x) {
				differing += ours[first][y][x] != theirs[first][y][x];
			}
		}
	}
	// DUMP=prefix: both pictures of the first unmatched frame, as prefix-ours.pgm and prefix-theirs.pgm.
	if (getenv("DUMP") && first >= 0) {
		for (int which = 0; which < 2; ++which) {
			char path[512];
			snprintf(path, sizeof(path), "%s-%s.pgm", getenv("DUMP"), which ? "theirs" : "ours");
			FILE* o = fopen(path, "wb");
			fprintf(o, "P5\n%d %d\n255\n", GB_WIDTH, GB_HEIGHT);
			for (int y = 0; y < GB_HEIGHT; ++y) {
				for (int x = 0; x < GB_WIDTH; ++x) {
					uint16_t v = which ? theirs[first][y][x] : ours[first][y][x];
					fputc(colorModel ? ((v & 31) + (v >> 5 & 31) + (v >> 10 & 31)) * 255 / 93 : 255 - v * 85, o);
				}
			}
			fclose(o);
		}
	}
	// How many frames' work RAM differs.
	int ramFrames = 0;
	for (int i = 0; i < frames; ++i) {
		ramFrames += memcmp(ourRam[i], theirRam[i], ramBytes) != 0;
	}
	// The first frame whose cartridge RAM differs (the part both have), and where.
	int sramFirst = -1, sramAt = -1;
	size_t sramBytes = gb.cart.ramSize < SRAM_MAX ? gb.cart.ramSize : SRAM_MAX;
	for (int i = 0; i < frames && sramFirst < 0 && gb.cart.hasBattery; ++i) {
		for (size_t a = 0; a < sramBytes; ++a) {
			if (ourSram[i][a] != theirSram[i][a]) {
				sramFirst = i;
				sramAt = 0xA000 + (int) a;
				break;
			}
		}
	}
	if (getenv("SRAMDIFF") && sramFirst >= 0) {
		for (size_t a = 0, shown = 0; a < sramBytes && shown < 12; ++a) {
			if (ourSram[sramFirst][a] != theirSram[sramFirst][a]) {
				++shown;
				fprintf(stderr, "frame %d cart $%05zX ours %02X theirs %02X (frame before: ours %02X theirs %02X)\n", sramFirst, 0xA000 + a, ourSram[sramFirst][a], theirSram[sramFirst][a], sramFirst ? ourSram[sramFirst - 1][a] : 0, sramFirst ? theirSram[sramFirst - 1][a] : 0);
			}
		}
	}
	// RAMFROM=n: the first frame from n on whose work RAM differs, and how many bytes.
	if (getenv("RAMFROM")) {
		for (int i = atoi(getenv("RAMFROM")); i < frames; ++i) {
			int bytes = 0, at = -1;
			for (int a = 0; a < (int) ramBytes; ++a) {
				if (ourRam[i][a] != theirRam[i][a]) {
					at = at < 0 ? a : at;
					++bytes;
				}
			}
			if (bytes) {
				fprintf(stderr, "frame %d: %d bytes differ from $%04X\n", i, bytes, 0xC000 + at);
			}
		}
	}
	if (getenv("RAMFRAMES")) {
		fprintf(stderr, "work RAM differs in %d frames\n", ramFrames);
	}
	// The first frame whose work RAM differs, and where.
	int ramFirst = -1, ramAt = -1;
	for (int i = 0; i < frames && ramFirst < 0; ++i) {
		for (int a = 0; a < (int) ramBytes; ++a) {
			if (ourRam[i][a] != theirRam[i][a]) {
				ramFirst = i;
				ramAt = 0xC000 + a;
				break;
			}
		}
	}
	if (getenv("RAMDIFF") && ramFirst >= 0) {
		for (int a = 0, shown = 0; a < (int) ramBytes && shown < (getenv("RAMALL") ? 100000 : 12); ++a) {
			if (ourRam[ramFirst][a] != theirRam[ramFirst][a]) {
				fprintf(stderr, "frame %d $%04X ours %02X theirs %02X (frame before: ours %02X theirs %02X)\n", ramFirst, 0xC000 + a,
				        ourRam[ramFirst][a], theirRam[ramFirst][a], ramFirst ? ourRam[ramFirst - 1][a] : 0, ramFirst ? theirRam[ramFirst - 1][a] : 0);
				++shown;
			}
		}
	}
	if (getenv("AUDIORMS")) {
		fprintf(stderr, "audio rms %.1f over %ld samples\n", audioCount ? sqrt(audioSquares / audioCount) : 0.0, audioCount);
	}
	printf("type=%02x matched=%d/%d exact=%d first=%d pixels=%d ram=%d@%04X drift=%d sram=%d@%04X\n", rom[0x147], matched, n, exact, first, differing, ramFirst, ramAt < 0 ? 0 : ramAt, maxDrift, sramFirst, sramAt < 0 ? 0 : sramAt);
	return 0;
}
