# Building ctd

## You need

- A C++20 compiler (tested with GCC 13 and 14)
- CMake 3.24 or newer, and Ninja (Ubuntu 24.04 and Debian 12 or newer have
  them; Ubuntu 22.04's CMake is too old)
- Internet access when CMake configures: it downloads SDL 3.4.18 and
  Nuked-OPL3 and checks each against a SHA-256. A mismatch stops the build.

## Linux

Debian/Ubuntu package names:

```
sudo apt install build-essential cmake ninja-build git pkg-config
```

For the window, at least `libx11-dev libxext-dev`. For all of SDL's X11
features also `libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev
libxss-dev libxtst-dev libxkbcommon-dev`. For Wayland: `libwayland-dev
libdecor-0-dev libxkbcommon-dev libegl-dev`.

For sound, `libpulse-dev` (or `libpipewire-0.3-dev`, `libasound2-dev`).

Without window headers the program builds but cannot open its window.
Without audio headers it is silent. If you install headers later, run
CMake again.

```
cmake -S . -B build -G Ninja
ninja -C build
```

The program is `build/ctd`.

## Windows (cross-built from Linux)

```
sudo apt install mingw-w64
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
ninja -C build-win
```

The program is `build-win/ctd.exe`. It needs no DLLs. If the MinGW-w64
compilers are not on `PATH`, set `MINGW_ROOT` to the folder that holds
their `bin/`.

A native Windows build (MSVC or MSYS2) isn't tested.

## Offline build

Unpack the two sources (THIRD_PARTY.md has their URLs and hashes) and
add `-DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL3-3.4.18` and
`-DFETCHCONTENT_SOURCE_DIR_NUKED_OPL3=/path/to/Nuked-OPL3-<commit>` to
the first CMake command. Sources given this way aren't checked against
their hashes.

## Release flags

`-DCOTD_RELEASE_FLAGS=ON` builds the way the downloads are built: optimized
for size, stripped, and with link-time optimization on Linux.

The Linux download is built in Valve's Steam Runtime 3 "sniper" SDK, so it
runs on any distribution with glibc 2.29 or newer. A build on your own
system runs on systems at least as new as yours.

## Packages

After a build, `cpack --config build/CPackConfig.cmake -B dist` (or
`build-win/CPackConfig.cmake`) makes the same archive as the downloads
(`.tar.xz` on Linux, `.zip` for Windows) in `dist`.

## Running

Put your game files in a `game` folder beside the program (`build/game`
for `build/ctd`) and start it. See README.md.
