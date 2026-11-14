// armGBA: stands in for SameBoy's gb.h under Super Junior SameDuck's laptop code (megaduck_laptop*.c),
// with only what that code uses. The serial port and the hooks to the rest of armGBA are in laptop.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "defs.h"

// No console output from a libretro core
#define printf(...) ((void) 0)
#include "megaduck_laptop.h"

enum { GB_ACCESSORY_NONE, GB_ACCESSORY_MEGADUCK_LAPTOP };

typedef void (*GB_serial_transfer_bit_start_callback_t)(GB_gameboy_t* gb, bool bit_received);
typedef bool (*GB_serial_transfer_bit_end_callback_t)(GB_gameboy_t* gb);

struct GB_gameboy_s {
	GB_megaduck_laptop_t megaduck_laptop;
	int accessory;
	GB_megaduck_laptop_set_time_callback megaduck_laptop_set_time_callback;
	GB_megaduck_laptop_get_time_callback megaduck_laptop_get_time_callback;
	bool use_megaduck_laptop_initial_sp;
	GB_serial_transfer_bit_start_callback_t serial_transfer_bit_start_callback;
	GB_serial_transfer_bit_end_callback_t serial_transfer_bit_end_callback;
	// Read only by logging that's compiled out
	unsigned serial_count, div_counter, pc;
	bool serial_master_clock;
};

#define GB_log(gb, ...) ((void) 0)

void GB_set_serial_transfer_bit_start_callback(GB_gameboy_t* gb, GB_serial_transfer_bit_start_callback_t callback);
void GB_set_serial_transfer_bit_end_callback(GB_gameboy_t* gb, GB_serial_transfer_bit_end_callback_t callback);
// The peripheral clocks one bit into the console's serial port
void GB_serial_set_data_bit(GB_gameboy_t* gb, bool data);
bool GB_serial_get_data_bit(GB_gameboy_t* gb);

// Hooks to the rest of armGBA (laptop.c)
bool duckLaptopRunCartInSlot(void);
void duckLaptopSpeak(int phrase);
void duckLaptopPrinted(const uint8_t* image, int width, int height);
