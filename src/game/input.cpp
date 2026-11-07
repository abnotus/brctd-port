// SPDX-License-Identifier: GPL-3.0-or-later
#include "game/input.hpp"

#include <cstdio>
#include <sstream>

#include "core/files.hpp"
#include "core/sha256.hpp"
#include "game/game.hpp"

namespace cotd::game {


namespace {

const std::pair<const char*, int> kNamed[] = {
    {"enter", kEnter}, {"esc", kEsc}, {"space", ' '}, {"bs", kBackspace}, {"up", kUp},
    {"down", kDown},   {"left", kLeft}, {"right", kRight}, {"home", kHome}, {"end", kEnd},
    {"pgup", kPgUp},   {"pgdn", kPgDn}, {"ins", kIns},     {"del", kDel},       {"center", kCenter}, {"hash", '#'},
    {"f1", 0x13B}, {"f2", 0x13C}, {"f3", 0x13D}, {"f4", 0x13E}, {"f5", 0x13F}, {"f6", 0x140},
    {"f7", 0x141}, {"f8", 0x142}, {"f9", 0x143}, {"f10", 0x144},   // the function keys: scan 0x3B..0x44
    {"poll", kPollMark}, {"idle", kIdle},
    // ^S and ^U (GetKey's sound keys, docs/notes/sound.md): the BIOS's
    // ASCII 13h and 15h.
    {"ctrl-s", 0x13}, {"ctrl-u", 0x15}, {"ctrl-d", 0x04},
    // The window's own hotkeys (F11 full screen, F12 combat speed): the
    // BIOS never returns them to the game (tools/bios_keys.py), so they
    // exist only for injected window tests, never in a route.
    {"f11", kHostF11}, {"f12", kHostF12},
    // The options screen's key (Shift+F12 in the window; D-0065): never
    // the original's, so a route with it is a test of the port only.
    {"options", kOptionsKey}, {"aspect", kAspectKey},
    {"code", kCodeKey}};

// Shift-, Ctrl- and Alt-F1..F10 (scan 0x54.., 0x5E.., 0x68..; ASCII 0):
// "shift-f1" .. "alt-f10" in key lists.
int modified_f_key(const std::string& tok) {
    static const std::pair<const char*, int> mods[] = {{"shift-f", 0x54}, {"ctrl-f", 0x5E}, {"alt-f", 0x68}};
    for (const auto& [m, base] : mods) {
        const std::string p = m;
        if (tok.size() <= p.size() || tok.rfind(p, 0) != 0) continue;
        const std::string n = tok.substr(p.size());
        if (n.find_first_not_of("0123456789") != std::string::npos || n.size() > 2) continue;
        const int i = std::stoi(n);
        if (i >= 1 && i <= 10) return 0x100 + base + i - 1;
    }
    return -1;
}

// Alt + a key as a code in the BIOS buffer: ASCII 0, then the letter's or
// the punctuation key's own scan code (Alt-Q 1000h, Alt-] 1B00h),
// Alt-1..Alt-0 7800h..8100h, Alt-- 8200h, Alt-= 8300h. "alt-q", "alt-]" in
// key lists. Session 78: INT 16h AH=00h never gives Alt+punctuation
// (measured; app/window.cpp): those names only force a code into a route.
constexpr char kScanLetters[] = "qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";   // scan 0x10..0x35
int alt_key(char c) {
    for (int i = 0; i < static_cast<int>(sizeof kScanLetters) - 1; ++i)
        if (kScanLetters[i] == c) return 0x100 + 0x10 + i;
    if (c >= '1' && c <= '9') return 0x100 + 0x78 + (c - '1');
    if (c == '0') return 0x100 + 0x81;
    if (c == '-') return 0x100 + 0x82;
    if (c == '=') return 0x100 + 0x83;
    return -1;
}
constexpr char kAltKeys[] = "abcdefghijklmnopqrstuvwxyz0123456789-=[];'`\\,./";

}  // namespace

int alt_letter_key(char c) { return alt_key(c); }

std::vector<std::string> key_tokens(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (auto h = line.find('#'); h != std::string::npos) line.erase(h);
        std::istringstream in(line);
        for (std::string tok; in >> tok;) {
            int times = 1;
            if (auto star = tok.find('*'); star != std::string::npos && star > 0) {
                times = std::stoi(tok.substr(star + 1));
                tok = tok.substr(0, star);
            }
            for (int i = 0; i < times; ++i) out.push_back(tok);
        }
    }
    return out;
}

