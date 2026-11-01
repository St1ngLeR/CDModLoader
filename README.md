<div align="center">

**English** | [Русский](README_RU.md)

</div>

# Crashday ModLoader
**ModLoader** is an ASI plugin that allows you to run a game with a different number of modifications, making their installation, editing and removal much easier, without changing a single file of the main game.

## Table of content
- [Installation & first launch](#installation--first-launch)
- [Installing mods](#installing-mods)
- [Uninstalling mods](#uninstalling-mods)
- [Setting up the ModLoader](#setting-up-the-modloader)
- [Setting up the profiles](#setting-up-the-profiles)
- [Mod profile launch argument](#mod-profile-launch-argument)
- [Separate save files for each mod profile](#separate-save-files-for-each-mod-profile)
- [Supported mod files](#supported-mod-files)
- [Merging track editor categories](#merging-track-editor-categories)

## Installation & first launch

1. Before you start working with the ModLoader, you need to use the Crashday **1.2** executable file. To make sure, check the file information with the values below:

| Executable file name  | Game version | File size (in bytes) | Checksum (CRC-32) | Checksum (MD-5) |
| --------------------- | ------------ | -------------------- | ----------------- | --------------- |
| Crashday.exe | 1.2 | 3378688 | 8D452A29 | F027022CF84FB0A7A9DB9770545346B9

2. To make ASI plugin work in the game you need to use [ThirteenAG's Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (recommended to use **dinput.dll 32-bit**) and put plugin files in to game directory.
3. You should launch the game for the first time with ModLoader and close it. If you see a new `modloader.ini` file and a `modloader` folder in the game directory, then the modloader is loaded and ready for further work.

## Installing mods

To install mods, place them into their own separate folder within the `modloader` folder. That's all.

For example:
```
modloader/
└── awesome_mod/
    ├── trkdata/
    ├── textures/
    └── ...
└── yet_another_awesome_mod/
    ├── sounds/
    ├── editor/
    └── ...
```

## Uninstalling mods

To remove modifications, simply delete their individual folders within `modloader`. If you wish to keep the mod files but not use them, you should disable these mods in the relevant profiles. (see [below](#setting-up-the-profiles))

## Setting up the ModLoader

The main settings are located within the `modloader.ini` file, which is locatred in the same location as the game's executable file.

- `Enabled` (default `1`) - Enable/disable Modloader without needing to interact with its .asi file.
- `Log` (default `0`) - Enable/disable the generation of a log file for the ModLoader. The log file is created in a big size and it's recommended to enable it if you encounter an easily repeatable failure.
- `LoadAsiPlugins` (default `1`) - Enable/disable the loading of ASI plugins. This can be useful if you want to run them independently of the ModLoader environment, or if you don't trust the plugins.
- `Profile` (default `default`) - Profile name at startup.

## Setting up the profiles

ModLoader loads mods into the game via profiles - sets of selected modifications configured at the user's discretion.

If at least one mod is installed, files for new profiles are created in the `modloader/.profiles` folder. The profile file is an INI file named after the profile specified by the user.

Inside the profile file, there are two sections: `[States]` and `[Priorities]`:
- The `[States]` section controls whether a mod is enabled or disabled, using `true` or `false`, respectively.
- The `[Priorities]` section determines the order in which the game loads various mods. This option is useful if you want to overwrite certain parts of mods.

For example:
```
[States]
CrashPatch=true
Widescreen Fix=true
Deathday=false
Jane's Mod=true
Default Nitro=true

[Priorities]
CrashPatch=50
Widescreen Fix=50
Deathday=50
Jane's Mod=50
Default Nitro=100
```

**Important!** If you run the game with the `default` profile selected, all new mods will be enabled automatically. In other profiles, mods will be disabled in order to preserve the user's specific selections for those profiles.

## Mod profile launch argument

To avoid constantly changing the selected profile in `modloader.ini`, you can instead launch the game with a specific profile via a shortcut by using the `-modprof` argument.

The following formats are accepted:
```
-modprof <profile_name>
-modprof "<profile_name>"
-modprof=<profile_name>
-modprof="<profile_name>"
```

As mentioned earlier, if the loader detects that this is a new profile, it will create a corresponding file in the profiles folder.

## Separate save files for each mod profile

To avoid disrupting your saves, each profile has its own save folder, thereby ensuring that the profiles remain isolated from one another.

For all profiles except `default`, save files are located in `modloader/.savegames/<profile_name>`. For the `default` profile, the path remains unchanged.

## Supported mod files

| File type  | Is supported? |
| ---------- | ------------- |
| Textures (.dds/.tga) | <p align=center>✅</p>
| 3D-models (.p3d) | <p align=center>✅</p>
| Sounds (.wav) | <p align=center>✅</p>
| Music (.mp3/.ogg) | <p align=center>✅</p>
| Human-readable files (.cca/.lst/.txt/.cfl/...) | <p align=center>✅</p>
| Plugins (.asi) | <p align=center>✅*</p>

\* <small>ASI plugins can be unstable, especially if they interact with the game's file system. It's recommended to evaluate the stability of each plugin individually.</small>

## Merging track editor categories

Although new mod files work without issues via ModLoader, a method has been devised to merge new categories of modded track editor elements (tiles, checkpoints, dynamic objects) with the default categories, ensuring full support.

For the merge to succeed, the mod must contain the files `fields.cat`, `dynobj.cat`, or `cps.cat` in the `trkdata/editor` folder. Within the files, the categories should be set up just as they would be ready for use in the game, but without the default categories.

For example - `awesome_mod/trkdata/editor/fields.cat`:
```
						# List for Crashday track editor tile categories
1						# Number of categories

Next:						# CFL file / PositionX / PositionY / Rotation in display
My awesome tiles						# Name
3						# Objects per category
tile1.cfl 1 1 0 My awesome tile 1
tile2.cfl 2 1 0 My awesome tile 2
tile3.cfl 3 1 0 My awesome tile 3
```

If all goes well, you'll see new categories from mods in the track editor, in addition to the default categories.

This moment is optional, mods for track elements can work without new categories, but their operation without configured categories is not guaranteed by the mods authors.
