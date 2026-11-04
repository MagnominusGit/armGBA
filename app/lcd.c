/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Game Boy screens' LCD grid and pixel shadow drawn on the GPU over the game's picture, their rows
// in the menus and their settings.

#include "hooks.h"
#include "ui.h"
#include "gb.h"
#include "lcd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Where the GPU grid's Game Boy pixels cast their shadow: 0 nowhere, then up and on clockwise, then
// SHADOW_TILT, the way the device tilts.
#define SHADOW_TILT 9

// LCD Rendering: the LCD grid and Game Boy pixel shadow drawn here on the GPU, or by the core on the
// CPU; never both.
static struct {
	bool onGpu;
	bool grid;
	int shadow;
} lcdSettings;

static SDL_Texture* overlay;
// For pixels whose own gaps differ (lcd.h's libretroLcdKeep): the overlay's alphas as grays and the
// rest of each pixel's, to multiply by, a picture of each pixel's gap color, and where its gaps are
// made at the size shown.
static SDL_Texture *gapMask, *insideMask, *gapLayer, *gapsPicture;
// LCD Subpixels: each column's factor for red, green and blue, multiplied in over the picture.
static SDL_Texture* stripes;
static int overlayWidth, overlayHeight, overlayShownWidth, overlayShownHeight, overlayLcd = -1;
static float overlayMix;
// The overlay's shares of gap, for keeping a Game Boy screen's brightness under it; whether the game's
// texture holds the picture darkened through its tables, from which solve, and whether a newer
// picture waits.
static struct LibretroLcdKeep overlayKeep;
static bool gameKept, gameKeptStale;
static unsigned gameKeptSolved;
static uint8_t *keptPicture, *keptGaps;
static size_t keptSize;
static SDL_Texture* shadow;
static bool shadowStale;
// The core's LCD grid: how many times larger it makes the picture, in halves, 1 when it's off or LCD
// Rendering is on the GPU.
float lcdCoreScale(void) {
	if (lcdSettings.onGpu) {
		return 1;
	}
	struct Option* const o = optionFind("armgba_lcd_grid");
	float const scale = o ? atoi(o->value) + (strchr(o->value, '.') ? 0.5f : 0) : 0;
	return scale > 1 ? scale : 1;
}

// The GPU LCD grid: the gaps between the console screen's pixels, drawn at the size the picture is
// shown, each spread over the two output pixels nearest it (lcd.h's libretroLcdGapSpread) and faded
// at small sizes (libretroLcdFade). The first Game Boy's and the Pocket's show the screen's color
// while off there, the Color's and Advance's a black matrix; lcd.h has each screen's values. It's
// blended as in linear light: over black, color c becomes c * (1 - a)^(1 / 2.2), which an ordinary
// blend gives with alpha 1 - (1 - a)^(1 / 2.2); the Game Boy's gaps, near its pixels' colors, take
// the same. It is white, tinted the gap's color as it's drawn, and how many of its pixels take each
// alpha goes to overlayKeep.
static void dropOverlay(void) {
	SDL_Texture** const all[] = { &overlay, &gapMask, &insideMask, &gapLayer, &stripes };
	for (size_t i = 0; i < sizeof(all) / sizeof(*all); ++i) {
		if (*all[i]) {
			SDL_DestroyTexture(*all[i]);
			*all[i] = NULL;
		}
	}
}

static SDL_Texture* makeMask(const uint32_t* pixels, int w, int h, SDL_BlendMode mode) {
	SDL_Texture* const t = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
	if (t) {
		SDL_UpdateTexture(t, NULL, pixels, w * 4);
		SDL_SetTextureBlendMode(t, mode);
		SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
	}
	return t;
}

