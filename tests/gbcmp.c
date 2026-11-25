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

static uint8_t (*theirs)[GB_HEIGHT][GB_WIDTH];
// Work RAM after each frame, from both cores.
static uint8_t (*theirRam)[0x2000];
static uint8_t (*ourRam)[0x2000];
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
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixelFormat = *(const unsigned*) data;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = systemDir;
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
	uint8_t(*out)[GB_WIDTH] = theirs[theirCount++];
	for (unsigned y = 0; y < GB_HEIGHT && y < height; ++y) {
		for (unsigned x = 0; x < GB_WIDTH && x < width; ++x) {
			unsigned g;
			if (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
				g = ((const uint32_t*) ((const uint8_t*) data + y * pitch))[x] >> 8 & 0xFF;
			} else {
				uint16_t p = ((const uint16_t*) ((const uint8_t*) data + y * pitch))[x];
				g = pixelFormat == RETRO_PIXEL_FORMAT_RGB565 ? (p >> 5 & 0x3F) * 255 / 63 : (p >> 5 & 0x1F) * 255 / 31;
			}
			out[y][x] = shade(g);
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

static size_t audioBatch(const int16_t* data, size_t frames) {
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

static uint64_t hashPicture(const uint8_t (*p)[GB_WIDTH]) {
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
	theirs = calloc(MAX_FRAMES, sizeof(*theirs));
	theirRam = calloc(frames, sizeof(*theirRam));
	ourRam = calloc(frames, sizeof(*ourRam));
	uint8_t(*ours)[GB_HEIGHT][GB_WIDTH] = calloc(frames, sizeof(*ours));

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
		SYM(retro_run)();
		const uint8_t* ram = ((void* (*)(unsigned)) dlsym(core, "retro_get_memory_data"))(RETRO_MEMORY_SYSTEM_RAM);
		size_t ramSize = ((size_t (*)(unsigned)) dlsym(core, "retro_get_memory_size"))(RETRO_MEMORY_SYSTEM_RAM);
		if (ram && ramSize >= 0x2000) {
			memcpy(theirRam[i], ram, 0x2000);
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
	gb.boot = armgba_dmg_boot;
	gb.bootSize = sizeof(armgba_dmg_boot);
	if (!gbLoad(&gb, rom, padded, GB_MODEL_DMG)) {
		fprintf(stderr, "armGB can't load it\n");
		return 2;
	}
	if (getenv("RAMOUT")) {
		FILE* o = fopen(getenv("RAMOUT"), "wb");
		fwrite(theirRam[0], 1, 0x2000, o);
		fclose(o);
	}
	// Work RAM starts as the other core's did at power-on (its first frame is the boot ROM's, which
	// leaves work RAM alone), so games that read it before writing it run alike. For this test only.
	memcpy(gb.wram, theirRam[0], 0x2000);
	for (int i = 0; i < frames; ++i) {
		gb.keys = buttonsAt(script, i);
		gbRunFrame(&gb);
		memcpy(ourRam[i], gb.wram, 0x2000);
		for (int y = 0; y < GB_HEIGHT; ++y) {
			for (int x = 0; x < GB_WIDTH; ++x) {
				ours[i][y][x] = gb.frame[y][x] == GB_SHADE_OFF ? 0 : gb.frame[y][x];
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
	int matched = 0, exact = 0, first = -1, unmatched = 0;
	for (int i = 0; i < n; ++i) {
		bool found = false;
		for (int d = -2; d <= 2 && !found; ++d) {
			if (i + d >= 0 && i + d < n && hashOurs[i] == hashTheirs[i + d]) {
				found = true;
			}
		}
		exact += hashOurs[i] == hashTheirs[i];
		if (found) {
			++matched;
		} else {
			++unmatched;
			if (first < 0) {
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
					fputc(255 - (which ? theirs[first][y][x] : ours[first][y][x]) * 85, o);
				}
			}
			fclose(o);
		}
	}
	// The first frame whose work RAM differs, and where.
	int ramFirst = -1, ramAt = -1;
	for (int i = 0; i < frames && ramFirst < 0; ++i) {
		for (int a = 0; a < 0x2000; ++a) {
			if (ourRam[i][a] != theirRam[i][a]) {
				ramFirst = i;
				ramAt = 0xC000 + a;
				break;
			}
		}
	}
	if (getenv("RAMDIFF") && ramFirst >= 0) {
		for (int a = 0, shown = 0; a < 0x2000 && shown < 12; ++a) {
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
	printf("type=%02x matched=%d/%d exact=%d first=%d pixels=%d ram=%d@%04X\n", rom[0x147], matched, n, exact, first, differing, ramFirst, ramAt < 0 ? 0 : ramAt);
	return 0;
}
