// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows build is a GUI program (goal 81, the owner: no console window
// behind the game). Started from a terminal, it prints there all the same.
#pragma once

// Windows: each of stdin, stdout and stderr the caller didn't redirect is
// joined to the parent process's console, when there is one (a terminal);
// started from Explorer there is none and none is made. Redirected streams
// (a file, a pipe: the tests, a script) stay as they are. Elsewhere: nothing.
void attach_parent_console();
