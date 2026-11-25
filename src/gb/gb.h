/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#ifndef ARMGB_GB_H
#define ARMGB_GB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// armGB: the Game Boy and Game Boy Color. Everything the machine is lives in struct GB, so a state
// is the struct itself; pointers into it are set again from offsets when a state loads.
//
// Time is counted in dots, the PPU's 4.194304 MHz clock, which no speed switch changes. A CPU
// M-cycle is 4 dots, or 2 in the Color's double speed. Times are 32-bit and compared by their
// difference, so they may wrap.

enum {
	GB_WIDTH = 160,
	GB_HEIGHT = 144,
	GB_LINE_DOTS = 456,
	GB_LINES = 154,
	GB_FRAME_DOTS = GB_LINE_DOTS * GB_LINES,
	GB_SIZE_VRAM = 0x4000,
	GB_SIZE_WRAM = 0x8000,
	GB_SIZE_OAM = 0xA0,
	GB_SIZE_HRAM = 0x7F,
	GB_SAMPLE_RATE = 32768,
	// What the frame holds where the LCD shows nothing (off, or its first frame on), besides the shades
	// 0 (lightest) to 3.
	GB_SHADE_OFF = 4,
};

// The interrupt flags.
enum {
	GB_IRQ_VBLANK = 1,
	GB_IRQ_STAT = 2,
	GB_IRQ_TIMER = 4,
	GB_IRQ_SERIAL = 8,
	GB_IRQ_JOYPAD = 16,
};

// The buttons, as libretro numbers them.
enum {
	GB_KEY_A = 1,
	GB_KEY_B = 2,
	GB_KEY_SELECT = 4,
	GB_KEY_START = 8,
	GB_KEY_RIGHT = 16,
	GB_KEY_LEFT = 32,
	GB_KEY_UP = 64,
	GB_KEY_DOWN = 128,
};

enum GBModel {
	GB_MODEL_DMG,
	GB_MODEL_CGB,
};

enum GBMapper {
	GB_MBC_NONE,
	GB_MBC1,
	GB_MBC2,
	GB_MBC3,
	GB_MBC5,
};

// The events, each at a time; the earliest decides when the CPU next stops to run them.
enum {
	GB_EVENT_PPU,
	GB_EVENT_TIMER,
	GB_EVENT_SERIAL,
	GB_EVENT_FRAME,
	GB_EVENT_DMA,
	GB_EVENT_HDMA,
	GB_EVENT_SPEED,
	GB_EVENTS,
};

struct GBCpu {
	uint8_t a, f, b, c, d, e, h, l;
	uint16_t sp, pc;
	bool ime;
	// EI takes effect after the instruction that follows it.
	bool imePending;
	bool halted;
	bool justHalted;
	// HALT with IME off and an interrupt already pending: the next byte is read twice.
	bool haltBug;
	bool stopped;
	bool locked;
	// A speed switch: 1 while the speed is about to change to double, 2 while halted until it is done
	// (an interrupt wakes it sooner).
	uint8_t switching;
};

struct GBTimer {
	// The 16-bit counter DIV is the top of, in CPU T-cycles, as of lastSync.
	uint16_t counter;
	uint32_t lastSync;
	uint8_t tima, tma, tac;
	// TIMA overflowed: it reads 0 until reloadAt, when it takes TMA and the interrupt is requested.
	bool reloading;
	uint32_t reloadAt;
	// When it last took TMA: a write to TIMA in that M-cycle is lost, and one to TMA lands in TIMA.
	uint32_t reloadedAt;
};

struct GBPpu {
	// The line the PPU is on (LY is the register, which changes a few dots into a line), when the line
	// started, and the step it takes next, at nextAt.
	uint8_t line;
	uint8_t ly;
	uint8_t step;
	uint32_t lineStart;
	uint32_t lineEnd;
	uint32_t drawStart;
	uint32_t nextAt;
	uint16_t mode3Length;
	// The objects on the line, found at mode 3's start, in OAM order: at most ten.
	uint8_t objectCount;
	uint8_t objects[10];
	uint8_t lcdc, stat, scy, scx, lyc, bgp, obp0, obp1, wy, wx, dma;
	// The mode STAT shows, and the one its interrupt line goes by (-1: none), which differ for a dot
	// at some edges.
	uint8_t mode;
	int8_t modeForInterrupt;
	// The LY that LYC is compared with: -1 while LY changes.
	int16_t lyCompare;
	bool coincidence;
	bool lycLine;
	// The STAT interrupt line: a request goes out when it rises.
	bool statLine;
	// The window's own line counter, and whether LY has matched WY this frame.
	uint8_t windowLine;
	bool windowSeen;
	// The line being shown just after the LCD is switched on, which has no OAM scan.
	bool firstLine;
	// The frame the LCD was switched on in, which the screen doesn't show.
	bool hiddenFrame;
	// When the CPU is locked out of OAM and VRAM, which isn't quite when STAT's mode says.
	bool oamReadBlocked, oamWriteBlocked, vramReadBlocked, vramWriteBlocked;
};

