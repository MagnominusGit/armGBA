/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */

// gbsync dmg|cgb rom [max instructions]: armGB and SameBoy from power-on through armGBA's boot ROM, one
// instruction at a time, no input; stops at the first instruction whose registers or DIV counter differ.
// SameBoy's power-on RAM is copied in first. HALT, STOP and interrupts take different steps in each, so a
// difference one step on either side brings back together is not one.
#include "gb.h"
#include "../libretro/armgba-boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void sbLoad(int cgb, const uint8_t* boot, size_t bootSize, const uint8_t* rom, size_t romSize);
int sbStep(void); uint8_t sbRead(uint16_t); void sbRegs(uint16_t r[6]); int sbHalted(void); uint16_t sbDiv(void); int sbLine(void);
uint8_t sbIo(int); uint8_t sbIe(void); int sbIme(void); void sbApu(int on[4], int volume[4]); void sbSweep(int v[8]); int sbLfDiv(void);
const uint8_t* sbRam(void); size_t sbRamSize(void); const uint8_t* sbHram(void); const uint8_t* sbOam(void);
const uint8_t* sbSram(void); size_t sbSramSize(void); int sbDouble(void);
static struct GB gb;
static void regsA(uint16_t r[6]) {
	r[0] = gb.cpu.a << 8 | gb.cpu.f; r[1] = gb.cpu.b << 8 | gb.cpu.c; r[2] = gb.cpu.d << 8 | gb.cpu.e; r[3] = gb.cpu.h << 8 | gb.cpu.l; r[4] = gb.cpu.sp; r[5] = gb.cpu.pc;
}
// Steps until the CPU is at an instruction it will run (not halted).
static void stepA(void) {
	uint16_t before = gb.cpu.pc; uint32_t t = gb.now;
	do {
		gb.frameEnded = false;
		gbCpuRun(&gb, gb.now + 1);
	} while (gb.cpu.halted || gb.cpu.stopped || (gb.cpu.pc == before && gb.now == t));
}
// After a HALT, armGB's step wakes and runs the next instruction too.
static void stepB(void) {
	bool slept = false, ime = false;
	uint16_t r[6]; sbRegs(r);
	bool stop = sbRead(r[5]) == 0x10;
	do { sbStep(); if (sbHalted() && !slept) { slept = true; ime = sbIme() || stop; } } while (sbHalted());
	// Woken into an interrupt, both stop at its vector.
	if (slept && !ime) sbStep();
}
int main(int argc, char** argv) {
	int cgb = !strcmp(argv[1], "cgb");
	FILE* f = fopen(argv[2], "rb"); static uint8_t rom[0x800000]; size_t n = fread(rom, 1, sizeof(rom), f); fclose(f);
	size_t p = 0x8000; while (p < n) p <<= 1;
	long max = argc > 3 ? atol(argv[3]) : 2000000000L;
	const uint8_t* boot = cgb ? armgba_cgb_boot : armgba_dmg_boot; size_t bootSize = cgb ? sizeof(armgba_cgb_boot) : sizeof(armgba_dmg_boot);
	sbLoad(cgb, boot, bootSize, rom, n);
	gb.boot = boot; gb.bootSize = bootSize;
	gbLoad(&gb, rom, p, cgb ? GB_MODEL_CGB : GB_MODEL_DMG);
	memcpy(gb.wram, sbRam(), sbRamSize() < sizeof(gb.wram) ? sbRamSize() : sizeof(gb.wram));
	memcpy(gb.hram, sbHram(), sizeof(gb.hram));
	memcpy(gb.oam, sbOam(), sizeof(gb.oam));
	if (sbSram()) memcpy(gb.sram, sbSram(), sbSramSize() < sizeof(gb.sram) ? sbSramSize() : sizeof(gb.sram));
	enum { H = 64 };
	uint16_t hist[H][6]; int divOffset = 0; bool divKnown = false;
	for (long i = 0; i < max; ++i) {
		stepA(); stepB();
		uint16_t a[6], b[6]; regsA(a); sbRegs(b);
		// The two step HALT, STOP and interrupts in different pieces: one more step on either side
		// that brings the registers together is the same run.
		for (int tries = 0; tries < 3 && memcmp(a, b, sizeof(a)); ++tries) {
			gbTimerSync(&gb);
			if ((int16_t) (gb.timer.counter - sbDiv()) < 0) {
				stepA(); regsA(a);
			} else {
				stepB(); sbRegs(b);
			}
		}
		gbTimerSync(&gb);
		int div = (int) (uint16_t) (gb.timer.counter - sbDiv());
		if (!divKnown && !gb.bootMapped) { divOffset = div; divKnown = true; }
		memcpy(hist[i % H], b, sizeof(b));
		bool regs = memcmp(a, b, sizeof(a)) != 0;
		bool timing = divKnown && div != divOffset;
		// STAT=1: the PPU too, LY and STAT's mode with the LCD on, after each instruction.
		bool ppu = false;
		uint8_t lyA = 0, statA = 0;
		if (getenv("STAT") && !gb.bootMapped && (gb.ppu.lcdc & 0x80) && (sbIo(0x40) & 0x80)) {
			gbPpuSync(&gb);
			lyA = gbPpuRead(&gb, 0x44); statA = gbPpuRead(&gb, 0x41);
			ppu = lyA != sbIo(0x44) || (statA & 3) != (sbIo(0x41) & 3);
		}
		// LFDIV=1: where the 1 MHz phase first differs.
		if (getenv("LFDIV") && gb.apu.power) {
			gbApuSync(&gb);
			int mine = !(((gb.apu.lastSync - gb.apu.epoch) >> 1) & 1), theirs = sbLfDiv();
			static int last = -1;
			if (mine != theirs && last != 0) {
				printf("lf_div differs from instruction %ld pc %04x double %d: armGB %d SameBoy %d\n", i, a[5], gb.doubleSpeed, mine, theirs);
			}
			last = mine == theirs;
		}
		// PPUTRACE=n: from instruction n on, LY and STAT on each side and armGB's line and dot.
		if (getenv("PPUTRACE") && i >= atol(getenv("PPUTRACE"))) {
			gbPpuSync(&gb);
			printf("%ld pc %04x | armGB line %u dot %d LY %02x STAT %02x | SameBoy LY %02x STAT %02x\n", i, a[5], gb.ppu.line,
			       (int) (gb.now - gb.ppu.lineStart), gbPpuRead(&gb, 0x44), gbPpuRead(&gb, 0x41), sbIo(0x44), sbIo(0x41));
		}
		// SWEEP=n: from instruction n on, channel 1's sweep on each side.
		if (getenv("SWEEP") && i >= atol(getenv("SWEEP"))) {
			gbApuSync(&gb);
			int v[8];
			sbSweep(v);
			const struct GBApu* p = &gb.apu;
			printf("%ld pc %04x | armGB on %d f %03x sh %03x add %03x calc %d rel %d hold %d cd %d | SameBoy on %d f %03x sh %03x add %03x calc %d rel %d hold %d cd %d\n", i, a[5],
			       p->square[0].on, p->square[0].frequency, p->sweepShadow, p->sweepAddend, p->sweepCalcCountdown, p->sweepReloadTimer, p->restartHold, p->sweepCountdown,
			       v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
		}
		// APU=1: the sound channels too, which are on and their envelopes' volumes.
		bool apu = false;
		if (getenv("APU") && !gb.bootMapped) {
			gbApuSync(&gb);
			int on[4], vol[4];
			sbApu(on, vol);
			const struct GBChannel* ch[4] = { &gb.apu.square[0], &gb.apu.square[1], &gb.apu.wave, &gb.apu.noise };
			// A channel's start can lag a few cycles (SameBoy's delayed starts): only a difference that
			// lasts counts.
			static int lasting[4];
			for (int k = 0; k < 4; ++k) {
				bool differs = ch[k]->on != on[k] || (vol[k] >= 0 && on[k] && ch[k]->on && ch[k]->volume != vol[k]);
				lasting[k] = differs ? lasting[k] + 1 : 0;
				if (lasting[k] > 40) {
					printf("  channel %d: armGB on %d volume %d | SameBoy on %d volume %d; NRx0-NRx4 armGB", k + 1, ch[k]->on, ch[k]->volume, on[k], vol[k]);
					uint8_t base = (uint8_t[]) { 0x10, 0x15, 0x1A, 0x1F }[k];
					for (int r = 0; r < 5; ++r) printf(" %02x", gb.apu.regs[base + r - 0x10]);
					printf(" SameBoy");
					for (int r = 0; r < 5; ++r) printf(" %02x", sbIo(base + r));
					printf("\n");
					apu = true;
				}
			}
		}
		if (regs || timing || ppu || apu) {
			printf("instruction %ld differs (%s), frame %u\n", i, regs ? "registers" : timing ? "DIV" : ppu ? "PPU" : "APU", gb.frameCount);
			printf("  armGB LY %02x STAT %02x line %u dot %d | SameBoy LY %02x STAT %02x\n", lyA, statA, gb.ppu.line, (int) (gb.now - gb.ppu.lineStart), sbIo(0x44), sbIo(0x41));
			for (int k = 24; k >= 1; --k) printf("  sameboy pc %04x af %04x\n", hist[(i - k + H) % H][5], hist[(i - k + H) % H][0]);
			printf("  armGB   af %04x bc %04x de %04x hl %04x sp %04x pc %04x  div %04x line %u double %d\n", a[0], a[1], a[2], a[3], a[4], a[5], gb.timer.counter, gb.ppu.line, gb.doubleSpeed);
			printf("  SameBoy af %04x bc %04x de %04x hl %04x sp %04x pc %04x  div %04x line %d double %d (offset %d now %d)\n", b[0], b[1], b[2], b[3], b[4], b[5], sbDiv(), sbLine(), sbDouble(), divOffset, div);
			printf("  armGB STAT reg %02x LYC %02x line %u dot %d | SameBoy STAT %02x LYC %02x\n", gb.ppu.stat, gb.ppu.lyc, gb.ppu.line, (int) (gb.now - gb.ppu.lineStart), sbIo(0x41), sbIo(0x45));
			if (getenv("OAM")) {
				printf("  OAM y,x:");
				for (int k = 0; k < 40; ++k) if (gb.oam[k * 4] && gb.oam[k * 4] < 160) printf(" %d:%d,%d", k, gb.oam[k * 4], gb.oam[k * 4 + 1]);
				printf("  SCX %d LCDC %02x WX %d WY %d\n", gb.ppu.scx, gb.ppu.lcdc, gb.ppu.wx, gb.ppu.wy);
			}
			printf("  armGB ie %02x if %02x ime %d | SameBoy ie %02x if %02x ime %d\n", gb.ie, gb.ifl, gb.cpu.ime, sbIe(), sbIo(0x0F), sbIme());
			return 1;
		}
	}
	printf("no difference in %ld instructions, frame %u\n", max, gb.frameCount);
}
