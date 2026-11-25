/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The bus: the memory map (Pan Docs, "Memory Map") and the I/O registers. Reads of cartridge ROM and
// work RAM go through readMap without a call; everything else comes here.

// Bits of each I/O register that read as 1 whatever is written (unused, or write-only).
static const uint8_t ioUnused[0x80] = {
	[0x00] = 0xC0, [0x02] = 0x7E, [0x03] = 0xFF, [0x07] = 0xF8, [0x08] = 0xFF, [0x09] = 0xFF, [0x0A] = 0xFF,
	[0x0B] = 0xFF, [0x0C] = 0xFF, [0x0D] = 0xFF, [0x0E] = 0xFF, [0x0F] = 0xE0,
	[0x10] = 0x80, [0x11] = 0x3F, [0x13] = 0xFF, [0x14] = 0xBF, [0x15] = 0xFF, [0x16] = 0x3F, [0x18] = 0xFF,
	[0x19] = 0xBF, [0x1A] = 0x7F, [0x1B] = 0xFF, [0x1C] = 0x9F, [0x1D] = 0xFF, [0x1E] = 0xBF, [0x1F] = 0xFF,
	[0x20] = 0xFF, [0x23] = 0xBF, [0x26] = 0x70, [0x27] = 0xFF, [0x28] = 0xFF, [0x29] = 0xFF, [0x2A] = 0xFF,
	[0x2B] = 0xFF, [0x2C] = 0xFF, [0x2D] = 0xFF, [0x2E] = 0xFF, [0x2F] = 0xFF,
	[0x41] = 0x80,
	[0x4C] = 0xFF, [0x4D] = 0xFF, [0x4E] = 0xFF, [0x4F] = 0xFF, [0x50] = 0xFF, [0x51] = 0xFF, [0x52] = 0xFF,
	[0x53] = 0xFF, [0x54] = 0xFF, [0x55] = 0xFF, [0x56] = 0xFF, [0x57] = 0xFF, [0x58] = 0xFF, [0x59] = 0xFF,
	[0x5A] = 0xFF, [0x5B] = 0xFF, [0x5C] = 0xFF, [0x5D] = 0xFF, [0x5E] = 0xFF, [0x5F] = 0xFF, [0x60] = 0xFF,
	[0x61] = 0xFF, [0x62] = 0xFF, [0x63] = 0xFF, [0x64] = 0xFF, [0x65] = 0xFF, [0x66] = 0xFF, [0x67] = 0xFF,
	[0x68] = 0xFF, [0x69] = 0xFF, [0x6A] = 0xFF, [0x6B] = 0xFF, [0x6C] = 0xFF, [0x6D] = 0xFF, [0x6E] = 0xFF,
	[0x6F] = 0xFF, [0x70] = 0xFF, [0x71] = 0xFF, [0x72] = 0xFF, [0x73] = 0xFF, [0x74] = 0xFF, [0x75] = 0xFF,
	[0x76] = 0xFF, [0x77] = 0xFF, [0x78] = 0xFF, [0x79] = 0xFF, [0x7A] = 0xFF, [0x7B] = 0xFF, [0x7C] = 0xFF,
	[0x7D] = 0xFF, [0x7E] = 0xFF, [0x7F] = 0xFF,
};

// Sets which ROM and work RAM banks the CPU reads, and which of them it reads directly.
void gbMapRom(struct GB* gb) {
	const struct GBCart* cart = &gb->cart;
	size_t mask = gb->romSize - 1;
	size_t low = (size_t) (cart->mapper == GB_MBC1 && cart->mbc1Mode ? cart->mbc1Upper << (cart->mbc1Multicart ? 4 : 5) : 0) * 0x4000 & mask;
	size_t high = (size_t) cart->romBank * 0x4000 & mask;
	for (int i = 0; i < 4; ++i) {
		gb->memMap[i] = gb->rom + low + i * 0x1000;
		gb->memMap[4 + i] = gb->rom + high + i * 0x1000;
	}
	unsigned bank = gb->cgbMode ? (gb->wramBank & 7 ? gb->wramBank & 7 : 1) : 1;
	gb->memMap[0xC] = gb->wram;
	gb->memMap[0xD] = gb->wram + bank * 0x1000;
	gb->memMap[0xE] = gb->wram;
	for (int i = 0; i < 16; ++i) {
		gb->readMap[i] = gb->dma.active ? NULL : gb->memMap[i];
	}
	if (gb->bootMapped) {
		gb->readMap[0] = NULL;
	}
}