struct GBSerial {
	uint8_t sb, sc;
	uint8_t bits;
	uint32_t nextBit;
};

// A sound channel. The timer counts dots to the next step of the waveform; length, envelope and
// sweep are clocked by the frame sequencer.
struct GBChannel {
	bool on;
	bool dac;
	bool lengthOn;
	uint16_t length;
	uint16_t frequency;
	uint32_t nextStep;
	uint8_t position;
	uint8_t volume;
	uint8_t envelopeTimer;
};

struct GBApu {
	bool power;
	struct GBChannel square[2];
	struct GBChannel wave;
	struct GBChannel noise;
	uint16_t lfsr;
	// The sweep (channel 1): its shadow frequency, timer, and whether it runs.
	uint16_t sweepShadow;
	uint8_t sweepTimer;
	bool sweepOn;
	bool sweepNegated;
	// The wave channel's sample buffer, and when it last read wave RAM.
	uint8_t waveSample;
	uint32_t waveReadAt;
	// The frame sequencer's next step (0 to 7).
	uint8_t frameStep;
	uint8_t regs[0x30];
	uint32_t lastSync;
	// Sample building: the time the sample being built starts, and its mixed sums so far.
	uint32_t sampleStart;
	int32_t partialLeft, partialRight;
};

// OAM DMA (Pan Docs, "OAM DMA Transfer"): written at start, an M-cycle's wait, then a byte an M-cycle.
struct GBDma {
	bool active;
	uint16_t source;
	uint32_t start;
	uint8_t done;
	// Restarted while moving bytes: the old transfer keeps OAM locked while the new one waits.
	bool overlap;
};

// The Game Boy Color's own hardware (Pan Docs, "CGB Registers").
struct GBCgb {
	// Palette RAM, 8 palettes of 4 colors as little-endian RGB555, and the index registers.
	uint8_t bgPalette[64];
	uint8_t objPalette[64];
	uint8_t bcps, ocps;
	// Objects by OAM position (0) or by X (1, as the original Game Boy).
	uint8_t opri;
	uint8_t key0;
	uint8_t undocumented[4];
	uint8_t rp;
	// VRAM DMA: where it reads and writes, the 16-byte blocks left, and whether it waits for H-blanks.
	uint16_t hdmaSource, hdmaDest;
	uint8_t hdmaBlocks;
	bool hdmaHblank;
};

struct GBCart {
	uint8_t mapper;
	bool hasRam, hasBattery, hasRtc;
	bool ramEnabled;
	uint16_t romBank;
	uint8_t ramBank;
	uint8_t mbc1Mode;
	uint8_t mbc1Low;
	uint8_t mbc1Upper;
	// MBC1 multicart (MBC1M): the upper bits bank by 16 banks, not 32.
	bool mbc1Multicart;
	uint32_t romBanks;
	uint32_t ramSize;
};

struct GB {
	struct GBCpu cpu;
	uint8_t model;
	bool cgbMode;
	bool doubleSpeed;
	// Dots per M-cycle: 4, or 2 at double speed.
	uint8_t mcycle;
	uint32_t now;
	uint32_t events[GB_EVENTS];
	uint32_t eventsOn;
	uint32_t nextEvent;
	uint8_t ie, ifl;
	uint8_t joypadSelect;
	uint8_t keys;
	struct GBTimer timer;
	struct GBPpu ppu;
	struct GBSerial serial;
	struct GBCart cart;
	struct GBDma dma;
	struct GBApu apu;
	struct GBCgb cgb;
	bool bootMapped;
	uint8_t wramBank;
	uint8_t vramBank;
	uint8_t vram[GB_SIZE_VRAM];
	uint8_t wram[GB_SIZE_WRAM];
	uint8_t oam[GB_SIZE_OAM];
	uint8_t hram[GB_SIZE_HRAM];
	uint8_t io[0x80];
	uint8_t sram[0x20000];
	uint16_t frame[GB_HEIGHT][GB_WIDTH];
	// The frame's sound, left and right interleaved, at GB_SAMPLE_RATE: the frontend takes it.
	int16_t audio[4096];
	uint32_t audioCount;
	// For each sample pair, whether any channel's DAC was on (the console's output capacitor is
	// connected only then).
	uint8_t audioDacs[2048];
	uint32_t frameCount;
	// Set when V-blank starts, so a frame's run can end there.
	bool frameEnded;
	// Set when the CPU must look again before its next instruction: an interrupt requested or enabled,
	// HALT, STOP, EI, RETI, a lock-up or the frame's end.
	bool attention;

