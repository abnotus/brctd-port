# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(cotd LANGUAGES C CXX)

# ctd: an engine for Countdown to Doomsday (README.md, BUILDING.md).
# The third-party libraries are SDL3 and Nuked-OPL3 (THIRD_PARTY.md): each
# fetched as source at configure time, checked against its SHA-256 and
# linked statically; nothing is vendored. For an offline build, point
# FETCHCONTENT_SOURCE_DIR_SDL3 and FETCHCONTENT_SOURCE_DIR_NUKED_OPL3 at
# unpacked copies of the same sources.

# ccache when installed (SDL3 included), with paths relative to the source
# tree so worktrees share hits. Off with -DCOTD_CCACHE=OFF.
option(COTD_CCACHE "Compile through ccache when it is found" ON)
find_program(CCACHE_PROGRAM ccache)
if(COTD_CCACHE AND CCACHE_PROGRAM AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  set(_ccache ${CMAKE_COMMAND} -E env CCACHE_BASEDIR=${CMAKE_SOURCE_DIR} ${CCACHE_PROGRAM})
  set(CMAKE_C_COMPILER_LAUNCHER ${_ccache})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${_ccache})
  # ccache can't cache a compile that uses a precompiled header (SDL3's).
  set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The release's flags (docs/notes/release.md, the checklist's step 1, the
# owner's decision in session 74's review): -Os (MinSizeRel) for the engine
# and SDL3 alike, the executable stripped, and link-time optimization
# except on Windows (MinGW-w64 GCC 13's LTO link stops with an internal
# compiler error). Off by default: the dev builds stay Release -O3.
option(COTD_RELEASE_FLAGS "The release's flags: -Os, stripped, LTO except on Windows" OFF)
if(COTD_RELEASE_FLAGS)
  set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
  if(NOT MSVC)
    # Goal 81: each function and datum in its own section, and the link
    # drops what nothing reaches (most of all the static C++ runtime's):
    # the Linux binary 4.5 -> 4.0 MB. For SDL3 too (set before it is fetched).
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
  endif()
  if(NOT WIN32)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _cotd_ipo OUTPUT _cotd_ipo_why LANGUAGES C CXX)
    if(NOT _cotd_ipo)
      message(FATAL_ERROR "COTD_RELEASE_FLAGS: no link-time optimization here: ${_cotd_ipo_why}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
set(COTD_SDL3_VERSION 3.4.18)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-${COTD_SDL3_VERSION}/SDL3-${COTD_SDL3_VERSION}.tar.gz
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE AND NOT DEFINED SDL_UNIX_CONSOLE_BUILD)
  # SDL needs X11 (with Xext) or Wayland development headers for desktop
  # windows. Without them, build SDL without desktop video: the program
  # builds, but its window won't open on this machine.
  find_path(COTD_XEXT_H X11/extensions/Xext.h)
  find_path(COTD_WAYLAND_H wayland-client.h)
  if(NOT COTD_XEXT_H AND NOT COTD_WAYLAND_H)
    message(WARNING "No X11 (libx11-dev, libxext-dev) or Wayland development "
                    "headers: SDL3 is built without desktop video, so the "
                    "window can't open.")
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
  elseif(COTD_XEXT_H)
    # SDL stops on a wanted X11 extension whose headers are missing; turn
    # off the ones this system lacks (they're optional for a window).
    foreach(pair XCURSOR:X11/Xcursor/Xcursor.h XDBE:X11/extensions/Xdbe.h
                 XFIXES:X11/extensions/Xfixes.h XINPUT:X11/extensions/XInput2.h
                 XRANDR:X11/extensions/Xrandr.h XSCRNSAVER:X11/extensions/scrnsaver.h
                 XSHAPE:X11/extensions/shape.h XSYNC:X11/extensions/sync.h
                 XTEST:X11/extensions/XTest.h)
      string(REPLACE ":" ";" pair "${pair}")
      list(GET pair 0 ext)
      list(GET pair 1 header)
      find_path(COTD_HAVE_${ext} ${header})
      if(NOT COTD_HAVE_${ext} AND NOT DEFINED SDL_X11_${ext})
        set(SDL_X11_${ext} OFF CACHE BOOL "" FORCE)
      endif()
    endforeach()
  endif()
endif()
FetchContent_MakeAvailable(SDL3)

