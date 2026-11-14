/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The Mega Duck laptops (CEFA Super Quique, Hartung Super Junior): their keyboard, clock, printer and
// speech sit behind a controller on the serial port. Super Junior SameDuck's emulation of that
// controller (megaduck_laptop*.c here, Expat-licensed, see LICENSE) runs as it does there; this file
// wires it to Gambatte's serial port and to the rest of armGBA.
//
// The console clocks a byte out: each bit goes to the controller as it would on the wire, and the
// bits it answers make the byte the console reads. The controller clocks bytes in on its own, a bit
// at a time: a bit lands only while the console's serial port waits on an outside clock (SC 80), and
// eight landed bits make a byte, with the serial interrupt.

#include "laptop.h"

#include "gb.h"
#include "megaduck_laptop_periph.h"
#include "../core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static GB_gameboy_t duck;
static bool connected;
static unsigned (*serialControl)(void);
static void (*serialIn)(uint8_t byte);
static bool (*cartInSlot)(void);
static uint8_t landing;
static int landed;
static enum DuckLaptopLanguage language;
// A key let go of before the laptop asked for the keys stays down until it has told the console of it,
// so a quick tap isn't lost between two of its polls
static uint8_t heldKey;
static bool releaseWaiting;
static uint8_t releaseFlags;

static void releaseIfReported(void) {
	if (releaseWaiting && duck.megaduck_laptop.last_key == heldKey) {
		releaseWaiting = false;
		GB_megaduck_laptop_key_release(&duck, MEGADUCK_KBD_CODE_NONE, releaseFlags);
	}
}

void GB_set_serial_transfer_bit_start_callback(GB_gameboy_t* gb, GB_serial_transfer_bit_start_callback_t callback) {
	gb->serial_transfer_bit_start_callback = callback;
}

void GB_set_serial_transfer_bit_end_callback(GB_gameboy_t* gb, GB_serial_transfer_bit_end_callback_t callback) {
	gb->serial_transfer_bit_end_callback = callback;
}

void GB_serial_set_data_bit(GB_gameboy_t* gb, bool data) {
	(void) gb;
	unsigned const control = serialControl ? serialControl() : 0;
	if ((control & 0x81) != 0x80) {
		return;
	}
	landing = (uint8_t) (landing << 1 | data);
	if (++landed == 8) {
		landed = 0;
		if (serialIn) {
			serialIn(landing);
		}
	}
}

bool GB_serial_get_data_bit(GB_gameboy_t* gb) {
	(void) gb;
	return true;
}

// Seconds the local time is ahead of UTC.
static long localOffset(time_t now) {
	struct tm const local = *localtime(&now);
	struct tm const utc = *gmtime(&now);
	int const days = local.tm_year != utc.tm_year ? (local.tm_year > utc.tm_year ? 1 : -1) : local.tm_yday - utc.tm_yday;
	return ((days * 24L + local.tm_hour - utc.tm_hour) * 60 + local.tm_min - utc.tm_min) * 60 + local.tm_sec - utc.tm_sec;
}

void duckLaptopConnect(bool on, int printer, enum DuckLaptopLanguage lang,
                       unsigned (*control)(void), void (*in)(uint8_t), bool (*slot)(void)) {
	memset(&duck, 0, sizeof(duck));
	landing = 0;
	landed = 0;
	heldKey = MEGADUCK_KBD_CODE_NONE;
	releaseWaiting = false;
	connected = on;
	language = lang;
	serialControl = control;
	serialIn = in;
	cartInSlot = slot;
	duckLaptopStopSpeech();
	if (on) {
		GB_connect_megaduck_laptop(&duck, NULL, NULL);
		// The clock starts on local time, where SameDuck's starts on UTC; a game that sets it moves it
		// from there.
		time_t const now = time(NULL);
		duck.megaduck_laptop.time_delta_rtc_vs_host = -localOffset(now);
		if (printer) {
			MD_printer_connect(printer == 1 ? MEGADUCK_PRINTER_TYPE_1_PASS : MEGADUCK_PRINTER_TYPE_2_PASS);
		}
	}
}

