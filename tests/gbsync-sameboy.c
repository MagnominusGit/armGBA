/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */

// SameBoy's side of gbsync (built with SameBoy's Core, -DSAMEBOY_DIR).
#include <Core/gb.h>
#include <string.h>
#include <stdint.h>
static GB_gameboy_t gb;
static uint32_t pixels[256 * 224];
static uint32_t encode(GB_gameboy_t* g, uint8_t r, uint8_t g8, uint8_t b) { return r << 16 | g8 << 8 | b; }
static void quietLog(GB_gameboy_t* g, const char* s, GB_log_attributes_t a) {}
void sbLoad(int cgb, const uint8_t* boot, size_t bootSize, const uint8_t* rom, size_t romSize) {
	GB_init(&gb, cgb ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
	GB_set_rgb_encode_callback(&gb, encode);
	GB_set_color_correction_mode(&gb, GB_COLOR_CORRECTION_DISABLED);
	GB_set_pixels_output(&gb, pixels);
	GB_set_async_input_callback(&gb, NULL);
	GB_set_log_callback(&gb, quietLog);
	GB_set_turbo_mode(&gb, true, true);
	// The clock counts the console's time, as armGB's Game Time.
	GB_set_rtc_mode(&gb, GB_RTC_MODE_ACCURATE);
	GB_load_boot_rom_from_buffer(&gb, boot, bootSize);
	GB_load_rom_from_buffer(&gb, rom, romSize);
	GB_reset(&gb);
}
// One GB_run; returns its cycles.
int sbStep(void) { return GB_run(&gb); }
void sbRegs(uint16_t r[6]) { GB_registers_t* x = GB_get_registers(&gb); r[0] = x->af; r[1] = x->bc; r[2] = x->de; r[3] = x->hl; r[4] = x->sp; r[5] = x->pc; }
int sbHalted(void) { return gb.halted || gb.stopped; }
uint16_t sbDiv(void) { return gb.div_counter; }
int sbLine(void) { return gb.current_line; }
uint8_t sbIo(int i) { return gb.io_registers[i]; }
uint8_t sbIe(void) { return gb.interrupt_enable; }
int sbIme(void) { return gb.ime; }
const uint8_t* sbRam(void) { return gb.ram; }
size_t sbRamSize(void) { return gb.ram_size; }
const uint8_t* sbHram(void) { return gb.hram; }
const uint8_t* sbOam(void) { return gb.oam; }
const uint8_t* sbVram(void) { return gb.vram; }
const uint8_t* sbSram(void) { return gb.mbc_ram; }
size_t sbSramSize(void) { return gb.mbc_ram_size; }
int sbDouble(void) { return gb.cgb_double_speed; }
uint8_t sbRead(uint16_t a) { return GB_safe_read_memory(&gb, a); }
