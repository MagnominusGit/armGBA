/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The cartridge header and the memory bank controllers (Pan Docs, "The Cartridge Header" and "MBCs").

static const uint32_t ramSizes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };

// Unlicensed MBC5 variants (bit orders and the second-logo CRCs as mGBA documents them, src/gb/mbc).
// Each order lists, for each bit of the result, the bit of the input it comes from.
static const uint8_t sintaxOrders[16][8] = {
	{ 2, 1, 4, 3, 6, 5, 0, 7 }, { 3, 2, 5, 4, 7, 6, 1, 0 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 4, 5, 2, 3, 0, 1, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 6, 7, 4, 5, 1, 3, 0, 2 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 7, 6, 1, 0, 3, 2, 5, 4 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 5, 4, 7, 6, 1, 0, 3, 2 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 2, 3, 4, 5, 6, 7, 0, 1 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};
static const uint8_t bbdData[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
	{ 0, 5, 1, 3, 4, 2, 6, 7 }, { 0, 4, 2, 3, 1, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 5, 3, 4, 2, 6, 7 },
};
static const uint8_t bbdBanks[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 3, 4, 2, 0, 1, 5, 6, 7 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 1, 2, 3, 4, 0, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};
static const uint8_t hitekData[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 6, 5, 3, 4, 1, 2, 7 }, { 0, 5, 6, 3, 4, 2, 1, 7 }, { 0, 6, 2, 3, 4, 5, 1, 7 },
	{ 0, 6, 1, 3, 4, 5, 2, 7 }, { 0, 1, 6, 3, 4, 5, 2, 7 }, { 0, 2, 6, 3, 4, 1, 5, 7 }, { 0, 6, 2, 3, 4, 1, 5, 7 },
};
static const uint8_t hitekBanks[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 3, 2, 1, 0, 4, 5, 6, 7 }, { 2, 1, 0, 3, 4, 5, 6, 7 }, { 1, 0, 3, 2, 4, 5, 6, 7 },
	{ 0, 3, 2, 1, 4, 5, 6, 7 }, { 2, 3, 0, 1, 4, 5, 6, 7 }, { 3, 0, 1, 2, 4, 5, 6, 7 }, { 2, 0, 3, 1, 4, 5, 6, 7 },
};
static const uint8_t ggb81Data[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 2, 1, 3, 4, 6, 5, 7 }, { 0, 6, 5, 3, 4, 2, 1, 7 }, { 0, 5, 1, 3, 4, 2, 6, 7 },
	{ 0, 5, 2, 3, 4, 1, 6, 7 }, { 0, 2, 6, 3, 4, 5, 1, 7 }, { 0, 1, 6, 3, 4, 2, 5, 7 }, { 0, 2, 5, 3, 4, 6, 1, 7 },
};

static uint8_t reorder(uint8_t value, const uint8_t order[8]) {
	uint8_t result = 0;
	for (int i = 0; i < 8; ++i) {
		result |= (value >> order[i] & 1) << i;
	}
	return result;
}

static uint32_t crc32(const uint8_t* data, size_t size) {
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) {
			crc = crc >> 1 ^ (0xEDB88320 & -(crc & 1));
		}
	}
	return ~crc;
}

// Which variant a cartridge is, by its second logo; a "fixed" dump (its last bank-0 byte 1) is plain.
static uint8_t variantOf(const struct GB* gb) {
	const uint8_t* rom = gb->rom;
	if (gb->romSize < 0x8000) {
		return GB_PLAIN;
	}
	// Vast Fame's logos ("V.fame", "SOUL", "DIGI" in italics) by the sum of their bytes (hhugboy).
	unsigned sum = 0;
	for (int i = 0x184; i < 0x1B4; ++i) {
		sum += rom[i];
	}
	if (sum == 4844 || sum == 6127 || sum == 4406) {
		return GB_VAST_FAME;
	}
	switch (crc32(&rom[0x184], 0x30)) {
	case 0x4FDAB691:
		return GB_HITEK;
	case 0xC7D8C1DF:
	case 0x6D1EA662:
		return rom[0x7FFF] != 0x01 ? GB_BBD : GB_PLAIN;
	case 0x79F34594:
	case 0x7E8C539B:
		return GB_GGB81;
	case 0x20D092E2:
	case 0xD2B57657:
		return rom[0x147] == 0x01 || (0x8000u << rom[0x148]) != gb->romSize ? GB_LI_CHENG : GB_PLAIN;
	case 0x6C1DCF2D:
	case 0x99E3449D:
		// Sintax cartridges hold up to 2 MiB; larger dumps of them are already unscrambled.
		return rom[0x7FFF] != 0x01 && gb->romSize <= 0x200000 ? GB_SINTAX : GB_PLAIN;
	default:
		return GB_PLAIN;
	}
}