bool duckLaptopConnected(void) {
	return connected;
}

uint8_t duckLaptopClockedByConsole(uint8_t out) {
	uint8_t in = 0;
	for (int bit = 7; bit >= 0; --bit) {
		if (duck.serial_transfer_bit_start_callback) {
			duck.serial_transfer_bit_start_callback(&duck, out >> bit & 1);
		}
		bool const back = duck.serial_transfer_bit_end_callback ? duck.serial_transfer_bit_end_callback(&duck) : true;
		in = (uint8_t) (in << 1 | back);
	}
	return in;
}

long duckLaptopCyclesToNext(void) {
	long const left = duck.megaduck_laptop.t_states_till_update;
	return left > 0 ? left : 1;
}

void duckLaptopRan(long cycles) {
	if (!connected) {
		return;
	}
	duck.megaduck_laptop.t_states_elapsed += (uint32_t) cycles;
	duck.megaduck_laptop.t_states_till_update -= (int32_t) cycles;
	if (duck.megaduck_laptop.t_states_till_update <= 0) {
		GB_megaduck_laptop_peripheral_update(&duck, (uint8_t) (cycles < 255 ? cycles : 255));
	}
	releaseIfReported();
}

// What the controller keeps between frames, for states
size_t duckLaptopStateSize(void) {
	return sizeof(duck.megaduck_laptop) + 2;
}

void duckLaptopSaveState(uint8_t* out) {
	memcpy(out, &duck.megaduck_laptop, sizeof(duck.megaduck_laptop));
	out[sizeof(duck.megaduck_laptop)] = landing;
	out[sizeof(duck.megaduck_laptop) + 1] = (uint8_t) landed;
}

void duckLaptopLoadState(const uint8_t* in) {
	memcpy(&duck.megaduck_laptop, in, sizeof(duck.megaduck_laptop));
	landing = in[sizeof(duck.megaduck_laptop)];
	landed = in[sizeof(duck.megaduck_laptop) + 1] & 7;
}

