/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The libretro core's Game Boy and Game Boy Color side, running on armGB (src/gb).

extern "C" {
#include "gb.h"
#include "libretro.h"
#include "../src/gb/gb.h"
}
#include "dmg07.h"
#include "link.h"
#include "printer.h"
#include "megaduck/laptop.h"
#include "armgba-boot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>

namespace {

enum {
	// Times here are in Game Boy sound samples, 2^21 a second, two dots each, as the link's, the
	// adapter's and the printer's are: a frame is 35112.
	NATIVE_RATE = 2097152,
	FRAME_SAMPLES = 35112,
	MAX_PLAYERS = 4,
	// With exact link timing, the most samples run between looks at the network.
	EXACT_PART = 2048,
	// The Game Boy Advance's own start before the Color's boot ROM inside it runs, in dots.
	ADVANCE_STALL = 971616,
};

void linkClocked();
unsigned laptopControl();
void laptopIn(uint8_t byte);
bool laptopSlot();
// How many native samples make one frontend sample.
unsigned ratio;
uint32_t cgbColors[32768];
// The boot ROM folder, kept so a game can be restarted as another model.
std::string systemPath;
bool hasSystemPath;
// The boot ROM choice from the option.
LibretroGbBoot bootWanted = LIBRETRO_GB_BOOT_NINTENDO;
// -1, or the kind of Mega Duck cartridge (see libretroGbSetMegaDuck)
int megaDuckKind = -1;
// The Mega Duck laptop's controller on the serial port (see libretroGbSetMegaDuckLaptop)
bool laptopWanted = false;
int laptopPrinter = 2;
DuckLaptopLanguage laptopLanguage = DUCK_LAPTOP_SPANISH;
// The cartridge in the laptop's slot, if any, padded to a power of two, and its kind
std::vector<unsigned char> laptopCartridge;
int laptopCartridgeKind = 0;
// Whether the cartridge in the laptop's slot has taken the system ROM's place, as states keep it
bool laptopSlotIn;
// The codes the Game Genie fills its empty slots with (libretroGbSetGameGenieCodes)
unsigned char genieDigits[27] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                                  0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10 };
unsigned char genieCounts[3];
// A cheat cartridge in front of the game (libretroGbSetActionReplay): its program, whether a game is on
// top, and what it keeps with the power off as it was last read out of the console
std::vector<unsigned char> replayProgram;
bool replayGame;
std::vector<unsigned char> replayKept;

// A cartridge's clock, kept by the frontend as the .rtc file. Other cores read their part of it:
// - 8 bytes: the Unix time the clock would have started from, gambatte-libretro's whole file;
// - "aGBm", and the Unix time it was saved;
// - an MBC3's registers at that time, 4 bytes each, seconds first, then the time again: what mGBA
//   reads of its own file;
// - the cycles into the second and the latched registers, which carry more detail;
// - the Clock Offset the clock was moved by, and how much of a move back it couldn't make, in
//   seconds.
// A HuC3 chip's file has "aGBh" and the chip's whole state after the saved time. Files from before
// the mGBA part ("aGBc") had the registers and the offset where mGBA's part is now.
enum {
	RTC_BASE = 0,
	RTC_MAGIC = 8,
	RTC_SAVED = 12,
	RTC_REGS = 20,
	MGBA_REGS = 20,
	MGBA_SAVED = 40,
	RTC_CYCLES = 48,
	RTC_LATCH = 52,
	RTC_MOVED = 64,
	RTC_SHORT = 72,
	RTC_SIZE = 80,
	OLD_OFFSET = RTC_REGS + 14,
	HUC3_STATE = GB_HUC3_STATE,
	HUC3_SIZE = RTC_REGS + HUC3_STATE,
	// In the HuC3 state: the cycles into the current minute, then I/O with the minutes and days as
	// 4-bit fields.
	HUC3_MINUTES = 4 + 0x10,
	HUC3_DAYS = 4 + 0x13,
	// A host clock before 2024 was never set, so it says nothing about the time the game was off.
	SANE_TIME = 1704067200,
};
// The clock's registers as armGB gives them (gbRtcGet), and the length of a second in its ticks.
enum { RTC_DH, RTC_DL, RTC_H, RTC_M, RTC_S, RTC_C, RTC_LATCHED };
unsigned long const RTC_SECOND = GB_RTC_SECOND;

// How the output capacitor is wired: a Game Boy's, connected while a DAC is on; a Game Boy Advance's,
// after its amp, always connected, per side with the speaker; or its headphones', one charge their
// two sides share.
enum class Wiring { GAME_BOY, ADVANCE, SHARED_RETURN };

// One Game Boy. There are two to four when the consoles of a link run here; s is the one being worked
// on.
struct Console {
	// The machine, kept for as long as the game is loaded: the frontend may hold a pointer to its
	// cartridge RAM.
	struct GB* gb;
	// The ROM it runs, padded to a power of two, and its boot ROM.
	std::vector<unsigned char> rom;
	std::vector<unsigned char> boot;
	// The cheat cartridge in front of the game, when there is one.
	struct GBCheatCart cheat;
	bool cheatOn;
	// The CRC of the boot ROM from the system folder it started with, 0 for armGBA's or none.
	uint32_t bootCrc;
	// Whether its serial port goes to another Game Boy, the printer or the laptop.
	bool linked;
	// The model it runs as, and whether it started without a boot ROM.
	LibretroGbModel model;
	bool noBoot;
	unsigned pressed;
	uint32_t pixels[LIBRETRO_GB_WIDTH * LIBRETRO_GB_HEIGHT];
	// The last whole picture, when a frame can end mid-picture.
	uint32_t shown[LIBRETRO_GB_WIDTH * LIBRETRO_GB_HEIGHT];
	// A frame's sound for the frontend: up to four frames' worth, two channels each.
	int16_t samples[(4 * FRAME_SAMPLES / 64 + 64) * 2];
	size_t sampleCount;
	// The output capacitor's charge per side, in samples x 256, and how much of it it keeps each
	// frontend sample, out of 0x10000 (0: none, the Game Boy Audio Filter off). Saved with states.
	// With headphones on a Game Boy Advance, the one charge both sides share is capacitor[0].
	int32_t capacitor[2];
	int32_t charge;
	Wiring wiring;
	size_t stateSize;
	// The game, kept so it can be restarted as another model.
	std::vector<unsigned char> game;
	LibretroGbModel chosen;
	// The model the Game Boy Model option asks for. Loading a state from another model may have
	// switched the machine away from it.
	LibretroGbModel wanted;
	// The boot ROM this game actually started with.
	LibretroGbBoot bootUsed;
	// The cheat codes in force. Loading the game again clears them, so they're reapplied.
	std::vector<std::string> codes;
	unsigned char rtcFile[HUC3_SIZE];
	// The Clock Offset the MBC3 clock was moved by, and how much of a move back the clock couldn't
	// make, at day 0; a move forwards makes that up first.
	long long clockMoved;
	long long clockShort;
	// The frontend fills rtcFile after the game is loaded, so the file is read just before the
	// first frame.
	bool rtcPending;
	// Native samples run past the end of the last frame, when the consoles of a link run here.
	long ahead;
	// A dot run past the last whole sample.
	uint32_t overDot;
	// Channel Selection: the charge for what's heard, with channels left out, apart from the
	// console's from when the first is (heardApart; before, it is the console's). Not saved: states
	// keep the console's.
	int32_t heardCapacitor[2];
	bool heardApart;
};
Console consoles[MAX_PLAYERS];
Console* s = &consoles[0];

// Layer Selection's layers shown (1 background, 2 sprites, 4 window) and Channel Selection's channels
// left out (bits 0-3: channels 1-4), for every console: only what's drawn and heard changes.
unsigned layersShown = 7;
unsigned channelsMuted;

void applyShownAndHeard(struct GB* gb) {
	gb->layersHidden = ~layersShown & 7;
	gb->channelsMuted = channelsMuted;
}

bool color(const Console& c) {
	return c.model == LIBRETRO_GB_CGB || c.model == LIBRETRO_GB_AGB;
}

// The Game Boy Audio Filter, and the output capacitor's charge factor a 4 MiHz cycle (Pan Docs, Audio
// Details): the original Game Boy's 0.999958 (about 28 Hz), the Pocket's and the Color's 0.998943
// (about 700 Hz). A Game Boy Advance has its own output's (GBA Audio Output), always connected, or
// with that off the original's. A Mega Duck is taken as an original Game Boy.
LibretroGbFilter audioFilter = LIBRETRO_GB_FILTER_ACCURATE;
LibretroAdvanceOutput advanceOutput = LIBRETRO_ADVANCE_SPEAKER;

void setCapacitor(Console& c) {
	c.wiring = Wiring::GAME_BOY;
	if (audioFilter == LIBRETRO_GB_FILTER_OFF) {
		c.charge = 0;
		return;
	}
	bool const advance = c.model == LIBRETRO_GB_AGB;
	if (audioFilter == LIBRETRO_GB_FILTER_ACCURATE && advance && advanceOutput != LIBRETRO_ADVANCE_OFF) {
		c.wiring = advanceOutput == LIBRETRO_ADVANCE_HEADPHONES ? Wiring::SHARED_RETURN : Wiring::ADVANCE;
		// Per frontend sample: 2097152 / ratio a second
		double const seconds = ratio / 2097152.0;
		c.charge = static_cast<int32_t>(std::lround(std::exp(-seconds / (ADVANCE_OUTPUT_OHMS(advanceOutput) * ADVANCE_OUTPUT_FARADS)) * 0x10000));
		return;
	}
	bool const pocketOrLater = !advance && (c.model == LIBRETRO_GB_MGB || color(c));
	double const perCycle = audioFilter == LIBRETRO_GB_FILTER_ACCURATE && pocketOrLater ? 0.998943 : 0.999958;
	// Per frontend sample: 4194304 / (2097152 / ratio) cycles
	c.charge = static_cast<int32_t>(std::lround(std::pow(perCycle, 2.0 * ratio) * 0x10000));
}
// The CPU Clock, in quarters of the console's own clock.
unsigned cpuQuarters = 4;
// The Clock options: Game Time, the speed in percent and the offset in seconds.
bool gameTime;
unsigned clockPercent = 100;
long long clockOffset;
// Real Time outside netplay: the clock runs on the device's time, so fast-forward and pauses don't
// move it, and loaded states leave it as it is.
bool deviceClock;
// How many consoles of a link run here (1 without one), and whether the Four Player Adapter joins
// them; two without it share the cable.
int players = 1;
bool adapterOn;

