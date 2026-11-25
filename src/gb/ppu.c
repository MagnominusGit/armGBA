/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * The STAT logic and the line timing follow SameBoy (Copyright (c) Lior Halphon), under the Expat (MIT) license:
 * see LICENSES/SameBoy.txt. */
#include "gb.h"

#include <string.h>

// The PPU (Pan Docs, "Rendering" and "STAT"). A line is 456 dots: OAM scan (mode 2) for 80, drawing
// (mode 3) for 172 dots or more, then H-blank (mode 0); lines 144 to 153 are V-blank (mode 1). The
// PPU moves from step to step by an event, and draws a whole line where mode 3 ends.
//
// When within a line LY, STAT's mode and the LY=LYC comparison change, and when the STAT interrupt
// line sees them, follow SameBoy's PPU (Lior Halphon, MIT; Core/display.c), which measured them.

enum {
	// Lines 0 to 143.
	STEP_OAM_WRITE,
	STEP_LY,
	STEP_OAM,
	STEP_OAM_LATE,
	STEP_MODE3,
	STEP_DRAW,
	STEP_MODE0,
	STEP_HBLANK_INT,
	STEP_PRE_OAM,
	STEP_LINE_END,
	// The first line after the LCD is switched on.
	STEP_FIRST,
	STEP_FIRST_OAM_WRITE,
	STEP_FIRST_MODE3,
	STEP_FIRST_VRAM,
	// Lines 144 to 152.
	STEP_V_START,
	STEP_V_LY,
	STEP_V_COMPARE,
	STEP_V_BLANK,
	// Line 153.
	STEP_L153_LY,
	STEP_L153_ZERO,
	STEP_L153_LYC0,
	STEP_L153_COMPARE,
};

enum {
	LCDC_BG = 0x01,
	LCDC_OBJ = 0x02,
	LCDC_TALL = 0x04,
	LCDC_BG_MAP = 0x08,
	LCDC_TILES = 0x10,
	LCDC_WINDOW = 0x20,
	LCDC_WINDOW_MAP = 0x40,
	LCDC_ON = 0x80,
};

// The objects on the line, in OAM order: at most ten.
static void findObjects(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	int height = ppu->lcdc & LCDC_TALL ? 16 : 8;
	int y = ppu->line;
	ppu->objectCount = 0;
	for (int i = 0; i < 40 && ppu->objectCount < 10; ++i) {
		int top = gb->oam[i * 4] - 16;
		if (y >= top && y < top + height) {
			ppu->objects[ppu->objectCount++] = i;
		}
	}
}

static bool windowOnLine(const struct GB* gb) {
	const struct GBPpu* ppu = &gb->ppu;
	return (ppu->lcdc & LCDC_WINDOW) && (gb->cgbMode || (ppu->lcdc & LCDC_BG)) && ppu->windowSeen && ppu->wx <= 166;
}

// How long mode 3 lasts: 172 dots, the fine scroll's, 6 for the window, and each object's penalty
// (Pan Docs, "Mode 3 length").
static unsigned drawLength(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	findObjects(gb);
	unsigned length = 172 + (ppu->scx & 7);
	bool window = windowOnLine(gb);
	if (window) {
		length += 6;
	}
	if (!(ppu->lcdc & LCDC_OBJ) && !gb->cgbMode) {
		return length;
	}
	bool tileSeen[32] = { false };
	for (int i = 0; i < ppu->objectCount; ++i) {
		int x = gb->oam[ppu->objects[i] * 4 + 1];
		if (x >= 168) {
			continue;
		}
		if (x == 0) {
			length += 11;
			continue;
		}
		int pixel = x - 8 + (window && x - 1 >= ppu->wx ? 0 : ppu->scx);
		int tile = (pixel >> 3) & 31;
		if (!tileSeen[tile]) {
			tileSeen[tile] = true;
			int right = 7 - (pixel & 7) - 2;
			if (right > 0) {
				length += right;
			}
		}
		length += 6;
	}
	return length;
}

static uint16_t tileAddress(const struct GBPpu* ppu, uint8_t tile) {
	if (ppu->lcdc & LCDC_TILES) {
		return tile * 16;
	}
	return 0x1000 + (int8_t) tile * 16;
}

// A tile row's two bytes to eight colour indices, a byte each, leftmost first.
static uint64_t spread[256];

