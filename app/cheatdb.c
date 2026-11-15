/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// RetroArch's cheat database for Import Cheats from RetroArch, when RetroArch's own isn't found (on
// Android, or with no RetroArch): the app downloads it as RetroArch's Online Updater does, libretro's
// cheats.zip, and keeps its Game Boy, Game Boy Color and Game Boy Advance folders in
// ~/.local/share/armgba/cheat-database, downloaded again once it's a month old (the old copy kept if
// that fails). Without either, only the game's own files come, from libretro-database on GitHub
// (which cheats.zip is made from), into cheat-database-game. On a worker thread; the core asks again
// until it's done (link.h).

#include "hooks.h"
#include "link.h"
#include "ui.h"

#include <dirent.h>
#include <file/archive_file.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZIP_URL "https://buildbot.libretro.com/assets/frontend/cheats.zip"
#define TREE_URL "https://api.github.com/repos/libretro/libretro-database/git/trees/master:"
#define FILE_URL "https://raw.githubusercontent.com/libretro/libretro-database/master/"

enum { REFRESH_SECONDS = 30 * 24 * 60 * 60, FRESH_SECONDS = 24 * 60 * 60 };

// Set by Extras' import: the copy is downloaded again first, unless it's from the last day.
static bool refreshWanted;

static const char* const systems[] = { "Nintendo - Game Boy", "Nintendo - Game Boy Color", "Nintendo - Game Boy Advance" };
enum { SYSTEMS = sizeof(systems) / sizeof(*systems), ADVANCE = 2 };

// The download: none, running, or done with result for game (its folder in folder).
enum { IDLE, RUNNING, DONE };
static SDL_atomic_t state;
static int result;
static char game[PATH_SIZE];
static bool advance;
static char folder[PATH_SIZE];

// A folder beside the app's cheats folder (~/.local/share/armgba).
static void dataPath(char* out, size_t size, const char* name) {
	snprintf(out, size, "%s", cheatDirectory);
	char* const slash = strrchr(out, '/');
	if (slash) {
		*slash = '\0';
	}
	size_t const length = strlen(out);
	snprintf(out + length, size - length, "/%s", name);
}

static bool isFolder(const char* path) {
	struct stat info;
	return !stat(path, &info) && S_ISDIR(info.st_mode);
}

// A folder and those it's in.
static bool makeFolders(const char* path) {
	char at[PATH_SIZE];
	snprintf(at, sizeof(at), "%s", path);
	for (char* p = at + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			mkdir(at, 0755);
			*p = '/';
		}
	}
	return !mkdir(at, 0755) || isFolder(at);
}

// Removes a folder of the app's and what's in it.
static void removeFolder(const char* path) {
	DIR* const d = opendir(path);
	if (!d) {
		unlink(path);
		return;
	}
	for (struct dirent* e; (e = readdir(d));) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
			continue;
		}
		char inside[PATH_SIZE + 260];
		snprintf(inside, sizeof(inside), "%s/%s", path, e->d_name);
		if (isFolder(inside)) {
			removeFolder(inside);
		} else {
			unlink(inside);
		}
	}
	closedir(d);
	rmdir(path);
}

// Whether a copy of the database has every system's folder, and its age in seconds.
static bool hasDatabase(const char* path, long long* age) {
	char system[PATH_SIZE + 64];
	for (int i = 0; i < SYSTEMS; ++i) {
		snprintf(system, sizeof(system), "%s/%s", path, systems[i]);
		if (!isFolder(system)) {
			return false;
		}
	}
	struct stat info;
	*age = !stat(path, &info) ? (long long) (time(NULL) - info.st_mtime) : 0;
	return true;
}

// Percent-encoded, for a URL's path: all but letters, digits and -._~, and / where slashes is false.
static void encode(char* out, size_t size, const char* text, bool slashes) {
	size_t n = 0;
	for (const unsigned char* p = (const unsigned char*) text; *p && n + 4 < size; ++p) {
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p) ||
		    (slashes && *p == '/')) {
			out[n++] = (char) *p;
		} else {
			n += (size_t) snprintf(out + n, size - n, "%%%02X", *p);
		}
	}
	out[n] = '\0';
}

// cheats.zip's files in the systems' folders, written under the extraction's folder.
struct Extraction {
	char folder[PATH_SIZE];
	int files;
	bool failed;
};