Dmg07 adapter;
// The Game Boy Printer on player 1's port, and where its prints go.
Printer printer;
bool printerOn;
PrinterOutput printed;

// One transfer on the Four Player Adapter: each Game Boy waiting on the external clock takes the
// adapter's byte and gives its own.
void adapterTransfer() {
	uint8_t in[DMG07_PORTS];
	for (int i = 0; i < DMG07_PORTS; ++i) {
		in[i] = 0xFF;
		struct GB* const gb = i < players ? consoles[i].gb : 0;
		if (gb && (gbLinkStatus(gb, GB_LINK_CONTROL) & 0x81) == 0x80) {
			in[i] = gbLinkStatus(gb, GB_LINK_OUT);
			gbLinkStatus(gb, dmg07Out(&adapter, i));
		}
	}
	dmg07Transfer(&adapter, in);
}

uint32_t crc32(const unsigned char* bytes, size_t size) {
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < size; ++i) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; ++bit) {
			crc = crc >> 1 ^ (0xEDB88320 & (0 - (crc & 1)));
		}
	}
	return ~crc;
}

void put64(unsigned char* to, unsigned long long value) {
	for (int i = 0; i < 8; ++i) {
		to[i] = value >> i * 8;
	}
}

unsigned long long get64(const unsigned char* from) {
	unsigned long long value = 0;
	for (int i = 7; i >= 0; --i) {
		value = value << 8 | from[i];
	}
	return value;
}

// Advances a clock by some seconds the way the chip counts: a field past its range counts up to its
// last value and wraps to 0 without a carry; past 511 days, the day counter wraps and sets its
// carry bit.
void runClock(uint32_t r[11], unsigned long long seconds) {
	if (r[RTC_DH] & 0x40) {
		return;
	}
	static const unsigned limits[3] = { 60, 60, 24 };
	static const unsigned ends[3] = { 64, 64, 32 };
	static const int fields[3] = { RTC_S, RTC_M, RTC_H };
	unsigned long long carry = seconds;
	for (int i = 0; i < 3 && carry; ++i) {
		uint32_t& value = r[fields[i]];
		if (value >= limits[i]) {
			if (carry < ends[i] - value) {
				value += carry;
				return;
			}
			carry -= ends[i] - value;
			value = 0;
		}
		unsigned long long const total = value + carry;
		value = total % limits[i];
		carry = total / limits[i];
	}
	unsigned long long days = ((r[RTC_DH] & 1) << 8 | r[RTC_DL]) + carry;
	if (days > 511) {
		r[RTC_DH] |= 0x80;
	}
	r[RTC_DL] = days & 0xFF;
	r[RTC_DH] = (r[RTC_DH] & 0xFE) | (days >> 8 & 1);
}

// Moves a running clock by some seconds, forwards or back, though not before day 0. Returns how much
// of a move back it couldn't make.
long long moveClock(uint32_t r[11], long long seconds) {
	if (r[RTC_DH] & 0x40 || r[RTC_H] >= 24 || r[RTC_M] >= 60 || r[RTC_S] >= 60) {
		return 0;
	}
	long long const day = 24 * 60 * 60;
	long long total = (((r[RTC_DH] & 1) << 8 | r[RTC_DL]) * 24 + r[RTC_H]) * 3600 + r[RTC_M] * 60 + r[RTC_S] + seconds;
	long long const shortBy = total < 0 ? -total : 0;
	total += shortBy;
	if (total >= 512 * day) {
		total %= 512 * day;
		r[RTC_DH] |= 0x80;
	}
	r[RTC_S] = total % 60;
	r[RTC_M] = total / 60 % 60;
	r[RTC_H] = total / 3600 % 24;
	r[RTC_DL] = total / day & 0xFF;
	r[RTC_DH] = (r[RTC_DH] & 0xFE) | (total / day >> 8 & 1);
	return shortBy;
}

// Moves the selected console's clock from one Clock Offset to another, making up first what an
// earlier move back couldn't make.
void moveBy(Console& c, uint32_t r[11], long long offset) {
	long long seconds = offset - c.clockMoved;
	if (seconds > 0) {
		long long const owed = seconds < c.clockShort ? seconds : c.clockShort;
		c.clockShort -= owed;
		seconds -= owed;
	}
	c.clockShort += moveClock(r, seconds);
	c.clockMoved = offset;
}

bool hasMbc3Clock(const Console& c) {
	return c.gb && c.gb->cart.mapper == GB_MBC3 && c.gb->cart.hasRtc;
}

bool hasHuC3(const Console& c) {
	return c.gb && c.gb->cart.mapper == GB_HUC3;
}

// How much of rtcFile the cartridge's clock uses, or 0 if it has none.
size_t rtcSize() {
	return hasMbc3Clock(*s) ? RTC_SIZE : hasHuC3(*s) ? HUC3_SIZE : 0;
}

// Reads a 12-bit count stored as three 4-bit fields, lowest first.
unsigned long getNibbles(const unsigned char* at) {
	return (at[0] & 0xF) | (at[1] & 0xF) << 4 | (at[2] & 0xF) << 8;
}

void putNibbles(unsigned char* at, unsigned long value) {
	for (int i = 0; i < 3; ++i) {
		at[i] = value >> i * 4 & 0xF;
	}
}

// Advances a HuC3 clock by some seconds the way the chip counts: minutes up to 1440, then days up
// to 4096.
void runHuC3(unsigned char* state, unsigned long long seconds) {
	unsigned long long const minute = RTC_SECOND * 60;
	unsigned long long cycles = (unsigned long long) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3];
	cycles = cycles % minute + seconds % 60 * RTC_SECOND;
	unsigned long long minutes = getNibbles(state + HUC3_MINUTES) + seconds / 60 + cycles / minute;
	cycles %= minute;
	unsigned long long const days = getNibbles(state + HUC3_DAYS) + minutes / 1440;
	putNibbles(state + HUC3_MINUTES, minutes % 1440);
	putNibbles(state + HUC3_DAYS, days & 0xFFF);
	for (int i = 0; i < 4; ++i) {
		state[i] = cycles >> (24 - i * 8);
	}
}

unsigned long get32(const unsigned char* from) {
	return from[0] | from[1] << 8 | from[2] << 16 | (unsigned long) from[3] << 24;
}

void put32(unsigned char* to, unsigned long value) {
	for (int i = 0; i < 4; ++i) {
		to[i] = value >> i * 8;
	}
}

// Whether a file holds mGBA's registers and the time they were saved, sane ones.
bool mgbaFile(const unsigned char* file, unsigned long long now) {
	static const unsigned ends[5] = { 64, 64, 32, 256, 256 };
	unsigned long long const saved = get64(file + MGBA_SAVED);
	for (int i = 0; i < 5; ++i) {
		if (get32(file + MGBA_REGS + i * 4) >= ends[i]) {
			return false;
		}
	}
	return saved >= SANE_TIME / 2 && saved <= now + 366 * 86400ull;
}

// The device's time for the clock, in microseconds.
int64_t deviceMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Loads the clock from the .rtc file, armGBA's, mGBA's or gambatte-libretro's, and advances it by the
// time the game was off, when the host clock can tell, except in Game Time.
void readRtcFile() {
	s->rtcPending = false;
	unsigned long long const now = std::time(0);
	const unsigned char* const file = s->rtcFile;
	unsigned long long saved = get64(file + RTC_SAVED);
	if (hasHuC3(*s)) {
		unsigned char state[HUC3_STATE];
		if (!std::memcmp(file + RTC_MAGIC, "aGBh", 4)) {
			std::memcpy(state, file + RTC_REGS, sizeof(state));
		} else {
			// gambatte-libretro's: the minutes and days since the clock's start.
			unsigned long long const base = get64(file + RTC_BASE);
			if (!base || base < SANE_TIME / 2 || now < SANE_TIME || base > now) {
				return;
			}
			gbHuc3Get(s->gb, state);
			std::memset(state, 0, 4);
			putNibbles(state + HUC3_MINUTES, 0);
			putNibbles(state + HUC3_DAYS, 0);
			saved = base;
		}
		if (!gameTime && saved >= SANE_TIME / 2 && now >= SANE_TIME && now >= saved) {
			runHuC3(state, now - saved);
		}
		gbHuc3Set(s->gb, state);
		return;
	}
	uint32_t r[11] = { 0 };
	bool const ours = !std::memcmp(file + RTC_MAGIC, "aGBm", 4), older = !std::memcmp(file + RTC_MAGIC, "aGBc", 4);
	s->clockMoved = s->clockShort = 0;
	if (older) {
		const unsigned char* const regs = file + RTC_REGS;
		for (int i = 0; i < 5; ++i) {
			r[i] = regs[i];
			r[i + RTC_LATCHED] = regs[9 + i];
		}
		r[RTC_C] = get32(regs + 5);
		s->clockMoved = (long long) get64(file + OLD_OFFSET);
	} else if (ours || mgbaFile(file, now)) {
		static const int fields[5] = { RTC_S, RTC_M, RTC_H, RTC_DL, RTC_DH };
		for (int i = 0; i < 5; ++i) {
			r[fields[i]] = get32(file + MGBA_REGS + i * 4);
			r[fields[i] + RTC_LATCHED] = ours ? file[RTC_LATCH + fields[i]] : r[fields[i]];
		}
		saved = get64(file + MGBA_SAVED);
		if (ours) {
			r[RTC_C] = get32(file + RTC_CYCLES);
			s->clockMoved = (long long) get64(file + RTC_MOVED);
			s->clockShort = (long long) get64(file + RTC_SHORT);
		}
	} else {
		// gambatte-libretro's: the clock shows the time since its start, unless that start is in the
		// future.
		unsigned long long const base = get64(file + RTC_BASE);
		if (base && base >= SANE_TIME / 2 && now >= SANE_TIME && base <= now) {
			runClock(r, now - base);
			for (int i = 0; i < 5; ++i) {
				r[i + RTC_LATCHED] = r[i];
			}
		}
		saved = 0;
	}
	r[RTC_DH] &= 0xC1;
	r[RTC_DH + RTC_LATCHED] &= 0xC1;
	for (int i = RTC_H; i <= RTC_S; ++i) {
		r[i] &= i == RTC_H ? 0x1F : 0x3F;
		r[i + RTC_LATCHED] &= i == RTC_H ? 0x1F : 0x3F;
	}
	r[RTC_C] %= RTC_SECOND;
	if (!gameTime && saved >= SANE_TIME && now >= saved) {
		runClock(r, now - saved);
	}
	moveBy(*s, r, clockOffset);
	gbRtcSet(s->gb, r);
}