static void initSpread(void) {
	if (spread[1]) {
		return;
	}
	for (int b = 0; b < 256; ++b) {
		uint64_t v = 0;
		for (int i = 0; i < 8; ++i) {
			v |= (uint64_t) (b >> (7 - i) & 1) << (i * 8);
		}
		spread[b] = v;
	}
}

static inline uint64_t tileRow(const struct GB* gb, uint16_t address) {
	return spread[gb->vram[address]] | spread[gb->vram[address + 1]] << 1;
}

// Draws the line whole: background and window a tile at a time, then the objects.
static void drawLine(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	int y = ppu->line;
	if (y >= GB_HEIGHT) {
		return;
	}
	initSpread();
	uint8_t index[GB_WIDTH + 16];
	uint16_t* out = gb->frame[y];
	if (ppu->lcdc & LCDC_BG) {
		uint8_t row[21 * 8];
		uint16_t map = (ppu->lcdc & LCDC_BG_MAP ? 0x1C00 : 0x1800) + ((ppu->scy + y) & 0xFF) / 8 * 32;
		unsigned fineY = (ppu->scy + y) & 7;
		unsigned column = ppu->scx >> 3;
		for (int t = 0; t < 21; ++t) {
			uint8_t tile = gb->vram[map + ((column + t) & 31)];
			uint64_t pixels = tileRow(gb, tileAddress(ppu, tile) + fineY * 2);
			memcpy(&row[t * 8], &pixels, 8);
		}
		memcpy(index, &row[ppu->scx & 7], GB_WIDTH);
		if (windowOnLine(gb)) {
			uint16_t wmap = (ppu->lcdc & LCDC_WINDOW_MAP ? 0x1C00 : 0x1800) + ppu->windowLine / 8 * 32;
			unsigned wfine = ppu->windowLine & 7;
			int start = ppu->wx - 7;
			int first = start < 0 ? 0 : start;
			for (int t = (first - start) / 8; t * 8 + start < GB_WIDTH; ++t) {
				uint64_t pixels = tileRow(gb, tileAddress(ppu, gb->vram[wmap + t]) + wfine * 2);
				uint8_t tilePixels[8];
				memcpy(tilePixels, &pixels, 8);
				for (int i = 0; i < 8; ++i) {
					int x = t * 8 + i + start;
					if (x >= first && x < GB_WIDTH) {
						index[x] = tilePixels[i];
					}
				}
			}
			++ppu->windowLine;
		}
	} else {
		memset(index, 0, GB_WIDTH);
	}
	uint16_t lut[4] = { ppu->bgp & 3, ppu->bgp >> 2 & 3, ppu->bgp >> 4 & 3, ppu->bgp >> 6 & 3 };
	for (int x = 0; x < GB_WIDTH; ++x) {
		out[x] = lut[index[x]];
	}
	if (!(ppu->lcdc & LCDC_OBJ)) {
		return;
	}
	int height = ppu->lcdc & LCDC_TALL ? 16 : 8;
	// Which object owns each pixel: the one with the smallest X, then the first in OAM.
	uint8_t ownerX[GB_WIDTH];
	memset(ownerX, 0xFF, sizeof(ownerX));
	for (int i = 0; i < ppu->objectCount; ++i) {
		const uint8_t* object = &gb->oam[ppu->objects[i] * 4];
		int left = object[1] - 8;
		if (left <= -8 || left >= GB_WIDTH) {
			continue;
		}
		uint8_t flags = object[3];
		int line = y - (object[0] - 16);
		if (flags & 0x40) {
			line = height - 1 - line;
		}
		uint8_t tile = height == 16 ? object[2] & 0xFE : object[2];
		uint64_t pixels = tileRow(gb, tile * 16 + line * 2);
		uint8_t colors[8];
		memcpy(colors, &pixels, 8);
		uint8_t palette = flags & 0x10 ? ppu->obp1 : ppu->obp0;
		for (int px = 0; px < 8; ++px) {
			int x = left + px;
			if (x < 0 || x >= GB_WIDTH || ownerX[x] <= object[1]) {
				continue;
			}
			int color = colors[flags & 0x20 ? 7 - px : px];
			if (!color) {
				continue;
			}
			ownerX[x] = object[1];
			if ((flags & 0x80) && index[x]) {
				continue;
			}
			out[x] = palette >> (color * 2) & 3;
		}
	}
}

