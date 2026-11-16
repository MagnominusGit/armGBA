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
// Set for Download Cheat Database Again: the copy is downloaded again whatever its age.
static bool refreshNow;

static const char* const systems[] = { "Nintendo - Game Boy", "Nintendo - Game Boy Color", "Nintendo - Game Boy Advance" };
enum { SYSTEMS = sizeof(systems) / sizeof(*systems), ADVANCE = 2 };

// The download: none, running, or done with result for game (its folder in folder).
enum { IDLE, RUNNING, DONE };
static SDL_atomic_t state;
// Set while a file is downloading, as opposed to unpacking.
static SDL_atomic_t downloading;
static int result;
// Whether the last download brought cheats.zip, as opposed to failing (the old copy kept, or the game's own files)
static bool gotDatabase;
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
	SDL_AtomicSet(&downloading, 1);
	bool ok = httpDownload(ZIP_URL, zip) == 200;
	// Unpacking next, or on to the game's own files, still downloading
	SDL_AtomicSet(&downloading, !ok);
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
		SDL_AtomicSet(&downloading, 1);
		char* const text = makeFolders(system) && httpDownload(url, listing) == 200 ? readText(listing) : NULL;
		SDL_AtomicSet(&downloading, 0);
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
	gotDatabase = downloadDatabase(whole);
	if (gotDatabase || hasDatabase(whole, &age)) {
		snprintf(folder, sizeof(folder), "%s", whole);
		result = CHEAT_FETCH_READY;
	} else if (downloadGame(own)) {
		snprintf(folder, sizeof(folder), "%s", own);
		result = CHEAT_FETCH_READY;
	}
	SDL_AtomicSet(&downloading, 0);
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
	if (have && !(refreshNow && start) && (age < stale || !start)) {
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
	// Downloading from the start, not unpacking, for the progress shown
	SDL_AtomicSet(&downloading, 1);
	SDL_AtomicSet(&state, RUNNING);
	SDL_Thread* const thread = SDL_CreateThread(download, "cheat database", NULL);
	if (!thread) {
		SDL_AtomicSet(&state, IDLE);
		return have ? (snprintf(out, outSize, "%s", whole), CHEAT_FETCH_READY) : CHEAT_FETCH_NONE;
	}
	SDL_DetachThread(thread);
	return CHEAT_FETCH_BUSY;
}

static bool progress(int* doneKib, int* allKib);

void cheatDatabaseStart(void) {
	libretroSetCheatFetch(fetch);
	libretroSetCheatProgress(progress);
}

// The download's progress for the core's message: false once it's unpacking.
static bool progress(int* doneKib, int* allKib) {
	httpProgress(doneKib, allKib);
	return SDL_AtomicGet(&downloading) != 0;
}

// Extras > Cheats...: Import Cheats from RetroArch, and the cheat database in use. Without
// RetroArch's, the app's copy is brought up to date first, unless it's from the last day: the option
// goes to Import and the menu closes, and the core downloads, showing its progress, then imports.
static void importRow(int unused, int delta) {
	(void) unused;
	struct Option* const option = optionFind("armgba_cheat_import");
	if (!delta && option) {
		refreshWanted = true;
		optionSet(option, "import");
		uiCloseMenu();
	}
}

// Cheat Cartridge Reset, as Core > System has it: Left and Right (or a press) step through its
// choices, and the one shown as the menu closes is done, then goes back to Off.
static void resetRow(int unused, int delta) {
	(void) unused;
	struct Option* const option = optionFind("armgba_cheat_restore");
	if (!option || !option->valueCount) {
		return;
	}
	int at = 0;
	for (int v = 0; v < option->valueCount; ++v) {
		if (!strcmp(option->values[v], option->value)) {
			at = v;
		}
	}
	int const step = delta ? delta : 1;
	optionSet(option, option->values[((at + step) % option->valueCount + option->valueCount) % option->valueCount]);
}

static void openCartCheats(int unused, int delta);
static bool showDownload(void);
static void redownloadRow(int unused, int delta);

