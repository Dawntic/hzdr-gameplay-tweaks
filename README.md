# Horizon Zero Dawn Remastered RenderDoc Support

A `winhttp.dll` proxy mod that makes Horizon Zero Dawn Remastered capturable with RenderDoc. Forked from the
Gameplay Tweaks & Cheat Menu mod; everything not needed for RenderDoc has been removed.

## Installation

- For developers, edit `CMakeUserEnvVars.json` and set `GAME_ROOT_DIRECTORY` to Horizon's root directory. The build script will automatically copy library files to the game folder.

- For manual Steam installs, copy `winhttp.dll` and `mod_config.ini` to the game's root folder. An example path is: `C:\Program Files (x86)\Steam\steamapps\common\Horizon Zero Dawn Remastered\`.

## Building

### Requirements

- This repository and all of its submodules cloned.
- **Visual Studio 2022** 17.9.6 or newer.
- **CMake** 3.26 or newer.
- **Vcpkg**.

### hzdr-gameplay-tweaks (Option 1, Visual Studio UI)

1. Open `CMakeLists.txt` directly or open the root folder containing `CMakeLists.txt`.
2. Select one of the preset configurations from the dropdown, e.g. `Universal Release x64`.
3. Build and wait for compilation.
4. Build files are written to the bin folder. Done.

### hzdr-gameplay-tweaks (Option 2, Powershell Script)

1. Open a Powershell command window.
2. Run `.\Make-Release.ps1` and wait for compilation.
3. Build files from each configuration are written to the bin folder and archived. Done.

## Usage

Configure everything under `[RenderDoc]` in `mod_config.ini`. Set `LogToFile = true` under `[General]` to read the mod's log (`mod_log.txt`).

## License

- No license provided. TBD.
- Dependencies are under their respective licenses.