void writeRtcFile() {
	unsigned char* const file = s->rtcFile;
	unsigned long long const now = std::time(0);
	put64(file + RTC_SAVED, now);
	if (hasHuC3(*s)) {
		std::memcpy(file + RTC_MAGIC, "aGBh", 4);
		unsigned char* const state = file + RTC_REGS;
		gbHuc3Get(s->gb, state);
		unsigned long long const cycles = (unsigned long long) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3];
		unsigned long long const shown = getNibbles(state + HUC3_DAYS) * 86400ull + getNibbles(state + HUC3_MINUTES) * 60 + cycles / RTC_SECOND;
		put64(file + RTC_BASE, now - shown);
		return;
	}
	uint32_t r[11];
	gbRtcGet(s->gb, r);
	unsigned long long const shown = (((((r[RTC_DH] & 1) << 8 | r[RTC_DL]) * 24 + r[RTC_H]) * 60
	                                  + r[RTC_M]) * 60 + r[RTC_S]);
	put64(file + RTC_BASE, now - shown);
	std::memcpy(file + RTC_MAGIC, "aGBm", 4);
	static const int fields[5] = { RTC_S, RTC_M, RTC_H, RTC_DL, RTC_DH };
	for (int i = 0; i < 5; ++i) {
		put32(file + MGBA_REGS + i * 4, r[fields[i]]);
		file[RTC_LATCH + fields[i]] = r[fields[i] + RTC_LATCHED];
	}
	put64(file + MGBA_SAVED, now);
	put32(file + RTC_CYCLES, r[RTC_C]);
	put64(file + RTC_MOVED, s->clockMoved);
	put64(file + RTC_SHORT, s->clockShort);
}

// Reads the first 2304 bytes of a boot ROM file (the size of the largest boot ROM), or nothing if
// there is no file.
std::vector<unsigned char> readBoot(std::string const& path) {
	std::vector<unsigned char> bytes(2305);
	FILE* const f = std::fopen(path.c_str(), "rb");
	if (!f) {
		return std::vector<unsigned char>();
	}
	bytes.resize(std::fread(&bytes[0], 1, bytes.size(), f));
	std::fclose(f);
	return bytes;
}

// Whether a boot ROM is Nintendo's: each of Nintendo's holds the logo it checks cartridges against,
// while SameBoy's draw it from the cartridge.
bool nintendoBoot(std::vector<unsigned char> const& bytes) {
	static const unsigned char logo[48] = {
		0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
		0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
		0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
	};
	return std::search(bytes.begin(), bytes.end(), logo, logo + sizeof(logo)) != bytes.end();
}

// Finds the boot ROM for the model in the system directory, or returns none. It looks for
// Nintendo's boot ROM under every name that ROCKNIX's BIOS check, gbdev's archive, SameBoy and Gearboy
// use for it, then for a SameBoy boot ROM under the same names. A Game Boy Advance takes only its own.
std::string bootRom(const char* systemDirectory, LibretroGbModel model) {
	static const char* const names[4][3] = {
		{ "gb_bios.bin", "dmg_boot.bin" },
		{ "gbc_bios.bin", "cgb_boot.bin" },
		// The later Game Boy Advances', the first ones', and the name SameBoy also uses.
		{ "cgb_agb_boot.bin", "cgb_agb0_boot.bin", "agb_boot.bin" },
		// The Game Boy's boot ROM would leave the wrong value in A for a Pocket, so without the
		// Pocket's there is none.
		{ "mgb_boot.bin" },
	};
	std::string other;
	for (int i = 0; systemDirectory && i < 3 && names[model - 1][i]; ++i) {
		std::string const path = std::string(systemDirectory) + "/" + names[model - 1][i];
		std::vector<unsigned char> const bytes = readBoot(path);
		if (bytes.empty()) {
			continue;
		}
		if (nintendoBoot(bytes)) {
			return path;
		}
		if (other.empty()) {
			other = path;
		}
	}
	return other;
}

// Returns armGBA's boot ROM for the model (gbboot/).
std::vector<unsigned char> armgbaBoot(LibretroGbModel model) {
	switch (model) {
	case LIBRETRO_GB_CGB:
		return std::vector<unsigned char>(armgba_cgb_boot, armgba_cgb_boot + sizeof(armgba_cgb_boot));
	case LIBRETRO_GB_AGB:
		return std::vector<unsigned char>(armgba_agb_boot, armgba_agb_boot + sizeof(armgba_agb_boot));
	case LIBRETRO_GB_MGB:
		return std::vector<unsigned char>(armgba_mgb_boot, armgba_mgb_boot + sizeof(armgba_mgb_boot));
	default:
		return std::vector<unsigned char>(armgba_dmg_boot, armgba_dmg_boot + sizeof(armgba_dmg_boot));
	}
}

// The original Game Boy screen chosen: 0 plain grays, 1 the first model's, 2 the Pocket's, 3 the
// Light's with its backlight on.
int screen;
bool correction;
// On the Game Boy Advance model, the Color's panel shown instead of the Advance's.
bool colorPanel;
// The contrast wheel of the first Game Boy, the Pocket and the Light, -5 to 5.
int contrast;
// The four shades shown now, the contrast wheel's included.
unsigned long shown[4];
// What the color table was last built for: 0 uncorrected, 1 a Game Boy Color's screen, 2 a Game Boy
// Advance's.
int built = -1;

// Builds the Game Boy Color's colors as its screen shows them, the way SameBoy does (Lior Halphon,
// MIT license; its "modern balanced" correction). Each channel goes through a response curve, then
// some blue is mixed into green. The Game Boy Advance's screen has its own curves and mixes less.
void correctedColors(bool advance) {
	static const unsigned char cgbCurve[32] = { 0, 6, 12, 20, 28, 36, 45, 56, 66, 76, 88, 100, 113, 125, 137, 149,
	                                            161, 172, 182, 192, 202, 210, 218, 225, 232, 238, 243, 247, 250, 252, 254, 255 };
	static const unsigned char agbCurve[32] = { 0, 3, 8, 14, 20, 26, 33, 40, 47, 54, 62, 70, 78, 86, 94, 103,
	                                            112, 120, 129, 138, 147, 157, 166, 176, 185, 195, 205, 215, 225, 235, 245, 255 };
	const unsigned char* const curve = advance ? agbCurve : cgbCurve;
	double linear[32];
	for (int i = 0; i < 32; ++i) {
		linear[i] = std::pow(curve[i] / 255.0, 1.6);
	}
	double const greenShare = advance ? 5 / 6.0 : 3 / 4.0;
	for (unsigned i = 0; i < 32768; ++i) {
		unsigned const r = curve[i & 0x1F];
		unsigned const g = i >> 5 & 0x1F;
		unsigned const b = i >> 10 & 0x1F;
		unsigned const mixed = std::lround(std::pow(linear[g] * greenShare + linear[b] * (1 - greenShare), 1 / 1.6) * 255);
		cgbColors[i] = 0xFF000000 | r << 16 | mixed << 8 | curve[b];
	}
}

// Each screen: its color while the game has it off, its four shades, and the color its pixels go
// toward driven harder than the darkest shade. The plain grays are gambatte-libretro's. The first
// model's and the Pocket's are BGB's (bgb.bircd.org/reality): real ones photographed under
// color-accurate light and corrected with a ColorChecker. A first Game Boy's pixels go toward dark
// blue at the most contrast, as in BGB's contrast wheel; the Pocket's only darker. The Light is the
// Pocket's screen lit from behind, in the backlight's cyan (hue 180; photographed Lights run from
// about 157 to 191). A Light photographed with its light off and on, the contrast wheel unchanged,
// shows each shade's share of the lightest when lit as about the unlit share to the power 0.85, so
// each color here is the Pocket's share of its lightest shade to that power. The lightest shade
// passes the square root of the Pocket's share of the screen switched off: light crosses the screen
// once rather than twice.
struct Screen {
	unsigned long off;
	unsigned long shades[4];
	unsigned long full;
};
const Screen screens[4] = {
	{ 0xFFFFFF, { 0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000 }, 0x000000 },
	{ 0x948A04, { 0x759833, 0x588F51, 0x3B7560, 0x2E615A }, 0x00005E },
	{ 0xC2CFA8, { 0xADBF92, 0x96A67C, 0x727E64, 0x5A635C }, 0x2C322E },
	{ 0x60F1F1, { 0x59DFDF, 0x4EC6C6, 0x3C9E9E, 0x308383 }, 0x194B4B },
};