static void cheatsPage(void) {
	const char* game = NULL;
	bool advance = false, retroArchs = false;
	if (showDownload()) {
		return;
	}
	if (libretroCheatImportFor(&game, &advance, &retroArchs)) {
		addItem("Import Cheats from RetroArch", importRow, 0);
		helpFor("Adds the latest RetroArch cheats for the game on top to the cheat cartridge's list, switched off.");
		addItem("Cheats for the Game on Top...", openCartCheats, 0);
		helpFor("The cheats RetroArch's cheat database has that the cartridge takes for the game on top.");
	}
	const struct Option* const reset = optionFind("armgba_cheat_restore");
	if (reset && reset->visible) {
		const char* shown = reset->value;
		for (int v = 0; v < reset->valueCount; ++v) {
			if (!strcmp(reset->values[v], reset->value) && reset->labels[v]) {
				shown = reset->labels[v];
			}
		}
		addValue("Cheat Cartridge Reset", shown, resetRow, 0);
		helpFor(reset->info);
	}
	char whole[PATH_SIZE];
	dataPath(whole, sizeof(whole), "cheat-database");
	long long age = 0;
	static char status[64];
	bool const have = !retroArchs && hasDatabase(whole, &age);
	if (retroArchs) {
		snprintf(status, sizeof(status), "RetroArch's");
	} else if (have) {
		time_t const when = time(NULL) - (time_t) age;
		struct tm day;
		if (!localtime_r(&when, &day) || !strftime(status, sizeof(status), "Downloaded %d %b %Y", &day)) {
			snprintf(status, sizeof(status), "Downloaded");
		}
	} else {
		snprintf(status, sizeof(status), "Not Downloaded");
	}
	addChoice("Cheat Database", status, NULL, 0);
	helpFor(retroArchs ? "RetroArch's own, which its Online Updater keeps up to date."
	                   : "The app's copy of RetroArch's, downloaded again as an import needs it.");
	if (!retroArchs && game) {
		addItem(have ? "Download Cheat Database Again" : "Download Cheat Database", redownloadRow, 0);
		helpFor("Downloads RetroArch's cheat database now, as its Online Updater does (about 40 MB), keeping the copy there is if that fails.");
	}
}

static void openCheatsPage(int unused, int delta) {
	(void) unused;
	if (!delta) {
		pushMenu(cheatsPage, "Cheats");
	}
}

void cheatExtrasRows(void) {
	const char* game;
	bool advance, retroArchs;
	const struct Option* const reset = optionFind("armgba_cheat_restore");
	if (libretroCheatImportFor(&game, &advance, &retroArchs) || (reset && reset->visible)) {
		addItem("Cheats...", openCheatsPage, 0);
		helpFor("The cheat cartridge's lists: importing RetroArch's cheats, and resetting them.");
	}
}

// RetroArch's cheats for a game, from its database's .cht files: each file, and its cheats' names, read
// as a page opens (and again once a download ends), not each frame.
struct ChtFile {
	char path[PATH_SIZE + 300];
	char name[256];
	int count;
	char (*names)[128];
};
static struct ChtFile* chtFiles;
static int chtFileCount;
static bool chtListed;

static void forgetChtFiles(void) {
	for (int i = 0; i < chtFileCount; ++i) {
		free(chtFiles[i].names);
	}
	free(chtFiles);
	chtFiles = NULL;
	chtFileCount = 0;
	chtListed = false;
}

// The database to read: RetroArch's, else the app's copy, else the game's own files the app fetched.
static bool databaseNow(char* out, size_t size) {
	if (libretroRetroArchCheats(out, size)) {
		return true;
	}
	char folder[PATH_SIZE];
	long long age;
	dataPath(folder, sizeof(folder), "cheat-database");
	if (!hasDatabase(folder, &age)) {
		dataPath(folder, sizeof(folder), "cheat-database-game");
		if (!isFolder(folder)) {
			return false;
		}
	}
	snprintf(out, size, "%s", folder);
	return true;
}

static void readChtNames(struct ChtFile* file) {
	FILE* const f = fopen(file->path, "r");
	if (!f) {
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		int index;
		char field[16];
		char* const equals = strchr(line, '=');
		if (!equals || sscanf(line, "cheat%d_%15[a-z]", &index, field) != 2 || strcmp(field, "desc") || index < 0 || index > 4000) {
			continue;
		}
		char* value = equals + 1 + strspn(equals + 1, " \t\"");
		value[strcspn(value, "\"\r\n")] = '\0';
		char (*grown)[128] = realloc(file->names, (size_t) (file->count + 1) * sizeof(*grown));
		if (!grown) {
			break;
		}
		file->names = grown;
		snprintf(file->names[file->count++], 128, "%s", *value ? value : "(no name)");
	}
	fclose(f);
}

