# Third-party code

ctd is licensed under GPL-3.0-or-later (`LICENSE`). It links two libraries.
CMake downloads each as source at build time and links it statically;
nothing is copied into this repository. Each release also offers both
sources beside its downloads.

## SDL 3.4.18

- Licence: zlib
- Source: <https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz>
- SHA-256: `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`

## Nuked-OPL3, commit 765ec962e473aeb767e4cba74ffdc8f588ffbfe8

- Licence: LGPL-2.1-or-later
- Source: <https://github.com/nukeykt/Nuked-OPL3/archive/765ec962e473aeb767e4cba74ffdc8f588ffbfe8.tar.gz>
- SHA-256: `2fad908c3904d3ef51b55e0d6a8980c7142942e2d0baa7e99bf038cf0a41199d`

## The compilers' runtime

Both programs also contain their compiler's runtime, linked statically:
GCC's libstdc++ and libgcc (the GPL with the GCC Runtime Library
Exception, which needs no notice) and, in the Windows program, the
MinGW-w64 runtime (its C runtime under the ZPL-2.1, winpthreads and
other parts under MIT/BSD-style terms). The Windows download carries the
MinGW-w64 runtime's notices in `licenses/MinGW-w64.txt`.

The downloads' `licenses/` folder holds each library's licence text
(`SDL3.txt`, `Nuked-OPL3.txt`).
