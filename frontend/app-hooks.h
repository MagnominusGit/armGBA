/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// What a core's own app adds to the shared one, where libretro's interface can't say it: its own
// ways of drawing the picture, rows in the menus and settings. Each may be NULL; the app then does
// what it does for any core.

#ifndef ARMGBA_FRONTEND_APP_HOOKS_H
#define ARMGBA_FRONTEND_APP_HOOKS_H

#include "frontend.h"

#include <stdio.h>

struct AppHooks {
	// Once, before the window opens.
	void (*start)(void);
	// The core gave a new picture (in app.picture): true when the hook puts it in the game's texture
	// itself, as it draws.
	bool (*pictureCame)(void);
	// The game's texture was made again (the filter changed): the picture goes into it.
	void (*pictureUpload)(void);
	// The game's picture drawn into to, each frame shown.
	void (*pictureDraw)(SDL_Rect to);
	// The renderer is about to be made again: textures made with it go first.
	void (*rendererGoing)(void);
	// How many times larger the core makes its picture (its own effects); the window is sized for
	// the console's screen.
	int (*coreScale)(void);
	// A core option's value as the core reads it; NULL keeps the option's own.
	const char* (*optionRead)(const struct Option* option);
	// Whether a core option's row shows, given whether the core shows it.
	bool (*optionShown)(const struct Option* option, bool shown);
	// Rows at the start of a group of core options (by its category key), and on General > Shaders
	// after the filter.
	void (*groupRows)(const char* category);
	void (*shaderRows)(void);
	// The app's own settings in frontend.cfg: their defaults (at the start and on Reset All Settings),
	// a line read (true when it was the hook's alone), and its lines, written after the filter's.
	void (*settingsDefaults)(void);
	bool (*settingRead)(const char* key, const char* value, int n, bool yes);
	void (*settingsWrite)(FILE* f);
	// Netplay: this device's player (-1 when not in a session), the firmware every device running
	// every console must match (a CRC, 0 for none, and its name), an option's value as the session
	// has it, a joining player's save taken in (why not, or NULL), and a joined session ended, the
	// game playing on or not.
	void (*netplayPlayer)(int player);
	uint32_t (*firmwareCrc)(const char** name);
	const char* (*sessionValue)(const char* key, const char* value);
	const char* (*takeSave)(int player, const void* save, size_t size, const void* rtc, size_t rtcSize);
	void (*sessionEnded)(void);
	void (*playOn)(void);
	// Save Manager's folders: save folders, game folders, and whether the game folders come first.
	void (*saveManagerFolders)(const char* const* saves, int saveCount, const char* const* games, int gameCount, bool gamesFirst);
	// The console RetroAchievements files a game under (rc_consoles.h), 0 when unknown.
	uint32_t (*cheevosConsole)(const uint8_t* data, size_t size);
};

extern const struct AppHooks appHooks;

#endif
