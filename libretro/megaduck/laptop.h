/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Mega Duck laptops' keyboard, clock, printer and speech, on the serial port (see laptop.c)

#ifndef MEGADUCK_LAPTOP_GLUE_H
#define MEGADUCK_LAPTOP_GLUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DUCK_LAPTOP_AUDIO_RATE = 32768 };

enum DuckLaptopLanguage { DUCK_LAPTOP_SPANISH, DUCK_LAPTOP_GERMAN };

// Plugs the laptop's controller in, or takes it out. printer: 0 none, 1 one pass, 2 two passes.
// control reads the console's SC, in puts a byte the controller clocked in, slot runs the cartridge
// in the laptop's slot if there is one.
void duckLaptopConnect(bool on, int printer, enum DuckLaptopLanguage language,
                       unsigned (*control)(void), void (*in)(uint8_t), bool (*slot)(void));
bool duckLaptopConnected(void);
// The console clocked out a byte: what it reads back
uint8_t duckLaptopClockedByConsole(uint8_t out);
// Cycles (4 MiHz) until the controller does something next, and what has run since
long duckLaptopCyclesToNext(void);
void duckLaptopRan(long cycles);
size_t duckLaptopStateSize(void);
void duckLaptopSaveState(uint8_t* out);
void duckLaptopLoadState(const uint8_t* in);
// A key of the frontend's keyboard
void duckLaptopKeyboard(bool down, unsigned keycode, uint32_t character, uint16_t modifiers);
// The laptop's speech, over the console's sound (stereo, DUCK_LAPTOP_AUDIO_RATE)
const int16_t* duckLaptopMixSpeech(const int16_t* samples, size_t frames);
void duckLaptopStopSpeech(void);

#ifdef __cplusplus
}
#endif

#endif