// The keyboard: a key as the laptop has it, from what the key types, else from which key it is (as
// Super Junior SameDuck maps a PC keyboard). Shift, Caps Lock and Ctrl (the piano keys) carry over.
static uint8_t keyForCharacter(uint32_t c, uint8_t* modifiers) {
#define SHIFTED(k) (*modifiers |= MEGADUCK_KBD_FLAG_SHIFT, (k))
#define PLAIN(k) (*modifiers &= (uint8_t) ~MEGADUCK_KBD_FLAG_SHIFT, (k))
	if (c >= 'a' && c <= 'z') {
		c -= 'a' - 'A';
	}
	switch (c) {
	case 0x00B7: return SHIFTED(MEGADUCK_KBD_CODE_3);
	case 0x00A1: return PLAIN(MEGADUCK_KBD_CODE_EXCLAMATION_FLIPPED);
	case 0x00BF: return SHIFTED(MEGADUCK_KBD_CODE_EXCLAMATION_FLIPPED);
	case 0x00BA: return MEGADUCK_KBD_CODE_O_OVER_LINE;
	case 0x00DC: case 0x00FC: return MEGADUCK_KBD_CODE_U_UMLAUT;
	case 0x00D1: case 0x00F1: return MEGADUCK_KBD_CODE_N_TILDE;
	case '1': return PLAIN(MEGADUCK_KBD_CODE_1);
	case '2': return PLAIN(MEGADUCK_KBD_CODE_2);
	case '3': return PLAIN(MEGADUCK_KBD_CODE_3);
	case '4': return PLAIN(MEGADUCK_KBD_CODE_4);
	case '5': return PLAIN(MEGADUCK_KBD_CODE_5);
	case '6': return PLAIN(MEGADUCK_KBD_CODE_6);
	case '7': return PLAIN(MEGADUCK_KBD_CODE_7);
	case '8': return PLAIN(MEGADUCK_KBD_CODE_8);
	case '9': return PLAIN(MEGADUCK_KBD_CODE_9);
	case '0': return PLAIN(MEGADUCK_KBD_CODE_0);
	case '\'': return PLAIN(MEGADUCK_KBD_CODE_SINGLE_QUOTE);
	case '!': return SHIFTED(MEGADUCK_KBD_CODE_1);
	case '"': return SHIFTED(MEGADUCK_KBD_CODE_2);
	case '$': return SHIFTED(MEGADUCK_KBD_CODE_4);
	case '%': return SHIFTED(MEGADUCK_KBD_CODE_5);
	case '&': return SHIFTED(MEGADUCK_KBD_CODE_6);
	case '/': return SHIFTED(MEGADUCK_KBD_CODE_7);
	case '(': return SHIFTED(MEGADUCK_KBD_CODE_8);
	case ')': return SHIFTED(MEGADUCK_KBD_CODE_9);
	case '\\': return SHIFTED(MEGADUCK_KBD_CODE_0);
	case '?': return SHIFTED(MEGADUCK_KBD_CODE_SINGLE_QUOTE);
	case 'Q': return MEGADUCK_KBD_CODE_Q;
	case 'W': return MEGADUCK_KBD_CODE_W;
	case 'E': return MEGADUCK_KBD_CODE_E;
	case 'R': return MEGADUCK_KBD_CODE_R;
	case 'T': return MEGADUCK_KBD_CODE_T;
	case 'Y': return MEGADUCK_KBD_CODE_Y;
	case 'U': return MEGADUCK_KBD_CODE_U;
	case 'I': return MEGADUCK_KBD_CODE_I;
	case 'O': return MEGADUCK_KBD_CODE_O;
	case 'P': return MEGADUCK_KBD_CODE_P;
	case '`': return PLAIN(MEGADUCK_KBD_CODE_BACKTICK);
	case ']': return PLAIN(MEGADUCK_KBD_CODE_RIGHT_SQ_BRACKET);
	case '[': return SHIFTED(MEGADUCK_KBD_CODE_BACKTICK);
	case '*': return SHIFTED(MEGADUCK_KBD_CODE_RIGHT_SQ_BRACKET);
	case 'A': return MEGADUCK_KBD_CODE_A;
	case 'S': return MEGADUCK_KBD_CODE_S;
	case 'D': return MEGADUCK_KBD_CODE_D;
	case 'F': return MEGADUCK_KBD_CODE_F;
	case 'G': return MEGADUCK_KBD_CODE_G;
	case 'H': return MEGADUCK_KBD_CODE_H;
	case 'J': return MEGADUCK_KBD_CODE_J;
	case 'K': return MEGADUCK_KBD_CODE_K;
	case 'L': return MEGADUCK_KBD_CODE_L;
	case 'Z': return MEGADUCK_KBD_CODE_Z;
	case 'X': return MEGADUCK_KBD_CODE_X;
	case 'C': return MEGADUCK_KBD_CODE_C;
	case 'V': return MEGADUCK_KBD_CODE_V;
	case 'B': return MEGADUCK_KBD_CODE_B;
	case 'N': return MEGADUCK_KBD_CODE_N;
	case 'M': return MEGADUCK_KBD_CODE_M;
	case ',': return PLAIN(MEGADUCK_KBD_CODE_COMMA);
	case '.': return PLAIN(MEGADUCK_KBD_CODE_PERIOD);
	case '-': return PLAIN(MEGADUCK_KBD_CODE_DASH);
	case ';': return SHIFTED(MEGADUCK_KBD_CODE_COMMA);
	case ':': return SHIFTED(MEGADUCK_KBD_CODE_PERIOD);
	case '_': return SHIFTED(MEGADUCK_KBD_CODE_DASH);
	case ' ': return MEGADUCK_KBD_CODE_SPACE;
	case '<': return PLAIN(MEGADUCK_KBD_CODE_LESS_THAN);
	case '>': return SHIFTED(MEGADUCK_KBD_CODE_LESS_THAN);
	case '=': return PLAIN(MEGADUCK_KBD_CODE_EQUALS);
	case '+': return PLAIN(MEGADUCK_KBD_CODE_PLUS);
	}
	return MEGADUCK_KBD_CODE_NONE;
#undef SHIFTED
#undef PLAIN
}