namespace {

int parse_key(const std::string& tok);

// "burst:N:KEY" (N 1..15), or -1.
int parse_burst(const std::string& tok) {
    if (tok.rfind("burst:", 0) != 0) return -1;
    const std::size_t colon = tok.find(':', 6);
    int n = 0;
    try {
        if (colon != std::string::npos) n = std::stoi(tok.substr(6, colon - 6));
    } catch (const std::exception&) {
    }
    if (n < 1 || n > kBiosBuffer) throw Error("a burst needs burst:N:KEY with N 1..15: '" + tok + "'");
    const int key = parse_key(tok.substr(colon + 1));
    if (is_click(key) || is_burst(key) || key == kPollMark || key == kIdle || key > 0xFFFF)
        throw Error("a burst repeats a key: '" + tok + "'");
    return burst_key(n, key);
}

int parse_key(const std::string& tok) {
    int key = parse_burst(tok);
    for (const auto& [n, k] : kNamed)
        if (tok == n) key = k;
    if (key < 0 && tok.size() == 5 && tok.rfind("alt-", 0) == 0) key = alt_key(tok[4]);
    if (key < 0) key = modified_f_key(tok);
    if (key < 0 && tok.rfind("pad:", 0) == 0) {
        const int b = pad_button(tok.substr(4));
        if (b < 0) throw Error("unknown gamepad button '" + tok + "'");
        key = kPadBase + b;
    }
    if (key < 0 && (tok.rfind("click:", 0) == 0 || tok.rfind("rclick:", 0) == 0)) {
        const std::size_t colon = tok.find(':'), comma = tok.find(',', colon);
        int x = -1, y = -1;
        try {
            if (comma != std::string::npos) {
                x = std::stoi(tok.substr(colon + 1, comma - colon - 1));
                y = std::stoi(tok.substr(comma + 1));
            }
        } catch (const std::exception&) {
        }
        if (x < 0 || x > 319 || y < 0 || y > 199) throw Error("a click needs x 0..319 and y 0..199: '" + tok + "'");
        key = click_key(tok[0] == 'r' ? 2 : 1, x, y);
    }
    if (key < 0) {
        if (tok.size() != 1) throw Error("unknown key '" + tok + "'");
        key = static_cast<unsigned char>(tok[0]);
    }
    return key;
}

}  // namespace

std::vector<int> parse_keys(const std::string& text) {
    std::vector<int> out;
    for (const std::string& tok : key_tokens(text)) out.push_back(parse_key(tok));
    return out;
}

int pad_button(const std::string& name) {
    // SDL3's SDL_GamepadButton order and names (SDL_gamepad.h).
    static const char* const kNames[kPadTokens] = {
        "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
        "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3",
        "paddle4", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "lefttrigger", "righttrigger",
        "stickup", "stickdown", "stickleft", "stickright",
        "stickupleft", "stickupright", "stickdownleft", "stickdownright"};
    for (int i = 0; i < kPadTokens; ++i)
        if (name == kNames[i]) return i;
    return -1;
}

std::string key_name(int key) {
    if (is_pad(key)) {
        for (const char* n : {"a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
                              "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "lefttrigger", "righttrigger", "stickup",
                              "stickdown", "stickleft", "stickright", "stickupleft", "stickupright", "stickdownleft",
                              "stickdownright"})
            if (pad_button(n) == key - kPadBase) return std::string("pad:") + n;
    }
    if (is_burst(key)) return "burst:" + std::to_string(burst_count(key)) + ":" + key_name(burst_of(key));
    if (is_click(key))
        return std::string(click_buttons(key) == 2 ? "rclick:" : "click:") + std::to_string(click_x(key)) + "," +
               std::to_string(click_y(key));
    for (const auto& [n, k] : kNamed)
        if (k == key) return n;
    if (key > 0x20 && key < 0x7F && key != '#') return std::string(1, static_cast<char>(key));
    for (const char* c = kAltKeys; *c; ++c)
        if (alt_key(*c) == key) return std::string("alt-") + *c;
    for (const char* m : {"shift-f", "ctrl-f", "alt-f"})
        for (int i = 1; i <= 10; ++i)
            if (modified_f_key(m + std::to_string(i)) == key) return m + std::to_string(i);
    return "key" + hex(static_cast<std::uint32_t>(key), 3);   // not replayable; shouldn't occur
}