static void makeOverlay(SDL_Rect to) {
	uint32_t gap = 0;
	enum LibretroLcd const lcd = libretroLcd(&gap);
	bool const grid = lcdSettings.onGpu && lcdSettings.grid;
	float const pitchX = app.gameWidth ? (float) to.w / app.gameWidth : 0;
	float const mix = libretroLcdHasSubpixels(lcd) ? libretroLcdSubpixelMix * libretroLcdSubpixelFade(pitchX) : 0;
	if (overlay && grid && overlayWidth == app.gameWidth && overlayHeight == app.gameHeight && overlayShownWidth == to.w &&
	    overlayShownHeight == to.h && overlayLcd == (int) lcd && overlayMix == mix) {
		return;
	}
	dropOverlay();
	overlayWidth = app.gameWidth;
	overlayHeight = app.gameHeight;
	overlayShownWidth = to.w;
	overlayShownHeight = to.h;
	overlayLcd = (int) lcd;
	overlayMix = mix;
	if (!grid || !app.gameWidth || to.w <= 0 || to.h <= 0) {
		return;
	}
	int const w = to.w, h = to.h;
	uint32_t* pixels = calloc((size_t) w * h, 4);
	int* across = calloc((size_t) w, sizeof(int));
	if (!pixels || !across) {
		free(pixels);
		free(across);
		return;
	}
	uint8_t linear[256];
	for (int a = 0; a < 256; ++a) {
		linear[a] = (uint8_t) lroundf((1 - powf(1 - a / 255.0f, 1 / 2.2f)) * 255);
	}
	const struct LibretroLcdLook* const look = &libretroLcdLooks[lcd];
	// Each column's and row's share of gap as an alpha, 0 to 255; a pixel takes either's.
	float const pitchY = (float) h / app.gameHeight;
	float const fade = libretroLcdFade(fminf(pitchX, pitchY));
	for (int x = 0; x < w; ++x) {
		across[x] = (int) lroundf(libretroLcdGapSpread(x, pitchX, look->share * pitchX) * look->across * fade * 255);
	}
	memset(overlayKeep.weights, 0, sizeof(overlayKeep.weights));
	for (int y = 0; y < h; ++y) {
		int const down = (int) lroundf(libretroLcdGapSpread(y, pitchY, look->share * pitchY) * look->down * fade * 255);
		uint32_t* const row = pixels + (size_t) y * w;
		for (int x = 0; x < w; ++x) {
			uint8_t const alpha = linear[across[x] + down - (across[x] * down + 127) / 255];
			row[x] = (uint32_t) alpha << 24 | 0xFFFFFF;
			++overlayKeep.weights[alpha];
		}
	}
	for (int a = 0; a < 256; ++a) {
		overlayKeep.shares[a] = a / 255.0f;
	}
	overlayKeep.count = 256;
	overlayKeep.stale = true;
	overlay = makeMask(pixels, w, h, SDL_BLENDMODE_BLEND);
	for (size_t i = 0; i < (size_t) w * h; ++i) {
		pixels[i] = 0xFF000000 | (pixels[i] >> 24) * 0x010101;
	}
	gapMask = makeMask(pixels, w, h, SDL_BLENDMODE_MOD);
	for (size_t i = 0; i < (size_t) w * h; ++i) {
		pixels[i] ^= 0xFFFFFF;
	}
	insideMask = makeMask(pixels, w, h, SDL_BLENDMODE_MOD);
	gapLayer = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
	if (gapLayer) {
		SDL_SetTextureBlendMode(gapLayer, SDL_BLENDMODE_ADD);
	}
	if (mix > 0) {
		// One row, stretched down the picture; each factor in linear light, gamma encoded to multiply by.
		// Each pixel's stripes across the columns whose middles fall in it, as bands
		// (libretroLcdBandShare), as many as the narrowest pixel's columns.
		int* const first = across;
		for (int x = 0; x < w; ++x) {
			first[x] = x > 0 && (int) ((x + 0.5f) / pitchX) == (int) ((x - 0.5f) / pitchX) ? first[x - 1] : x;
		}
		// The narrowest pixel, leaving out the picture's first and last, which rounding may trim.
		int bands = (int) pitchX;
		for (int x = 0; x + 1 < w; ++x) {
			if (first[x + 1] != first[x] && first[x]) {
				bands = x - first[x] + 1 < bands ? x - first[x] + 1 : bands;
			}
		}
		bands = bands > 0 ? bands : 1;
		for (int x = 0; x < w; ++x) {
			int last = x;
			while (last + 1 < w && first[last + 1] == first[x]) {
				++last;
			}
			float const taken = (float) (last - first[x] + 1), from = (x - first[x]) / taken, to = (x - first[x] + 1) / taken;
			uint32_t c = 0xFF000000;
			for (int k = 0; k < 3; ++k) {
				float const share = libretroLcdBandShare(from, to, bands, libretroLcdStripeOf(lcd, k));
				c |= (uint32_t) lroundf(powf(libretroLcdStripeFactor(share, mix), 1 / 2.2f) * 255) << (16 - 8 * k);
			}
			pixels[x] = c;
		}
		stripes = makeMask(pixels, w, 1, SDL_BLENDMODE_MOD);
	}
	free(pixels);
	free(across);
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

// The shadow the first Game Boy's and the Pocket's pixels cast on the reflector behind them, the way
// the setting points (lcd.h has how far and how dark): a texture a dot to each of the picture's
// pixels, remade when the picture changes, drawn scaled smoothly over the picture.
static void drawShadow(SDL_Rect to, uint32_t lit) {
	if (!shadow || shadowStale) {
		int w = 0, h = 0;
		if (shadow) {
			SDL_QueryTexture(shadow, NULL, NULL, &w, &h);
		}
		if (!shadow || w != app.gameWidth || h != app.gameHeight) {
			if (shadow) {
				SDL_DestroyTexture(shadow);
			}
			shadow = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, app.gameWidth, app.gameHeight);
			if (!shadow) {
				return;
			}
			SDL_SetTextureBlendMode(shadow, SDL_BLENDMODE_BLEND);
			SDL_SetTextureScaleMode(shadow, SDL_ScaleModeLinear);
		}
		void* pixels;
		int pitch;
		if (SDL_LockTexture(shadow, NULL, &pixels, &pitch) != 0) {
			return;
		}
		// A pixel's darkness as an alpha (lcd.h). Game Boy pictures, the only ones with a shadow, are
		// XRGB8888.
		int start, scale;
		libretroLcdShadowFrom(lit, libretroGbDarkestColor(), &start, &scale);
		for (int y = 0; y < app.gameHeight; ++y) {
			const uint32_t* const row = (const uint32_t*) (app.picture + y * app.picturePitch);
			uint32_t* const out = (uint32_t*) ((uint8_t*) pixels + y * pitch);
			for (int x = 0; x < app.gameWidth; ++x) {
				uint32_t const c = row[x];
				int const level = libretroLcdLuminanceOf(c);
				int const dark = level < start ? (int) ((int64_t) (start - level) * scale >> 16) : 0;
				out[x] = (uint32_t) (dark < 255 ? dark : 255) << 24;
			}
		}
		SDL_UnlockTexture(shadow);
		shadowStale = false;
	}
	// Up, then on clockwise, or the tilt's way.
	float dx = tiltX, dy = tiltY;
	if (lcdSettings.shadow != SHADOW_TILT) {
		float const reach = libretroLcdReach(lcdSettings.shadow - 1);
		dx = libretroLcdWays[lcdSettings.shadow - 1][0] * reach;
		dy = libretroLcdWays[lcdSettings.shadow - 1][1] * reach;
	}
	SDL_Rect const cast = { to.x + (int) lroundf(dx * to.w / app.gameWidth), to.y + (int) lroundf(dy * to.h / app.gameHeight), to.w, to.h };
	SDL_RenderSetClipRect(app.renderer, &to);
	SDL_RenderCopy(app.renderer, shadow, NULL, &cast);
	SDL_RenderSetClipRect(app.renderer, NULL);
}