void setColors() {
	Screen const& now = screens[screen];
	// The wheel sets the drive voltages of the whole STN screen together (DMG-LCD-06: its 30k pot sets
	// the IR3E02's bias levels), from about -11 V to -14 V end to end, so 12% either side of the
	// middle, each step a tenth of the turn. An STN pixel's darkness is an S-curve of its voltage, as
	// steep as BGB's contrast wheel shows: all the way down the screen is blank, and all the way up
	// even the lightest shade is nearly as dark as it goes, 10% to 90% dark over about 7% of the
	// voltage. A shade's darkness places it on its
	// screen's own path, from the screen switched off through the four shades to its darkest, so
	// its color changes along the way as a real screen's does: the lightest a little past the screen
	// switched off, the others by their brightness, the darkest nine tenths of the way.
	auto const linear = [](unsigned long color, int bits) {
		double const c = (color >> bits & 0xFF) / 255.0;
		return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
	};
	auto const luminance = [&](unsigned long color) {
		return 0.2126 * linear(color, 16) + 0.7152 * linear(color, 8) + 0.0722 * linear(color, 0);
	};
	// Where a shade sits on the wheel's S-curve at the middle of its turn (before), from its darkness
	// in linear light, and where the turn moves it (after): 0 the screen switched off, 1 its darkest.
	auto const wheel = [&](Screen const& on, int shade, double places[6]) {
		double const lightest = 0.03, darkest = 0.95, width = 0.12 / (std::log((1 - lightest) / lightest) * 2.12);
		double const top = luminance(on.shades[0]), bottom = luminance(on.shades[3]);
		places[0] = 0, places[1] = lightest, places[4] = darkest, places[5] = 1;
		for (int k = 2; k <= 3; ++k) {
			places[k] = lightest + (darkest - lightest) * (top - luminance(on.shades[k - 1])) / (top - bottom);
		}
		double const before = places[shade + 1];
		double const voltage = (1 + width * std::log(before / (1 - before))) * (1 + 0.12 * contrast / 5);
		return 1 / (1 + std::exp(-(voltage - 1) / width));
	};
	auto const contrasted = [&](Screen const& on, int shade) {
		double places[6];
		double const after = wheel(on, shade, places);
		unsigned long const path[6] = { on.off, on.shades[0], on.shades[1], on.shades[2], on.shades[3], on.full };
		int k = 0;
		while (k < 4 && after > places[k + 1]) {
			++k;
		}
		double const t = std::min(std::max((after - places[k]) / (places[k + 1] - places[k]), 0.0), 1.0);
		unsigned long result = 0;
		for (int bits = 0; bits < 24; bits += 8) {
			double const l = linear(path[k], bits) + (linear(path[k + 1], bits) - linear(path[k], bits)) * t;
			double const c = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
			result |= (unsigned long) std::lround(c * 255) << bits;
		}
		return result;
	};
	// Grayscale is no real screen, and already white to black, so each shade moves as the Pocket's same
	// shade does, by the share of the way that one goes to the Pocket's darkest when turned up, or to
	// it switched off when turned down: to black, or to white.
	auto const followed = [&](int shade) {
		// The shares are of perceived brightness, so the faint picture a real screen keeps at either
		// end of the wheel stays too.
		auto const perceived = [&](unsigned long color) {
			double const l = luminance(color);
			return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
		};
		Screen const& real = screens[2];
		double const from = perceived(real.shades[shade]), to = perceived(contrasted(real, shade));
		double const was = perceived(now.shades[shade]);
		double const c = to <= from ? was * (1 - (from - to) / (from - perceived(real.full)))
		                            : was + (1 - was) * (to - from) / (perceived(real.off) - from);
		unsigned long const gray = (unsigned long) std::lround(std::min(std::max(c, 0.0), 1.0) * 255);
		return gray << 16 | gray << 8 | gray;
	};
	for (int i = 0; i < 4; ++i) {
		shown[i] = !contrast ? now.shades[i] : screen == 0 ? followed(i) : contrasted(now, i);
	}
	// Game Boy screen shades are shown as they are; color correction is only for a Game Boy Color's
	// screen.
	int const table = correction && color(*s) ? (s->model == LIBRETRO_GB_AGB && !colorPanel ? 2 : 1) : 0;
	if (table != built) {
		built = table;
		if (table) {
			correctedColors(table == 2);
		} else {
			for (unsigned i = 0; i < 32768; ++i) {
				unsigned const r = i & 0x1F;
				unsigned const g = i >> 5 & 0x1F;
				unsigned const b = i >> 10 & 0x1F;
				cgbColors[i] = 0xFF000000 | ((r * 255 + 15) / 31) << 16 | ((g * 255 + 15) / 31) << 8 | ((b * 255 + 15) / 31);
			}
		}
	}
	// The shades go through the same table, as a color would.
	auto const asShown = [](unsigned long rgb) {
		return cgbColors[(rgb >> 19 & 0x1F) | (rgb >> 11 & 0x1F) << 5 | (rgb >> 3 & 0x1F) << 10];
	};
	for (int c = 0; c < MAX_PLAYERS; ++c) {
		struct GB* const gb = consoles[c].gb;
		if (!gb) {
			continue;
		}
		gb->videoTable = cgbColors;
		for (int i = 0; i < 4; ++i) {
			gb->videoColors[i] = asShown(shown[i]);
		}
		gb->videoColors[4] = asShown(now.off);
	}
}

void restartAs(LibretroGbModel model);

// Reads what the cheat cartridge keeps out of the console, if one is in, so a new game starts with it.
void keepReplayFlash() {
	size_t size = 0;
	const unsigned char* const kept = s->cheatOn ? gbCheatCartKept(&s->cheat, &size) : 0;
	if (kept) {
		replayKept.assign(kept, kept + size);
	}
}

// Sachen's multicarts show the boot ROM their own logo through a lock that isn't emulated, and keep
// their header where their scrambled address lines put it.
bool isSachen(const unsigned char* bytes, size_t size) {
	return size >= 0x200 && ((bytes[0x104] == 0xCE && bytes[0x144] == 0xED && bytes[0x114] == 0x66)
	                         || (bytes[0x184] == 0xCE && bytes[0x1C4] == 0xED && bytes[0x194] == 0x66));
}

// The model a game's header asks for.
LibretroGbModel headerModel(const unsigned char* header, size_t size, bool sachen) {
	if (sachen) {
		return (header[0x151] & 0xBF) == 0x80 ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
	}
	// The header's CGB flag: 0x80 for color games that also run on a Game Boy, 0xC0 for color-only
	// games. Any other value is part of an older game's title. So is 0x80 in an unlicensed game
	// whose title runs right up to it and whose header names no known cartridge type.
	unsigned const flag = size > 0x14B ? header[0x143] : 0;
	unsigned const type = size > 0x14B ? header[0x147] : 0;
	bool const knownType = type <= 0x22 || type >= 0xFC;
	bool const title = !knownType && header[0x14B] != 0x33 && header[0x142] > 0x20 && header[0x142] < 0x7F;
	return flag == 0xC0 || (flag == 0x80 && !title) ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
}

// A ROM padded with zeros to a power of two, at least 32 KiB, as armGB banks it.
std::vector<unsigned char> padded(const unsigned char* bytes, size_t size) {
	size_t room = 0x8000;
	while (room < size) {
		room <<= 1;
	}
	std::vector<unsigned char> rom(room, 0);
	std::memcpy(&rom[0], bytes, size);
	return rom;
}

GBModel armgbModel(LibretroGbModel model) {
	switch (model) {
	case LIBRETRO_GB_CGB:
		return GB_MODEL_CGB;
	case LIBRETRO_GB_AGB:
		return GB_MODEL_AGB;
	case LIBRETRO_GB_MGB:
		return GB_MODEL_MGB;
	default:
		return GB_MODEL_DMG;
	}
}

void linkClockedBy(void*) {
	linkClocked();
}

void applyCodes(Console& c) {
	gbClearCheats(c.gb);
	for (std::string const& code : c.codes) {
		gbAddCheat(c.gb, code.c_str());
	}
}