// SameBoy's GB_STAT_update: the coincidence bit and the STAT interrupt line, from the comparison
// LY and the mode the interrupt goes by.
static void updateStat(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	if (!(ppu->lcdc & LCDC_ON)) {
		return;
	}
	if (gb->dma.active && ppu->mode == 2) {
		ppu->mode = 0;
	}
	bool single = gb->model != GB_MODEL_CGB || !gb->doubleSpeed;
	if (ppu->lyCompare >= 0 || single) {
		if (ppu->lyCompare == ppu->lyc) {
			ppu->lycLine = true;
			ppu->coincidence = true;
		} else {
			if (ppu->lyCompare >= 0) {
				ppu->lycLine = false;
			}
			ppu->coincidence = false;
		}
	}
	bool line;
	switch (ppu->modeForInterrupt) {
	case 0:
		line = ppu->stat & 0x08;
		break;
	case 1:
		line = ppu->stat & 0x10;
		break;
	case 2:
		line = ppu->stat & 0x20;
		break;
	default:
		line = false;
		break;
	}
	if ((ppu->stat & 0x40) && ppu->lycLine) {
		line = true;
	}
	if (line && !ppu->statLine) {
		gbRequest(gb, GB_IRQ_STAT);
	}
	ppu->statLine = line;
}

static void next(struct GB* gb, int step, uint32_t at) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->step = step;
	ppu->nextAt = at;
}

// A new line starts, at time start.
static void beginLine(struct GB* gb, uint32_t start) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->lineStart = start;
	ppu->lineEnd = start + GB_LINE_DOTS;
	if (ppu->line < GB_HEIGHT) {
		bool cgb = gb->model == GB_MODEL_CGB;
		ppu->oamWriteBlocked = cgb && !gb->doubleSpeed;
		next(gb, STEP_OAM_WRITE, start + 2);
	} else {
		ppu->lyCompare = -1;
		updateStat(gb);
		next(gb, ppu->line == GB_LINES - 1 ? STEP_L153_LY : STEP_V_LY, start + 2);
	}
}

// Mode 3 has begun to fetch: the line is drawn as a whole where it ends.
static void startDrawing(struct GB* gb, uint32_t at) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->drawStart = at;
	ppu->mode3Length = drawLength(gb);
	next(gb, STEP_MODE0, at + ppu->mode3Length - 5);
}

