/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// LCD Rendering: the LCD effects on the GPU (lcd-gl.c) or by the core on the CPU, their rows in the
// menus and their settings.

#include "hooks.h"
#include "lcd-gl.h"
#include "ui.h"
#include "gb.h"
#include "lcd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Where the GPU's Game Boy pixels cast their shadow: 0 nowhere, then up and on clockwise, then
// SHADOW_TILT, the way the device tilts.
#define SHADOW_TILT 9

// LCD Rendering: the LCD effects drawn by the core on the CPU, or all of them here on the GPU, never
// some of each; the GPU's grid, and its pixel shadow.
static struct {
	bool onGpu;
	bool grid;
	int shadow;
} lcdSettings;

// The effects are on the GPU when they're set to be and the renderer can draw them; otherwise the
// core draws them.
static bool gpuOn(void) {
	return lcdSettings.onGpu && lcdGlUsable();
}

// How many times larger the core makes the picture, by LCD Scale or Fit Screen; 1 with the effects on
// the GPU.
float lcdCoreScale(void) {
	return gpuOn() || libretroLcdShownScale < 1 ? 1 : libretroLcdShownScale;
}

// Tilt: the shadow falls the way gravity pulls across the screen, its Distance with the screen
// upright and none with it lying flat, from the accelerometer (open only while it's used), smoothed.
static int accelerometerIndex = -1;
static SDL_Sensor* accelerometer;
static float tiltX, tiltY;

static bool tiltAvailable(void) {
	return accelerometerIndex >= 0;
}

void lcdStart(void) {
#ifdef __ANDROID__
	if (SDL_InitSubSystem(SDL_INIT_SENSOR) != 0) {
		return;
	}
	SDL_EventState(SDL_SENSORUPDATE, SDL_IGNORE);
	for (int i = 0; i < SDL_NumSensors(); ++i) {
		if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) {
			accelerometerIndex = i;
			return;
		}
	}
#endif
}

static void followTilt(bool on) {
	if (!on || accelerometerIndex < 0) {
		if (accelerometer) {
			SDL_SensorClose(accelerometer);
			accelerometer = NULL;
		}
		return;
	}
	if (!accelerometer && !(accelerometer = SDL_SensorOpen(accelerometerIndex))) {
		return;
	}
	// Its events are ignored, which stops SDL updating it, so it's updated here.
	SDL_SensorUpdate();
	float a[3];
	if (SDL_SensorGetData(accelerometer, a, 3) != 0) {
		return;
	}
	// The accelerometer's axes are the device's own, x right and y up as it's naturally held, and it
	// reads the push against gravity; down across the screen as the display's turned (SDL names
	// Android's rotations 0, 90, 180 and 270 portrait, landscape, portrait flipped, landscape flipped).
	float x, y;
	switch (SDL_GetDisplayOrientation(0)) {
	case SDL_ORIENTATION_LANDSCAPE:
		x = a[1], y = a[0];
		break;
	case SDL_ORIENTATION_PORTRAIT_FLIPPED:
		x = a[0], y = -a[1];
		break;
	case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
		x = -a[1], y = -a[0];
		break;
	default:
		x = -a[0], y = a[1];
		break;
	}
	float const reach = libretroLcdShadowDistance;
	x *= reach / SDL_STANDARD_GRAVITY;
	y *= reach / SDL_STANDARD_GRAVITY;
	float const length = sqrtf(x * x + y * y);
	if (length > reach) {
		x *= reach / length;
		y *= reach / length;
	}
	tiltX += (x - tiltX) * 0.1f;
	tiltY += (y - tiltY) * 0.1f;
}

// The gap color that keeps the screen's brightness under the grid at this size (lcd.h's
// libretroLcdKeep), from how much gap each output pixel shows, as an alpha blended in linear light.
static uint32_t keptGap(enum LibretroLcd lcd, uint32_t off, int w, int h) {
	static struct LibretroLcdKeep keep;
	static int keptWidth, keptHeight, keptLcd = -1;
	if (w != keptWidth || h != keptHeight || (int) lcd != keptLcd) {
		keptWidth = w;
		keptHeight = h;
		keptLcd = (int) lcd;
		uint8_t linear[256];
		for (int a = 0; a < 256; ++a) {
			linear[a] = (uint8_t) lroundf((1 - powf(1 - a / 255.0f, 1 / 2.2f)) * 255);
		}
		const struct LibretroLcdLook* const look = &libretroLcdLooks[lcd];
		float const pitchX = (float) w / app.gameWidth, pitchY = (float) h / app.gameHeight;
		float const fade = libretroLcdFade(fminf(pitchX, pitchY));
		int* const across = malloc(sizeof(int) * (size_t) w);
		if (!across) {
			return off;
		}
		for (int x = 0; x < w; ++x) {
			across[x] = (int) lroundf(libretroLcdEdgeGap(x, pitchX, look->share * pitchX) * look->across * fade * 255);
		}
		memset(keep.weights, 0, sizeof(keep.weights));
		for (int y = 0; y < h; ++y) {
			int const down = (int) lroundf(libretroLcdEdgeGap(y, pitchY, look->share * pitchY) * look->down * fade * 255);
			for (int x = 0; x < w; ++x) {
				++keep.weights[linear[across[x] + down - (across[x] * down + 127) / 255]];
			}
		}
		free(across);
		for (int a = 0; a < 256; ++a) {
			keep.shares[a] = a / 255.0f;
		}
		keep.count = 256;
		keep.stale = true;
	}
	return libretroLcdKeep(&keep) ? keep.gap : off;
}