// What the switchable bank's bytes read as: in a data bit order, or XORed with Sintax's key for the
// bank written.
static void romScramble(struct GBCart* cart) {
	const uint8_t* order = NULL;
	uint8_t key = 0;
	switch (cart->variant) {
	case GB_SINTAX:
		key = cart->xorKeys[cart->bankWritten & 3];
		break;
	case GB_BBD:
		order = bbdData[cart->dataOrder];
		break;
	case GB_HITEK:
		order = hitekData[cart->dataOrder];
		break;
	case GB_GGB81:
		order = ggb81Data[cart->dataOrder];
		break;
	default:
		break;
	}
	cart->romTransformed = key || (order && memcmp(order, bbdData[0], 8));
	for (int i = 0; i < 256; ++i) {
		cart->romLut[i] = (order ? reorder(i, order) : i) ^ key;
	}
}

// Vast Fame's configuration writes ($6000-$7FFF, address bits 4-11 ignored). $96 at $7000 starts
// configuring and at $700F ends it; in between, each write to $6000 or $7000-$700A turns a running
// value right a bit and XORs it with the value written, and stores it. $7000 then sets the bytes given
// out (how many, then where, and the bytes) and $7008 what covers bank 0.
static bool vastFameSensible(const uint8_t at[11]) {
	unsigned length = at[0] & 7;
	uint16_t address = at[2] << 8 | at[1];
	return length < 4 || (at[3] == 0 ? address < 0x4000 : address >= 0x4000 && address < 0x8000);
}

static void vastFameWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBVastFame* vf = &gb->cart.vf;
	uint16_t at = address & 0xF00F;
	if (at == 0x7000 && value == 0x96) {
		vf->config = true;
		vf->running[0] = 0x00;
		vf->running[1] = 0x10;
		return;
	}
	if (at == 0x700F && value == 0x96) {
		vf->config = false;
		return;
	}
	if (!vf->config || !(at == 0x6000 || (at >= 0x7000 && at <= 0x700A))) {
		return;
	}
	for (int k = 0; k < 2; ++k) {
		vf->running[k] = (uint8_t) (vf->running[k] >> 1 | vf->running[k] << 7) ^ value;
		if (at == 0x6000) {
			vf->at6000[k] = vf->running[k];
		} else {
			vf->at7000[k][at - 0x7000] = vf->running[k];
		}
	}
	if (at == 0x7000) {
		// The starting value is the one whose bytes are given out somewhere they can be read.
		if (vf->which < 0) {
			vf->which = !vastFameSensible(vf->at7000[0]) && vastFameSensible(vf->at7000[1]);
		}
		const uint8_t* stored = vf->at7000[vf->which];
		unsigned length = stored[0] & 7;
		vf->length = length >= 4 ? length - 3 : 0;
		vf->left = 0;
		vf->bank = stored[3];
		vf->address = stored[2] << 8 | stored[1];
		memcpy(vf->bytes, &stored[4], 4);
	} else if (at == 0x7008) {
		int which = vf->which < 0 ? 0 : vf->which;
		const uint8_t* stored = vf->at7000[which];
		vf->replaceFrom = stored[10] << 8 | stored[9];
		vf->replaceBank = vf->at6000[which];
		vf->replace = (stored[8] & 0xF) == 0xF;
	}
	gbMapRom(gb);
}

uint8_t gbCartReadVastFame(struct GB* gb, uint16_t address) {
	struct GBVastFame* vf = &gb->cart.vf;
	bool waited = vf->bank == 0 ? address < 0x4000 : vf->bank == gb->cart.romBank && address >= 0x4000;
	if (waited && address == vf->address && !vf->left && vf->length) {
		vf->left = vf->length;
		gbMapRom(gb);
	}
	if (vf->left) {
		uint8_t value = vf->bytes[vf->length - vf->left];
		if (!--vf->left) {
			gbMapRom(gb);
		}
		return value;
	}
	if (vf->replace && address >= vf->replaceFrom && address < 0x4000) {
		return gb->rom[(((size_t) vf->replaceBank << 14) & (gb->romSize - 1)) + address];
	}
	return address < 0x4000 ? gb->memMap[address >> 12][address & 0xFFF] : gbCartRomByte(gb, gb->memMap[address >> 12][address & 0xFFF]);
}

