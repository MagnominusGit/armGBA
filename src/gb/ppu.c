/* Copyright (c) 2026 Magnominus
 * SPDX-License-Identifier: MIT */
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

// The objects on a line, in OAM order: at most ten.
struct LineObjects {
	int count;
	uint8_t index[10];
};

static void findObjects(const struct GB* gb, int y, struct LineObjects* out) {
	const struct GBPpu* ppu = &gb->ppu;
	int height = ppu->lcdc & LCDC_TALL ? 16 : 8;
	out->count = 0;
	for (int i = 0; i < 40 && out->count < 10; ++i) {
		int top = gb->oam[i * 4] - 16;
		if (y >= top && y < top + height) {
			out->index[out->count++] = i;
		}
	}
}

static bool windowOnLine(const struct GB* gb) {
	const struct GBPpu* ppu = &gb->ppu;
	return (ppu->lcdc & LCDC_WINDOW) && (gb->cgbMode || (ppu->lcdc & LCDC_BG)) && ppu->windowSeen && ppu->wx <= 166;
}

// How long mode 3 lasts: 172 dots, the fine scroll's, 6 for the window, and each object's penalty
// (Pan Docs, "Mode 3 length").
static unsigned drawLength(const struct GB* gb) {
	const struct GBPpu* ppu = &gb->ppu;
	unsigned length = 172 + (ppu->scx & 7);
	bool window = windowOnLine(gb);
	if (window) {
		length += 6;
	}
	if (!(ppu->lcdc & LCDC_OBJ) && !gb->cgbMode) {
		return length;
	}
	struct LineObjects objects;
	findObjects(gb, ppu->line, &objects);
	bool tileSeen[32] = { false };
	for (int i = 0; i < objects.count; ++i) {
		int x = gb->oam[objects.index[i] * 4 + 1];
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

// The colour index of a pixel of a tile row: bit 0 from the first byte, bit 1 from the second.
static inline int tilePixel(uint8_t lo, uint8_t hi, int x) {
	int shift = 7 - x;
	return (lo >> shift & 1) | (hi >> shift & 1) << 1;
}

static uint16_t tileAddress(const struct GBPpu* ppu, uint8_t tile) {
	if (ppu->lcdc & LCDC_TILES) {
		return tile * 16;
	}
	return 0x1000 + (int8_t) tile * 16;
}

static void drawLine(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	int y = ppu->line;
	if (y >= GB_HEIGHT) {
		return;
	}
	uint8_t index[GB_WIDTH];
	uint16_t* out = gb->frame[y];
	if (ppu->lcdc & LCDC_BG) {
		uint16_t map = ppu->lcdc & LCDC_BG_MAP ? 0x1C00 : 0x1800;
		int bgY = (ppu->scy + y) & 0xFF;
		for (int x = 0; x < GB_WIDTH; ++x) {
			int bgX = (ppu->scx + x) & 0xFF;
			uint8_t tile = gb->vram[map + (bgY >> 3) * 32 + (bgX >> 3)];
			uint16_t row = tileAddress(ppu, tile) + (bgY & 7) * 2;
			index[x] = tilePixel(gb->vram[row], gb->vram[row + 1], bgX & 7);
		}
		if (windowOnLine(gb)) {
			uint16_t wmap = ppu->lcdc & LCDC_WINDOW_MAP ? 0x1C00 : 0x1800;
			int wy = ppu->windowLine;
			int start = ppu->wx - 7;
			for (int x = start < 0 ? 0 : start; x < GB_WIDTH; ++x) {
				int wxp = x - start;
				uint8_t tile = gb->vram[wmap + (wy >> 3) * 32 + (wxp >> 3)];
				uint16_t row = tileAddress(ppu, tile) + (wy & 7) * 2;
				index[x] = tilePixel(gb->vram[row], gb->vram[row + 1], wxp & 7);
			}
			++ppu->windowLine;
		}
	} else {
		memset(index, 0, sizeof(index));
	}
	for (int x = 0; x < GB_WIDTH; ++x) {
		out[x] = ppu->bgp >> (index[x] * 2) & 3;
	}
	if (!(ppu->lcdc & LCDC_OBJ)) {
		return;
	}
	struct LineObjects objects;
	findObjects(gb, y, &objects);
	int height = ppu->lcdc & LCDC_TALL ? 16 : 8;
	// Which object owns each pixel: the one with the smallest X, then the first in OAM.
	uint8_t ownerX[GB_WIDTH];
	memset(ownerX, 0xFF, sizeof(ownerX));
	for (int i = 0; i < objects.count; ++i) {
		const uint8_t* object = &gb->oam[objects.index[i] * 4];
		int left = object[1] - 8;
		uint8_t flags = object[3];
		int row = y - (object[0] - 16);
		if (flags & 0x40) {
			row = height - 1 - row;
		}
		uint8_t tile = height == 16 ? object[2] & 0xFE : object[2];
		uint16_t address = tile * 16 + row * 2;
		uint8_t lo = gb->vram[address];
		uint8_t hi = gb->vram[address + 1];
		uint8_t palette = flags & 0x10 ? ppu->obp1 : ppu->obp0;
		for (int px = 0; px < 8; ++px) {
			int x = left + px;
			if (x < 0 || x >= GB_WIDTH || ownerX[x] <= object[1]) {
				continue;
			}
			int color = tilePixel(lo, hi, flags & 0x20 ? 7 - px : px);
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
	gbSchedule(gb, GB_EVENT_PPU, at);
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

void gbPpuEvent(struct GB* gb) {
	while (gb->ppu.lcdc & LCDC_ON && gbSince(gb->now, gb->ppu.nextAt) >= 0) {
		advance(gb);
	}
}

void gbPpuSync(struct GB* gb) {
	gbPpuEvent(gb);
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
