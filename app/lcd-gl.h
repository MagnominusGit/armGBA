/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_APP_LCD_GL_H
#define ARMGBA_APP_LCD_GL_H

// The LCD effects drawn on the GPU (lcd-gl.c).

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

// What to draw: LCD Ghosting (0 off, then 1 to 4 quarters) with the screen's times to darken and to
// lighten in milliseconds, and Interlaced Rows with the driven rows' share of those times; the grid's
// gaps (their width as a share of a pixel, how much shows across and down) in their color, which
// keeps the screen's brightness; the unlit screen's color; the pixel shadow (scale 0 for none), where
// it starts and how dark it gets per luminance, cast x and y dots away; LCD Subpixels' mix, red first
// unless bgr; and the filter (frontend.h's) the picture is drawn with where nothing else is.
struct LcdGlLook {
	int ghosting;
	bool interlaced;
	float darken, lighten, lead;
	bool gaps;
	float share, across, down;
	uint32_t gap, unlit;
	float shadowStart, shadowScale, shadowX, shadowY;
	float subpixels;
	bool bgr;
	int smooth;
};

// Whether the renderer draws with GL, so the effects can run, and they haven't failed on it.
bool lcdGlUsable(void);
// A new picture from the core, faded in when it's next drawn.
void lcdGlNewPicture(void);
// Draws the picture with the effects at to; false if it couldn't, and it's then drawn plainly.
bool lcdGlDraw(SDL_Rect to, const struct LcdGlLook* look);
// The renderer is going: everything made on it goes too.
void lcdGlStop(void);

#endif