	// Not saved: the cartridge and where it is mapped, and the frontend's hooks.
	const uint8_t* rom;
	size_t romSize;
	const uint8_t* boot;
	size_t bootSize;
	// Where each 4 KiB of ROM and work RAM is; readMap is the same, with NULL where reads must go
	// through gbRead (the boot ROM, OAM DMA).
	const uint8_t* memMap[16];
	const uint8_t* readMap[16];
	// A frontend's picture, when it wants colors drawn there instead of shades in frame: the four
	// shades' colors, then the screen's with the LCD off.
	uint32_t* video;
	uint32_t videoColors[5];
	// With a frontend's picture, the Color's 32768 RGB555 colors as it shows them.
	const uint32_t* videoTable;
	// Serial bytes shifted out with no partner, for the test runner.
	void (*serialOut)(void* user, uint8_t byte);
	void* user;
};

// gb.c
bool gbLoad(struct GB*, const uint8_t* rom, size_t size, enum GBModel);
void gbReset(struct GB*);
void gbRunFrame(struct GB*);
void gbSchedule(struct GB*, int event, uint32_t at);
void gbUnschedule(struct GB*, int event);
void gbRunEvents(struct GB*);
void gbRequest(struct GB*, uint8_t irq);
void gbStop(struct GB*);
void gbSpeedEvent(struct GB*);
void gbBlankFrame(struct GB*);
size_t gbStateSize(void);
void gbSaveState(const struct GB*, void* data);
bool gbLoadState(struct GB*, const void* data, size_t size);

// cpu.c
void gbCpuRun(struct GB*, uint32_t until);

// memory.c
void gbDmaSync(struct GB*);
void gbDmaStart(struct GB*, uint8_t page);
uint8_t gbRead(struct GB*, uint16_t address);
void gbWrite(struct GB*, uint16_t address, uint8_t value);
void gbMapRom(struct GB*);
uint8_t gbIoRead(struct GB*, uint8_t reg);
void gbIoWrite(struct GB*, uint8_t reg, uint8_t value);

// timer.c
void gbTimerSync(struct GB*);
uint8_t gbTimerRead(struct GB*, uint8_t reg);
void gbTimerWrite(struct GB*, uint8_t reg, uint8_t value);
void gbTimerEvent(struct GB*);

// ppu.c
void gbPpuReset(struct GB*);
void gbPpuSync(struct GB*);
uint8_t gbPpuRead(struct GB*, uint8_t reg);
void gbPpuWrite(struct GB*, uint8_t reg, uint8_t value);
void gbPpuEvent(struct GB*);
void gbPpuReplan(struct GB*);
void gbPpuFreeze(struct GB*, uint32_t dots);

// apu.c
void gbApuReset(struct GB*);
void gbApuSync(struct GB*);
void gbApuDivReset(struct GB*);
uint8_t gbApuRead(struct GB*, uint8_t reg);
uint8_t gbApuPcm(struct GB*, bool high);
void gbApuWrite(struct GB*, uint8_t reg, uint8_t value);

// serial.c
void gbSerialWrite(struct GB*, uint8_t reg, uint8_t value);
void gbSerialEvent(struct GB*);

// cgb.c
uint8_t gbCgbRead(struct GB*, uint8_t reg);
void gbCgbWrite(struct GB*, uint8_t reg, uint8_t value);
void gbHdmaHblank(struct GB*);
void gbHdmaEvent(struct GB*);

// cart.c
bool gbCartInit(struct GB*);
void gbCartWrite(struct GB*, uint16_t address, uint8_t value);
uint8_t gbCartReadRam(struct GB*, uint16_t address);
void gbCartWriteRam(struct GB*, uint16_t address, uint8_t value);

static inline int32_t gbSince(uint32_t now, uint32_t then) {
	return (int32_t) (now - then);
}

#endif