static int extracted(const char* name, const char* extensions, const uint8_t* data, unsigned mode, uint32_t packed, uint32_t size,
                     uint32_t crc, struct archive_extract_userdata* user) {
	(void) extensions;
	struct Extraction* const x = user->cb_data;
	size_t const length = strlen(name);
	// A file right in a system's folder: no other slash, so nothing outside it
	if (length < 5 || strcasecmp(name + length - 4, ".cht")) {
		return 1;
	}
	for (int i = 0; i < SYSTEMS; ++i) {
		size_t const n = strlen(systems[i]);
		if (strncmp(name, systems[i], n) || name[n] != '/' || strchr(name + n + 1, '/')) {
			continue;
		}
		char path[PATH_SIZE + 512];
		snprintf(path, sizeof(path), "%s/%s", x->folder, name);
		if (file_archive_perform_mode(path, NULL, data, mode, packed, size, crc, user)) {
			++x->files;
		} else {
			x->failed = true;
		}
	}
	return 1;
}

// The whole database: cheats.zip, its systems' folders in place of the copy at to.
static bool downloadDatabase(const char* to) {
	char zip[PATH_SIZE], fresh[PATH_SIZE];
	dataPath(zip, sizeof(zip), "cheat-database.zip");
	dataPath(fresh, sizeof(fresh), "cheat-database.new");
	removeFolder(fresh);
	bool ok = httpDownload(ZIP_URL, zip) == 200;
	struct Extraction x = { .files = 0 };
	snprintf(x.folder, sizeof(x.folder), "%s", fresh);
	for (int i = 0; ok && i < SYSTEMS; ++i) {
		char system[PATH_SIZE + 64];
		snprintf(system, sizeof(system), "%s/%s", fresh, systems[i]);
		ok = makeFolders(system);
	}
	if (ok) {
		file_archive_transfer_t transfer = { .type = ARCHIVE_TRANSFER_INIT };
		struct archive_extract_userdata user = { .cb_data = &x };
		bool fine = true;
		while (!file_archive_parse_file_iterate(&transfer, &fine, zip, NULL, extracted, &user)) {
		}
		ok = fine && !x.failed && x.files > 0;
	}
	unlink(zip);
	if (ok) {
		char old[PATH_SIZE];
		dataPath(old, sizeof(old), "cheat-database.old");
		removeFolder(old);
		rename(to, old);
		ok = !rename(fresh, to);
		removeFolder(ok ? old : fresh);
		if (!ok) {
			rename(old, to);
		}
	} else {
		removeFolder(fresh);
	}
	return ok;
}

// Reads a whole file, with a 0 after it.
static char* readText(const char* path) {
	FILE* const f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long const size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* const text = size >= 0 ? malloc((size_t) size + 1) : NULL;
	if (text && fread(text, 1, (size_t) size, f) == (size_t) size) {
		text[size] = '\0';
	} else {
		free(text);
		fclose(f);
		return NULL;
	}
	fclose(f);
	return text;
}

// Just the game's files: its systems' listings from GitHub, and the files in them for it. False when
// a listing couldn't be had.
static bool downloadGame(const char* to) {
	removeFolder(to);
	char listing[PATH_SIZE];
	dataPath(listing, sizeof(listing), "cheat-database-list.json");
	for (int i = advance ? ADVANCE : 0; i < (advance ? SYSTEMS : ADVANCE); ++i) {
		char system[PATH_SIZE + 64];
		snprintf(system, sizeof(system), "%s/%s", to, systems[i]);
		char path[512], encoded[1024], url[1536];
		snprintf(path, sizeof(path), "cht/%s", systems[i]);
		encode(encoded, sizeof(encoded), path, false);
		snprintf(url, sizeof(url), TREE_URL "%s", encoded);
		char* const text = makeFolders(system) && httpDownload(url, listing) == 200 ? readText(listing) : NULL;
		unlink(listing);
		if (!text || !strstr(text, "\"tree\"")) {
			free(text);
			removeFolder(to);
			return false;
		}
		// Each "path":"name" in it, its quotes and backslashes escaped
		for (const char* p = strstr(text, "\"path\":"); p; p = strstr(p, "\"path\":")) {
			p += 7;
			while (*p == ' ') {
				++p;
			}
			if (*p++ != '"') {
				continue;
			}
			char name[512];
			size_t n = 0;
			bool plain = true;
			for (; *p && *p != '"' && n + 1 < sizeof(name); ++p) {
				if (*p == '\\') {
					++p;
					plain &= *p == '"' || *p == '\\' || *p == '/';
				}
				name[n++] = *p;
			}
			name[n] = '\0';
			if (!plain || strchr(name, '/') || !chtFileFits(name, game)) {
				continue;
			}
			snprintf(path, sizeof(path), "cht/%s/%s", systems[i], name);
			encode(encoded, sizeof(encoded), path, true);
			snprintf(url, sizeof(url), FILE_URL "%s", encoded);
			char file[PATH_SIZE + 600];
			snprintf(file, sizeof(file), "%s/%s", system, name);
			if (httpDownload(url, file) != 200) {
				unlink(file);
			}
		}
		free(text);
	}
	return true;
}