static int byChtName(const void* a, const void* b) {
	return strcmp(((const struct ChtFile*) a)->name, ((const struct ChtFile*) b)->name);
}

// The game's files in the database, false without a database.
static bool listChtFiles(const char* game, bool advance) {
	if (chtListed) {
		return true;
	}
	forgetChtFiles();
	char folder[PATH_SIZE];
	if (!databaseNow(folder, sizeof(folder))) {
		return false;
	}
	for (int i = advance ? ADVANCE : 0; i < (advance ? SYSTEMS : ADVANCE); ++i) {
		char system[PATH_SIZE + 64];
		snprintf(system, sizeof(system), "%s/%s", folder, systems[i]);
		DIR* const d = opendir(system);
		for (struct dirent* e; d && (e = readdir(d));) {
			size_t const length = strlen(e->d_name);
			if (length < 5 || strcasecmp(e->d_name + length - 4, ".cht") || !chtFileFits(e->d_name, game)) {
				continue;
			}
			struct ChtFile* const grown = realloc(chtFiles, (size_t) (chtFileCount + 1) * sizeof(*grown));
			if (!grown) {
				break;
			}
			chtFiles = grown;
			struct ChtFile* const file = &chtFiles[chtFileCount++];
			memset(file, 0, sizeof(*file));
			snprintf(file->path, sizeof(file->path), "%s/%s", system, e->d_name);
			snprintf(file->name, sizeof(file->name), "%.*s", (int) (length - 4), e->d_name);
			readChtNames(file);
		}
		if (d) {
			closedir(d);
		}
	}
	if (chtFileCount) {
		qsort(chtFiles, (size_t) chtFileCount, sizeof(*chtFiles), byChtName);
	}
	chtListed = true;
	return true;
}

// While the database downloads: a row saying so, and the progress popping up. True while it does.
static bool showDownload(void) {
	static bool wasRunning;
	bool const running = SDL_AtomicGet(&state) == RUNNING;
	if (!running) {
		if (wasRunning) {
			// What it brought is read again
			forgetChtFiles();
			if (gotDatabase) {
				osd("Cheat database downloaded");
			} else {
				osd(result == CHEAT_FETCH_READY ? "Couldn't download the cheat database; the copy there is kept"
				                                : "Couldn't download the cheat database");
			}
			wasRunning = false;
		}
		return false;
	}
	wasRunning = true;
	int done, all;
	httpProgress(&done, &all);
	char text[100];
	if (!SDL_AtomicGet(&downloading)) {
		snprintf(text, sizeof(text), "Unpacking Cheats");
		osdProgress(text, 100);
	} else if (all > 0) {
		snprintf(text, sizeof(text), "Downloading Cheats: %.1f of %.1f MB", done / 1024.0, all / 1024.0);
		osdProgress(text, (int) ((long long) done * 100 / all));
	} else {
		snprintf(text, sizeof(text), "Downloading Cheats: %.1f MB", done / 1024.0);
		osdProgress(text, 0);
	}
	addInfo("Downloading the cheat database...", "");
	return true;
}

static void downloadRow(int unused, int delta) {
	(void) unused;
	const char* game;
	bool advance;
	char folder[PATH_SIZE];
	if (!delta && libretroCheatGame(&game, &advance) && SDL_AtomicGet(&state) != RUNNING) {
		forgetChtFiles();
		fetch(game, advance, true, folder, sizeof(folder));
	}
}

// Download Cheat Database Again, for the game the page is for: the app's copy, whatever its age.
static void redownloadRow(int unused, int delta) {
	(void) unused;
	const char* game;
	bool advance, retroArchs;
	char folder[PATH_SIZE];
	if (delta || SDL_AtomicGet(&state) == RUNNING) {
		return;
	}
	if (!libretroCheatGame(&game, &advance) && !libretroCheatImportFor(&game, &advance, &retroArchs)) {
		return;
	}
	// A download finished and not yet taken by the core is done with
	SDL_AtomicSet(&state, IDLE);
	forgetChtFiles();
	refreshNow = true;
	fetch(game, advance, true, folder, sizeof(folder));
	refreshNow = false;
}

static void offerDownload(void) {
	addItem("Download Cheat Database", downloadRow, 0);
	helpFor("Downloads RetroArch's cheat database as its Online Updater does, about 40 MB.");
}

