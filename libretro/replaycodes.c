/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Action Replay's codes as text, beside what it keeps: each game's codes, by the names its
// menus give them, switched on or off, and the codes as they're written down. Read from what each
// cartridge keeps (nothing about it is published):
// - Game Boy, Action Replay Online and Xtreme, GameShark Online: its flash chip has the games at
//   8000 (a count, then 18 bytes each: an id of 12 bits with flags above it, 20 marking the game
//   chosen last, and a name of 16) and the codes at A000, C000 and E000 (a count, then 18 bytes
//   each: the game's id, with 10 above it while the code is on, a name of 12, and its 4 bytes).
// - Game Boy, Pro Action Replay: its RAM has 4 codes of 4 bytes at 4A, an empty one all 0.
// - Game Boy Advance: the flash chip has "GBA_" at 21000 and the games at 21040 (a count of 32
//   bits, then for each its name of 20 and a count of its codes, each a count of its 32-bit words
//   with flags in the top byte, 80 while it's on, a name of 20 and the words).
// - Game Boy Advance, Action Replay MAX: two copies of a directory, at 34000 and 3A000, the newer
//   with the larger number at 5FFA in it; 32 bytes a game (its 4-letter code, a name ended by 0,
//   the first of its blocks of 80 at 1C), the block at 30000 + 80 per block holding its codes as
//   the other cartridges' do.

#include "core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Text {
	char* data;
	size_t length;
	size_t room;
	bool failed;
};

// The text stops growing past this, as a damaged file could make any amount.
enum { MOST_TEXT = 4 << 20 };

static void add(struct Text* text, const char* format, ...) {
	if (text->length > MOST_TEXT) {
		text->failed = true;
		return;
	}
	for (;;) {
		va_list args;
		va_start(args, format);
		const size_t left = text->room - text->length;
		const int wrote = text->data ? vsnprintf(text->data + text->length, left, format, args) : -1;
		va_end(args);
		if (wrote >= 0 && (size_t) wrote < left) {
			text->length += (size_t) wrote;
			return;
		}
		const size_t room = text->room * 2 + (wrote > 0 ? (size_t) wrote : 0) + 4096;
		char* const data = realloc(text->data, room);
		if (!data) {
			text->failed = true;
			return;
		}
		text->data = data;
		text->room = room;
	}
}

// A name of at most size bytes, ended by 0 or the size, its trailing spaces left out, and anything
// but plain letters, figures and signs shown as ?.
static const char* nameOf(const uint8_t* bytes, size_t size) {
	static char name[64];
	size_t length = 0;
	while (length < size && length < sizeof(name) - 1 && bytes[length]) {
		const uint8_t c = bytes[length];
		name[length++] = c >= 0x20 && c < 0x7F ? (char) c : '?';
	}
	while (length && name[length - 1] == ' ') {
		--length;
	}
	name[length] = '\0';
	return name;
}

static unsigned read16(const uint8_t* kept, size_t at) {
	return kept[at] | kept[at + 1] << 8;
}

static unsigned read16Big(const uint8_t* kept, size_t at) {
	return kept[at] << 8 | kept[at + 1];
}

static uint32_t read32(const uint8_t* kept, size_t at) {
	return kept[at] | kept[at + 1] << 8 | kept[at + 2] << 16 | (uint32_t) kept[at + 3] << 24;
}

enum {
	GB_GAMES = 0x8000,
	GB_ENTRY = 18,
	GB_CHOSEN = 0x20,
	GB_ON = 0x10,
	GB_FIRST_CODES = 0x4A,
	GB_FIRST_SLOTS = 4,
	GB_FIRST_RAM = 0x1000,
	GBA_MARK = 0x21000,
	GBA_GAMES = 0x21040,
	GBA_NAME = 20,
	GBA_ON = 0x80,
	MAX_DIRECTORY_A = 0x34000,
	MAX_DIRECTORY_B = 0x3A000,
	MAX_DIRECTORY_SIZE = 0x6000,
	MAX_NUMBER = 0x5FFA,
	MAX_ENTRY = 32,
	MAX_BLOCKS = 0x30000,
	MAX_BLOCK = 0x80,
};

static const size_t gbCodeBanks[] = { 0xA000, 0xC000, 0xE000 };

// A bank's count of entries, or 0 for an erased or impossible one.
static unsigned gbCount(const uint8_t* kept, size_t at) {
	const unsigned count = read16(kept, at);
	return count != 0xFFFF && 2 + count * GB_ENTRY <= 0x2000 ? count : 0;
}

static bool readGbLater(struct Text* text, const uint8_t* kept, size_t size) {
	if (size < 0x10000) {
		return false;
	}
	const unsigned games = gbCount(kept, GB_GAMES);
	if (!games) {
		return false;
	}
	for (unsigned i = 0; i < games; ++i) {
		const uint8_t* const game = kept + GB_GAMES + 2 + i * GB_ENTRY;
		if (game[1] & GB_CHOSEN) {
			add(text, "\nGame chosen last: %s\n", nameOf(game + 2, 16));
		}
	}
	for (unsigned i = 0; i < games; ++i) {
		const uint8_t* const game = kept + GB_GAMES + 2 + i * GB_ENTRY;
		const unsigned id = game[0] | (game[1] & 0x0F) << 8;
		bool named = false;
		for (size_t bank = 0; bank < COUNT(gbCodeBanks); ++bank) {
			const unsigned codes = gbCount(kept, gbCodeBanks[bank]);
			for (unsigned j = 0; j < codes; ++j) {
				const uint8_t* const code = kept + gbCodeBanks[bank] + 2 + j * GB_ENTRY;
				if ((code[0] | (code[1] & 0x0F) << 8) != id) {
					continue;
				}
				if (!named) {
					add(text, "\n%s\n", nameOf(game + 2, 16));
					named = true;
				}
				add(text, "  %-3s  %-12s  %02X%02X%02X%02X\n", code[1] & GB_ON ? "on" : "off", nameOf(code + 2, 12),
					code[14], code[15], code[16], code[17]);
			}
		}
	}
	return true;
}