static uint8_t keyForKey(unsigned key, bool ctrl) {
	if (ctrl) {
		switch (key) {
		case RETROK_F1: return MEGADUCK_KBD_CODE_PIANO_DO_SHARP;
		case RETROK_F2: return MEGADUCK_KBD_CODE_PIANO_RE_SHARP;
		case RETROK_F3: return MEGADUCK_KBD_CODE_PIANO_FA_SHARP;
		case RETROK_F4: return MEGADUCK_KBD_CODE_PIANO_SOL_SHARP;
		case RETROK_F5: return MEGADUCK_KBD_CODE_PIANO_LA_SHARP;
		case RETROK_F6: return MEGADUCK_KBD_CODE_PIANO_DO_2_SHARP;
		case RETROK_F7: return MEGADUCK_KBD_CODE_PIANO_RE_2_SHARP;
		case RETROK_F8: return MEGADUCK_KBD_CODE_PIANO_FA_2_SHARP;
		case RETROK_F9: return MEGADUCK_KBD_CODE_PIANO_SOL_2_SHARP;
		case RETROK_F10: return MEGADUCK_KBD_CODE_PIANO_LA_2_SHARP;
		case RETROK_BACKQUOTE: return MEGADUCK_KBD_CODE_PIANO_DO;
		case RETROK_1: return MEGADUCK_KBD_CODE_PIANO_RE;
		case RETROK_2: return MEGADUCK_KBD_CODE_PIANO_MI;
		case RETROK_3: return MEGADUCK_KBD_CODE_PIANO_FA;
		case RETROK_4: return MEGADUCK_KBD_CODE_PIANO_SOL;
		case RETROK_5: return MEGADUCK_KBD_CODE_PIANO_LA;
		case RETROK_6: return MEGADUCK_KBD_CODE_PIANO_SI;
		case RETROK_7: return MEGADUCK_KBD_CODE_PIANO_DO_2;
		case RETROK_8: return MEGADUCK_KBD_CODE_PIANO_RE_2;
		case RETROK_9: return MEGADUCK_KBD_CODE_PIANO_MI_2;
		case RETROK_0: return MEGADUCK_KBD_CODE_PIANO_FA_2;
		case RETROK_MINUS: return MEGADUCK_KBD_CODE_PIANO_SOL_2;
		case RETROK_EQUALS: return MEGADUCK_KBD_CODE_PIANO_LA_2;
		case RETROK_BACKSPACE: return MEGADUCK_KBD_CODE_PIANO_SI_2;
		}
	}
	switch (key) {
	case RETROK_TAB: return MEGADUCK_KBD_CODE_HELP;
	case RETROK_F1: return MEGADUCK_KBD_CODE_F1;
	case RETROK_F2: return MEGADUCK_KBD_CODE_F2;
	case RETROK_F3: return MEGADUCK_KBD_CODE_F3;
	case RETROK_F4: return MEGADUCK_KBD_CODE_F4;
	case RETROK_F5: return MEGADUCK_KBD_CODE_F5;
	case RETROK_F6: return MEGADUCK_KBD_CODE_F6;
	case RETROK_F7: return MEGADUCK_KBD_CODE_F7;
	case RETROK_F8: return MEGADUCK_KBD_CODE_F8;
	case RETROK_F9: return MEGADUCK_KBD_CODE_F9;
	case RETROK_F10: return MEGADUCK_KBD_CODE_F10;
	case RETROK_F11: return MEGADUCK_KBD_CODE_F11;
	case RETROK_ESCAPE: return MEGADUCK_KBD_CODE_ESCAPE;
	case RETROK_BACKSPACE: return MEGADUCK_KBD_CODE_BACKSPACE;
	case RETROK_RETURN:
	case RETROK_KP_ENTER: return MEGADUCK_KBD_CODE_ENTER;
	case RETROK_DELETE: return MEGADUCK_KBD_CODE_DELETE;
	case RETROK_PAGEUP: return MEGADUCK_KBD_CODE_PAGE_UP;
	case RETROK_PAGEDOWN: return MEGADUCK_KBD_CODE_PAGE_DOWN;
	case RETROK_KP_MULTIPLY: return MEGADUCK_KBD_CODE_MULTIPLY;
	case RETROK_DOWN: return MEGADUCK_KBD_CODE_ARROW_DOWN;
	case RETROK_KP_MINUS: return MEGADUCK_KBD_CODE_MINUS;
	case RETROK_LEFT: return MEGADUCK_KBD_CODE_ARROW_LEFT;
	case RETROK_KP_EQUALS: return MEGADUCK_KBD_CODE_EQUALS;
	case RETROK_RIGHT: return MEGADUCK_KBD_CODE_ARROW_RIGHT;
	case RETROK_KP_DIVIDE: return MEGADUCK_KBD_CODE_DIVIDE;
	case RETROK_UP: return MEGADUCK_KBD_CODE_ARROW_UP;
	case RETROK_KP_PLUS: return MEGADUCK_KBD_CODE_PLUS;
	case RETROK_PRINT:
	case RETROK_SCROLLOCK: return MEGADUCK_KBD_CODE_PRINTSCREEN_RIGHT;
	}
	return MEGADUCK_KBD_CODE_NONE;
}

