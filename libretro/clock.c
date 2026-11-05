/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "core.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// The Clock options, and the Game Boy Advance cartridge's clock. The core gives that clock its time:
// the device's in Real Time, plus the Clock Offset; the console's own in Game Time and in netplay,
// where every device must read the same. The console's state holds that time, so rollback and loaded
// states bring it along. In Game Time it is kept beside the save as the .rtc file:
// - 4 bytes, "aGBt";
// - 8 bytes, the time without the Clock Offset, in the console's cycles since 1970.
// Game Boy cartridges' clocks are Gambatte's, in gb-gambatte.cpp, on the same terms.

enum {
	CLOCK_MAGIC = 0,
	CLOCK_TIME = 4,
	CLOCK_SIZE = 12,
	SECOND = 1 << 24,
	FRAME_CYCLES = 280896,
	HOUR = 60 * 60,
};

static bool gameTime;
// Clock Speed, in percent of the console's time (0 stopped), and Clock Offset, in seconds.
static unsigned percent = 100;
static int64_t offset;
static uint8_t files[MAX_PLAYERS][CLOCK_SIZE];
// Each console's time without the offset, as of its last frame; 0 until its file is read.
static int64_t times[MAX_PLAYERS];

static bool hasClock(int player) {
	return !gameBoy && player < playersHere && gbas[player] && gbas[player]->gpio.devices & HW_RTC;
}

// The device's time, in the console's cycles since 1970.
static int64_t deviceTime(void) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return (int64_t) now.tv_sec * SECOND + (int64_t) now.tv_nsec * SECOND / 1000000000;
}

static int64_t get64(const uint8_t* from) {
	uint64_t value = 0;
	for (int i = 7; i >= 0; --i) {
		value = value << 8 | from[i];
	}
	return (int64_t) value;
}

static void put64(uint8_t* to, int64_t value) {
	for (int i = 0; i < 8; ++i) {
		to[i] = (uint64_t) value >> i * 8;
	}
}

// Whether the console keeps the time, rather than the device.
static bool consoleTime(void) {
	return gameTime || netplaySeen;
}

void clockFrame(void) {
	if (gameBoy) {
		libretroGbDeviceTime(!consoleTime());
	}
	for (int i = 0; i < playersHere; ++i) {
		if (!hasClock(i)) {
			continue;
		}
		int64_t* const kept = &gbas[i]->gpio.rtc.kept;
		if (!times[i]) {
			bool const saved = gameTime && !memcmp(files[i] + CLOCK_MAGIC, "aGBt", 4);
			times[i] = saved ? get64(files[i] + CLOCK_TIME) : deviceTime();
		}
		if (!consoleTime()) {
			times[i] = deviceTime();
			*kept = times[i] + offset * SECOND;
		} else if (!*kept) {
			*kept = times[i] + offset * SECOND;
		} else {
			*kept += (int64_t) FRAME_CYCLES * (gameTime ? percent : 100) / 100;
			times[i] = *kept - offset * SECOND;
		}
		memcpy(files[i] + CLOCK_MAGIC, "aGBt", 4);
		put64(files[i] + CLOCK_TIME, times[i]);
	}
}

void* clockMemory(int player, size_t* size) {
	*size = gameTime && hasClock(player) ? CLOCK_SIZE : 0;
	return *size ? files[player] : NULL;
}

void clockForget(void) {
	memset(files, 0, sizeof(files));
	memset(times, 0, sizeof(times));
}

// Game Boy: the clock as a dead battery leaves it, which a game may notice and ask for the time.
// Game Boy Advance: the device's time again, the Clock Offsets back to 0 and Game Time from now.
static void resetClock(void) {
	if (gameBoy) {
		notify(libretroGbResetClock() ? "Reset Clock: the clock is cleared, as by a dead battery" : "Reset Clock: this cartridge has no MBC3 clock", 240);
		return;
	}
	struct retro_variable zero = { "armgba_clock_days", "0" };
	environment(RETRO_ENVIRONMENT_SET_VARIABLE, &zero);
	zero.key = "armgba_clock_hours";
	environment(RETRO_ENVIRONMENT_SET_VARIABLE, &zero);
	offset = 0;
	bool reset = false;
	for (int i = 0; i < playersHere; ++i) {
		if (hasClock(i)) {
			times[i] = deviceTime();
			gbas[i]->gpio.rtc.kept = times[i];
			reset = true;
		}
	}
	notify(reset ? "Reset Clock: the clock is at the device's time" : "Reset Clock: this cartridge has no clock", 240);
}

void readClockOptions(bool act) {
	static const struct Choice speeds[] = {
		{ "stopped", 0 }, { "0.1x", 10 }, { "0.25x", 25 }, { "0.5x", 50 }, { "0.75x", 75 }, { "1x", 100 }, { "1.25x", 125 },
		{ "1.5x", 150 }, { "2x", 200 }, { "3x", 300 }, { "4x", 400 }, { "6x", 600 }, { "10x", 1000 }, { "24x", 2400 }, { "60x", 6000 },
	};
	gameTime = optionIs("armgba_clock_source", "game");
	percent = optionChoice("armgba_clock_speed", speeds, COUNT(speeds), 100);
	const char* days = option("armgba_clock_days");
	const char* hours = option("armgba_clock_hours");
	int64_t const wanted = (int64_t) clampInt(days ? atoi(days) : 0, -30, 30) * 24 * HOUR + (int64_t) clampInt(hours ? atoi(hours) : 0, -23, 23) * HOUR;
	for (int i = 0; consoleTime() && i < playersHere; ++i) {
		if (hasClock(i) && gbas[i]->gpio.rtc.kept) {
			gbas[i]->gpio.rtc.kept += (wanted - offset) * SECOND;
		}
	}
	offset = wanted;
	libretroGbSetClock(gameTime, percent, offset);
	// Reset Clock acts once, when Allow Clock Reset is on, which then goes back to Off.
	if (optionIs("armgba_clock_reset", "reset")) {
		struct retro_variable no = { "armgba_clock_reset", "no" };
		environment(RETRO_ENVIRONMENT_SET_VARIABLE, &no);
		if (act && optionIs("armgba_clock_reset_allowed", "enabled")) {
			resetClock();
			struct retro_variable off = { "armgba_clock_reset_allowed", "disabled" };
			environment(RETRO_ENVIRONMENT_SET_VARIABLE, &off);
			showOptions();
		}
	}
}