// Runs the step due now.
static void advance(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	bool single = gb->model != GB_MODEL_CGB || !gb->doubleSpeed;
	switch (ppu->step) {
	case STEP_OAM_WRITE:
		ppu->oamWriteBlocked = gb->model == GB_MODEL_CGB;
		next(gb, STEP_LY, ppu->lineStart + 3);
		break;
	case STEP_LY:
		ppu->oamReadBlocked = true;
		ppu->ly = ppu->line;
		ppu->lyCompare = ppu->line ? -1 : 0;
		if (ppu->line) {
			ppu->modeForInterrupt = 2;
			ppu->mode = 0;
		} else if (gb->model != GB_MODEL_CGB) {
			ppu->mode = 0;
		}
		updateStat(gb);
		next(gb, STEP_OAM, ppu->lineStart + 4);
		break;
	case STEP_OAM:
		ppu->oamReadBlocked = true;
		ppu->oamWriteBlocked = true;
		ppu->mode = 2;
		ppu->modeForInterrupt = 2;
		ppu->lyCompare = ppu->line;
		if (ppu->ly == ppu->wy) {
			ppu->windowSeen = true;
		}
		updateStat(gb);
		ppu->modeForInterrupt = -1;
		updateStat(gb);
		next(gb, STEP_OAM_LATE, ppu->lineStart + 80);
		break;
	case STEP_OAM_LATE:
		// Near the scan's end, VRAM writes open for a moment, and reads close on the original.
		ppu->vramReadBlocked = gb->model != GB_MODEL_CGB;
		ppu->vramWriteBlocked = false;
		ppu->oamWriteBlocked = gb->model == GB_MODEL_CGB;
		next(gb, STEP_MODE3, ppu->lineStart + 84);
		break;
	case STEP_MODE3:
		ppu->mode = 3;
		ppu->modeForInterrupt = 3;
		ppu->vramReadBlocked = true;
		ppu->vramWriteBlocked = true;
		ppu->oamReadBlocked = true;
		ppu->oamWriteBlocked = true;
		updateStat(gb);
		next(gb, STEP_DRAW, ppu->lineStart + 89);
		break;
	case STEP_DRAW:
		startDrawing(gb, ppu->nextAt);
		break;
	case STEP_MODE0:
		// Mode 3 ends: STAT shows mode 0 now, and its interrupt comes a dot later.
		drawLine(gb);
		if (single) {
			ppu->mode = 0;
			ppu->modeForInterrupt = 0;
			ppu->oamReadBlocked = false;
			ppu->vramReadBlocked = false;
			ppu->oamWriteBlocked = false;
			ppu->vramWriteBlocked = false;
		}
		next(gb, STEP_HBLANK_INT, ppu->nextAt + 1);
		break;
	case STEP_HBLANK_INT:
		ppu->oamReadBlocked = false;
		ppu->vramReadBlocked = false;
		ppu->oamWriteBlocked = false;
		ppu->vramWriteBlocked = false;
		ppu->mode = 0;
		ppu->modeForInterrupt = 0;
		updateStat(gb);
		next(gb, STEP_PRE_OAM, ppu->lineEnd - 2);
		break;
	case STEP_PRE_OAM:
		if (ppu->line != GB_HEIGHT - 1) {
			ppu->modeForInterrupt = 2;
		}
		next(gb, STEP_LINE_END, ppu->lineEnd);
		break;
	case STEP_LINE_END:
		if (++ppu->line == GB_LINES) {
			ppu->line = 0;
			ppu->windowLine = 0;
			ppu->windowSeen = false;
		}
		ppu->firstLine = false;
		beginLine(gb, ppu->lineEnd);
		break;
	case STEP_FIRST:
		// SameBoy's line 0 after the LCD is switched on: mode 0 instead of the OAM scan.
		ppu->mode = 0;
		ppu->lyCompare = 0;
		ppu->modeForInterrupt = -1;
		ppu->oamReadBlocked = false;
		ppu->vramReadBlocked = false;
		ppu->oamWriteBlocked = false;
		ppu->vramWriteBlocked = false;
		updateStat(gb);
		next(gb, STEP_FIRST_OAM_WRITE, ppu->nextAt + 76);
		break;
	case STEP_FIRST_OAM_WRITE:
		ppu->oamWriteBlocked = true;
		next(gb, STEP_FIRST_MODE3, ppu->nextAt + 2);
		break;
	case STEP_FIRST_MODE3:
		ppu->mode = 3;
		ppu->modeForInterrupt = 3;
		ppu->oamWriteBlocked = true;
		ppu->oamReadBlocked = true;
		ppu->vramReadBlocked = gb->model != GB_MODEL_CGB || gb->doubleSpeed;
		ppu->vramWriteBlocked = ppu->vramReadBlocked;
		next(gb, STEP_FIRST_VRAM, ppu->nextAt + 5);
		break;
	case STEP_FIRST_VRAM:
		ppu->vramReadBlocked = true;
		ppu->vramWriteBlocked = true;
		startDrawing(gb, ppu->nextAt);
		break;
	case STEP_V_LY:
		ppu->ly = ppu->line;
		if (ppu->line == GB_HEIGHT && !ppu->statLine && (ppu->stat & 0x20)) {
			gbRequest(gb, GB_IRQ_STAT);
		}
		next(gb, STEP_V_COMPARE, ppu->lineStart + 4);
		break;
	case STEP_V_COMPARE:
		ppu->lyCompare = ppu->line;
		updateStat(gb);
		if (ppu->line == GB_HEIGHT) {
			next(gb, STEP_V_BLANK, ppu->lineStart + 5);
		} else {
			next(gb, STEP_LINE_END, ppu->lineEnd);
		}
		break;
	case STEP_V_BLANK:
		ppu->mode = 1;
		gbRequest(gb, GB_IRQ_VBLANK);
		if (!ppu->statLine && (ppu->stat & 0x20)) {
			gbRequest(gb, GB_IRQ_STAT);
		}
		ppu->modeForInterrupt = 1;
		updateStat(gb);
		gb->frameEnded = true;
		++gb->frameCount;
		next(gb, STEP_LINE_END, ppu->lineEnd);
		break;
	case STEP_L153_LY:
		ppu->ly = 153;
		next(gb, STEP_L153_ZERO, ppu->lineStart + (gb->model == GB_MODEL_CGB ? 4 : 6));
		break;
	case STEP_L153_ZERO:
		if (single && gb->model != GB_MODEL_CGB) {
			ppu->ly = 0;
		}
		ppu->lyCompare = 153;
		updateStat(gb);
		next(gb, STEP_L153_LYC0, ppu->lineStart + 8);
		break;
	case STEP_L153_LYC0:
		ppu->ly = 0;
		ppu->lyCompare = gb->model == GB_MODEL_CGB && gb->doubleSpeed ? 153 : -1;
		updateStat(gb);
		next(gb, STEP_L153_COMPARE, ppu->lineStart + 12);
		break;
	case STEP_L153_COMPARE:
		ppu->lyCompare = 0;
		updateStat(gb);
		next(gb, STEP_LINE_END, ppu->lineEnd);
		break;
	}
}

