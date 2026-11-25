/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// Pixels for a line. On the Color (gbExpandTiles): each tile's eight colour indices (a byte each,
// leftmost first) through its palette's four colours, and the indices with the tile's priority bit
// (4) beside them. On the original (gbShadeLine): a line's colour indices through its four shades.
// AVX2 on x86-64 where the CPU has it, NEON on ARM, else the plain loops, which they match exactly.

static void shadePlain(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count) {
	for (int x = 0; x < count; ++x) {
		out[x] = lut[index[x]];
	}
}

static void expandPlain(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count) {
	for (int t = 0; t < count; ++t) {
		uint8_t indices[8];
		memcpy(indices, &pixels[t], 8);
		for (int i = 0; i < 8; ++i) {
			line[t * 8 + i] = colors[t][indices[i]];
		}
		uint64_t withFront = pixels[t] | 0x0101010101010101ULL * front[t];
		memcpy(&info[t * 8], &withFront, 8);
	}
}

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>

#define AVX2 __attribute__((target("avx2")))

AVX2 static void expandAvx2(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count) {
	for (int t = 0; t < count; ++t) {
		__m256i palette = _mm256_castsi128_si256(_mm_loadu_si128((const __m128i*) colors[t]));
		__m256i indices = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*) &pixels[t]));
		_mm256_storeu_si256((__m256i*) &line[t * 8], _mm256_permutevar8x32_epi32(palette, indices));
		uint64_t withFront = pixels[t] | 0x0101010101010101ULL * front[t];
		memcpy(&info[t * 8], &withFront, 8);
	}
}

AVX2 static void shadeAvx2(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count) {
	__m256i palette = _mm256_castsi128_si256(_mm_loadu_si128((const __m128i*) lut));
	int x = 0;
	for (; x + 8 <= count; x += 8) {
		__m256i indices = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*) &index[x]));
		_mm256_storeu_si256((__m256i*) &out[x], _mm256_permutevar8x32_epi32(palette, indices));
	}
	shadePlain(out + x, index + x, lut, count - x);
}

static bool hasAvx2(void) {
#ifdef __AVX2__
	return true;
#else
	static int known;
	if (!known) {
		__builtin_cpu_init();
		known = __builtin_cpu_supports("avx2") ? 1 : -1;
	}
	return known > 0;
#endif
}

void gbShadeLine(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count) {
	if (hasAvx2()) {
		shadeAvx2(out, index, lut, count);
	} else {
		shadePlain(out, index, lut, count);
	}
}

void gbExpandTiles(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count) {
	if (hasAvx2()) {
		expandAvx2(line, info, pixels, colors, front, count);
	} else {
		expandPlain(line, info, pixels, colors, front, count);
	}
}

#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

// Each pixel's index picks four bytes of the palette: the index times 4, plus 0 to 3.
static inline uint8x16_t paletteBytes(uint8x8_t indices, int half) {
	static const uint8_t spread[2][16] = { { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 },
	                                       { 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7 } };
	static const uint8_t lanes[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 };
	uint8x8x2_t table = { { vshl_n_u8(indices, 2), vdup_n_u8(0) } };
	uint8x8_t low = vtbl2_u8(table, vld1_u8(spread[half]));
	uint8x8_t high = vtbl2_u8(table, vld1_u8(spread[half] + 8));
	return vaddq_u8(vcombine_u8(low, high), vld1q_u8(lanes));
}

// Eight pixels' colours from their indices (0-3) and a palette of four.
static inline void shadeEight(uint32_t* out, uint8x8_t indices, uint8x16_t palette) {
#if defined(__aarch64__)
	uint8x16_t first = vqtbl1q_u8(palette, paletteBytes(indices, 0));
	uint8x16_t second = vqtbl1q_u8(palette, paletteBytes(indices, 1));
#else
	uint8x8x2_t table = { { vget_low_u8(palette), vget_high_u8(palette) } };
	uint8x16_t a = paletteBytes(indices, 0), b = paletteBytes(indices, 1);
	uint8x16_t first = vcombine_u8(vtbl2_u8(table, vget_low_u8(a)), vtbl2_u8(table, vget_high_u8(a)));
	uint8x16_t second = vcombine_u8(vtbl2_u8(table, vget_low_u8(b)), vtbl2_u8(table, vget_high_u8(b)));
#endif
	vst1q_u8((uint8_t*) out, first);
	vst1q_u8((uint8_t*) (out + 4), second);
}

void gbShadeLine(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count) {
	uint8x16_t palette = vld1q_u8((const uint8_t*) lut);
	int x = 0;
	for (; x + 8 <= count; x += 8) {
		shadeEight(out + x, vld1_u8(&index[x]), palette);
	}
	shadePlain(out + x, index + x, lut, count - x);
}

void gbExpandTiles(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count) {
	for (int t = 0; t < count; ++t) {
		uint8x8_t indices = vreinterpret_u8_u64(vld1_u64(&pixels[t]));
		shadeEight(&line[t * 8], indices, vld1q_u8((const uint8_t*) colors[t]));
		vst1_u8(&info[t * 8], vorr_u8(indices, vdup_n_u8(front[t])));
	}
}

#else

void gbShadeLine(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count) {
	shadePlain(out, index, lut, count);
}

void gbExpandTiles(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count) {
	expandPlain(line, info, pixels, colors, front, count);
}

#endif

// The plain loops, for checking the others against.
void gbShadeLinePlain(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count) {
	shadePlain(out, index, lut, count);
}


void gbExpandTilesPlain(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count) {
	expandPlain(line, info, pixels, colors, front, count);
}