static int download(void* unused) {
	(void) unused;
	char whole[PATH_SIZE], own[PATH_SIZE];
	dataPath(whole, sizeof(whole), "cheat-database");
	dataPath(own, sizeof(own), "cheat-database-game");
	long long age;
	char base[PATH_SIZE];
	dataPath(base, sizeof(base), "");
	makeFolders(base);
	result = CHEAT_FETCH_NONE;
	if (downloadDatabase(whole) || hasDatabase(whole, &age)) {
		snprintf(folder, sizeof(folder), "%s", whole);
		result = CHEAT_FETCH_READY;
	} else if (downloadGame(own)) {
		snprintf(folder, sizeof(folder), "%s", own);
		result = CHEAT_FETCH_READY;
	}
	SDL_AtomicSet(&state, DONE);
	return 0;
}

static int fetch(const char* gamePath, bool forAdvance, bool start, char* out, size_t outSize) {
	char whole[PATH_SIZE];
	dataPath(whole, sizeof(whole), "cheat-database");
	long long age = 0;
	bool const have = hasDatabase(whole, &age);
	int const now = SDL_AtomicGet(&state);
	if (now == RUNNING) {
		return CHEAT_FETCH_BUSY;
	}
	if (now == DONE) {
		SDL_AtomicSet(&state, IDLE);
		// The game's own files only do for the game they were for
		if (result == CHEAT_FETCH_READY && (have || !strcmp(game, gamePath))) {
			snprintf(out, outSize, "%s", folder);
			return CHEAT_FETCH_READY;
		}
		if (result != CHEAT_FETCH_READY && !strcmp(game, gamePath)) {
			return CHEAT_FETCH_NONE;
		}
	}
	long long const stale = refreshWanted ? FRESH_SECONDS : REFRESH_SECONDS;
	refreshWanted = refreshWanted && start ? false : refreshWanted;
	if (have && (age < stale || !start)) {
		snprintf(out, outSize, "%s", whole);
		return CHEAT_FETCH_READY;
	}
	if (!start || !httpPrepare()) {
		if (have) {
			snprintf(out, outSize, "%s", whole);
			return CHEAT_FETCH_READY;
		}
		return CHEAT_FETCH_NONE;
	}
	snprintf(game, sizeof(game), "%s", gamePath);
	advance = forAdvance;
	SDL_AtomicSet(&state, RUNNING);
	SDL_Thread* const thread = SDL_CreateThread(download, "cheat database", NULL);
	if (!thread) {
		SDL_AtomicSet(&state, IDLE);
		return have ? (snprintf(out, outSize, "%s", whole), CHEAT_FETCH_READY) : CHEAT_FETCH_NONE;
	}
	SDL_DetachThread(thread);
	return CHEAT_FETCH_BUSY;
}

void cheatDatabaseStart(void) {
	libretroSetCheatFetch(fetch);
}

// Extras: Import Cheats from RetroArch with a press, as Core > System has it (the option goes to
// Import, which the core takes as the game runs again), the app's copy of the database brought up to
// date first.
static void importRow(int unused, int delta) {
	(void) unused;
	struct Option* const option = optionFind("armgba_cheat_import");
	if (!delta && option) {
		refreshWanted = true;
		optionSet(option, "import");
		uiCloseMenu();
	}
}

void cheatExtrasRows(void) {
	const struct Option* const option = optionFind("armgba_cheat_import");
	if (option && option->visible) {
		addItem("Import Cheats from RetroArch", importRow, 0);
		helpFor("Adds the latest RetroArch cheats for the game on top to the cheat cartridge's list, switched off.");
	}
}