// Whether a step can request an interrupt, as STAT and LYC stand: only such steps are events. The
// others run when something reads or writes the PPU, or on the way to one that is.
static bool interrupting(const struct GB* gb, int step, int line) {
	const struct GBPpu* ppu = &gb->ppu;
	bool lyc = ppu->stat & 0x40;
	switch (step) {
	case STEP_LY:
		return (line && (ppu->stat & 0x20)) || (!line && lyc && ppu->lyc == 0);
	case STEP_OAM:
		return (ppu->stat & 0x20) || (lyc && ppu->lyc == line);
	case STEP_DRAW:
	case STEP_FIRST_VRAM:
		// Where mode 3 ends is worked out here.
		return ppu->stat & 0x08;
	case STEP_HBLANK_INT:
		return ppu->stat & 0x08;
	case STEP_FIRST:
		return lyc && ppu->lyc == 0;
	case STEP_V_LY:
		return line == GB_HEIGHT && (ppu->stat & 0x20);
	case STEP_V_COMPARE:
		return lyc && ppu->lyc == line;
	case STEP_V_BLANK:
		return true;
	case STEP_L153_ZERO:
		return lyc && ppu->lyc == 153;
	case STEP_L153_COMPARE:
		return lyc && ppu->lyc == 0;
	default:
		return false;
	}
}

// Schedules the PPU's event at the first step from here that can request an interrupt, following the
// steps' times as advance() takes them without running them. Past the start of mode 3, where its
// length is not yet known, it goes on from the line's end.
static void plan(struct GB* gb) {
	const struct GBPpu* ppu = &gb->ppu;
	if (!(ppu->lcdc & LCDC_ON)) {
		gbUnschedule(gb, GB_EVENT_PPU);
		return;
	}
	int step = ppu->step;
	int line = ppu->line;
	uint32_t at = ppu->nextAt;
	uint32_t lineStart = ppu->lineStart;
	uint32_t lineEnd = ppu->lineEnd;
	unsigned late153 = gb->model == GB_MODEL_CGB ? 4 : 6;
	for (int guard = 0; guard < 2400; ++guard) {
		if (interrupting(gb, step, line)) {
			break;
		}
		switch (step) {
		case STEP_OAM_WRITE:
			step = STEP_LY;
			at = lineStart + 3;
			break;
		case STEP_LY:
			step = STEP_OAM;
			at = lineStart + 4;
			break;
		case STEP_OAM:
			step = STEP_OAM_LATE;
			at = lineStart + 80;
			break;
		case STEP_OAM_LATE:
			step = STEP_MODE3;
			at = lineStart + 84;
			break;
		case STEP_MODE3:
			step = STEP_DRAW;
			at = lineStart + 89;
			break;
		case STEP_FIRST:
			step = STEP_FIRST_OAM_WRITE;
			at += 76;
			break;
		case STEP_FIRST_OAM_WRITE:
			step = STEP_FIRST_MODE3;
			at += 2;
			break;
		case STEP_FIRST_MODE3:
			step = STEP_FIRST_VRAM;
			at += 5;
			break;
		case STEP_MODE0:
			// Mode 3's end is known once it has started.
			step = STEP_HBLANK_INT;
			at += 1;
			break;
		case STEP_DRAW:
		case STEP_FIRST_VRAM:
		case STEP_HBLANK_INT:
			step = STEP_PRE_OAM;
			at = lineEnd - 2;
			break;
		case STEP_PRE_OAM:
			step = STEP_LINE_END;
			at = lineEnd;
			break;
		case STEP_LINE_END:
			line = line + 1 == GB_LINES ? 0 : line + 1;
			lineStart = lineEnd;
			lineEnd = lineStart + GB_LINE_DOTS;
			step = line < GB_HEIGHT ? STEP_OAM_WRITE : line == GB_LINES - 1 ? STEP_L153_LY : STEP_V_LY;
			at = lineStart + 2;
			break;
		case STEP_V_LY:
			step = STEP_V_COMPARE;
			at = lineStart + 4;
			break;
		case STEP_V_COMPARE:
			step = line == GB_HEIGHT ? STEP_V_BLANK : STEP_LINE_END;
			at = line == GB_HEIGHT ? lineStart + 5 : lineEnd;
			break;
		case STEP_V_BLANK:
		case STEP_L153_COMPARE:
			step = STEP_LINE_END;
			at = lineEnd;
			break;
		case STEP_L153_LY:
			step = STEP_L153_ZERO;
			at = lineStart + late153;
			break;
		case STEP_L153_ZERO:
			step = STEP_L153_LYC0;
			at = lineStart + 8;
			break;
		case STEP_L153_LYC0:
			step = STEP_L153_COMPARE;
			at = lineStart + 12;
			break;
		}
	}
	gbSchedule(gb, GB_EVENT_PPU, at);
}

