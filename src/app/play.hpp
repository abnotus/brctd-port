// SPDX-License-Identifier: GPL-3.0-or-later
// The start of play: the game folder beside the program, identified; the
// saves folder; the settings file; the window and the key log.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "app/settings.hpp"
#include "app/window.hpp"
#include "core/files.hpp"
#include "game/game.hpp"

namespace cotd {

// Set when a player started the game: its errors are shown in a message
// box too (show_player_message).
extern bool g_player;

// The game folder: `game` beside the executable, wherever it is started
// from; "game" in the current folder only when SDL can't tell where the
// executable is.
std::string default_game_dir();

// The port's own folder, where what it writes goes (nothing is written
// into the game folder): the executable's folder, else (when that can't
// be written) the user's data folder beside settings.ini. With a trailing
// separator; "" (the current folder) when neither is known.
const std::string& port_folder();

std::filesystem::path u8_path(const std::string& s);

// A number option's value: the whole value a number in [lo, hi], else an
// error naming the option and its form.
long number_option(const std::string& value, long lo, long hi, const std::string& form);

std::string now_text(const char* fmt);

// TP Randomize's seed from the current time (0BE4:11D7).
std::uint32_t randomize_now();

// The Log Book's text (D-0083): "on" (the built-in sealed text) or "off".
void configure_logbook(game::Game& g, const std::string& game_dir, const std::string& text);

// What a session of play runs with.
struct PlayConfig {
    std::string game_dir;
    std::string settings_path;   // "" for none
    std::string release;         // the identified release
    std::string save_dir;
    std::string carried;         // what the first start copied in, or ""
    WindowOptions opt;
    std::uint32_t seed = 0;
    std::string fixes;           // game::parse_fixes's names
    std::string logbook = "on";
    bool passcodes = true;
    std::string keylog;          // the key log's file; "" for the default, "off" for none
};

// The folder's release, or an Error that explains the folder; its file
// list (names, sizes and hashes, no game data) is written beside the
// settings file for the player to send.
std::string identify_folder(const GameFiles& files, const std::string& settings_path);

// The save folder at c.save_dir: the original's saves copied in at the
// first start (when `carry`), and an Error when it can't be written.
void prepare_saves(PlayConfig& c, bool carry);

// The player's options: the settings file's, else the play defaults.
void player_options(PlayConfig& c, Settings& settings, const GameFiles& files);

// The game in its window until it ends; 0 when it ended as a game does.
int run_play(const GameFiles& files, PlayConfig& c);

// The player's start: `ctd` with no arguments.
int play();

}  // namespace cotd