namespace {

const std::pair<const char*, bool Fixes::*> kFixNames[] = {
    {"stash-load", &Fixes::stash_load},
    {"stale-stash", &Fixes::stale_stash},
    {"roster-backup", &Fixes::roster_backup},
    {"wipe-load", &Fixes::wipe_load},
    {"script-load", &Fixes::script_load},
};

}  // namespace

Fixes parse_fixes(const std::string& text) {
    Fixes f;
    if (text.empty() || text == "none") return f;
    std::istringstream in(text);
    std::string tok;
    while (std::getline(in, tok, ',')) {
        bool found = false;
        for (const auto& [name, member] : kFixNames) {
            if (tok == name || tok == "all") {
                f.*member = true;
                found = true;
            }
        }
        if (!found) throw Error("fixes: '" + tok + "' is none of stash-load, stale-stash, roster-backup, wipe-load, script-load, all, none");
    }
    return f;
}

std::string fixes_name(const Fixes& f) {
    std::string s;
    for (const auto& [name, member] : kFixNames)
        if (f.*member) s += (s.empty() ? "" : ",") + std::string(name);
    return s.empty() ? "none" : s;
}

LoggingInput::LoggingInput(Input& inner, const std::string& path, const std::string& header)
    : inner_(inner), path_(path) {
    f_ = std::fopen(path.c_str(), "w");
    if (!f_) throw Error("cannot write the key log " + path);
    std::fputs(header.c_str(), f_);
    std::fflush(f_);
}

LoggingInput::~LoggingInput() {
    if (f_) std::fclose(f_);
}

int LoggingInput::read_key(const Framebuffer& screen, const char* what) {
    // The options screen's keys (Shift+F12, CHANGE's M in play) and the
    // keys read while it is up are the port's, never the game's: not
    // logged, so the log replays in the original (D-0065).
    const int k = inner_.read_key(screen, what);
    if (unlogged(k)) return k;
    std::fprintf(f_, "%-8s # %s\n", key_name(k).c_str(), what);
    std::fflush(f_);
    return k;
}

int LoggingInput::read_key_for(const Framebuffer& screen, const char* what, int ms) {
    const int k = inner_.read_key_for(screen, what, ms);
    if (unlogged(k)) return k;
    if (k >= 0) {
        std::fprintf(f_, "%-8s # %s\n", key_name(k).c_str(), what);
        std::fflush(f_);
    }
    return k;
}

bool LoggingInput::key_waiting() {
    const bool w = inner_.key_waiting();
    if (w) {
        std::fprintf(f_, "%-8s # a key waiting at the AI's poll\n", "poll");
        std::fflush(f_);
    }
    return w;
}

double parse_combat_speed(const std::string& text) {
    if (text == "original") return 1.0;
    if (text == "fast") return kFastCombat;
    std::size_t used = 0;
    double f = -1;
    try {
        f = std::stod(text, &used);
    } catch (const std::exception&) {
    }
    if (used != text.size() || !(f >= 0.25 && f <= 10.0))
        throw Error("combat speed (original, fast or a rate 0.25..10): '" + text + "'");
    return 1.0 / f;   // the rate shown and typed; the factor on the waits
}

std::string combat_speed_name(double f) {
    if (f == 1.0) return "original";
    if (f == kFastCombat) return "fast";
    char b[16];
    std::snprintf(b, sizeof b, "%g", 1.0 / f);   // the rate: 1.5 = 1.5 times faster
    return b;
}

TextSpeed parse_text_speed(const std::string& text) {
    if (text == "typewriter") return TextSpeed::Typewriter;
    if (text == "instant") return TextSpeed::Instant;
    throw Error("text style (typewriter or instant): '" + text + "'");
}

std::string text_speed_name(TextSpeed t) { return t == TextSpeed::Instant ? "instant" : "typewriter"; }

void LoggingInput::timed_out() {
    std::fprintf(f_, "idle     # the menu's timeout\n");
    std::fflush(f_);
}

void LoggingInput::note(const std::string& line) {
    std::fprintf(f_, "# %s\n", line.c_str());
    std::fflush(f_);
}

}  // namespace cotd::game