static void catchUp(struct GB* gb) {
	while (gb->ppu.lcdc & LCDC_ON && gbSince(gb->now, gb->ppu.nextAt) >= 0) {
		advance(gb);
	}
}

void gbPpuEvent(struct GB* gb) {
	catchUp(gb);
	plan(gb);
}

void gbPpuSync(struct GB* gb) {
	catchUp(gb);
}

static void lcdOn(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->line = 0;
	ppu->ly = 0;
	ppu->firstLine = true;
	ppu->windowLine = 0;
	ppu->windowSeen = false;
	// The first line is 8 dots short, and starts a dot late on the original Game Boy.
	unsigned late = gb->model == GB_MODEL_CGB ? 0 : 1;
	ppu->lineStart = gb->now + late;
	ppu->lineEnd = gb->now + late + GB_LINE_DOTS - 8;
	next(gb, STEP_FIRST, gb->now + late);
	plan(gb);
}

static void lcdOff(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->oamReadBlocked = false;
	ppu->vramReadBlocked = false;
	ppu->oamWriteBlocked = false;
	ppu->vramWriteBlocked = false;
	ppu->ly = 0;
	ppu->line = 0;
	ppu->mode = 0;
	ppu->windowLine = 0;
	ppu->windowSeen = false;
	gbUnschedule(gb, GB_EVENT_PPU);
}

void gbPpuReset(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	memset(ppu, 0, sizeof(*ppu));
	ppu->lcdc = 0x91;
	ppu->bgp = 0xFC;
	ppu->modeForInterrupt = -1;
	lcdOn(gb);
}

uint8_t gbPpuRead(struct GB* gb, uint8_t reg) {
	struct GBPpu* ppu = &gb->ppu;
	gbPpuSync(gb);
	switch (reg) {
	case 0x40:
		return ppu->lcdc;
	case 0x41:
		return 0x80 | (ppu->stat & 0x78) | (ppu->coincidence ? 4 : 0) | (ppu->lcdc & LCDC_ON ? ppu->mode : 0);
	case 0x42:
		return ppu->scy;
	case 0x43:
		return ppu->scx;
	case 0x44:
		return ppu->ly;
	case 0x45:
		return ppu->lyc;
	case 0x46:
		return ppu->dma;
	case 0x47:
		return ppu->bgp;
	case 0x48:
		return ppu->obp0;
	case 0x49:
		return ppu->obp1;
	case 0x4A:
		return ppu->wy;
	default:
		return ppu->wx;
	}
}

void gbPpuWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	struct GBPpu* ppu = &gb->ppu;
	gbPpuSync(gb);
	switch (reg) {
	case 0x40: {
		uint8_t old = ppu->lcdc;
		ppu->lcdc = value;
		if ((old & LCDC_ON) && !(value & LCDC_ON)) {
			lcdOff(gb);
		} else if (!(old & LCDC_ON) && (value & LCDC_ON)) {
			lcdOn(gb);
		}
		break;
	}
	case 0x41:
		ppu->stat = value & 0x78;
		updateStat(gb);
		plan(gb);
		break;
	case 0x42:
		ppu->scy = value;
		break;
	case 0x43:
		ppu->scx = value;
		break;
	case 0x44:
		break;
	case 0x45:
		ppu->lyc = value;
		updateStat(gb);
		plan(gb);
		break;
	case 0x46:
		ppu->dma = value;
		gbDmaStart(gb, value);
		break;
	case 0x47:
		ppu->bgp = value;
		break;
	case 0x48:
		ppu->obp0 = value;
		break;
	case 0x49:
		ppu->obp1 = value;
		break;
	case 0x4A:
		ppu->wy = value;
		break;
	default:
		ppu->wx = value;
		break;
	}
}
