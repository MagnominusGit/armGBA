# What the standalone app (frontend/) is for this core: its names, Android package, look and the
# games it opens. CMake writes them into app-info.h for the C code.
set(APP_ID armgba)
set(APP_NAME armGBA)
set(APP_WORDMARK ARMGBA)
set(APP_JAVA_PACKAGE org.armgba.app)
set(APP_JAVA_CLASS Armgba)
# The console's screen: the window is a multiple of it before a game says its own.
set(APP_WINDOW 240 160)
# The game list's files (archives too), and what's looked for inside an archive.
set(APP_EXTENSIONS gba gb gbc zip 7z gba2 gb2 bin)
set(APP_ARCHIVED gba gb gbc bin gba2 gb2)
# The systems: each one's key (its folder setting is <key>_folder), name, and the folder names it's
# found by in the Games folder (its key and these, any case).
set(APP_SYSTEMS gb gbc gba)
set(APP_SYSTEM_gb "Game Boy" gameboy "game boy")
set(APP_SYSTEM_gbc "Game Boy Color" gameboycolor "game boy color")
set(APP_SYSTEM_gba "Game Boy Advance" gameboyadvance "game boy advance")
# Help texts that name the console's buttons and what the app draws over its picture.
set(APP_HELP_SWAP "Off: A is the right button, as on a Game Boy. On: A and B swap, and X and Y.")
set(APP_HELP_SHADERS "The filter, and the GPU's LCD grid and Game Boy pixel shadow.")