// An unlicensed variant's writes, before MBC5 sees them: false when MBC5 doesn't.
static bool variantWrite(struct GB* gb, uint16_t address, uint8_t* value) {
	struct GBCart* cart = &gb->cart;
	switch (cart->variant) {
	case GB_SINTAX:
		if (address >= 0x2000 && address < 0x3000) {
			cart->bankWritten = *value;
			*value = reorder(*value, sintaxOrders[cart->bankOrder]);
		} else if ((address & 0xF0F0) == 0x5010) {
			// The bank order, then the bank written again in it.
			cart->bankOrder = *value & 0xF;
			gbCartWrite(gb, 0x2000, cart->bankWritten);
			return false;
		} else if (address >= 0x7000 && (address & 0xF0) >= 0x20 && (address & 0xF0) <= 0x50) {
			cart->xorKeys[((address & 0xF0) >> 4) - 2] = *value;
		}
		romScramble(cart);
		return true;
	case GB_BBD:
	case GB_HITEK:
		switch (address & 0xF0FF) {
		case 0x2000:
			*value = reorder(*value, (cart->variant == GB_BBD ? bbdBanks : hitekBanks)[cart->bankOrder]);
			break;
		case 0x2001:
			cart->dataOrder = *value & 7;
			break;
		case 0x2080:
			cart->bankOrder = *value & 7;
			break;
		}
		romScramble(cart);
		return true;
	case GB_GGB81:
		if ((address & 0xF0FF) == 0x2001) {
			cart->dataOrder = *value & 7;
		}
		romScramble(cart);
		return true;
	case GB_LI_CHENG:
		return !(address > 0x2100 && address < 0x3000);
	case GB_VAST_FAME:
		if (address < 0x6000) {
			return true;
		}
		vastFameWrite(gb, address, *value);
		return false;
	default:
		return true;
	}
}