static bool readGbFirst(struct Text* text, const uint8_t* kept, size_t size) {
	if (size != GB_FIRST_RAM) {
		return false;
	}
	add(text, "\n");
	for (unsigned i = 0; i < GB_FIRST_SLOTS; ++i) {
		const uint8_t* const code = kept + GB_FIRST_CODES + i * 4;
		if (code[0] | code[1] | code[2] | code[3]) {
			add(text, "Code %u  %02X%02X%02X%02X\n", i + 1, code[0], code[1], code[2], code[3]);
		} else {
			add(text, "Code %u  (empty)\n", i + 1);
		}
	}
	return true;
}

// One game's codes from at, as the Game Boy Advance cartridges keep them, up to end; returns
// where they end, or 0 if they don't fit.
static size_t gbaGame(struct Text* text, const uint8_t* kept, size_t at, size_t end, const char* name) {
	if (at + 4 > end) {
		return 0;
	}
	const uint32_t codes = read32(kept, at);
	at += 4;
	if (codes > 10000) {
		return 0;
	}
	if (codes) {
		add(text, "\n%s\n", name);
	}
	for (uint32_t i = 0; i < codes; ++i) {
		if (at + 4 + GBA_NAME > end) {
			return 0;
		}
		const uint32_t head = read32(kept, at);
		const size_t words = head & 0xFFFFFF;
		if (words > (end - at - 4 - GBA_NAME) / 4) {
			return 0;
		}
		add(text, "  %-3s  %-20s", head >> 24 & GBA_ON ? "on" : "off", nameOf(kept + at + 4, GBA_NAME));
		const size_t first = at + 4 + GBA_NAME;
		for (size_t w = 0; w < words; ++w) {
			if (w && w % 2 == 0) {
				add(text, "\n  %-3s  %-20s", "", "");
			}
			add(text, w % 2 ? " %08X" : "  %08X", (unsigned) read32(kept, first + w * 4));
		}
		add(text, "\n");
		at = first + words * 4;
	}
	return at;
}

static bool readGba(struct Text* text, const uint8_t* kept, size_t size) {
	if (size < GBA_GAMES + 4 || memcmp(kept + GBA_MARK, "GBA_", 4)) {
		return false;
	}
	const uint32_t games = read32(kept, GBA_GAMES);
	if (games > 10000) {
		return false;
	}
	size_t at = GBA_GAMES + 4;
	for (uint32_t i = 0; i < games; ++i) {
		if (at + GBA_NAME > size) {
			break;
		}
		char name[GBA_NAME + 1];
		snprintf(name, sizeof(name), "%s", nameOf(kept + at, GBA_NAME));
		at = gbaGame(text, kept, at + GBA_NAME, size, name);
		if (!at) {
			break;
		}
	}
	if (!games) {
		add(text, "\nNo codes.\n");
	}
	return true;
}

static bool readMax(struct Text* text, const uint8_t* kept, size_t size) {
	if (size < 0x80000) {
		return false;
	}
	// Each directory ends FFFF and its number; an erased one is all FFFF.
	const unsigned a = read16Big(kept, MAX_DIRECTORY_A + MAX_NUMBER - 2) == 0xFFFF ? read16Big(kept, MAX_DIRECTORY_A + MAX_NUMBER) : 0xFFFF;
	const unsigned b = read16Big(kept, MAX_DIRECTORY_B + MAX_NUMBER - 2) == 0xFFFF ? read16Big(kept, MAX_DIRECTORY_B + MAX_NUMBER) : 0xFFFF;
	if (a == 0xFFFF && b == 0xFFFF) {
		return false;
	}
	const size_t directory = b == 0xFFFF || (a != 0xFFFF && a > b) ? MAX_DIRECTORY_A : MAX_DIRECTORY_B;
	for (size_t entry = directory; entry + MAX_ENTRY <= directory + MAX_NUMBER; entry += MAX_ENTRY) {
		if (kept[entry] == 0xFF) {
			continue;
		}
		char name[MAX_ENTRY];
		snprintf(name, sizeof(name), "%s", nameOf(kept + entry + 4, 0x1B - 4));
		const size_t at = MAX_BLOCKS + (size_t) read16Big(kept, entry + 0x1C) * MAX_BLOCK;
		if (at < size) {
			gbaGame(text, kept, at, size, name);
		}
	}
	return true;
}

void replayCodesWrite(const char* file, const char* title, const uint8_t* kept, size_t size, bool gameBoy) {
	if (!file || !*file || !kept) {
		return;
	}
	struct Text text = { NULL, 0, 0, false };
	add(&text, "%s\n", title);
	add(&text, "The codes this Action Replay holds, as its menus show them. armGBA writes this file as they\n"
	           "change; editing it changes nothing.\n");
	const bool read = gameBoy ? readGbLater(&text, kept, size) || readGbFirst(&text, kept, size)
	                          : readGba(&text, kept, size) || readMax(&text, kept, size);
	if (read && !text.failed) {
		writeWholeFile(file, text.data, text.length);
	}
	free(text.data);
}
