/* Copyright (c) 2026 Magnominus
 * SPDX-License-Identifier: MIT */
#include "gb.h"

#include <string.h>

// The PPU (Pan Docs, "Rendering" and "STAT"). A line is 456 dots: OAM scan (mode 2) for 80, drawing
// (mode 3) for 172 dots or more, then H-blank (mode 0); lines 144 to 153 are V-blank (mode 1). The
// PPU moves from step to step by an event, and draws a whole line where mode 3 ends.

enum {
	STEP_OAM,
	STEP_DRAW,
	STEP_HBLANK,
	STEP_LINE_END,
	STEP_VBLANK,
	STEP_LY_ZERO,
	STEP_VBLANK_END,
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
	findObjects(gb, ppu->ly, &objects);
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
	int y = ppu->ly;
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

// The STAT interrupt line: a request goes out when it rises.
static void updateStat(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	bool line = (ppu->lyMatch && (ppu->stat & 0x40)) || (ppu->mode == 0 && (ppu->stat & 0x08)) ||
	            (ppu->mode == 1 && (ppu->stat & 0x10)) || (ppu->mode == 2 && (ppu->stat & 0x20));
	if (line && !ppu->statLine) {
		gbRequest(gb, GB_IRQ_STAT);
	}
	ppu->statLine = line;
}

static void compareLy(struct GB* gb) {
	gb->ppu.lyMatch = gb->ppu.ly == gb->ppu.lyc;
}

static void next(struct GB* gb, int step, uint32_t offset) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->step = step;
	ppu->nextAt = ppu->lineStart + offset;
	gbSchedule(gb, GB_EVENT_PPU, ppu->nextAt);
}

static void startLine(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	if (ppu->ly < GB_HEIGHT) {
		if (ppu->ly == ppu->wy) {
			ppu->windowSeen = true;
		}
		ppu->mode = ppu->firstLine ? 0 : 2;
		compareLy(gb);
		updateStat(gb);
		next(gb, STEP_DRAW, 80);
		return;
	}
	if (ppu->ly == GB_HEIGHT) {
		ppu->mode = 1;
		compareLy(gb);
		gbRequest(gb, GB_IRQ_VBLANK);
		// The OAM interrupt goes off at V-blank's start too.
		bool oam = ppu->stat & 0x20;
		updateStat(gb);
		if (oam && !ppu->statLine) {
			gbRequest(gb, GB_IRQ_STAT);
		}
		gb->frameEnded = true;
		++gb->frameCount;
		next(gb, STEP_LINE_END, GB_LINE_DOTS);
		return;
	}
	compareLy(gb);
	updateStat(gb);
	if (ppu->ly == GB_LINES - 1) {
		next(gb, STEP_LY_ZERO, 4);
	} else {
		next(gb, STEP_LINE_END, GB_LINE_DOTS);
	}
}

// Runs the step due now.
static void advance(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	switch (ppu->step) {
	case STEP_DRAW:
		ppu->firstLine = false;
		ppu->mode = 3;
		ppu->mode3Length = drawLength(gb);
		updateStat(gb);
		next(gb, STEP_HBLANK, 80 + ppu->mode3Length);
		break;
	case STEP_HBLANK:
		drawLine(gb);
		ppu->mode = 0;
		updateStat(gb);
		next(gb, STEP_LINE_END, GB_LINE_DOTS);
		break;
	case STEP_LINE_END:
		ppu->lineStart += GB_LINE_DOTS;
		++ppu->ly;
		startLine(gb);
		break;
	case STEP_LY_ZERO:
		// Line 153 reads as 0 after its first dots.
		ppu->ly = 0;
		compareLy(gb);
		updateStat(gb);
		next(gb, STEP_VBLANK_END, GB_LINE_DOTS);
		break;
	case STEP_VBLANK_END:
		ppu->lineStart += GB_LINE_DOTS;
		ppu->ly = 0;
		ppu->windowLine = 0;
		ppu->windowSeen = false;
		startLine(gb);
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

void gbPpuReset(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	memset(ppu, 0, sizeof(*ppu));
	ppu->lcdc = 0x91;
	ppu->stat = 0x80;
	ppu->bgp = 0xFC;
	ppu->lineStart = gb->now;
	startLine(gb);
}

uint8_t gbPpuRead(struct GB* gb, uint8_t reg) {
	struct GBPpu* ppu = &gb->ppu;
	gbPpuSync(gb);
	switch (reg) {
	case 0x40:
		return ppu->lcdc;
	case 0x41:
		return 0x80 | (ppu->stat & 0x78) | (ppu->lyMatch ? 4 : 0) | (ppu->lcdc & LCDC_ON ? ppu->mode : 0);
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
			ppu->ly = 0;
			ppu->mode = 0;
			ppu->windowLine = 0;
			ppu->windowSeen = false;
			gbUnschedule(gb, GB_EVENT_PPU);
			updateStat(gb);
		} else if (!(old & LCDC_ON) && (value & LCDC_ON)) {
			ppu->ly = 0;
			ppu->lineStart = gb->now;
			ppu->firstLine = true;
			startLine(gb);
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
		if (ppu->lcdc & LCDC_ON) {
			compareLy(gb);
			updateStat(gb);
		}
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
