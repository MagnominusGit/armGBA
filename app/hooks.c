/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "hooks.h"
#include "link.h"

const struct AppHooks appHooks = {
	.start = lcdStart,
	.pictureCame = lcdPictureCame,
	.pictureUpload = lcdPictureUpload,
	.pictureDraw = lcdPictureDraw,
	.rendererGoing = lcdRendererGoing,
	.coreScale = lcdCoreScale,
	.optionRead = lcdOptionRead,
	.optionShown = lcdOptionShown,
	.groupRows = lcdGroupRows,
	.shaderRows = lcdShaderRows,
	.settingsDefaults = lcdSettingsDefaults,
	.settingRead = lcdSettingRead,
	.settingsWrite = lcdSettingsWrite,
	.netplayPlayer = libretroSetNetplayPlayer,
	.firmwareCrc = libretroFirmwareCrc,
	.sessionValue = netSessionValue,
	.takeSave = libretroTakeSave,
	.sessionEnded = libretroSessionEnded,
	.playOn = libretroPlayOn,
	.saveManagerFolders = libretroSaveManagerFolders,
	.cheevosConsole = netCheevosConsole,
};
