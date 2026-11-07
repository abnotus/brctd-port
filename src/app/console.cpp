// SPDX-License-Identifier: GPL-3.0-or-later
#include "console.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

void attach_parent_console() {
    auto given = [](DWORD id) {
        HANDLE h = GetStdHandle(id);
        return h != nullptr && h != INVALID_HANDLE_VALUE;
    };
    // Read before AttachConsole, which can set the handles it joins.
    const bool in = given(STD_INPUT_HANDLE), out = given(STD_OUTPUT_HANDLE), err = given(STD_ERROR_HANDLE);
    if (in && out && err) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;   // no terminal: Explorer, a shortcut
    if (!out) std::freopen("CONOUT$", "w", stdout);
    if (!err) std::freopen("CONOUT$", "w", stderr);
    if (!in) std::freopen("CONIN$", "r", stdin);
}
#else
void attach_parent_console() {}
#endif