// The game's texture from the picture, darkened through overlayKeep's tables when keeping.
static void uploadGame(bool keeping) {
	const uint8_t* data = app.picture;
	size_t const size = app.picturePitch * (size_t) app.gameHeight;
	if (keeping && size > keptSize) {
		uint8_t* const more = realloc(keptPicture, size);
		keptPicture = more ? more : keptPicture;
		uint8_t* const moreGaps = realloc(keptGaps, size);
		keptGaps = moreGaps ? moreGaps : keptGaps;
		keeping = more && moreGaps;
		keptSize = keeping ? size : keptSize;
	}
	bool const wide = app.format == RETRO_PIXEL_FORMAT_XRGB8888;
	if (keeping && !wide) {
		// RGB565, through the tables for it.
		const uint16_t(*const inside)[64] = overlayKeep.table565[0];
		const uint16_t(*const around)[64] = overlayKeep.table565[1];
		for (size_t i = 0; i < size; i += 2) {
			unsigned const c = *(const uint16_t*) (data + i), r = c >> 11, g = c >> 5 & 0x3F, b = c & 0x1F;
			*(uint16_t*) (keptPicture + i) = (uint16_t) (inside[0][r] | inside[1][g] | inside[2][b]);
			*(uint16_t*) (keptGaps + i) = (uint16_t) (around[0][r] | around[1][g] | around[2][b]);
		}
	} else if (keeping) {
		// ARGB8888, blue first in memory.
		for (size_t i = 0; i < size; i += 4) {
			keptPicture[i] = overlayKeep.table[2][data[i]];
			keptPicture[i + 1] = overlayKeep.table[1][data[i + 1]];
			keptPicture[i + 2] = overlayKeep.table[0][data[i + 2]];
			keptPicture[i + 3] = data[i + 3];
			keptGaps[i] = overlayKeep.gaps[2][data[i]];
			keptGaps[i + 1] = overlayKeep.gaps[1][data[i + 1]];
			keptGaps[i + 2] = overlayKeep.gaps[0][data[i + 2]];
			keptGaps[i + 3] = 0xFF;
		}
	}
	if (keeping) {
		Uint32 const format = wide ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB565;
		Uint32 now = 0;
		int w = 0, h = 0;
		if (gapsPicture) {
			SDL_QueryTexture(gapsPicture, &now, NULL, &w, &h);
		}
		if (!gapsPicture || now != format || w != app.gameWidth || h != app.gameHeight) {
			if (gapsPicture) {
				SDL_DestroyTexture(gapsPicture);
			}
			gapsPicture = SDL_CreateTexture(app.renderer, format, SDL_TEXTUREACCESS_STREAMING, app.gameWidth, app.gameHeight);
		}
		if (gapsPicture) {
			SDL_SetTextureBlendMode(gapsPicture, SDL_BLENDMODE_NONE);
			SDL_SetTextureScaleMode(gapsPicture, app.gameSmooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
			SDL_UpdateTexture(gapsPicture, NULL, keptGaps, (int) app.picturePitch);
		}
		data = keptPicture;
	}
	SDL_UpdateTexture(app.game, NULL, data, (int) app.picturePitch);
	gameKept = keeping;
	gameKeptStale = false;
	gameKeptSolved = overlayKeep.solved;
}


// A new picture: kept for the shadow, and darkened when it's drawn when the game's texture is kept.
bool lcdPictureCame(void) {
	shadowStale = true;
	if (gameKept) {
		gameKeptStale = true;
	}
	return gameKept;
}

void lcdPictureUpload(void) {
	uploadGame(false);
}

void lcdPictureDraw(SDL_Rect to) {
	makeOverlay(to);
	uint32_t lit = 0;
	libretroLcd(&lit);
	bool const keeping = overlay && app.picture && libretroLcdKeep(&overlayKeep);
	if (keeping ? !gameKept || gameKeptStale || gameKeptSolved != overlayKeep.solved : gameKept) {
		uploadGame(keeping);
	}
	SDL_RenderCopy(app.renderer, app.game, NULL, &to);
	if (gameKept && gapsPicture && gapMask && insideMask && gapLayer) {
		// Each pixel's own gaps: the gap colors times the overlay's alphas, made at the size shown,
		// added to the picture times the rest.
		SDL_SetRenderTarget(app.renderer, gapLayer);
		SDL_RenderCopy(app.renderer, gapsPicture, NULL, NULL);
		SDL_RenderCopy(app.renderer, gapMask, NULL, NULL);
		SDL_SetRenderTarget(app.renderer, NULL);
		SDL_RenderCopy(app.renderer, insideMask, NULL, &to);
		SDL_RenderCopy(app.renderer, gapLayer, NULL, &to);
	} else if (overlay) {
		uint32_t const gap = gameKept ? overlayKeep.gap : lit;
		SDL_SetTextureColorMod(overlay, gap >> 16 & 0xFF, gap >> 8 & 0xFF, gap & 0xFF);
		SDL_RenderCopy(app.renderer, overlay, NULL, &to);
	}
	if (stripes) {
		SDL_RenderCopy(app.renderer, stripes, NULL, &to);
	}
	bool const shadowed = overlay && lcdSettings.shadow && app.picture && app.format == RETRO_PIXEL_FORMAT_XRGB8888 && libretroLcdShadow();
	followTilt(shadowed && lcdSettings.shadow == SHADOW_TILT);
	if (shadowed) {
		drawShadow(to, lit);
	}
}

void lcdRendererGoing(void) {
	dropOverlay();
	if (gapsPicture) {
		SDL_DestroyTexture(gapsPicture);
		gapsPicture = NULL;
	}
	if (shadow) {
		SDL_DestroyTexture(shadow);
		shadow = NULL;
	}
}

// With LCD Rendering on the GPU, the core's grid is off whatever size it's set to.
const char* lcdOptionRead(const struct Option* option) {
	return lcdSettings.onGpu && !strcmp(option->key, "armgba_lcd_grid") ? "disabled" : NULL;
}

// The core's LCD grid and pixel shadow show only with LCD Rendering on the CPU; the shadow's
// distance, on the GPU, with the app's own shadow on, and LCD Subpixels with its grid on the Color's
// and Advance's screens.
bool lcdOptionShown(const struct Option* option, bool shown) {
	if (lcdSettings.onGpu && !strcmp(option->key, "armgba_lcd_shadow_distance")) {
		return lcdSettings.grid && lcdSettings.shadow && libretroLcdShadow();
	}
	if (lcdSettings.onGpu && !strcmp(option->key, "armgba_lcd_subpixels")) {
		uint32_t gap;
		return lcdSettings.grid && libretroLcdHasSubpixels(libretroLcd(&gap));
	}
	return shown && !(lcdSettings.onGpu && (!strcmp(option->key, "armgba_lcd_grid") || !strcmp(option->key, "armgba_lcd_shadow")));
}

enum {
	SET_LCD_RENDERING,
	SET_GPU_GRID,
	SET_SHADOW,
};

static void changeLcd(int which, int delta) {
	int const d = delta ? delta : 1;
	switch (which) {
	// The core reads its grid again, now off or back at its size.
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

// LCD Rendering, the same row on Core > Video and Shaders: which of them draws the LCD grid and pixel
// shadow.
static void addLcdRendering(void) {
	addValue("LCD Rendering", lcdSettings.onGpu ? "GPU" : "CPU", changeLcd, SET_LCD_RENDERING);
	helpFor("Who draws the LCD grid and pixel shadow: the CPU (Core \xE2\x80\xBA Video) or the GPU (Shaders).");
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
	if (!lcdSettings.onGpu) {
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
	lcdSettings.shadow = 4;
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
		lcdSettings.shadow = n >= 0 && n <= SHADOW_TILT ? n : 4;
	} else {
		return false;
	}
	return true;
}

void lcdSettingsWrite(FILE* f) {
	fprintf(f, "lcd_rendering = %s\ngpu_lcd_grid = %s\nlcd_shadow = %d\n", lcdSettings.onGpu ? "gpu" : "cpu", lcdSettings.grid ? "true" : "false", lcdSettings.shadow);
}