// Game > Cheats > RetroArch Cheats: each of the database's files for the game, which a press adds to
// the game's cheats, switched off, and their cheats.
static void addFileRow(int which, int delta) {
	if (delta || which >= chtFileCount) {
		return;
	}
	int const added = cheatListImport(chtFiles[which].path);
	if (added < 0) {
		osd("Can't read %s", chtFiles[which].name);
		return;
	}
	cheatListApply();
	cheatListSave();
	osd(added == 1 ? "Added 1 cheat" : "Added %d cheats", added);
	popMenu();
}

static void gameCheatsPage(void) {
	const char* game;
	bool advance;
	if (!libretroCheatGame(&game, &advance) || showDownload()) {
		return;
	}
	if (!listChtFiles(game, advance)) {
		offerDownload();
		return;
	}
	if (!chtFileCount) {
		addInfo("RetroArch has no cheats for this game", "");
	}
	for (int i = 0; i < chtFileCount; ++i) {
		static char labels[64][48];
		addHeader(chtFiles[i].name);
		if (i < 64) {
			snprintf(labels[i], sizeof(labels[i]), chtFiles[i].count == 1 ? "Add This Cheat" : "Add These %d Cheats", chtFiles[i].count);
			addItem(labels[i], addFileRow, i);
			helpFor("Adds them to this game's cheats, switched off, for you to switch on.");
		}
		for (int c = 0; c < chtFiles[i].count; ++c) {
			addInfo(chtFiles[i].names[c], "");
		}
	}
	char retroArchs[PATH_SIZE];
	if (!libretroRetroArchCheats(retroArchs, sizeof(retroArchs))) {
		addHeader("Cheat Database");
		addItem("Download Cheat Database Again", redownloadRow, 0);
		helpFor("Downloads RetroArch's cheat database now, as its Online Updater does (about 40 MB), keeping the copy there is if that fails.");
	}
}

static void openGameCheats(int unused, int delta) {
	(void) unused;
	if (!delta) {
		forgetChtFiles();
		pushMenu(gameCheatsPage, "RetroArch Cheats");
	}
}

void cheatGameRows(void) {
	const char* game;
	bool advance;
	if (libretroCheatGame(&game, &advance)) {
		addItem("RetroArch Cheats...", openGameCheats, 0);
		helpFor("The cheats RetroArch's cheat database has for this game, to add to its list.");
	}
}

// Extras > Cheats > Cheats for the game on top: those Import would take into the cartridge.
static char (*cartNames)[96];
static size_t cartNameCount;
static bool cartListed;
// The build RetroArch's codes are for, when not the game on top's, else -1; and the game's.
static int cartOtherId = -1, cartGameId = -1;

static void cartCheatsPage(void) {
	const char* game;
	bool advance, retroArchs;
	if (!libretroCheatImportFor(&game, &advance, &retroArchs) || showDownload()) {
		cartListed = false;
		return;
	}
	char folder[PATH_SIZE];
	if (!databaseNow(folder, sizeof(folder))) {
		offerDownload();
		return;
	}
	if (!cartListed) {
		enum { MOST = 1024 };
		if (!cartNames) {
			cartNames = malloc(MOST * sizeof(*cartNames));
		}
		cartNameCount = cartNames ? libretroCartridgeCheatNames(folder, cartNames, MOST, &cartOtherId, &cartGameId) : 0;
		cartListed = true;
	}
	if (!cartNameCount) {
		addInfo("RetroArch has no cheats this cartridge takes for the game", "");
		return;
	}
	addItem("Import Cheats from RetroArch", importRow, 0);
	helpFor("Adds those the cartridge doesn't have yet (not their master codes, which it has), switched off.");
	if (cartOtherId >= 0) {
		static char other[96];
		snprintf(other, sizeof(other), "For another version of the game (%04X; this one is %04X)", cartOtherId, cartGameId);
		addInfo(other, "");
		helpFor("RetroArch's codes for this title were made for another build of it, so they may not work on this one.");
	}
	static char heading[64];
	snprintf(heading, sizeof(heading), "In RetroArch's Cheat Database (%zu)", cartNameCount);
	addHeader(heading);
	for (size_t i = 0; i < cartNameCount; ++i) {
		addInfo(cartNames[i], "");
	}
}

static void openCartCheats(int unused, int delta) {
	(void) unused;
	if (!delta) {
		cartListed = false;
		pushMenu(cartCheatsPage, "Cheats for the Game on Top");
	}
}
