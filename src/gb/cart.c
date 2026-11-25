/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The cartridge header and the memory bank controllers (Pan Docs, "The Cartridge Header" and "MBCs").

static const uint32_t ramSizes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };

bool gbCartInit(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	const uint8_t* rom = gb->rom;
	memset(cart, 0, sizeof(*cart));
	uint8_t type = rom[0x147];
	switch (type) {
	case 0x00:
	case 0x08:
	case 0x09:
		cart->mapper = GB_MBC_NONE;
		break;
	case 0x01:
	case 0x02:
	case 0x03:
		cart->mapper = GB_MBC1;
		break;
	case 0x05:
	case 0x06:
		cart->mapper = GB_MBC2;
		break;
	case 0x0F:
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
		cart->mapper = GB_MBC3;
		cart->hasRtc = type <= 0x10;
		break;
	case 0x19:
	case 0x1A:
	case 0x1B:
	case 0x1C:
	case 0x1D:
	case 0x1E:
		cart->mapper = GB_MBC5;
		break;
	default:
		cart->mapper = GB_MBC5;
		break;
	}
	cart->hasBattery = type == 0x03 || type == 0x06 || type == 0x09 || type == 0x0F || type == 0x10 || type == 0x13 ||
	                   type == 0x1B || type == 0x1E;
	cart->ramSize = rom[0x149] < 6 ? ramSizes[rom[0x149]] : 0;
	if (cart->mapper == GB_MBC2) {
		cart->ramSize = 0x200;
	}
	cart->hasRam = cart->ramSize != 0;
	// Banks of 16 KiB; the ROM's size is rounded up to a power of two when it is loaded.
	cart->romBanks = (uint32_t) (gb->romSize / 0x4000);
	if (!cart->romBanks) {
		cart->romBanks = 1;
	}
	cart->romBank = 1;
	// A 1 MiB MBC1 cartridge with a second header at bank $10 is a multicart.
	if (cart->mapper == GB_MBC1 && gb->romSize == 0x100000 && !memcmp(rom + 0x104, rom + 0x40104, 0x30)) {
		cart->mbc1Multicart = true;
	}
	return true;
}

static void mbc1Banks(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	uint8_t low = cart->mbc1Low;
	low = low ? low : 1;
	cart->romBank = cart->mbc1Multicart ? (cart->mbc1Upper << 4) | (low & 0xF) : (cart->mbc1Upper << 5) | low;
	cart->ramBank = cart->mbc1Mode ? cart->mbc1Upper : 0;
}

void gbCartWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCart* cart = &gb->cart;
	uint16_t romBank = cart->romBank;
	uint8_t mode = cart->mbc1Mode, upper = cart->mbc1Upper;
	switch (cart->mapper) {
	case GB_MBC1:
		switch (address >> 13) {
		case 0:
			cart->ramEnabled = (value & 0xF) == 0xA;
			break;
		case 1:
			cart->mbc1Low = value & 0x1F;
			mbc1Banks(gb);
			break;
		case 2:
			cart->mbc1Upper = value & 3;
			mbc1Banks(gb);
			break;
		default:
			cart->mbc1Mode = value & 1;
			mbc1Banks(gb);
			break;
		}
		break;
	case GB_MBC2:
		if (address < 0x4000) {
			if (address & 0x100) {
				cart->romBank = value & 0xF ? value & 0xF : 1;
			} else {
				cart->ramEnabled = (value & 0xF) == 0xA;
			}
		}
		break;
	case GB_MBC3:
		switch (address >> 13) {
		case 0:
			cart->ramEnabled = (value & 0xF) == 0xA;
			break;
		case 1:
			cart->romBank = value & 0x7F ? value & 0x7F : 1;
			break;
		case 2:
			cart->ramBank = value;
			break;
		default:
			break;
		}
		break;
	case GB_MBC5:
		switch (address >> 12) {
		case 0:
		case 1:
			cart->ramEnabled = value == 0xA;
			break;
		case 2:
			cart->romBank = (cart->romBank & 0x100) | value;
			break;
		case 3:
			cart->romBank = (cart->romBank & 0xFF) | (value & 1) << 8;
			break;
		case 4:
		case 5:
			cart->ramBank = value & 0xF;
			break;
		default:
			break;
		}
		break;
	default:
		return;
	}
	// Games write the bank they already have all the time.
	if (cart->romBank != romBank || cart->mbc1Mode != mode || cart->mbc1Upper != upper) {
		gbMapRom(gb);
	}
}

static int32_t ramOffset(const struct GB* gb, uint16_t address) {
	const struct GBCart* cart = &gb->cart;
	if (!cart->hasRam || !cart->ramEnabled) {
		return -1;
	}
	if (cart->mapper == GB_MBC2) {
		return address & 0x1FF;
	}
	if (cart->mapper == GB_MBC3 && cart->ramBank > 3) {
		return -1;
	}
	return (cart->ramBank * 0x2000 + (address & 0x1FFF)) & (cart->ramSize - 1);
}

uint8_t gbCartReadRam(struct GB* gb, uint16_t address) {
	int32_t offset = ramOffset(gb, address);
	if (offset < 0) {
		return 0xFF;
	}
	if (gb->cart.mapper == GB_MBC2) {
		return gb->sram[offset] | 0xF0;
	}
	return gb->sram[offset];
}

void gbCartWriteRam(struct GB* gb, uint16_t address, uint8_t value) {
	int32_t offset = ramOffset(gb, address);
	if (offset >= 0) {
		gb->sram[offset] = gb->cart.mapper == GB_MBC2 ? value & 0xF : value;
	}
}