// The bus a DMA source or a CPU access is on: the external bus (cartridge and work RAM), or VRAM's.
static bool onVramBus(uint16_t address) {
	return address >= 0x8000 && address < 0xA000;
}

// A byte for OAM DMA, read without the PPU's locks.
static uint8_t dmaByte(struct GB* gb, uint16_t address) {
	switch (address >> 12) {
	case 0x8:
	case 0x9:
		return gb->vram[(gb->vramBank & 1) * 0x2000 + (address & 0x1FFF)];
	case 0xA:
	case 0xB:
		return gbCartReadRam(gb, address);
	case 0xE:
	case 0xF:
		return gb->memMap[0xC + (address >> 12 & 1)][address & 0xFFF];
	default:
		return gb->memMap[address >> 12][address & 0xFFF];
	}
}

// Which byte the DMA moves in the M-cycle ending now: -1 while it waits, 160 when it's done.
static int dmaIndex(const struct GB* gb) {
	int32_t cycles = gbSince(gb->now, gb->dma.start) / gb->mcycle;
	if (cycles < 2) {
		return -1;
	}
	return cycles - 2 < 160 ? cycles - 2 : 160;
}

// Copies the bytes moved by now.
void gbDmaSync(struct GB* gb) {
	struct GBDma* dma = &gb->dma;
	if (!dma->active) {
		return;
	}
	int index = dmaIndex(gb);
	// Lines still to be drawn see OAM as it was.
	gbPpuSync(gb);
	// The byte of the M-cycle ending now is moved by its end too.
	int upTo = index < 0 ? 0 : index < 160 ? index + 1 : 160;
	for (; dma->done < upTo; ++dma->done) {
		gb->oam[dma->done] = dmaByte(gb, dma->source + dma->done);
	}
	if (dma->done == 160 && index == 160) {
		dma->active = false;
		gbUnschedule(gb, GB_EVENT_DMA);
		gbMapRom(gb);
	}
}

void gbDmaStart(struct GB* gb, uint8_t page) {
	struct GBDma* dma = &gb->dma;
	gbDmaSync(gb);
	dma->overlap = dma->active && dmaIndex(gb) >= 0;
	dma->active = true;
	dma->source = page << 8;
	dma->start = gb->now;
	dma->done = 0;
	gbSchedule(gb, GB_EVENT_DMA, gb->now + 162 * gb->mcycle);
	gbMapRom(gb);
}

// During the transfer, OAM reads as $FF and a read on the source's bus gets the byte being moved.
static bool dmaConflict(struct GB* gb, uint16_t address, uint8_t* value) {
	gbDmaSync(gb);
	struct GBDma* dma = &gb->dma;
	if (!dma->active) {
		return false;
	}
	int index = dmaIndex(gb);
	if (index >= 0) {
		dma->overlap = false;
	}
	bool oam = address >= 0xFE00 && address < 0xFF00;
	if (index < 0 && dma->overlap && oam) {
		*value = 0xFF;
		return true;
	}
	if (index < 0 || index >= 160) {
		return false;
	}
	if (oam) {
		*value = 0xFF;
		return true;
	}
	if (address >= 0xFE00 || onVramBus(address) != onVramBus(dma->source)) {
		return false;
	}
	*value = dmaByte(gb, dma->source + index);
	return true;
}

static uint8_t joypad(const struct GB* gb) {
	uint8_t value = 0xCF | gb->joypadSelect;
	if (!(gb->joypadSelect & 0x10)) {
		value &= ~(gb->keys >> 4 & 0xF);
	}
	if (!(gb->joypadSelect & 0x20)) {
		value &= ~(gb->keys & 0xF);
	}
	return value;
}

uint8_t gbIoRead(struct GB* gb, uint8_t reg) {
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	switch (reg) {
	case 0x00:
		return joypad(gb);
	case 0x01:
		return gb->serial.sb;
	case 0x02:
		return gb->serial.sc | ioUnused[reg];
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
		return gbTimerRead(gb, reg);
	case 0x0F:
		return gb->ifl | 0xE0;
	case 0x10 ... 0x3F:
		return gbApuRead(gb, reg) | ioUnused[reg];
	case 0x40:
	case 0x41:
	case 0x42:
	case 0x43:
	case 0x44:
	case 0x45:
	case 0x46:
	case 0x47:
	case 0x48:
	case 0x49:
	case 0x4A:
	case 0x4B:
		return gbPpuRead(gb, reg);
	default:
		return gb->io[reg] | ioUnused[reg];
	}
}

void gbIoWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	switch (reg) {
	case 0x00:
		gb->joypadSelect = value & 0x30;
		break;
	case 0x01:
	case 0x02:
		gbSerialWrite(gb, reg, value);
		break;
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
		gbTimerWrite(gb, reg, value);
		break;
	case 0x0F:
		gb->ifl = value & 0x1F;
		break;
	case 0x10 ... 0x3F:
		gbApuWrite(gb, reg, value);
		break;
	case 0x50:
		if (value & 1 && gb->bootMapped) {
			gb->bootMapped = false;
			gbMapRom(gb);
		}
		break;
	case 0x40:
	case 0x41:
	case 0x42:
	case 0x43:
	case 0x44:
	case 0x45:
	case 0x46:
	case 0x47:
	case 0x48:
	case 0x49:
	case 0x4A:
	case 0x4B:
		gbPpuWrite(gb, reg, value);
		break;
	default:
		gb->io[reg] = value;
		break;
	}
}

uint8_t gbRead(struct GB* gb, uint16_t address) {
	uint8_t conflict;
	if (gb->dma.active && dmaConflict(gb, address, &conflict)) {
		return conflict;
	}
	switch (address >> 12) {
	case 0x0:
		if (gb->bootMapped && (address < 0x100 || (gb->bootSize > 0x100 && address >= 0x200 && address < 0x900))) {
			return gb->boot[address];
		}
		return gb->memMap[0][address];
	case 0x8:
	case 0x9:
		gbPpuSync(gb);
		if (gb->ppu.vramReadBlocked) {
			return 0xFF;
		}
		return gb->vram[(gb->vramBank & 1) * 0x2000 + (address & 0x1FFF)];
	case 0xA:
	case 0xB:
		return gbCartReadRam(gb, address);
	case 0xF:
		if (address < 0xFE00) {
			return gb->memMap[0xD][address & 0xFFF];
		}
		if (address < 0xFEA0) {
			gbPpuSync(gb);
			if (gb->ppu.oamReadBlocked) {
				return 0xFF;
			}
			return gb->oam[address & 0xFF];
		}
		if (address < 0xFF00) {
			return 0;
		}
		if (address < 0xFF80) {
			return gbIoRead(gb, address);
		}
		if (address < 0xFFFF) {
			return gb->hram[address & 0x7F];
		}
		return gb->ie;
	default:
		return gb->memMap[address >> 12][address & 0xFFF];
	}
}

void gbWrite(struct GB* gb, uint16_t address, uint8_t value) {
	switch (address >> 12) {
	case 0x0:
	case 0x1:
	case 0x2:
	case 0x3:
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
		gbCartWrite(gb, address, value);
		return;
	case 0x8:
	case 0x9:
		gbPpuSync(gb);
		if (!gb->ppu.vramWriteBlocked) {
			gb->vram[(gb->vramBank & 1) * 0x2000 + (address & 0x1FFF)] = value;
		}
		return;
	case 0xA:
	case 0xB:
		gbCartWriteRam(gb, address, value);
		return;
	case 0xC:
	case 0xE:
		gb->wram[address & 0xFFF] = value;
		return;
	case 0xD:
		((uint8_t*) gb->memMap[0xD])[address & 0xFFF] = value;
		return;
	default:
		if (address < 0xFE00) {
			((uint8_t*) gb->memMap[0xD])[address & 0xFFF] = value;
		} else if (address < 0xFEA0) {
			gbPpuSync(gb);
			gbDmaSync(gb);
			if (!gb->ppu.oamWriteBlocked && !(gb->dma.active && (dmaIndex(gb) >= 0 || gb->dma.overlap))) {
				gb->oam[address & 0xFF] = value;
			}
		} else if (address < 0xFF00) {
		} else if (address < 0xFF80) {
			gbIoWrite(gb, address, value);
		} else if (address < 0xFFFF) {
			gb->hram[address & 0x7F] = value;
		} else {
			gb->ie = value;
		}
		return;
	}
}
