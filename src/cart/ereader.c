/* Copyright (c) 2013-2020 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <stdlib.h>
#include <string.h>

// The e-Reader, as mGBA has it: a cartridge with a dot-code scanner. A card is drawn as the dots its
// strip would show, and the scanner hands the e-Reader's program one column of them at a time; the
// program decodes them itself.

#define EREADER_BLOCK_SIZE 40

enum {
	CONTROL0_DATA = 1,
	CONTROL0_CLOCK = 2,
	CONTROL0_DIRECTION = 4,
	CONTROL0_LED_ENABLE = 8,
	CONTROL0_SCAN = 16,
	CONTROL1_SCANLINE = 2,
};

enum {
	SERIAL_INACTIVE = 0,
	SERIAL_STARTING,
	SERIAL_BIT_0,
	SERIAL_END_BIT = SERIAL_BIT_0 + 8,
};

enum {
	COMMAND_IDLE = 0,
	COMMAND_WRITE_DATA = 1,
	COMMAND_SET_INDEX = 0x22,
	COMMAND_READ_DATA = 0x23,
};

static const int EREADER_NYBBLE_5BIT[16][5] = {
	{ 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 1 },
	{ 0, 0, 0, 1, 0 },
	{ 1, 0, 0, 1, 0 },
	{ 0, 0, 1, 0, 0 },
	{ 0, 0, 1, 0, 1 },
	{ 0, 0, 1, 1, 0 },
	{ 1, 0, 1, 1, 0 },
	{ 0, 1, 0, 0, 0 },
	{ 0, 1, 0, 0, 1 },
	{ 0, 1, 0, 1, 0 },
	{ 1, 0, 1, 0, 0 },
	{ 0, 1, 1, 0, 0 },
	{ 0, 1, 1, 0, 1 },
	{ 1, 0, 0, 0, 1 },
	{ 1, 0, 0, 0, 0 }
};

static const uint8_t EREADER_CALIBRATION_TEMPLATE[] = {
	0x43, 0x61, 0x72, 0x64, 0x2d, 0x45, 0x20, 0x52, 0x65, 0x61, 0x64, 0x65, 0x72, 0x20, 0x32, 0x30,
	0x30, 0x31, 0x00, 0x00, 0xcf, 0x72, 0x2f, 0x37, 0x3a, 0x3a, 0x3a, 0x38, 0x33, 0x30, 0x30, 0x37,
	0x3a, 0x39, 0x37, 0x35, 0x33, 0x2f, 0x2f, 0x34, 0x36, 0x36, 0x37, 0x36, 0x34, 0x31, 0x2d, 0x30,
	0x32, 0x34, 0x35, 0x35, 0x34, 0x30, 0x2a, 0x2d, 0x2d, 0x2f, 0x31, 0x32, 0x31, 0x2f, 0x29, 0x2a,
	0x2c, 0x2b, 0x2c, 0x2e, 0x2e, 0x2d, 0x18, 0x2d, 0x8f, 0x03, 0x00, 0x00, 0xc0, 0xfd, 0x77, 0x00,
	0x00, 0x00, 0x01
};

static const uint16_t EREADER_ADDRESS_CODES[] = {
	1023,
	1174,
	2628,
	3373,
	4233,
	6112,
	6450,
	7771,
	8826,
	9491,
	11201,
	11432,
	12556,
	13925,
	14519,
	16350,
	16629,
	18332,
	18766,
	20007,
	21379,
	21738,
	23096,
	23889,
	24944,
	26137,
	26827,
	28578,
	29190,
	30063,
	31677,
	31956,
	33410,
	34283,
	35641,
	35920,
	37364,
	38557,
	38991,
	40742,
	41735,
	42094,
	43708,
	44501,
	45169,
	46872,
	47562,
	48803,
	49544,
	50913,
	51251,
	53082,
	54014,
	54679
};

static const uint8_t DUMMY_HEADER_STRIP[2][0x10] = {
	{ 0x00, 0x30, 0x01, 0x01, 0x00, 0x01, 0x05, 0x10, 0x00, 0x00, 0x10, 0x13, 0x00, 0x00, 0x02, 0x00 },
	{ 0x00, 0x30, 0x01, 0x02, 0x00, 0x01, 0x08, 0x10, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x01, 0x00 }
};

static const uint8_t DUMMY_HEADER_FIXED[0x16] = {
	0x00, 0x00, 0x10, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x08, 0x4e, 0x49, 0x4e, 0x54, 0x45, 0x4e,
	0x44, 0x4f, 0x00, 0x22, 0x00, 0x09
};

static const uint8_t BLOCK_HEADER[2][0x18] = {
	{ 0x00, 0x02, 0x00, 0x01, 0x40, 0x10, 0x00, 0x1c, 0x10, 0x6f, 0x40, 0xda, 0x39, 0x25, 0x8e, 0xe0, 0x7b, 0xb5, 0x98, 0xb6, 0x5b, 0xcf, 0x7f, 0x72 },
	{ 0x00, 0x03, 0x00, 0x19, 0x40, 0x10, 0x00, 0x2c, 0x0e, 0x88, 0xed, 0x82, 0x50, 0x67, 0xfb, 0xd1, 0x43, 0xee, 0x03, 0xc6, 0xc6, 0x2b, 0x2c, 0x93 }
};

static const uint8_t RS_POW[] = {
	0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x87, 0x89, 0x95, 0xad, 0xdd, 0x3d, 0x7a, 0xf4,
	0x6f, 0xde, 0x3b, 0x76, 0xec, 0x5f, 0xbe, 0xfb, 0x71, 0xe2, 0x43, 0x86, 0x8b, 0x91, 0xa5, 0xcd,
	0x1d, 0x3a, 0x74, 0xe8, 0x57, 0xae, 0xdb, 0x31, 0x62, 0xc4, 0x0f, 0x1e, 0x3c, 0x78, 0xf0, 0x67,
	0xce, 0x1b, 0x36, 0x6c, 0xd8, 0x37, 0x6e, 0xdc, 0x3f, 0x7e, 0xfc, 0x7f, 0xfe, 0x7b, 0xf6, 0x6b,
	0xd6, 0x2b, 0x56, 0xac, 0xdf, 0x39, 0x72, 0xe4, 0x4f, 0x9e, 0xbb, 0xf1, 0x65, 0xca, 0x13, 0x26,
	0x4c, 0x98, 0xb7, 0xe9, 0x55, 0xaa, 0xd3, 0x21, 0x42, 0x84, 0x8f, 0x99, 0xb5, 0xed, 0x5d, 0xba,
	0xf3, 0x61, 0xc2, 0x03, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0x07, 0x0e, 0x1c, 0x38, 0x70, 0xe0,
	0x47, 0x8e, 0x9b, 0xb1, 0xe5, 0x4d, 0x9a, 0xb3, 0xe1, 0x45, 0x8a, 0x93, 0xa1, 0xc5, 0x0d, 0x1a,
	0x34, 0x68, 0xd0, 0x27, 0x4e, 0x9c, 0xbf, 0xf9, 0x75, 0xea, 0x53, 0xa6, 0xcb, 0x11, 0x22, 0x44,
	0x88, 0x97, 0xa9, 0xd5, 0x2d, 0x5a, 0xb4, 0xef, 0x59, 0xb2, 0xe3, 0x41, 0x82, 0x83, 0x81, 0x85,
	0x8d, 0x9d, 0xbd, 0xfd, 0x7d, 0xfa, 0x73, 0xe6, 0x4b, 0x96, 0xab, 0xd1, 0x25, 0x4a, 0x94, 0xaf,
	0xd9, 0x35, 0x6a, 0xd4, 0x2f, 0x5e, 0xbc, 0xff, 0x79, 0xf2, 0x63, 0xc6, 0x0b, 0x16, 0x2c, 0x58,
	0xb0, 0xe7, 0x49, 0x92, 0xa3, 0xc1, 0x05, 0x0a, 0x14, 0x28, 0x50, 0xa0, 0xc7, 0x09, 0x12, 0x24,
	0x48, 0x90, 0xa7, 0xc9, 0x15, 0x2a, 0x54, 0xa8, 0xd7, 0x29, 0x52, 0xa4, 0xcf, 0x19, 0x32, 0x64,
	0xc8, 0x17, 0x2e, 0x5c, 0xb8, 0xf7, 0x69, 0xd2, 0x23, 0x46, 0x8c, 0x9f, 0xb9, 0xf5, 0x6d, 0xda,
	0x33, 0x66, 0xcc, 0x1f, 0x3e, 0x7c, 0xf8, 0x77, 0xee, 0x5b, 0xb6, 0xeb, 0x51, 0xa2, 0xc3, 0x00,
};

static const uint8_t RS_REV[] = {
	0xff, 0x00, 0x01, 0x63, 0x02, 0xc6, 0x64, 0x6a, 0x03, 0xcd, 0xc7, 0xbc, 0x65, 0x7e, 0x6b, 0x2a,
	0x04, 0x8d, 0xce, 0x4e, 0xc8, 0xd4, 0xbd, 0xe1, 0x66, 0xdd, 0x7f, 0x31, 0x6c, 0x20, 0x2b, 0xf3,
	0x05, 0x57, 0x8e, 0xe8, 0xcf, 0xac, 0x4f, 0x83, 0xc9, 0xd9, 0xd5, 0x41, 0xbe, 0x94, 0xe2, 0xb4,
	0x67, 0x27, 0xde, 0xf0, 0x80, 0xb1, 0x32, 0x35, 0x6d, 0x45, 0x21, 0x12, 0x2c, 0x0d, 0xf4, 0x38,
	0x06, 0x9b, 0x58, 0x1a, 0x8f, 0x79, 0xe9, 0x70, 0xd0, 0xc2, 0xad, 0xa8, 0x50, 0x75, 0x84, 0x48,
	0xca, 0xfc, 0xda, 0x8a, 0xd6, 0x54, 0x42, 0x24, 0xbf, 0x98, 0x95, 0xf9, 0xe3, 0x5e, 0xb5, 0x15,
	0x68, 0x61, 0x28, 0xba, 0xdf, 0x4c, 0xf1, 0x2f, 0x81, 0xe6, 0xb2, 0x3f, 0x33, 0xee, 0x36, 0x10,
	0x6e, 0x18, 0x46, 0xa6, 0x22, 0x88, 0x13, 0xf7, 0x2d, 0xb8, 0x0e, 0x3d, 0xf5, 0xa4, 0x39, 0x3b,
	0x07, 0x9e, 0x9c, 0x9d, 0x59, 0x9f, 0x1b, 0x08, 0x90, 0x09, 0x7a, 0x1c, 0xea, 0xa0, 0x71, 0x5a,
	0xd1, 0x1d, 0xc3, 0x7b, 0xae, 0x0a, 0xa9, 0x91, 0x51, 0x5b, 0x76, 0x72, 0x85, 0xa1, 0x49, 0xeb,
	0xcb, 0x7c, 0xfd, 0xc4, 0xdb, 0x1e, 0x8b, 0xd2, 0xd7, 0x92, 0x55, 0xaa, 0x43, 0x0b, 0x25, 0xaf,
	0xc0, 0x73, 0x99, 0x77, 0x96, 0x5c, 0xfa, 0x52, 0xe4, 0xec, 0x5f, 0x4a, 0xb6, 0xa2, 0x16, 0x86,
	0x69, 0xc5, 0x62, 0xfe, 0x29, 0x7d, 0xbb, 0xcc, 0xe0, 0xd3, 0x4d, 0x8c, 0xf2, 0x1f, 0x30, 0xdc,
	0x82, 0xab, 0xe7, 0x56, 0xb3, 0x93, 0x40, 0xd8, 0x34, 0xb0, 0xef, 0x26, 0x37, 0x0c, 0x11, 0x44,
	0x6f, 0x78, 0x19, 0x9a, 0x47, 0x74, 0xa7, 0xc1, 0x23, 0x53, 0x89, 0xfb, 0x14, 0x5d, 0xf8, 0x97,
	0x2e, 0x4b, 0xb9, 0x60, 0x0f, 0xed, 0x3e, 0xe5, 0xf6, 0x87, 0xa5, 0x17, 0x3a, 0xa3, 0x3c, 0xb7,
};

static const uint8_t RS_GG[] = {
	0x00, 0x4b, 0xeb, 0xd5, 0xef, 0x4c, 0x71, 0x00, 0xf4, 0x00, 0x71, 0x4c, 0xef, 0xd5, 0xeb, 0x4b
};

static inline uint16_t le16(const uint8_t* p) {
	return p[0] | p[1] << 8;
}

static struct EReaderCards* cards(struct GBA* gba) {
	if (!gba->ereader.cards) {
		gba->ereader.cards = calloc(1, sizeof(struct EReaderCards));
	}
	return gba->ereader.cards;
}

void ereaderReset(struct GBA* gba) {
	struct EReader* e = &gba->ereader;
	memset(e->data, 0, sizeof(e->data));
	e->registerUnk = 0;
	e->registerReset = 4;
	e->control0 = 0;
	e->control1 = 0x80;
	e->led = 0;
	e->state = SERIAL_INACTIVE;
	e->activeRegister = 0;
}

void ereaderFree(struct GBA* gba) {
	struct EReaderCards* c = gba->ereader.cards;
	if (!c) {
		return;
	}
	for (int i = 0; i < EREADER_CARDS_MAX; ++i) {
		free(c->data[i]);
	}
	free(c);
	gba->ereader.cards = NULL;
}

// Every e-Reader is calibrated at the factory, into two sectors of its Flash; an empty save gets
// the settings a real one has.
void ereaderCalibrate(struct GBA* gba) {
	uint8_t* save = gba->savedata.data;
	if (!(gba->gpio.devices & HW_EREADER) || !save || gba->savedata.type != SAVEDATA_FLASH1M) {
		return;
	}
	for (uint32_t at = 0xD000; at <= 0xE000; at += 0x1000) {
		if (save[at] == 0xFF) {
			memset(&save[at], 0, 0x1000);
			memcpy(&save[at], EREADER_CALIBRATION_TEMPLATE, sizeof(EREADER_CALIBRATION_TEMPLATE));
			gba->savedata.dirty = true;
		}
	}
}

static void queueStrip(struct EReaderCards* c, const uint8_t* data, size_t size) {
	for (int i = 0; i < EREADER_CARDS_MAX; ++i) {
		if (c->data[i]) {
			continue;
		}
		c->data[i] = malloc(size);
		if (c->data[i]) {
			memcpy(c->data[i], data, size);
			c->size[i] = size;
		}
		return;
	}
}

// Cards for the next scans, up to EREADER_CARDS_MAX strips waiting. A file of several strips, raw
// or not, is one strip after another.
void ereaderQueueCard(struct GBA* gba, const void* data, size_t size) {
	static const size_t single[] = { 2912, 2112, 2076, 1872, 1344, 1308, 5456, 3520 };
	static const size_t strip[] = { 2912, 1872, 2112, 1344 };
	struct EReaderCards* c = cards(gba);
	if (!c || !size) {
		return;
	}
	size_t step = 0;
	for (size_t i = 0; i < sizeof(single) / sizeof(*single) && !step; ++i) {
		step = size == single[i] ? size : 0;
	}
	for (size_t i = 0; i < sizeof(strip) / sizeof(*strip) && !step; ++i) {
		step = size % strip[i] ? 0 : strip[i];
	}
	if (!step) {
		return;
	}
	for (size_t at = 0; at < size; at += step) {
		queueStrip(c, (const uint8_t*) data + at, step);
	}
}

void ereaderWrite(struct GBA* gba, uint32_t address, uint16_t value) {
	struct EReader* e = &gba->ereader;
	address &= 0x700FF;
	switch (address >> 17) {
	case 0:
		e->registerUnk = value & 0xF;
		break;
	case 1:
		e->registerReset = (value & 0x8A) | 4;
		if (value & 2) {
			ereaderReset(gba);
		}
		break;
	default:
		break;
	}
}

uint16_t ereaderRead(struct GBA* gba, uint32_t address) {
	struct EReader* e = &gba->ereader;
	address &= 0x700FF;
	switch (address >> 17) {
	case 0:
		return e->registerUnk;
	case 1:
		return e->registerReset;
	case 2:
		if (address > 0x40088) {
			return 0;
		}
		return e->data[(address & 0xFE) >> 1];
	default:
		return 0;
	}
}

static void anchor(uint8_t* origin) {
	static const uint8_t rows[5] = { 0x0E, 0x1F, 0x1F, 0x1F, 0x0E };
	for (int y = 0; y < 5; ++y) {
		for (int x = 0; x < 5; ++x) {
			if (rows[y] >> (4 - x) & 1) {
				origin[EREADER_DOTCODE_STRIDE * y + x] = 1;
			}
		}
	}
}

static void alignment(uint8_t* origin) {
	for (int x = 8; x <= 18; x += 2) {
		origin[x] = 1;
	}
	for (int x = 21; x <= 31; x += 2) {
		origin[x] = 1;
	}
}

static void blockAddress(uint8_t* origin, int a) {
	origin[EREADER_DOTCODE_STRIDE * 7 + 2] = 1;
	uint16_t addr = EREADER_ADDRESS_CODES[a];
	for (int i = 0; i < 16; ++i) {
		origin[EREADER_DOTCODE_STRIDE * (16 + i) + 2] = (addr >> (15 - i)) & 1;
	}
}

static void reedSolomon(const uint8_t* input, uint8_t* output) {
	uint8_t rsBuffer[64] = { 0 };
	for (int i = 0; i < 48; ++i) {
		rsBuffer[63 - i] = input[i];
	}
	for (int i = 0; i < 48; ++i) {
		unsigned z = RS_REV[rsBuffer[63 - i] ^ rsBuffer[15]];
		for (int j = 15; j >= 0; --j) {
			unsigned x = 0;
			if (j != 0) {
				x = rsBuffer[j - 1];
			}
			if (z != 0xFF) {
				unsigned y = RS_GG[j];
				if (y != 0xFF) {
					y += z;
					if (y >= 0xFF) {
						y -= 0xFF;
					}
					x ^= RS_POW[y];
				}
			}
			rsBuffer[j] = x;
		}
	}
	for (int i = 0; i < 16; ++i) {
		output[15 - i] = ~rsBuffer[i];
	}
}

// Draws a card's strip. It can come as the dots' raw data (with or without the error-correcting
// bytes, and with or without the strip's own header) or as a bitmap of the dots.
static void drawCard(uint8_t* dots, const uint8_t* cdata, size_t size) {
	memset(dots, 0, EREADER_DOTCODE_SIZE);

	uint8_t blockRS[44][0x10];
	uint8_t block0[0x30] = { 0 };
	bool parsed = false;
	bool bitmap = false;
	bool reducedHeader = false;
	size_t blocks;
	int base = 0;
	switch (size) {
	case 2076:
		memcpy(block0, DUMMY_HEADER_STRIP[1], sizeof(DUMMY_HEADER_STRIP[1]));
		reducedHeader = true;
		// Fall through.
	case 2112:
		parsed = true;
		// Fall through.
	case 2912:
		base = 25;
		blocks = 28;
		break;
	case 1308:
		memcpy(block0, DUMMY_HEADER_STRIP[0], sizeof(DUMMY_HEADER_STRIP[0]));
		reducedHeader = true;
		// Fall through.
	case 1344:
		parsed = true;
		// Fall through.
	case 1872:
		base = 1;
		blocks = 18;
		break;
	case 5456:
		bitmap = true;
		blocks = 124;
		break;
	case 3520:
		bitmap = true;
		blocks = 80;
		break;
	default:
		return;
	}

	size_t i;
	if (bitmap) {
		for (i = 0; i < 40; ++i) {
			const uint8_t* line = &cdata[(i + 2) * blocks];
			uint8_t* origin = &dots[EREADER_DOTCODE_STRIDE * i + 200];
			for (size_t x = 0; x < blocks; ++x) {
				uint8_t byte = line[x];
				if (x == 123) {
					byte &= 0xE0;
				}
				for (int b = 0; b < 8; ++b) {
					origin[x * 8 + b] = (byte >> (7 - b)) & 1;
				}
			}
		}
		return;
	}

	for (i = 0; i < blocks + 1; ++i) {
		uint8_t* origin = &dots[35 * i + 200];
		anchor(&origin[EREADER_DOTCODE_STRIDE * 0]);
		anchor(&origin[EREADER_DOTCODE_STRIDE * 35]);
		blockAddress(origin, base + i);
	}
	if (parsed) {
		if (reducedHeader) {
			memcpy(&block0[0x10], DUMMY_HEADER_FIXED, sizeof(DUMMY_HEADER_FIXED));
			block0[0x0D] = cdata[0x0];
			block0[0x0C] = cdata[0x1];
			block0[0x10] = cdata[0x2];
			block0[0x11] = cdata[0x3];
			for (int j = 0; j < 8; ++j) {
				block0[0x26 + j] = cdata[0x4 + j];
			}
			for (i = 0; i < 12; ++i) {
				block0[0x2E] ^= cdata[i];
			}
			unsigned dataChecksum = 0;
			for (i = 1; i < (size + 36) / 48; ++i) {
				const uint8_t* block = &cdata[i * 48 - 36];
				reedSolomon(block, blockRS[i]);
				unsigned fragmentChecksum = 0;
				for (int j = 0; j < 0x30; j += 2) {
					fragmentChecksum ^= block[j];
					fragmentChecksum ^= block[j + 1];
					dataChecksum += block[j] << 8 | block[j + 1];
				}
				block0[0x2F] += fragmentChecksum;
			}
			block0[0x13] = (~dataChecksum) >> 8;
			block0[0x14] = ~dataChecksum;
			for (i = 0; i < 0x2F; ++i) {
				block0[0x2F] += block0[i];
			}
			block0[0x2F] = ~block0[0x2F];
			reedSolomon(block0, blockRS[0]);
		} else {
			for (i = 0; i < size / 48; ++i) {
				reedSolomon(&cdata[i * 48], blockRS[i]);
			}
		}
	}
	size_t blockId = 0;
	size_t byteOffset = 0;
	for (i = 0; i < blocks; ++i) {
		uint8_t block[1040];
		uint8_t* origin = &dots[35 * i + 200];
		alignment(&origin[EREADER_DOTCODE_STRIDE * 2]);
		alignment(&origin[EREADER_DOTCODE_STRIDE * 37]);

		const uint8_t* blockData;
		uint8_t parsedBlockData[104];
		if (parsed) {
			memset(parsedBlockData, 0, sizeof(parsedBlockData));
			const uint8_t* header = BLOCK_HEADER[size == 1344 ? 0 : 1];
			parsedBlockData[0] = header[(2 * i) % 0x18];
			parsedBlockData[1] = header[(2 * i) % 0x18 + 1];
			for (int j = 2; j < 104; ++j) {
				if (byteOffset >= 0x40) {
					break;
				}
				if (byteOffset >= 0x30) {
					parsedBlockData[j] = blockRS[blockId][byteOffset - 0x30];
				} else if (!reducedHeader) {
					parsedBlockData[j] = cdata[blockId * 0x30 + byteOffset];
				} else if (blockId > 0) {
					parsedBlockData[j] = cdata[blockId * 0x30 + byteOffset - 36];
				} else {
					parsedBlockData[j] = block0[byteOffset];
				}
				++blockId;
				if (blockId * 0x30 >= size) {
					blockId = 0;
					++byteOffset;
				}
			}
			blockData = parsedBlockData;
		} else {
			blockData = &cdata[i * 104];
		}
		for (int b = 0; b < 104; ++b) {
			for (int k = 0; k < 5; ++k) {
				block[b * 10 + k] = EREADER_NYBBLE_5BIT[blockData[b] >> 4][k];
				block[b * 10 + 5 + k] = EREADER_NYBBLE_5BIT[blockData[b] & 0xF][k];
			}
		}

		int b = 0;
		for (int y = 0; y < 3; ++y) {
			memcpy(&origin[EREADER_DOTCODE_STRIDE * (4 + y) + 7], &block[b], 26);
			b += 26;
		}
		for (int y = 0; y < 26; ++y) {
			memcpy(&origin[EREADER_DOTCODE_STRIDE * (7 + y) + 3], &block[b], 34);
			b += 34;
		}
		for (int y = 0; y < 3; ++y) {
			memcpy(&origin[EREADER_DOTCODE_STRIDE * (33 + y) + 7], &block[b], 26);
			b += 26;
		}
	}
}

// Puts the next waiting card under the scanner, or none.
static void scanCard(struct GBA* gba) {
	struct EReaderCards* c = cards(gba);
	if (!c) {
		return;
	}
	if (c->loaded) {
		memset(c->dots, 0, sizeof(c->dots));
	}
	for (int i = 0; i < EREADER_CARDS_MAX; ++i) {
		if (!c->data[i]) {
			continue;
		}
		gba->ereader.scanX = -24;
		c->loaded = true;
		drawCard(c->dots, c->data[i], c->size[i]);
		free(c->data[i]);
		c->data[i] = NULL;
		c->size[i] = 0;
		break;
	}
}

// A line of the sensor: 20 words of 16 pixels, three to each of the strip's dots, the last word
// first. Until a card has been scanned, nothing is under it.
static void readData(struct GBA* gba) {
	struct EReader* e = &gba->ereader;
	memset(e->data, 0, EREADER_BLOCK_SIZE);
	struct EReaderCards* c = cards(gba);
	if (c && !c->loaded) {
		scanCard(gba);
	}
	if (c && c->loaded) {
		int y = e->scanY - 10;
		if (y >= 0 && y < 120) {
			const uint8_t* origin = &c->dots[EREADER_DOTCODE_STRIDE * (y / 3) + 16];
			for (int i = 0; i < 20; ++i) {
				uint16_t word = 0;
				int x = e->scanX + i * 16;
				for (int k = 0; k < 8; ++k) {
					word |= origin[(x + k) / 3] << (8 + k);
					word |= origin[(x + 8 + k) / 3] << k;
				}
				e->data[19 - i] = word;
			}
		}
	}
	e->control1 |= CONTROL1_SCANLINE;
	if (e->control0 & CONTROL0_LED_ENABLE) {
		uint16_t led = e->led * 2;
		if (led > 0x4000) {
			led = 0x4000;
		}
		gbaRaiseIrq(gba, IRQ_GAMEPAK, -(int32_t) led);
	}
}

// The serial port to the sensor is driven a pin at a time: a start condition, then bytes, each bit
// on the clock's falling edge.
static void writeControl0(struct GBA* gba, uint8_t value) {
	struct EReader* e = &gba->ereader;
	uint8_t control = value & 0x7F;
	uint8_t old = e->control0;
	bool oldClock = old & CONTROL0_CLOCK;
	bool oldData = old & CONTROL0_DATA;
	if (e->state == SERIAL_INACTIVE) {
		if (oldClock && oldData && !(control & CONTROL0_DATA)) {
			e->state = SERIAL_STARTING;
		}
	} else if (oldClock && !oldData && (control & CONTROL0_DATA)) {
		e->state = SERIAL_INACTIVE;
	} else if (e->state == SERIAL_STARTING) {
		if (oldClock && !oldData && !(control & CONTROL0_CLOCK)) {
			e->state = SERIAL_BIT_0;
			e->command = COMMAND_IDLE;
		}
	} else if (oldClock && !(control & CONTROL0_CLOCK)) {
		if (control & CONTROL0_DIRECTION) {
			e->byte |= (control & CONTROL0_DATA) << (7 - (e->state - SERIAL_BIT_0));
			++e->state;
			if (e->state == SERIAL_END_BIT) {
				switch (e->command) {
				case COMMAND_IDLE:
					e->command = e->byte;
					break;
				case COMMAND_SET_INDEX:
					e->activeRegister = e->byte;
					e->command = COMMAND_WRITE_DATA;
					break;
				case COMMAND_WRITE_DATA: {
					unsigned reg = e->activeRegister & 0x7F;
					// Register 0 and 0x57-0x5A are read-only; there are none past 0x5A.
					if (reg != 0 && reg < 0x57) {
						e->serial[reg] = e->byte;
					}
					++e->activeRegister;
					break;
				}
				default:
					break;
				}
				e->state = SERIAL_BIT_0;
				e->byte = 0;
			}
		} else if (e->command == COMMAND_READ_DATA) {
			int bit = 0;
			if ((e->activeRegister & 0x7F) < 0x5A) {
				bit = e->serial[e->activeRegister & 0x7F] >> (7 - (e->state - SERIAL_BIT_0));
			}
			control = (control & ~CONTROL0_DATA) | (bit & 1);
			++e->state;
			if (e->state == SERIAL_END_BIT) {
				++e->activeRegister;
			}
		}
	} else if (!(control & CONTROL0_DIRECTION)) {
		// The error bit clears.
		control &= ~CONTROL0_DATA;
	}
	e->control0 = control;
	if (!(old & CONTROL0_SCAN) && (control & CONTROL0_SCAN)) {
		if (e->scanX > 0) {
			scanCard(gba);
		}
		e->scanX = 0;
		e->scanY = 0;
	} else if ((control & CONTROL0_LED_ENABLE) && (control & CONTROL0_SCAN) && !(e->control1 & CONTROL1_SCANLINE)) {
		readData(gba);
	}
}

static void writeControl1(struct GBA* gba, uint8_t value) {
	struct EReader* e = &gba->ereader;
	uint8_t control = (value & 0x32) | 0x80;
	e->control1 = control;
	if ((e->control0 & CONTROL0_SCAN) && !(control & CONTROL1_SCANLINE)) {
		++e->scanY;
		if (e->scanY == (e->serial[0x15] | (e->serial[0x14] << 8))) {
			e->scanY = 0;
			if (e->scanX < 4050) {
				e->scanX += 210;
			}
		}
		readData(gba);
	}
}

void ereaderWriteFlash(struct GBA* gba, uint32_t address, uint8_t value) {
	struct EReader* e = &gba->ereader;
	switch (address & 0xFFFF) {
	case 0xFFB0:
		writeControl0(gba, value);
		break;
	case 0xFFB1:
		writeControl1(gba, value);
		break;
	case 0xFFB2:
		e->led = (e->led & 0xFF00) | value;
		break;
	case 0xFFB3:
		e->led = (e->led & 0x00FF) | value << 8;
		break;
	default:
		break;
	}
}

uint8_t ereaderReadFlash(struct GBA* gba, uint32_t address) {
	switch (address & 0xFFFF) {
	case 0xFFB0:
		return gba->ereader.control0;
	case 0xFFB1:
		return gba->ereader.control1;
	default:
		return 0;
	}
}