// Starts the game as the chosen model, or as its header asks for, in the console's machine.
bool start(LibretroGbModel model, char* error, size_t errorSize) {
	s->chosen = model;
	const unsigned char* const bytes = &s->game[0];
	size_t const size = s->game.size();
	const char* const systemDirectory = hasSystemPath ? systemPath.c_str() : 0;
	bool const megaDuck = megaDuckKind >= 0;
	bool const sachen = !megaDuck && isSachen(bytes, size);
	if (megaDuck) {
		model = LIBRETRO_GB_DMG;
	} else if (model == LIBRETRO_GB_AUTO) {
		model = headerModel(bytes, size, sachen);
	}
	// Starts through Nintendo's boot ROM when the system directory has it, else armGBA's; with Boot
	// ROM off, in the state a boot ROM would have left the machine. A Mega Duck has none. A cheat
	// cartridge with nothing on top shows the boot ROM no logo, so it starts without one.
	bool const bootable = !sachen && !megaDuck && (replayProgram.empty() || replayGame);
	std::string const path = bootable && bootWanted == LIBRETRO_GB_BOOT_NINTENDO ? bootRom(systemDirectory, model) : std::string();
	std::vector<unsigned char> const fromFile = path.empty() ? std::vector<unsigned char>() : readBoot(path);
	s->boot = !fromFile.empty() ? fromFile : bootable && bootWanted != LIBRETRO_GB_BOOT_OFF ? armgbaBoot(model) : std::vector<unsigned char>();
	s->bootUsed = bootWanted;
	s->bootCrc = fromFile.empty() ? 0 : crc32(&fromFile[0], fromFile.size());
	s->model = model;
	// The Color's boot ROM is 0000-00FF and 0200-08FF; the original's 0000-00FF.
	size_t const bootSize = color(*s) ? 0x900 : 0x100;
	if (s->boot.size() < bootSize) {
		s->boot.clear();
	}
	s->noBoot = s->boot.empty();
	bool const laptop = megaDuck && laptopWanted && players == 1;
	// What the cheat cartridge keeps goes into the new game as the last one left it.
	keepReplayFlash();
	if (s->cheatOn) {
		gbCheatCartFree(&s->cheat);
		s->cheatOn = false;
	}
	if (!s->gb) {
		s->gb = static_cast<struct GB*>(std::calloc(1, sizeof(struct GB)));
		if (!s->gb) {
			std::snprintf(error, errorSize, "armGBA has no memory for the Game Boy");
			return false;
		}
	}
	struct GB* const gb = s->gb;
	s->rom = padded(bytes, size);
	gb->boot = s->noBoot ? 0 : &s->boot[0];
	gb->bootSize = s->noBoot ? 0 : bootSize;
	gb->video = s->pixels;
	gb->videoTable = cgbColors;
	gb->cpuQuarters = cpuQuarters;
	gb->rtcRealTime = deviceClock;
	gb->rtcPercent = clockPercent;
	gb->hostMicros = deviceMicros();
	gb->megaDuck = megaDuck ? megaDuckKind + 1 : 0;
	gb->duckLaptop = laptop;
	applyShownAndHeard(gb);
	if (!gbLoad(gb, &s->rom[0], s->rom.size(), armgbModel(model))) {
		std::snprintf(error, errorSize, "armGBA can't run this Game Boy game");
		return false;
	}
	gb->user = s;
	gb->linkClocked = players > 1 ? 0 : linkClockedBy;
	duckLaptopConnect(laptop, laptopPrinter, laptopLanguage, laptopControl, laptopIn, laptopSlot);
	s->linked = players == 1 && (printerOn || laptop);
	laptopSlotIn = false;
	if (!replayProgram.empty()) {
		if (!gbCheatCartInit(&s->cheat, &replayProgram[0], replayProgram.size(), replayGame)) {
			std::snprintf(error, errorSize, "armGBA can't run this cheat cartridge");
			return false;
		}
		s->cheatOn = true;
		size_t keptSize = 0;
		unsigned char* const kept = gbCheatCartKept(&s->cheat, &keptSize);
		if (kept && keptSize == replayKept.size()) {
			std::memcpy(kept, &replayKept[0], keptSize);
		}
		std::memcpy(s->cheat.s.fill, genieDigits, sizeof(genieDigits));
		std::memcpy(s->cheat.s.fillCount, genieCounts, sizeof(genieCounts));
		gb->cheatCart = &s->cheat;
		gbReset(gb);
	}
	if (model == LIBRETRO_GB_AGB && !s->noBoot) {
		gbStall(gb, ADVANCE_STALL);
	}
	gbLinkStatus(gb, players > 1 || s->linked ? GB_LINK_ON : GB_LINK_OFF);
	applyCodes(*s);
	setColors();
	s->overDot = 0;
	s->capacitor[0] = s->capacitor[1] = 0;
	s->heardApart = false;
	setCapacitor(*s);
	s->pressed = 0;
	std::memset(s->pixels, 0, sizeof(s->pixels));
	std::memset(s->shown, 0, sizeof(s->shown));
	gbBlankFrame(gb);
	return true;
}

// The eight bits of a transfer this Game Boy clocked are out: the other one's byte, the printer's or
// the laptop's comes back.
void linkClocked() {
	struct GB* const gb = s->gb;
	gbLinkStatus(gb, GB_LINK_ACK);
	int const out = gbLinkStatus(gb, GB_LINK_OUT);
	if (duckLaptopConnected()) {
		gbLinkStatus(gb, duckLaptopClockedByConsole(static_cast<uint8_t>(out)));
		return;
	}
	bool const toPrinter = printerOn && !linkGbConnected();
	gbLinkStatus(gb, toPrinter ? printerTransfer(&printer, out, gbTime(gb) / 2, printed) : linkGbTransfer(out));
}

void updateLink() {
	bool const linked = linkGbConnected() || printerOn || duckLaptopConnected();
	if (s->linked != linked) {
		s->linked = linked;
		gbLinkStatus(s->gb, s->linked ? GB_LINK_ON : GB_LINK_OFF);
	}
}

}

extern "C" {

bool libretroGbLoad(const void* rom, size_t size, const char* systemDirectory, enum LibretroGbModel model, bool colorCorrection,
                    unsigned audioRate, char* error, size_t errorSize) {
	libretroGbUnload();
	s->game.assign(static_cast<const unsigned char*>(rom), static_cast<const unsigned char*>(rom) + size);
	systemPath = systemDirectory ? systemDirectory : "";
	hasSystemPath = systemDirectory != 0;
	correction = colorCorrection;
	ratio = NATIVE_RATE / audioRate;
	s->stateSize = 0;
	s->wanted = model;
	printerReset(&printer);
	if (!start(model, error, errorSize)) {
		libretroGbUnload();
		return false;
	}
	std::memset(s->rtcFile, 0, sizeof(s->rtcFile));
	s->rtcPending = rtcSize();
	return true;
}

void libretroGbUnload(void) {
	if (s->cheatOn) {
		gbCheatCartFree(&s->cheat);
		s->cheatOn = false;
	}
	std::free(s->gb);
	s->gb = 0;
	s->codes.clear();
}

void libretroGbReset(enum LibretroGbModel model) {
	s->wanted = model;
	if (model == s->chosen && s->bootUsed == bootWanted) {
		// The laptop powers on into its system ROM.
		if (laptopSlotIn) {
			gbSwapMegaDuckRom(s->gb, &s->rom[0], s->rom.size(), static_cast<GBDuckKind>(megaDuckKind));
			laptopSlotIn = false;
		}
		gbReset(s->gb);
		if (s->model == LIBRETRO_GB_AGB && !s->noBoot) {
			gbStall(s->gb, ADVANCE_STALL);
		}
		return;
	}
	restartAs(model);
}

// Restarts the selected console as its power switch would, keeping it on the link.
void libretroGbRestart(void) {
	libretroGbReset(s->wanted);
	if (players > 1) {
		s->gb->linkClocked = 0;
		gbLinkStatus(s->gb, GB_LINK_ON);
	}
	s->linked = false;
}

}

namespace {

// Restarts the game in the same machine as another model, or with the boot ROM switched on or off.
// Its cartridge RAM stays as it was and where it was, since the frontend may hold a pointer to it,
// and so does its clock.
void restartAs(LibretroGbModel model) {
	size_t size;
	const unsigned char* const ram = static_cast<const unsigned char*>(libretroGbSaveRam(&size));
	std::vector<unsigned char> const kept(ram, ram + size);
	uint32_t clock[11];
	unsigned char huc3[HUC3_STATE];
	if (hasMbc3Clock(*s)) {
		gbRtcGet(s->gb, clock);
	} else if (hasHuC3(*s)) {
		gbHuc3Get(s->gb, huc3);
	}
	LibretroGbModel const before = s->chosen;
	char error[64];
	if (!start(model, error, sizeof(error))) {
		start(before, error, sizeof(error));
	}
	void* const now = libretroGbSaveRam(&size);
	if (now && size == kept.size() && size) {
		std::memcpy(now, &kept[0], size);
	}
	if (hasMbc3Clock(*s) && !s->rtcPending) {
		gbRtcSet(s->gb, clock);
	} else if (hasHuC3(*s) && !s->rtcPending) {
		gbHuc3Set(s->gb, huc3);
	}
}

}

extern "C" {

void libretroGbSetColorCorrection(bool on) {
	correction = on;
	setColors();
}

void libretroGbSetColorPanel(bool panel) {
	colorPanel = panel;
	setColors();
}

void libretroGbSetBootRom(enum LibretroGbBoot boot) {
	bootWanted = boot;
}

void libretroGbSetAudioFilter(enum LibretroGbFilter filter) {
	audioFilter = filter;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		setCapacitor(consoles[i]);
	}
}

void libretroGbSetAdvanceOutput(enum LibretroAdvanceOutput output) {
	advanceOutput = output;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		setCapacitor(consoles[i]);
	}
}

void libretroGbSetMegaDuck(int kind) {
	megaDuckKind = kind;
}

void libretroGbSetMegaDuckLaptop(bool on, int printerKind, int language) {
	laptopWanted = on;
	laptopPrinter = printerKind;
	laptopLanguage = language ? DUCK_LAPTOP_GERMAN : DUCK_LAPTOP_SPANISH;
}

void libretroGbSetGameGenieCodes(const uint8_t digits[27], const uint8_t counts[3]) {
	std::memcpy(genieDigits, digits, sizeof(genieDigits));
	std::memcpy(genieCounts, counts, sizeof(genieCounts));
	if (s->cheatOn) {
		std::memcpy(s->cheat.s.fill, genieDigits, sizeof(genieDigits));
		std::memcpy(s->cheat.s.fillCount, genieCounts, sizeof(genieCounts));
	}
}

bool libretroGbSwapGame(const void* rom, size_t size, bool game, char* error, size_t errorSize) {
	if (!s->gb || replayProgram.empty()) {
		return false;
	}
	keepReplayFlash();
	std::vector<unsigned char> const before(s->game);
	bool const hadGame = replayGame;
	s->game.assign(static_cast<const unsigned char*>(rom), static_cast<const unsigned char*>(rom) + size);
	replayGame = game;
	bool const ok = start(s->wanted, error, errorSize);
	if (!ok) {
		// The game before goes back in.
		s->game = before;
		replayGame = hadGame;
		char ignored[256];
		start(s->wanted, ignored, sizeof(ignored));
	}
	std::memset(s->rtcFile, 0, sizeof(s->rtcFile));
	s->rtcPending = rtcSize();
	return ok;
}