void duckLaptopKeyboard(bool down, unsigned keycode, uint32_t character, uint16_t modifiers) {
	if (!connected) {
		return;
	}
	uint8_t flags = MEGADUCK_KBD_FLAGS_NONE;
	if (modifiers & RETROKMOD_CAPSLOCK) {
		flags |= MEGADUCK_KBD_FLAG_CAPSLOCK;
	}
	if (modifiers & RETROKMOD_SHIFT) {
		flags |= MEGADUCK_KBD_FLAG_SHIFT;
	}
	if (!down) {
		releaseWaiting = heldKey != MEGADUCK_KBD_CODE_NONE;
		releaseFlags = flags;
		if (!releaseWaiting) {
			GB_megaduck_laptop_key_release(&duck, MEGADUCK_KBD_CODE_NONE, flags);
		}
		releaseIfReported();
		return;
	}
	uint8_t key = keyForKey(keycode, modifiers & RETROKMOD_CTRL);
	// With Ctrl, the key is a piano key: the character a frontend sends with it isn't typed
	if (key == MEGADUCK_KBD_CODE_NONE && character && !(modifiers & RETROKMOD_CTRL)) {
		key = keyForCharacter(character, &flags);
	}
	if (key != MEGADUCK_KBD_CODE_NONE) {
		GB_megaduck_laptop_key_set(&duck, key, flags);
		heldKey = key;
		releaseWaiting = false;
	}
}

// Speech: the laptop plays one of six recorded phrases. armGBA plays megaduck-info's recordings of
// them, when they're in the system folder under their names there
// (megaduck_laptop_speech_spanish_1.wav to _6, megaduck_laptop_speech_german_1.wav to _6).
static int16_t* speech;
static size_t speechLength;
static size_t speechAt;

void duckLaptopStopSpeech(void) {
	free(speech);
	speech = NULL;
	speechLength = speechAt = 0;
}