bool gbCartInit(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	const uint8_t* rom = gb->rom;
	memset(cart, 0, sizeof(*cart));
	cart->rtc.lastSync = gb->now;
	cart->rtc.lastHost = gb->hostMicros;
	// Cartridge RAM with no save yet reads as all 1s, as other emulators start it.
	memset(gb->sram, 0xFF, sizeof(gb->sram));
	uint8_t type = rom[0x147];
	cart->variant = variantOf(gb);
	if (cart->variant == GB_HITEK) {
		cart->bankOrder = cart->dataOrder = 7;
	} else if (cart->variant == GB_SINTAX) {
		cart->bankOrder = 0xF;
	}
	romScramble(cart);
	cart->vf.which = -1;
	switch (cart->variant != GB_PLAIN ? 0x1B : type) {
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
	// Sintax's cartridge RAM needs no enabling (mGBA).
	if (cart->variant == GB_SINTAX) {
		cart->ramEnabled = cart->hasRam;
	}
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
	if (cart->variant != GB_PLAIN) {
		bool mbc5 = variantWrite(gb, address, &value);
		// Its bytes may read differently now.
		gbMapRom(gb);
		if (!mbc5) {
			return;
		}
	}
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
			// Any write latches the clock (SameBoy's MBC3).
			if (cart->hasRtc) {
				gbRtcSync(gb);
				memcpy(cart->rtc.latched, cart->rtc.regs, sizeof(cart->rtc.latched));
			}
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

// MBC3's clock (Pan Docs, "MBC3"; SameBoy): registers 6, 6, 5, 8 and 3 bits wide. A field past its
// range counts up to its last value and wraps to 0 without a carry; past 511 days the day counter wraps
// and sets the carry bit, which stays until written. Writing the seconds restarts the second.
static const uint8_t rtcMasks[5] = { 0x3F, 0x3F, 0x1F, 0xFF, 0xC1 };

static bool rtcSelected(const struct GB* gb) {
	const struct GBCart* cart = &gb->cart;
	return cart->mapper == GB_MBC3 && cart->hasRtc && cart->ramBank >= 0x08 && cart->ramBank <= 0x0C;
}

static void rtcAdvance(struct GBRtc* rtc, uint64_t seconds) {
	static const uint8_t limits[3] = { 60, 60, 24 };
	static const uint8_t ends[3] = { 64, 64, 32 };
	uint64_t carry = seconds;
	for (int i = 0; i < 3 && carry; ++i) {
		uint8_t* value = &rtc->regs[i];
		if (*value >= limits[i]) {
			if (carry < (uint64_t) (ends[i] - *value)) {
				*value += carry;
				return;
			}
			carry -= ends[i] - *value;
			*value = 0;
		}
		uint64_t total = *value + carry;
		*value = total % limits[i];
		carry = total / limits[i];
	}
	if (!carry) {
		return;
	}
	uint64_t days = ((rtc->regs[4] & 1) << 8 | rtc->regs[3]) + carry;
	if (days > 511) {
		rtc->regs[4] |= 0x80;
	}
	rtc->regs[3] = days & 0xFF;
	rtc->regs[4] = (rtc->regs[4] & 0xFE) | (days >> 8 & 1);
}

void gbRtcSync(struct GB* gb) {
	struct GBRtc* rtc = &gb->cart.rtc;
	uint64_t ticks;
	if (gb->rtcRealTime) {
		int64_t micros = gb->hostMicros - rtc->lastHost;
		ticks = micros > 0 ? (uint64_t) micros * GB_RTC_SECOND / 1000000 : 0;
	} else {
		uint64_t parts = (uint64_t) (uint32_t) (gb->now - rtc->lastSync) * gb->rtcPercent + rtc->fraction;
		ticks = parts / 100;
		rtc->fraction = parts % 100;
	}
	rtc->lastSync = gb->now;
	rtc->lastHost = gb->hostMicros;
	if (rtc->regs[4] & 0x40) {
		return;
	}
	ticks += rtc->ticks;
	rtc->ticks = ticks % GB_RTC_SECOND;
	rtcAdvance(rtc, ticks / GB_RTC_SECOND);
}

void gbRtcGet(struct GB* gb, uint32_t r[11]) {
	struct GBRtc* rtc = &gb->cart.rtc;
	gbRtcSync(gb);
	for (int i = 0; i < 5; ++i) {
		r[4 - i] = rtc->regs[i];
		r[10 - i] = rtc->latched[i];
	}
	r[5] = rtc->ticks;
}

void gbRtcSet(struct GB* gb, const uint32_t r[11]) {
	struct GBRtc* rtc = &gb->cart.rtc;
	for (int i = 0; i < 5; ++i) {
		rtc->regs[i] = r[4 - i] & rtcMasks[i];
		rtc->latched[i] = r[10 - i] & rtcMasks[i];
	}
	rtc->ticks = r[5] % GB_RTC_SECOND;
	rtc->fraction = 0;
	rtc->lastSync = gb->now;
	rtc->lastHost = gb->hostMicros;
}

void gbRtcSetSpeed(struct GB* gb, unsigned percent) {
	gbRtcSync(gb);
	gb->rtcPercent = percent;
}

void gbRtcSetRealTime(struct GB* gb, bool on) {
	gbRtcSync(gb);
	gb->rtcRealTime = on;
	gb->cart.rtc.lastHost = gb->hostMicros;
}

uint8_t gbCartReadRam(struct GB* gb, uint16_t address) {
	if (rtcSelected(gb)) {
		return gb->cart.ramEnabled ? gb->cart.rtc.latched[gb->cart.ramBank - 0x08] : 0xFF;
	}
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
	if (rtcSelected(gb)) {
		if (gb->cart.ramEnabled) {
			struct GBRtc* rtc = &gb->cart.rtc;
			unsigned reg = gb->cart.ramBank - 0x08;
			gbRtcSync(gb);
			rtc->regs[reg] = value & rtcMasks[reg];
			if (reg == 0) {
				rtc->ticks = 0;
				rtc->fraction = 0;
			}
		}
		return;
	}
	int32_t offset = ramOffset(gb, address);
	if (offset >= 0) {
		gb->sram[offset] = gb->cart.mapper == GB_MBC2 ? value & 0xF : value;
	}
}
