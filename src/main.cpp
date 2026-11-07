// SPDX-License-Identifier: GPL-3.0-or-later
// ctd: a native engine for Countdown to Doomsday (README.md).
//
//   ctd            play: the game in the `game` folder beside the program,
//                  its saves, settings and key logs beside the program
//                  (README.md, OPTIONS.md)
//   ctd version    print the build
//
// Exit status: 0 ok; 1 usage; 2 rejected input (an unidentified game
// folder, damaged data); 3 the game's own fatal error.

#include <cstdio>
#include <exception>
#include <string>

#include "app/console.hpp"
#include "app/play.hpp"
#include "app/window.hpp"
#include "core/common.hpp"
#include "game/game.hpp"
#include "version.hpp"

int main(int argc, char** argv) {
    using namespace cotd;
    attach_parent_console();   // Windows: a GUI program's terminal output (D-0094)
    const std::string cmd = argc > 1 ? argv[1] : "";
    try {
        if (cmd == "version") {
            std::printf("%s\n", COTD_VERSION);
            return 0;
        }
        if (argc > 1) {
            std::fprintf(stderr, "ctd: takes no options (`ctd` plays, `ctd version` prints the build)\n");
            return 1;
        }
        return play();
    } catch (const Fatal& e) {
        std::printf("%s\n", e.what());
        if (g_player) show_player_message(e.what(), true);
        return 3;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ctd: error: %s\n", e.what());
        if (g_player) {
            // At most 30 lines in the box (a folder with every file missing).
            std::string text = e.what();
            std::size_t at = 0;
            for (int n = 0; n < 30 && at != std::string::npos; ++n) at = text.find('\n', at + 1);
            if (at != std::string::npos && at + 1 < text.size()) text = text.substr(0, at) + "\n...";
            show_player_message(text, true);
        }
        return 2;
    }
}