static uint32_t le32(const uint8_t* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

// A PCM WAV, 8 or 16 bits, at any rate and channel count, to mono at rate
static bool loadWav(const char* path, unsigned rate) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long const size = ftell(f);
	rewind(f);
	uint8_t* file = size > 44 ? malloc(size) : NULL;
	bool read = file && fread(file, 1, size, f) == (size_t) size;
	fclose(f);
	if (!read || memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) {
		free(file);
		return false;
	}
	unsigned channels = 0, bits = 0, sourceRate = 0;
	const uint8_t* data = NULL;
	uint32_t dataSize = 0;
	for (long at = 12; at + 8 <= size;) {
		uint32_t const chunkSize = le32(file + at + 4);
		if (!memcmp(file + at, "fmt ", 4) && chunkSize >= 16 && at + 8 + 16 <= size) {
			unsigned const format = file[at + 8] | file[at + 9] << 8;
			channels = file[at + 10] | file[at + 11] << 8;
			sourceRate = le32(file + at + 12);
			bits = file[at + 22] | file[at + 23] << 8;
			if (format != 1) {
				channels = 0;
			}
		} else if (!memcmp(file + at, "data", 4)) {
			data = file + at + 8;
			dataSize = chunkSize <= (uint32_t) (size - at - 8) ? chunkSize : (uint32_t) (size - at - 8);
		}
		at += 8 + chunkSize + (chunkSize & 1);
	}
	if (!data || !channels || !sourceRate || (bits != 8 && bits != 16)) {
		free(file);
		return false;
	}
	size_t const frameBytes = channels * bits / 8;
	size_t const frames = dataSize / frameBytes;
	size_t const length = (size_t) ((double) frames * rate / sourceRate);
	int16_t* out = length ? malloc(length * sizeof(int16_t)) : NULL;
	if (!out) {
		free(file);
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		size_t const from = (size_t) ((double) i * sourceRate / rate);
		const uint8_t* p = data + (from < frames ? from : frames - 1) * frameBytes;
		int sum = 0;
		for (unsigned c = 0; c < channels; ++c) {
			sum += bits == 8 ? (p[c] - 128) << 8 : (int16_t) (p[2 * c] | p[2 * c + 1] << 8);
		}
		out[i] = (int16_t) (sum / (int) channels);
	}
	free(file);
	duckLaptopStopSpeech();
	speech = out;
	speechLength = length;
	return true;
}

void duckLaptopSpeak(int phrase) {
	const char* system = NULL;
	environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system);
	char path[PATH_SIZE];
	snprintf(path, sizeof(path), "%s/megaduck_laptop_speech_%s_%d.wav", system && *system ? system : ".",
	         language == DUCK_LAPTOP_GERMAN ? "german" : "spanish", phrase);
	if (!loadWav(path, DUCK_LAPTOP_AUDIO_RATE)) {
		static bool told;
		if (!told) {
			told = true;
			char message[PATH_SIZE + 64];
			snprintf(message, sizeof(message), "Mega Duck speech: no %s", path);
			notify(message, 240);
		}
	}
}

const int16_t* duckLaptopMixSpeech(const int16_t* samples, size_t frames) {
	static int16_t mixed[2 * 4096];
	if (!speech || frames > COUNT(mixed) / 2) {
		return samples;
	}
	for (size_t i = 0; i < frames; ++i) {
		int const voice = speechAt < speechLength ? speech[speechAt++] / 2 : 0;
		for (int side = 0; side < 2; ++side) {
			int const v = samples[2 * i + side] + voice;
			mixed[2 * i + side] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
	if (speechAt >= speechLength) {
		duckLaptopStopSpeech();
	}
	return mixed;
}

// A printed page: saved as the Game Boy Printer's are, as far down as anything is printed
void duckLaptopPrinted(const uint8_t* image, int width, int height) {
	int rows = height;
	while (rows > 0) {
		const uint8_t* row = image + (size_t) (rows - 1) * width;
		int x = 0;
		while (x < width && row[x] == 0xFF) {
			++x;
		}
		if (x < width) {
			break;
		}
		--rows;
	}
	if (rows) {
		printoutPage(image, rows + 8 < height ? rows + 8 : height, true, (uint32_t) rows);
		printoutFinish();
	}
}

bool duckLaptopRunCartInSlot(void) {
	return cartInSlot && cartInSlot();
}