// What the GPU draws: the core's LCD Ghosting, Interlaced Rows, LCD Subpixels and Pixel Shadow
// Distance settings, with the GPU's grid and pixel shadow, for the screen loaded (lcd.h has its values).
static void fillLook(struct LcdGlLook* look, SDL_Rect to) {
	memset(look, 0, sizeof(*look));
	uint32_t off = 0;
	enum LibretroLcd const lcd = libretroLcd(&off);
	bool const tft = libretroLcdHasSubpixels(lcd);
	struct Option* const ghosting = optionFind("armgba_lcd_ghosting");
	static const char* const levels[] = { "25", "half", "75", "enabled" };
	for (int i = 0; ghosting && i < 4; ++i) {
		look->ghosting = !strcmp(ghosting->value, levels[i]) ? i + 1 : look->ghosting;
	}
	struct Option* const interlaced = optionFind("armgba_lcd_interleave");
	look->interlaced = tft && interlaced && !strcmp(interlaced->value, "enabled");
	// The times to darken and to lighten, in milliseconds, as lcd.c's screenTimes.
	look->darken = lcd == LIBRETRO_LCD_DMG ? 50 : lcd == LIBRETRO_LCD_POCKET ? 25 : 15;
	look->lighten = lcd == LIBRETRO_LCD_DMG ? 110 : lcd == LIBRETRO_LCD_POCKET ? 50 : 30;
	look->lead = lcd == LIBRETRO_LCD_ADVANCE ? 0.8f : 0.75f;
	look->gaps = lcdSettings.grid;
	look->share = libretroLcdLooks[lcd].share;
	look->across = libretroLcdLooks[lcd].across;
	look->down = libretroLcdLooks[lcd].down;
	look->gap = look->gaps ? keptGap(lcd, off, to.w, to.h) : off;
	look->unlit = off;
	look->subpixels = look->gaps && tft ? libretroLcdSubpixelMix : 0;
	look->bgr = lcd == LIBRETRO_LCD_ADVANCE;
	look->smooth = app.gameSmooth;
	bool const shadowed = look->gaps && lcdSettings.shadow && app.format == RETRO_PIXEL_FORMAT_XRGB8888 && libretroLcdShadow();
	followTilt(shadowed && lcdSettings.shadow == SHADOW_TILT);
	if (shadowed) {
		// A pixel's darkness as lcd.h has it: (start - luminance) * scale >> 16 of 255.
		int start, scale;
		libretroLcdShadowFrom(off, libretroGbDarkestColor(), &start, &scale);
		look->shadowStart = start / 65535.0f;
		look->shadowScale = scale * (65535.0f / 65536 / 255);
		look->shadowX = tiltX;
		look->shadowY = tiltY;
		if (lcdSettings.shadow != SHADOW_TILT) {
			float const reach = libretroLcdReach(lcdSettings.shadow - 1);
			look->shadowX = libretroLcdWays[lcdSettings.shadow - 1][0] * reach;
			look->shadowY = libretroLcdWays[lcdSettings.shadow - 1][1] * reach;
		}
	}
}

bool lcdPictureCame(void) {
	lcdGlNewPicture();
	return false;
}

void lcdPictureUpload(void) {
	SDL_UpdateTexture(app.game, NULL, app.picture, (int) app.picturePitch);
}

void lcdPictureDraw(SDL_Rect to) {
	if (gpuOn()) {
		struct LcdGlLook look;
		fillLook(&look, to);
		if ((look.ghosting || look.interlaced || look.gaps) && lcdGlDraw(to, &look)) {
			return;
		}
	} else {
		followTilt(false);
	}
	SDL_RenderCopy(app.renderer, app.game, NULL, &to);
}

void lcdRendererGoing(void) {
	lcdGlStop();
}

// With the effects on the GPU, the core draws none of them, whatever they're set to.
const char* lcdOptionRead(const struct Option* option) {
	return gpuOn() && !strcmp(option->key, "armgba_lcd_effects") ? "disabled" : NULL;
}