void libretroGbSetActionReplay(const void* program, size_t size, bool game) {
	const unsigned char* const bytes = static_cast<const unsigned char*>(program);
	replayProgram.assign(bytes, bytes + (program ? size : 0));
	replayGame = game;
	replayKept.clear();
}

void libretroGbActionReplayControls(bool button, bool switchOn) {
	if (s->cheatOn) {
		s->cheat.button = button;
		s->cheat.switchOn = switchOn;
	}
}

uint8_t* libretroGbActionReplayFlash(size_t* size) {
	*size = 0;
	return s->cheatOn ? gbCheatCartKept(&s->cheat, size) : 0;
}

bool libretroGbActionReplayFlashChanged(void) {
	bool const changed = s->cheatOn && s->cheat.flashChanged;
	if (s->cheatOn) {
		s->cheat.flashChanged = false;
	}
	return changed;
}

void libretroGbSetMegaDuckSlot(const void* rom, size_t size, int kind) {
	laptopCartridge = rom ? padded(static_cast<const unsigned char*>(rom), size) : std::vector<unsigned char>();
	laptopCartridgeKind = kind;
}

void libretroGbKeyboard(bool down, unsigned keycode, uint32_t character, uint16_t modifiers) {
	duckLaptopKeyboard(down, keycode, character, modifiers);
}

const int16_t* libretroGbMixSpeech(const int16_t* samples, size_t frames) {
	return duckLaptopConnected() ? duckLaptopMixSpeech(samples, frames) : samples;
}

void libretroGbSetScreen(int which) {
	screen = which;
	setColors();
}

void libretroGbSetLayersShown(unsigned mask) {
	layersShown = mask & 7;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb) {
			applyShownAndHeard(consoles[i].gb);
		}
	}
}

void libretroGbSetMutedChannels(unsigned mask) {
	channelsMuted = mask & 0xF;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb) {
			applyShownAndHeard(consoles[i].gb);
		}
	}
}

void libretroGbSetContrast(int steps) {
	contrast = steps;
	setColors();
}

uint32_t libretroGbShade(int shade) {
	return (uint32_t) shown[shade];
}

uint32_t libretroGbDarkestColor(void) {
	return screens[screen].shades[3];
}

uint32_t libretroGbOffColor(void) {
	return screens[screen].off;
}

bool libretroGbOnAdvance(void) {
	return s->model == LIBRETRO_GB_AGB;
}

uint32_t libretroGbBootCrc(void) {
	uint32_t crc = 0;
	for (int i = 0; i < players; ++i) {
		crc = (crc << 1 | crc >> 31) ^ consoles[i].bootCrc;
	}
	return crc;
}

}

namespace {

// The console's sound leaves through a capacitor that keeps DC out of it, connected only while a
// channel's DAC is on (Pan Docs, Audio Details); armGB says for each sample whether one is.
inline int16_t throughCapacitor(int32_t* capacitor, int side, int32_t in, bool connected) {
	if (!s->charge) {
		return static_cast<int16_t>(in);
	}
	return connected ? outputCapacitor(&capacitor[side], in, s->charge) : 0;
}

// A frontend sample out through the capacitor: the console's, or what's heard
inline void throughOutput(int32_t* capacitor, int32_t left, int32_t right, bool dacs, int16_t* out) {
	if (s->wiring == Wiring::SHARED_RETURN) {
		outputSharedReturn(&capacitor[0], left, right, s->charge, out);
	} else {
		// A Game Boy Advance's capacitor is after its amp: connected whatever the DACs do
		bool const connected = dacs || s->wiring == Wiring::ADVANCE;
		out[0] = throughCapacitor(capacitor, 0, left, connected);
		out[1] = throughCapacitor(capacitor, 1, right, connected);
	}
}

// The sound armGB made, out through the capacitor. With channels left out, what's heard goes through
// its own charge beside the console's, which states keep.
void takeSound() {
	struct GB* const gb = s->gb;
	bool const heard = gb->channelsMuted;
	if (!heard) {
		s->heardApart = false;
	} else if (!s->heardApart) {
		s->heardCapacitor[0] = s->capacitor[0];
		s->heardCapacitor[1] = s->capacitor[1];
		s->heardApart = true;
	}
	for (uint32_t i = 0; i + 1 < gb->audioCount; i += 2) {
		if (s->sampleCount + 2 > sizeof(s->samples) / sizeof(s->samples[0])) {
			break;
		}
		int16_t* const out = &s->samples[s->sampleCount];
		bool const dacs = gb->audioDacs[i / 2];
		throughOutput(s->capacitor, gb->audio[i], gb->audio[i + 1], dacs, out);
		if (heard) {
			throughOutput(s->heardCapacitor, gb->heard[i], gb->heard[i + 1], dacs, out);
		}
		s->sampleCount += 2;
	}
	gb->audioCount = 0;
}

// Runs the selected console for some samples, or to the end of a frame; count becomes the samples run.
// Returns whether a frame ended, its picture then in shown too.
bool runFor(std::size_t& count) {
	uint32_t dots = static_cast<uint32_t>(count * 2) - s->overDot;
	bool const ended = gbRunFor(s->gb, &dots);
	uint32_t const total = dots + s->overDot;
	count = total / 2;
	s->overDot = total % 2;
	takeSound();
	if (ended) {
		std::memcpy(s->shown, s->pixels, sizeof(s->shown));
	}
	return ended;
}

// Starts a frame on the selected console: its buttons, the device's time, and its clock file once
// loaded.
void beginFrame(uint16_t keys) {
	s->pressed = keys & 0xFF;
	s->gb->keys = s->pressed;
	s->gb->hostMicros = deviceMicros();
	s->sampleCount = 0;
	if (s->rtcPending) {
		readRtcFile();
	}
}

void endFrame() {
	if (rtcSize()) {
		writeRtcFile();
	}
}

// The cable between the two consoles here. When one has clocked out its eight bits, it takes the
// other's byte. The other takes this one's only if its serial port is waiting for a clock or
// clocking too. Both then stop their own clocks, since the transfer is done.
void exchange(Console& clocked, Console& other) {
	gbLinkStatus(clocked.gb, GB_LINK_ACK);
	int const out = gbLinkStatus(clocked.gb, GB_LINK_OUT);
	gbLinkStatus(clocked.gb, gbLinkStatus(other.gb, GB_LINK_OUT));
	gbLinkStatus(clocked.gb, GB_LINK_STOP);
	gbLinkStatus(other.gb, out);
	gbLinkStatus(other.gb, GB_LINK_STOP);
}

// Sound samples until a Game Boy's transfer on its own clock ends: -1 for none, 0 when due.
long serialLeft(Console& c) {
	if (gbLinkStatus(c.gb, GB_LINK_CLOCKED)) {
		return 0;
	}
	int const dots = gbLinkStatus(c.gb, GB_LINK_LEFT);
	return dots < 0 ? -1 : dots / 2;
}

// The fewest samples a transfer a Game Boy could start now takes: eight bits at 8192 Hz, or on a
// Game Boy Color with SC's fast bit, at 262144 Hz.
long shortestTransfer(Console& c) {
	return c.gb->cgbMode && gbLinkStatus(c.gb, GB_LINK_CONTROL) & 0x02 ? 64 : 2048;
}

// Where the consoles next meet: at the frame's end, at the next transfer's end on the cable or the
// adapter, and no later than the shortest transfer any console could start from now.
long nextMeeting(long const done[]) {
	long now = done[0];
	for (int i = 1; i < players; ++i) {
		now = std::min(now, done[i]);
	}
	long meet = FRAME_SAMPLES;
	if (adapterOn) {
		meet = std::min<long>(meet, std::max<long>(adapter.at, now));
	}
	for (int i = 0; i < players; ++i) {
		long const left = serialLeft(consoles[i]);
		if (left >= 0) {
			meet = std::min(meet, done[i] + left);
		}
		if (!adapterOn) {
			meet = std::min(meet, now + shortestTransfer(consoles[i]));
		}
	}
	return meet;
}

// With every console at the meeting point: the transfers ending there.
void meet(long now) {
	if (adapterOn) {
		// On the adapter, a Game Boy's own clock reaches nothing.
		for (int i = 0; i < players; ++i) {
			if (serialLeft(consoles[i]) == 0) {
				gbLinkStatus(consoles[i].gb, GB_LINK_ACK);
				gbLinkStatus(consoles[i].gb, 0xFF);
				gbLinkStatus(consoles[i].gb, GB_LINK_STOP);
			}
		}
		while (adapter.at <= now) {
			adapterTransfer();
		}
		return;
	}
	for (int i = 0; i < players; ++i) {
		if (serialLeft(consoles[i]) == 0) {
			exchange(consoles[i], consoles[1 - i]);
		}
	}
}

// With exact link timing: this Game Boy's transfer ends, and the other's byte comes back.
void clockedExactly() {
	gbLinkStatus(s->gb, GB_LINK_ACK);
	int const in = linkGbTransfer(gbLinkStatus(s->gb, GB_LINK_OUT));
	gbLinkStatus(s->gb, in & 0xFF);
	gbLinkStatus(s->gb, GB_LINK_STOP);
}

// With exact link timing, a frame runs in parts. Each part ends at this Game Boy's own transfer
// end, or where the leading console has got to. Its own transfer is told to the other as soon as it
// starts. A long wait at a stop ends the frame early.
void runExactlyParts() {
	long done = 0;
	while (done < FRAME_SAMPLES) {
		long const own = serialLeft(*s);
		if (own > 0) {
			linkGbClocking(gbLinkStatus(s->gb, GB_LINK_OUT), own, shortestTransfer(*s));
		}
		linkGbPoll();
		if (own == 0) {
			clockedExactly();
			continue;
		}
		long const allowed = linkGbAllowance();
		if (allowed == 0) {
			if (!linkGbArrived()) {
				return;
			}
			continue;
		}
		long count = std::min<long>(FRAME_SAMPLES - done, EXACT_PART);
		count = std::min(count, allowed);
		count = own > 0 ? std::min(count, own) : count;
		std::size_t samples = count;
		runFor(samples);
		done += samples;
		linkGbRan(samples);
	}
}

// The laptop's controller: the console's SC, and a byte it clocks in
unsigned laptopControl() {
	return static_cast<unsigned>(gbLinkStatus(s->gb, GB_LINK_CONTROL));
}

void laptopIn(uint8_t byte) {
	gbLinkStatus(s->gb, byte);
}

bool laptopSlot() {
	if (laptopCartridge.empty()) {
		return false;
	}
	gbSwapMegaDuckRom(s->gb, &laptopCartridge[0], laptopCartridge.size(), static_cast<GBDuckKind>(laptopCartridgeKind));
	laptopSlotIn = true;
	return true;
}

// With a Mega Duck laptop, runs until a picture is drawn in parts that end where its controller does
// something next.
void runWithLaptop() {
	bool drawn = false;
	for (long done = 0; !drawn && done < 4L * FRAME_SAMPLES;) {
		std::size_t count = std::max<long>(1, std::min<long>((duckLaptopCyclesToNext() + 1) / 2, FRAME_SAMPLES));
		drawn = runFor(count);
		done += count;
		duckLaptopRan(2 * static_cast<long>(count));
	}
}

// Runs until a picture is drawn; linked, in sixteen parts with the cable checked between them.
void runUntilDrawn() {
	int const parts = s->linked ? 16 : 1;
	bool drawn = false;
	for (int runs = 0; !drawn && runs < 4 * parts; ++runs) {
		if (s->linked) {
			linkGbPoll();
		}
		std::size_t count = FRAME_SAMPLES / parts;
		drawn = runFor(count);
	}
}

void runExactly() {
	s->gb->linkClocked = 0;
	runExactlyParts();
	s->gb->linkClocked = linkClockedBy;
}

}

