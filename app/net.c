/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Netplay's and RetroAchievements' rules for the Game Boy consoles.

#include "hooks.h"
#include "link.h"
#include "rc_consoles.h"

#include <string.h>

// An option as the session has it: a host running armGBA's firmware, its system folder having no
// Nintendo BIOS or boot ROM, has the players run armGBA's too.
const char* netSessionValue(const char* key, const char* value) {
	const char* firmware;
	bool const own = !libretroFirmwareCrc(&firmware);
	if (own && ((!strcmp(key, "armgba_bios") && !strcmp(value, "nintendo")) || (!strcmp(key, "armgba_gb_boot_rom") && !strcmp(value, "enabled")))) {
		return "open-source";
	}
	return value;
}

// The console RetroAchievements files the game under, from its header.
uint32_t netCheevosConsole(const uint8_t* data, size_t size) {
	if (size >= 0xC0 && data[0xB2] == 0x96) {
		return RC_CONSOLE_GAMEBOY_ADVANCE;
	}
	if (size >= 0x150 && data[0x104] == 0xCE && data[0x105] == 0xED) {
		return data[0x143] & 0x80 ? RC_CONSOLE_GAMEBOY_COLOR : RC_CONSOLE_GAMEBOY;
	}
	return RC_CONSOLE_UNKNOWN;
}