# Nuked-OPL3 (LGPL-2.1-or-later; THIRD_PARTY.md): the OPL2/OPL3 chip
# the music driver's writes play on (src/app/audio.cpp). Fetched at a
# pinned commit, checked against its SHA-256; two C files, built here.
set(COTD_NUKED_OPL3_COMMIT 765ec962e473aeb767e4cba74ffdc8f588ffbfe8)
FetchContent_Declare(nuked_opl3
  URL https://github.com/nukeykt/Nuked-OPL3/archive/${COTD_NUKED_OPL3_COMMIT}.tar.gz
  URL_HASH SHA256=2fad908c3904d3ef51b55e0d6a8980c7142942e2d0baa7e99bf038cf0a41199d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(nuked_opl3)
if(NOT nuked_opl3_POPULATED)
  FetchContent_Populate(nuked_opl3)
endif()
add_library(nuked_opl3 STATIC ${nuked_opl3_SOURCE_DIR}/opl3.c)
target_include_directories(nuked_opl3 PUBLIC ${nuked_opl3_SOURCE_DIR})

add_library(cotd_core STATIC
  src/core/dax.cpp
  src/core/exepack.cpp
  src/core/files.cpp
  src/core/framebuffer.cpp
  src/core/gametext.cpp
  src/core/install.cpp
  src/core/real48.cpp
  src/core/sha256.cpp
  src/core/image.cpp
  src/core/logbook_txt.cpp
  src/core/seal.cpp
  src/core/music.cpp
  src/core/speaker.cpp
  src/core/sn76489.cpp
  src/core/view3d.cpp
  src/ecl/script.cpp
  src/game/game.cpp
  src/game/input.cpp
  src/game/interp.cpp
  src/game/modify.cpp
  src/game/save.cpp
  src/game/screen.cpp
  src/game/adventure.cpp
  src/game/travel.cpp
  src/game/options.cpp
  src/game/space_combat.cpp
  src/game/frontend.cpp
  src/game/change.cpp
  src/game/chargen.cpp
  src/game/sheet.cpp
  src/game/gear.cpp
  src/game/shop.cpp
  src/game/skills.cpp
  src/game/icons.cpp
  src/game/textwin.cpp
  src/game/train.cpp
  src/game/combat_rules.cpp
  src/game/combat.cpp
  src/game/combat_screen.cpp
  src/game/combat_turn.cpp
  src/game/combat_attack.cpp
  src/game/combat_after.cpp
  src/game/combat_move.cpp
  src/game/combat_ai.cpp
  src/game/combat_area.cpp
)
target_include_directories(cotd_core PUBLIC src)
if(MSVC)
  target_compile_options(cotd_core PRIVATE /W4)
else()
  target_compile_options(cotd_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The build's git description, for the key log (cmake/version.cmake).
add_custom_target(cotd_version ALL
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}
          -DOUT=${CMAKE_BINARY_DIR}/generated/version.hpp
          -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/version.hpp ${CMAKE_BINARY_DIR}/generated/version_rc.h)

# The sound lab's designs (data/sfx_adlib.toml) built in (D-0069).
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/generated/sfx_designs.hpp
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/data/sfx_adlib.toml
          -DOUT=${CMAKE_BINARY_DIR}/generated/sfx_designs.hpp
          -P ${CMAKE_SOURCE_DIR}/cmake/sfx_designs.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/data/sfx_adlib.toml ${CMAKE_SOURCE_DIR}/cmake/sfx_designs.cmake)
add_custom_target(cotd_sfx_designs DEPENDS ${CMAKE_BINARY_DIR}/generated/sfx_designs.hpp)

# The Log Book's sealed text (data/cotd.dat; CLAUDE.md hard rule 1's one
# exception, D-0083) built in; an empty one while the file is missing.
# COTD_SEALED_FILE names another file (a build without one: session 72's
# first-run check).
set(COTD_SEALED_FILE ${CMAKE_SOURCE_DIR}/data/cotd.dat CACHE FILEPATH "The Log Book's sealed text")
set(COTD_SEALED_DEPENDS ${CMAKE_SOURCE_DIR}/cmake/sealed_text.cmake)
if(EXISTS ${COTD_SEALED_FILE})
  list(APPEND COTD_SEALED_DEPENDS ${COTD_SEALED_FILE})
