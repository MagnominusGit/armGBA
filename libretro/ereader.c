/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// e-Reader cards through the frontend's disc menu: each card file added is a disc, and closing the
// tray puts the chosen card under the console's scanner, to be read at the next scan.

#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CARDS_MAX = 64 };

static char* cardPaths[CARDS_MAX];
static unsigned cardCount;
static unsigned cardIndex;
static bool trayOpen;
static bool calibrated;

// The console with the e-Reader in it.
static struct GBA* reader(void) {
	for (int i = 0; !gameBoy && i < playersHere; ++i) {
		if (gbas[i] && (gbas[i]->gpio.devices & HW_EREADER)) {
			return gbas[i];
		}
	}
	return NULL;
}

static void insertCard(const char* path) {
	struct GBA* machine = reader();
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!machine || !f) {
		if (f) {
			fclose(f);
		}
		return;
	}
	// Sixteen long strips at most.
	uint8_t* data = malloc(EREADER_CARDS_MAX * 2912);
	size_t size = data ? fread(data, 1, EREADER_CARDS_MAX * 2912, f) : 0;
	fclose(f);
	ereaderQueueCard(machine, data, size);
	free(data);
	notify("e-Reader card ready to scan", 180);
}

static bool setEjectState(bool ejected) {
	if (trayOpen && !ejected && cardIndex < cardCount) {
		insertCard(cardPaths[cardIndex]);
	}
	trayOpen = ejected;
	return true;
}

static bool getEjectState(void) {
	return trayOpen;
}

static unsigned getImageIndex(void) {
	return cardIndex;
}

static bool setImageIndex(unsigned index) {
	if (!trayOpen || index > cardCount) {
		return false;
	}
	cardIndex = index;
	return true;
}

static unsigned getNumImages(void) {
	return cardCount;
}

static bool replaceImageIndex(unsigned index, const struct retro_game_info* info) {
	if (index >= cardCount) {
		return false;
	}
	free(cardPaths[index]);
	if (info && info->path) {
		cardPaths[index] = strdup(info->path);
		return true;
	}
	memmove(&cardPaths[index], &cardPaths[index + 1], (cardCount - index - 1) * sizeof(*cardPaths));
	--cardCount;
	if (cardIndex > index || cardIndex == cardCount) {
		cardIndex = cardIndex ? cardIndex - 1 : 0;
	}
	return true;
}

static bool addImageIndex(void) {
	if (cardCount == CARDS_MAX) {
		return false;
	}
	cardPaths[cardCount++] = NULL;
	return true;
}

static bool setInitialImage(unsigned index, const char* path) {
	(void) index;
	(void) path;
	return false;
}

static bool getImagePath(unsigned index, char* path, size_t len) {
	if (index >= cardCount || !cardPaths[index]) {
		return false;
	}
	snprintf(path, len, "%s", cardPaths[index]);
	return true;
}

static bool getImageLabel(unsigned index, char* label, size_t len) {
	if (index >= cardCount || !cardPaths[index]) {
		return false;
	}
	const char* name = strrchr(cardPaths[index], '/');
	const char* back = strrchr(cardPaths[index], '\\');
	name = back > name ? back : name;
	snprintf(label, len, "%s", name ? name + 1 : cardPaths[index]);
	return true;
}

static struct retro_disk_control_ext_callback cardMenu = {
	setEjectState, getEjectState, getImageIndex, setImageIndex, getNumImages, replaceImageIndex,
	addImageIndex, setInitialImage, getImagePath, getImageLabel,
};

// As a game starts: the card menu, when a console has an e-Reader.
void ereaderStart(void) {
	ereaderForget();
	if (!reader()) {
		return;
	}
	if (!environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &cardMenu)) {
		environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &cardMenu);
	}
}

// Each frame: the first, once RetroArch has read the save in, gives a blank e-Reader its
// calibration.
void ereaderFrame(void) {
	if (calibrated) {
		return;
	}
	calibrated = true;
	for (int i = 0; !gameBoy && i < playersHere; ++i) {
		if (gbas[i]) {
			ereaderCalibrate(gbas[i]);
		}
	}
}

void ereaderForget(void) {
	for (unsigned i = 0; i < cardCount; ++i) {
		free(cardPaths[i]);
		cardPaths[i] = NULL;
	}
	cardCount = 0;
	cardIndex = 0;
	trayOpen = false;
	calibrated = false;
}