// With the effects on the GPU, the core's own rows go (CPU LCD Effects, LCD Scale, Pixel Gaps, its
// pixel shadow and LCD Effect Threads), and the settings the GPU shares show as the screen has them:
// LCD Ghosting; Interlaced Rows on the Color's and Advance's screens, LCD Subpixels there with the grid
// on; Pixel Shadow Distance with the GPU's shadow on.
bool lcdOptionShown(const struct Option* option, bool shown) {
	if (!gpuOn()) {
		return shown;
	}
	uint32_t gap;
	bool const tft = libretroLcdHasSubpixels(libretroLcd(&gap));
	static const char* const cpuOnly[] = { "armgba_lcd_effects", "armgba_lcd_grid", "armgba_lcd_gaps", "armgba_lcd_shadow", "armgba_lcd_threads" };
	for (size_t i = 0; i < sizeof(cpuOnly) / sizeof(*cpuOnly); ++i) {
		if (!strcmp(option->key, cpuOnly[i])) {
			return false;
		}
	}
	if (!strcmp(option->key, "armgba_lcd_ghosting")) {
		return true;
	}
	if (!strcmp(option->key, "armgba_lcd_interleave")) {
		return tft;
	}
	if (!strcmp(option->key, "armgba_lcd_shadow_distance")) {
		return lcdSettings.grid && lcdSettings.shadow && libretroLcdShadow();
	}
	if (!strcmp(option->key, "armgba_lcd_subpixels")) {
		return lcdSettings.grid && tft;
	}
	return shown;
}

enum {
	SET_LCD_RENDERING,
	SET_GPU_GRID,
	SET_SHADOW,
};

static void changeLcd(int which, int delta) {
	int const d = delta ? delta : 1;
	switch (which) {
	// The core reads its effects again, now off or back as they're set.
	case SET_LCD_RENDERING:
		lcdSettings.onGpu = !lcdSettings.onGpu;
		optionsChanged = true;
		refreshShownOptions();
		break;
	case SET_GPU_GRID:
		lcdSettings.grid = !lcdSettings.grid;
		break;
	// Off, then the eight directions.
	case SET_SHADOW:
		lcdSettings.shadow = wrap(lcdSettings.shadow, d, tiltAvailable() ? SHADOW_TILT + 1 : SHADOW_TILT);
		break;
	}
	settingsWrite();
}

// LCD Rendering, the same row on Core > Video and Shaders: whether the core draws the LCD effects on the
// CPU or the app on the GPU. Without a GL renderer, only the CPU can.
static void addLcdRendering(void) {
	addValue("LCD Rendering", gpuOn() ? "GPU" : "CPU", changeLcd, SET_LCD_RENDERING);
	helpFor(lcdGlUsable() ? "Who draws the LCD effects: the CPU (Core \xE2\x80\xBA Video) or the GPU, with its grid on Shaders."
	                      : "Who draws the LCD effects: the CPU, as this renderer can't draw them on the GPU.");
}

// LCD Effects starts with LCD Rendering, which Shaders shows too.
void lcdGroupRows(const char* category) {
	if (!strcmp(category, "video_lcd")) {
		addLcdRendering();
	}
}

// Shaders: the LCD grid and pixel shadow only with LCD Rendering on the GPU, the shadow only on the
// first Game Boy's and the Pocket's screens, or before a game is loaded.
void lcdShaderRows(void) {
	addLcdRendering();
	if (!gpuOn()) {
		return;
	}
	addValue("LCD Grid", onOff(lcdSettings.grid), changeLcd, SET_GPU_GRID);
	helpFor("The gaps between the screen's pixels, at any size; fainter when smaller.");
	if (lcdSettings.grid && (!app.loaded || libretroLcdShadow())) {
		static const char* const shadows[SHADOW_TILT + 1] = { "Off", "Up", "Up Right", "Right", "Down Right", "Down", "Down Left", "Left", "Up Left", "Tilt" };
		addValue("Game Boy Pixel Shadow", shadows[lcdSettings.shadow], changeLcd, SET_SHADOW);
		helpFor(tiltAvailable() ? "Where the Game Boy's pixel shadow falls; Tilt moves it as the device tilts."
		                        : "Where the Game Boy's pixel shadow falls.");
	}
}

void lcdSettingsDefaults(void) {
	lcdSettings.onGpu = true;
	lcdSettings.grid = false;
	lcdSettings.shadow = 0;
}

bool lcdSettingRead(const char* key, const char* value, int n, bool yes) {
	// The GPU LCD grid was once the third filter; the filter is the app's still.
	if (!strcmp(key, "filter")) {
		if (n == 2) {
			lcdSettings.grid = true;
		}
		return false;
	} else if (!strcmp(key, "lcd_rendering")) {
		lcdSettings.onGpu = strcmp(value, "cpu") != 0;
	} else if (!strcmp(key, "gpu_lcd_grid")) {
		lcdSettings.grid = yes;
	} else if (!strcmp(key, "lcd_shadow")) {
		lcdSettings.shadow = n >= 0 && n <= SHADOW_TILT ? n : 0;
	} else {
		return false;
	}
	return true;
}

void lcdSettingsWrite(FILE* f) {
	fprintf(f, "lcd_rendering = %s\ngpu_lcd_grid = %s\nlcd_shadow = %d\n", lcdSettings.onGpu ? "gpu" : "cpu", lcdSettings.grid ? "true" : "false", lcdSettings.shadow);
}
