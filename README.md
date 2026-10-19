Automatically switches audio outputs when devices are connected or disconnected.

## Requirements
* Windows 10 1607 or later
* [The Elder Scrolls V: Skyrim Special Edition](https://store.steampowered.com/app/489830), game version 1.6.1130 or later (SE 1.5.97 and VR are not supported)
* [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR/tree/ng)
	* Add the environment variable `COMMONLIB_SSE_FOLDER` with the value as the path to the CommonLibSSE-NG folder
* [Vcpkg](https://github.com/microsoft/vcpkg)
	* Add the environment variable `VCPKG_ROOT` with the value as the path to the folder containing vcpkg
* [Visual Studio](https://visualstudio.microsoft.com/)
	* Desktop development with C++, which includes CMake and Ninja

## Building
From a Developer Command Prompt for x64:
```
cmake --preset release
cmake --build build/release
```

## Tips
* Set the environment variable `SKYRIM_MODS_FOLDER` to copy the built plugin into `<SKYRIM_MODS_FOLDER>/AutoAudioSwitch`