extern "C" {

// Runs one frame: to the start of V-blank, or with the screen off, a frame's time.
const uint32_t* libretroGbRunFrame(uint16_t keys, const int16_t** out, size_t* frames) {
	beginFrame(keys);
	updateLink();
	bool const exactly = s->linked && linkExactOn() && !duckLaptopConnected();
	if (exactly) {
		runExactly();
	} else if (duckLaptopConnected()) {
		runWithLaptop();
	} else {
		runUntilDrawn();
	}
	endFrame();
	*out = s->samples;
	*frames = s->sampleCount / 2;
	return exactly ? s->shown : s->pixels;
}

void libretroGbRunPlayers(const uint16_t keys[], const uint32_t* pictures[], const int16_t* out[], size_t frames[]) {
	Console* const selected = s;
	long done[MAX_PLAYERS];
	for (int i = 0; i < players; ++i) {
		s = &consoles[i];
		beginFrame(keys[i]);
		done[i] = s->ahead;
	}
	// The consoles run in turns, the one furthest behind next, each up to where they next meet; there
	// they all stand on the same sample as a transfer ends.
	for (;;) {
		int i = 0;
		for (int j = 1; j < players; ++j) {
			i = done[j] < done[i] ? j : i;
		}
		if (done[i] >= FRAME_SAMPLES) {
			break;
		}
		long const meeting = nextMeeting(done);
		if (meeting <= done[i]) {
			meet(meeting);
			if (nextMeeting(done) <= done[i]) {
				// Nothing ended after all, as a game stopped its transfer: run on a little.
				s = &consoles[i];
				std::size_t count = 1;
				runFor(count);
				done[i] += count;
			}
			continue;
		}
		s = &consoles[i];
		std::size_t count = meeting - done[i];
		runFor(count);
		done[i] += count;
	}
	dmg07EndFrame(&adapter, FRAME_SAMPLES);
	for (int i = 0; i < players; ++i) {
		s = &consoles[i];
		endFrame();
		s->ahead = done[i] - FRAME_SAMPLES;
		pictures[i] = s->shown;
		out[i] = s->samples;
		frames[i] = s->sampleCount / 2;
	}
	s = selected;
}

// The cartridge's battery RAM, which the frontend loads and saves as the save file.
void* libretroGbSaveRam(size_t* size) {
	*size = s->gb ? s->gb->cart.ramSize : 0;
	return *size ? s->gb->sram : 0;
}

// The cartridge's clock, MBC3 or HuC3, which the frontend keeps as the .rtc file.
void* libretroGbRtc(size_t* size) {
	*size = rtcSize();
	return *size ? s->rtcFile : 0;
}

LibretroGbClock libretroGbClock(void) {
	return hasMbc3Clock(*s) ? LIBRETRO_GB_MBC3_CLOCK : hasHuC3(*s) ? LIBRETRO_GB_HUC3_CLOCK : LIBRETRO_GB_NO_CLOCK;
}

void libretroGbSetClock(bool timeOfGame, unsigned percent, long long offset) {
	gameTime = timeOfGame;
	clockPercent = gameTime ? percent : 100;
	clockOffset = offset;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		Console& c = consoles[i];
		if (!c.gb) {
			continue;
		}
		gbRtcSetSpeed(c.gb, clockPercent);
		if (hasMbc3Clock(c) && !c.rtcPending && c.clockMoved != offset) {
			uint32_t r[11];
			gbRtcGet(c.gb, r);
			moveBy(c, r, offset);
			gbRtcSet(c.gb, r);
		}
	}
}

void libretroGbDeviceTime(bool on) {
	deviceClock = on;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb) {
			gbRtcSetRealTime(consoles[i].gb, on);
		}
	}
}

bool libretroGbResetClock(void) {
	bool reset = false;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		Console& c = consoles[i];
		if (hasMbc3Clock(c)) {
			uint32_t r[11] = { 0 };
			r[RTC_DH] = r[RTC_DH + RTC_LATCHED] = 0x40;
			gbRtcSet(c.gb, r);
			c.rtcPending = false;
			c.clockMoved = clockOffset;
			c.clockShort = 0;
			reset = true;
		}
	}
	return reset;
}

// Converts a stick position into the tilt sensor's reading: lower to the right and towards the
// player; 0x70 is the cartridge tipped onto its side.
void libretroGbSetTilt(int x, int y) {
	if (s->gb) {
		s->gb->tiltX = -x * 0x70 / 32768;
		s->gb->tiltY = -y * 0x70 / 32768;
	}
}

// Flicks the console, for the one sensor reading the flick lasts.
void libretroGbFlick(void) {
	if (s->gb) {
		s->gb->flick = true;
	}
}

// Work RAM, for achievements and cheats.
void* libretroGbSystemRam(size_t* size) {
	*size = !s->gb ? 0 : gbColor(s->gb) ? 0x8000 : 0x2000;
	return *size ? s->gb->wram : 0;
}

size_t libretroGbMemoryMap(struct retro_memory_descriptor* out) {
	struct GB* const gb = s->gb;
	if (!gb) {
		return 0;
	}
	unsigned char* const rom = &s->rom[0];
	struct retro_memory_descriptor const areas[] = {
		{ RETRO_MEMDESC_SYSTEM_RAM, gb->wram, 0, 0xC000, 0, 0, 0x1000, 0 },
		{ RETRO_MEMDESC_SYSTEM_RAM, gb->wram, 0x1000, 0xD000, 0, 0, 0x1000, 0 },
		{ RETRO_MEMDESC_SYSTEM_RAM, gb->hram, 0, 0xFF80, 0, 0, GB_SIZE_HRAM, 0 },
		{ 0, gb->vram, 0, 0x8000, 0, 0, 0x2000, 0 },
		{ 0, gb->oam, 0, 0xFE00, 0xFFFFFFE0, 0, GB_SIZE_OAM, 0 },
		{ RETRO_MEMDESC_CONST, rom, 0, 0x0000, 0, 0, 0x4000, 0 },
		{ RETRO_MEMDESC_CONST, rom, 0x4000, 0x4000, 0, 0, 0x4000, 0 },
		{ 0, gb->io, 0, 0xFF00, 0, 0, 0x80, 0 },
	};
	size_t count = 0;
	for (std::size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); ++i) {
		out[count++] = areas[i];
	}
	if (gb->cart.ramSize) {
		struct retro_memory_descriptor const area = { 0, gb->sram, 0, 0xA000, ~(std::size_t) 0x1FFF, 0, gb->cart.ramSize, 0 };
		out[count++] = area;
	}
	// A Game Boy Color's work RAM banks 2 to 7, at the addresses achievements expect.
	if (gbColor(gb)) {
		struct retro_memory_descriptor const area = { RETRO_MEMDESC_SYSTEM_RAM, gb->wram, 0x2000, 0x10000, 0xFFFFA000, 0, 0x6000, 0 };
		out[count++] = area;
	}
	return count;
}

bool libretroGbIsColor(void) {
	return s->gb && gbColor(s->gb);
}

bool libretroGbBooting(void) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb && (consoles[i].gb->bootMapped || consoles[i].gb->cpu.stalled)) {
			return true;
		}
	}
	return false;
}

void libretroGbSetCpuClock(unsigned quarters, bool loaded) {
	cpuQuarters = quarters;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb) {
			gbSetCpuQuarters(consoles[i].gb, quarters, loaded);
		}
	}
}

