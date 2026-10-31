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
};

extern const struct AppHooks appHooks;

#endif
