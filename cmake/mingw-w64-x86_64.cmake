# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-compile for 64-bit Windows with MinGW-w64 (GCC, POSIX threads).
#
#   cmake -S . -B build-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#
# The compilers are looked up on PATH, then under MINGW_ROOT (a directory
# holding bin/x86_64-w64-mingw32-g++-posix, e.g. Debian/Ubuntu's mingw-w64
# packages unpacked without root: docs/notes/build.md).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(_triple x86_64-w64-mingw32)

set(MINGW_ROOT "$ENV{MINGW_ROOT}" CACHE PATH "MinGW-w64 prefix (contains bin/)")
set(_hints "")
if(MINGW_ROOT)
  list(APPEND _hints "${MINGW_ROOT}/bin")
endif()

find_program(CMAKE_C_COMPILER NAMES ${_triple}-gcc-posix ${_triple}-gcc HINTS ${_hints} REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${_triple}-g++-posix ${_triple}-g++ HINTS ${_hints} REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${_triple}-windres HINTS ${_hints})
find_program(CMAKE_AR NAMES ${_triple}-ar HINTS ${_hints})
find_program(CMAKE_RANLIB NAMES ${_triple}-ranlib HINTS ${_hints})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