endif()
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/generated/sealed_text.hpp
  COMMAND ${CMAKE_COMMAND} -DIN=${COTD_SEALED_FILE}
          -DOUT=${CMAKE_BINARY_DIR}/generated/sealed_text.hpp
          -P ${CMAKE_SOURCE_DIR}/cmake/sealed_text.cmake
  DEPENDS ${COTD_SEALED_DEPENDS})
add_custom_target(cotd_sealed_text DEPENDS ${CMAKE_BINARY_DIR}/generated/sealed_text.hpp)
add_dependencies(cotd_core cotd_sealed_text)
target_include_directories(cotd_core PRIVATE ${CMAKE_BINARY_DIR}/generated)

add_executable(cotd src/main.cpp src/app/window.cpp src/app/settings.cpp src/app/audio.cpp src/app/synth.cpp
  src/app/sfxdesign.cpp src/app/console.cpp src/app/play.cpp)
add_dependencies(cotd cotd_version cotd_sfx_designs)
# The program's file is ctd (session 78, the owner: cotd was the working
# name); the build target keeps the name cotd.
set_target_properties(cotd PROPERTIES OUTPUT_NAME ctd)
target_include_directories(cotd PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(cotd PRIVATE cotd_core nuked_opl3 SDL3::SDL3-static)
if(WIN32 AND NOT MSVC)
  # One self-contained .exe: no MinGW runtime DLLs next to it.
  target_link_options(cotd PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(COTD_RELEASE_FLAGS AND NOT MSVC)
  target_link_options(cotd PRIVATE -s)
endif()
if(COTD_RELEASE_FLAGS AND UNIX AND NOT APPLE)
  # Goal 81: the C++ runtime linked in, so the release needs only the
  # system's C library (glibc 2.29 and later when built in the Steam
  # Runtime's sniper SDK, as the release workflow builds it).
  target_link_options(cotd PRIVATE -static-libstdc++ -static-libgcc)
endif()
if(WIN32)
  # The manifest: the process code page UTF-8 (cmake/cotd.manifest).
  enable_language(RC)
  target_sources(cotd PRIVATE cmake/cotd.rc)
  # Goal 81 (the owner): a GUI program, so no console window opens behind
  # the game; src/app/console.cpp joins a terminal's console when started
  # from one (D-0094). main() stays the entry point (MinGW-w64's start-up).
  set_target_properties(cotd PROPERTIES WIN32_EXECUTABLE ON)
endif()

# The packages (CPack, goal 81): `cpack` in a build of the release's tree
# makes ctd-VERSION-linux-x86_64.tar.xz or ctd-VERSION-windows-x86_64.zip:
# the program, README.md, OPTIONS.md, LICENSE, the game folder with its
# note, and every licence in licenses/. Only the release's tree has the
# player's documents at its top (OPTIONS.md).
if(EXISTS ${CMAKE_SOURCE_DIR}/OPTIONS.md)
  file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION COTD_PACKAGE_VERSION LIMIT_COUNT 1)
  if(WIN32)
    set(_os windows)
    set(CPACK_GENERATOR ZIP)
  else()
    set(_os linux)
    set(CPACK_GENERATOR TXZ)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_arch x86_64)
  else()
    set(_arch ${CMAKE_SYSTEM_PROCESSOR})
  endif()
  install(TARGETS cotd RUNTIME DESTINATION .)
  install(FILES README.md OPTIONS.md LICENSE DESTINATION .)
  install(FILES cmake/package/PUT_YOUR_GAME_FILES_HERE.txt DESTINATION game)
  install(FILES THIRD_PARTY.md DESTINATION licenses)
  install(FILES ${sdl3_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME SDL3.txt)
  install(FILES ${nuked_opl3_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME Nuked-OPL3.txt)
  if(WIN32)
    # winpthreads' notice: the MinGW-w64 runtime linked into ctd.exe.
    install(FILES cmake/package/MinGW-w64.txt DESTINATION licenses)
  endif()
  set(CPACK_PACKAGE_NAME ctd)
  set(CPACK_PACKAGE_VERSION ${COTD_PACKAGE_VERSION})
  set(CPACK_PACKAGE_FILE_NAME ctd-${COTD_PACKAGE_VERSION}-${_os}-${_arch})
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
  set(CPACK_THREADS 0)
  include(CPack)
endif()