bool libretroGbSetCheats(const char* const* codes, size_t count) {
	bool understood = true;
	s->codes.clear();
	for (size_t i = 0; i < count; ++i) {
		std::string const all = codes[i] ? codes[i] : "";
		std::size_t pos = 0;
		while (pos < all.size()) {
			std::size_t const end = all.find_first_of("+ \t\r\n;", pos);
			std::string const code = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
			pos = end == std::string::npos ? all.size() : end + 1;
			if (!code.empty()) {
				s->codes.push_back(code);
			}
		}
	}
	if (s->gb) {
		gbClearCheats(s->gb);
		for (std::string const& code : s->codes) {
			understood &= gbAddCheat(s->gb, code.c_str());
		}
	}
	return understood;
}

}

namespace {

// A console's state: a mark, the machine, the output capacitor's charge, then whether the laptop's
// slot cartridge had taken the system ROM's place and the laptop's controller, the printer (player
// 1's), and the cheat cartridge, each with room whether or not it's there, so states keep one size.
// Netplay's states hold no capacitor charge, and loading one keeps this device's.
uint32_t const STATE_MARK = 0x53424761u;
bool netplayStates;

size_t partsSize(const Console& c) {
	return sizeof(STATE_MARK) + gbStateSize() + sizeof(c.capacitor) + 1 + duckLaptopStateSize() + sizeof(printer)
	       + (c.cheatOn ? gbCheatCartStateSize(&c.cheat) : 0);
}

size_t stateSizeOne() {
	if (!s->gb) {
		return 0;
	}
	s->stateSize = std::max(s->stateSize, partsSize(*s));
	return s->stateSize;
}

bool saveOne(void* data, size_t size) {
	if (!s->gb || size < stateSizeOne()) {
		return false;
	}
	unsigned char* at = static_cast<unsigned char*>(data);
	std::memset(at, 0, s->stateSize);
	std::memcpy(at, &STATE_MARK, sizeof(STATE_MARK));
	at += sizeof(STATE_MARK);
	gbSaveState(s->gb, at);
	at += gbStateSize();
	if (!netplayStates) {
		std::memcpy(at, s->capacitor, sizeof(s->capacitor));
	}
	at += sizeof(s->capacitor);
	*at++ = laptopSlotIn;
	if (duckLaptopConnected()) {
		duckLaptopSaveState(at);
	}
	at += duckLaptopStateSize();
	if (s == &consoles[0]) {
		std::memcpy(at, &printer, sizeof(printer));
	}
	at += sizeof(printer);
	if (s->cheatOn) {
		gbCheatCartSaveState(&s->cheat, at);
	}
	return true;
}

// On the device's time, a loaded state leaves the clock as it was, as the Advance's does, except
// before the first frame, where the state's clock wins over the .rtc file.
bool loadOne(const void* data, size_t size) {
	if (!s->gb || size < partsSize(*s)) {
		return false;
	}
	const unsigned char* at = static_cast<const unsigned char*>(data);
	uint32_t mark;
	std::memcpy(&mark, at, sizeof(mark));
	if (mark != STATE_MARK) {
		return false;
	}
	at += sizeof(mark);
	uint32_t clock[11];
	bool const keepClock = deviceClock && hasMbc3Clock(*s) && !s->rtcPending;
	if (keepClock) {
		gbRtcGet(s->gb, clock);
	}
	// The ROM the state ran (the system's or the slot cartridge's) goes in first, for the state's banks.
	bool const slot = at[gbStateSize() + sizeof(s->capacitor)] & 1;
	if (duckLaptopConnected() && !laptopCartridge.empty()) {
		s->gb->rom = slot ? &laptopCartridge[0] : &s->rom[0];
		s->gb->romSize = slot ? laptopCartridge.size() : s->rom.size();
		laptopSlotIn = slot;
	}
	GBModel const was = static_cast<GBModel>(s->gb->model);
	if (!gbLoadState(s->gb, at, gbStateSize())) {
		return false;
	}
	at += gbStateSize();
	if (!netplayStates) {
		std::memcpy(s->capacitor, at, sizeof(s->capacitor));
	}
	at += sizeof(s->capacitor) + 1;
	if (duckLaptopConnected()) {
		duckLaptopLoadState(at);
	}
	at += duckLaptopStateSize();
	if (s == &consoles[0]) {
		std::memcpy(&printer, at, sizeof(printer));
	}
	at += sizeof(printer);
	if (s->cheatOn) {
		gbCheatCartLoadState(&s->cheat, at);
	}
	s->heardApart = false;
	s->overDot = 0;
	// A state from another model runs as that model until the next restart.
	if (s->gb->model != was) {
		static const LibretroGbModel models[4] = { LIBRETRO_GB_DMG, LIBRETRO_GB_MGB, LIBRETRO_GB_CGB, LIBRETRO_GB_AGB };
		s->model = models[s->gb->model & 3];
		setCapacitor(*s);
	}
	setColors();
	// A state holds the clock, so one loaded before the first frame wins over the .rtc file.
	s->rtcPending = false;
	if (keepClock) {
		gbRtcSet(s->gb, clock);
	}
	gbRtcSetRealTime(s->gb, deviceClock);
	return true;
}

}

extern "C" {

// armGB's states are always the whole machine, exactly.
void libretroGbExactStates(bool) {
}

void libretroGbNetplayStates(bool netplay) {
	netplayStates = netplay;
}

// Link cable: the byte this Game Boy shifts out, and one shifted in from the other's transfer.
int libretroGbLinkOut(void) {
	return s->gb ? gbLinkStatus(s->gb, GB_LINK_OUT) : 0xFF;
}

void libretroGbLinkIn(int byte) {
	if (s->gb) {
		gbLinkStatus(s->gb, byte & 0xFF);
	}
}

int libretroGbLinkControl(void) {
	return s->gb ? gbLinkStatus(s->gb, GB_LINK_CONTROL) : 0;
}

void libretroGbSelect(int which) {
	s = &consoles[which];
}

void libretroGbSetPrinter(bool on, PrinterOutput output) {
	if (on && !printerOn) {
		printerReset(&printer);
	}
	printerOn = on;
	printed = output;
}

// Real consoles are never in step, and games rely on it: when two try to lead the link at the same
// moment, Pokemon's handshake never settles. So each console runs a part of a frame behind the one
// before.
void libretroGbSetPlayers(int count, bool withAdapter) {
	players = count;
	adapterOn = count > 1 && withAdapter;
	dmg07Reset(&adapter);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		static long const behind[MAX_PLAYERS] = { 0, FRAME_SAMPLES / 2, FRAME_SAMPLES / 4, FRAME_SAMPLES * 3 / 4 };
		consoles[i].ahead = behind[i];
		if (consoles[i].gb) {
			consoles[i].gb->linkClocked = count > 1 ? 0 : linkClockedBy;
			gbLinkStatus(consoles[i].gb, count > 1 ? GB_LINK_ON : GB_LINK_OFF);
			consoles[i].linked = false;
		}
	}
}

// With the consoles of a link here, a state holds, in order:
// - how far each console ran past the last frame;
// - the adapter;
// - each console's state, in its own fixed-size part;
// - their pictures, since a frame ends partway through drawing one and a state loaded for a
//   rollback has to carry on with it.
size_t libretroGbStateSize(void) {
	if (players < 2) {
		return stateSizeOne();
	}
	size_t size = players * sizeof(int32_t) + sizeof(adapter);
	Console* const selected = s;
	for (int i = 0; i < players; ++i) {
		s = &consoles[i];
		size += stateSizeOne() + sizeof(s->pixels) + sizeof(s->shown);
	}
	s = selected;
	return size;
}

bool libretroGbSaveState(void* data, size_t size) {
	if (players < 2) {
		return saveOne(data, size);
	}
	if (size < libretroGbStateSize()) {
		return false;
	}
	char* at = static_cast<char*>(data);
	for (int i = 0; i < players; ++i) {
		if (!consoles[i].gb) {
			return false;
		}
		int32_t const ahead = consoles[i].ahead;
		std::memcpy(at, &ahead, sizeof(ahead));
		at += sizeof(ahead);
	}
	std::memcpy(at, &adapter, sizeof(adapter));
	at += sizeof(adapter);
	Console* const selected = s;
	bool saved = true;
	for (int i = 0; i < players && saved; ++i) {
		s = &consoles[i];
		saved = saveOne(at, s->stateSize);
		at += s->stateSize;
	}
	for (int i = 0; i < players && saved; ++i) {
		std::memcpy(at, consoles[i].pixels, sizeof(consoles[i].pixels));
		at += sizeof(consoles[i].pixels);
		std::memcpy(at, consoles[i].shown, sizeof(consoles[i].shown));
		at += sizeof(consoles[i].shown);
	}
	s = selected;
	return saved;
}

bool libretroGbLoadState(const void* data, size_t size) {
	if (players < 2) {
		return loadOne(data, size);
	}
	if (size < libretroGbStateSize()) {
		return false;
	}
	const char* at = static_cast<const char*>(data);
	int32_t ahead[MAX_PLAYERS];
	for (int i = 0; i < players; ++i) {
		if (!consoles[i].gb) {
			return false;
		}
		std::memcpy(&ahead[i], at, sizeof(ahead[i]));
		at += sizeof(ahead[i]);
	}
	std::memcpy(&adapter, at, sizeof(adapter));
	at += sizeof(adapter);
	Console* const selected = s;
	bool loaded = true;
	for (int i = 0; i < players && loaded; ++i) {
		s = &consoles[i];
		loaded = loadOne(at, s->stateSize);
		s->ahead = ahead[i];
		at += s->stateSize;
	}
	for (int i = 0; i < players && loaded; ++i) {
		std::memcpy(consoles[i].pixels, at, sizeof(consoles[i].pixels));
		at += sizeof(consoles[i].pixels);
		std::memcpy(consoles[i].shown, at, sizeof(consoles[i].shown));
		at += sizeof(consoles[i].shown);
	}
	s = selected;
	return loaded;
}

}
