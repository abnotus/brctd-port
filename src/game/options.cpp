// SPDX-License-Identifier: GPL-3.0-or-later
// The port's options screen (D-0065, session 33): the port's own settings
// and the original's Messages speed in one list, built from the original's
// routines (Screen.LayoutState0's frame, ListMenu with its headings, the
// team menu's colours and keys). Opened from the team menu's JOYSTICK/MOUSE
// INITIALIZE (renamed Options; D-0044) and anywhere by the options key
// (Shift+F12). Every change applies at once and is kept (the settings file
// through the input, written when the screen closes, or the game's own
// variable as CHANGE keeps it); opening and
// closing it passes no game time and restores the screen and the menu
// state it found. The labels and values are the port's own strings.
#include <algorithm>
#include <tuple>

#include "game/game.hpp"

namespace cotd::game {

namespace {

struct OptionRow {
    const char* key;      // the settings file's key; "" for a heading
    const char* label;
    std::vector<std::pair<const char*, const char*>> values;   // value, its name
};

// The keys a gamepad button can be given on the screen, in their order.
const std::vector<std::pair<const char*, const char*>> kPadKeys = {
    {"enter", "Enter"}, {"esc", "Esc"},   {"space", "Space"}, {"y", "Y"},       {"n", "N"},
    {"pgup", "PgUp"},   {"pgdn", "PgDn"}, {"home", "Home"},   {"end", "End"},   {"ins", "Ins"},
    {"del", "Del"},     {"bs", "Backspace"}, {"none", "None"}};

// The list, grouped: Text (with the original's Messages speed), Fights,
// Window, Sound, Gamepad. The values cycle in this order on Select.
const std::vector<OptionRow>& rows() {
    static const std::vector<OptionRow> kRows = {
        {"", "Text", {}},
        {"messages", "Message speed", {}},   // var 0x4BFC, 0..9 (CHANGE -> Messages)
        {"text", "Text style", {{"typewriter", "Typewriter"}, {"instant", "Instant"}}},
        // Session 78 (the owner: fewer options): Queue input, Combat text,
        // Monster moves, Pointer, Music, the Speaker effects and the pad's
        // Text entry left the screen; each keeps its play default.
        {"", "Fights", {}},
        // The names the owner chose (session 35): the original's pace is
        // Default; the rates as such.
        {"combat_speed", "Combat speed",
         {{"original", "Default"}, {"1.25", "1.25x"}, {"1.5", "1.5x"}, {"2", "2x"}, {"fast", "4x"}}},
        {"", "Window", {}},
        {"aspect", "Aspect", {{"off", "Exact"}, {"4:3", "4:3"}}},
        {"fullscreen", "Full screen", {{"off", "Off"}, {"on", "On"}}},
        {"anim", "Animation", {}},           // DS:3BEE (CHANGE -> Anim; kept in the save)
        {"", "Sound", {}},
        // The effects: the original's Tandy versions (D-0093, the default:
        // the owner, session 78), their AdLib versions (D-0069; heard equal
        // to the speaker's) or none. Volume 0 mutes all.
        {"effects", "Effects", {{"tandy", "Tandy"}, {"adlib", "AdLib"}, {"off", "Off"}}},
        {"volume", "Volume",
         {{"10", "10"}, {"9", "9"}, {"8", "8"}, {"7", "7"}, {"6", "6"}, {"5", "5"}, {"4", "4"}, {"3", "3"},
          {"2", "2"}, {"1", "1"}, {"0", "Muted"}}},
        // The gamepad (D-0085, session 66): each button's key (the settings
        // file's pad_<button>); a key set in the file but not listed shows
        // as itself.
        {"", "Gamepad", {}},
        {"pad_a", "A", kPadKeys},
        {"pad_b", "B", kPadKeys},
        {"pad_x", "X", kPadKeys},
        {"pad_y", "Y", kPadKeys},
        {"pad_back", "Back", kPadKeys},
        {"pad_start", "Start", kPadKeys},
        {"pad_leftshoulder", "Left shoulder", kPadKeys},
        {"pad_rightshoulder", "Right shoulder", kPadKeys},
        {"pad_lefttrigger", "Left trigger", kPadKeys},
        {"pad_righttrigger", "Right trigger", kPadKeys},
        {"pad_leftstick", "Left stick press", kPadKeys},
        {"pad_rightstick", "Right stick press", kPadKeys},
    };
    return kRows;
}

std::string row_line(const std::string& label, const std::string& value) {
    // Label left, value right, in the list's width (38 cells).
    std::string s = label;
    const std::size_t w = 37;
    if (s.size() + 1 + value.size() < w) s += std::string(w - s.size() - value.size(), ' ');
    else s += ' ';
    return (s + value).substr(0, 40);
}

}  // namespace

void Game::options_screen() {
    if (!input_ || options_up_) return;
    options_up_ = true;
    // What the screen's routines touch, kept and restored: the screen and
    // its DAC, the state and the flags MenuScreen's clicks depend on, the
    // menu and list variables.
    const Framebuffer saved_screen = screen_;
    const auto saved = std::make_tuple(mode_, move_mode_, f6bd2_, f6bd7_, f8433_, f6b4b_, f6b4c_, f6b1e_, f8435_,
                                       highlight_, menu_sum_, list_top_, list_c0_, list_c1_, list_rows_,
                                       menu_prompt_fg_, menu_hot_fg_, menu_text_fg_, mouse_x_, mouse_y_);
    // The menu it opens over (session 51's audit): its timeout (the
    // title's 120 s, the demo's 30 s) would end the options' own menus
    // with its key, and the icon grid's and MODIFY's click flags would
    // route their clicks; none of them is the options screen's. The
    // time spent here doesn't count toward the timeout.
    const auto saved_menu = std::make_tuple(menu_timeout_cs_, menu_timeout_key_, f6b74_, f6b77_, anim_t0_);
    const long opened_cs = input_->clock_cs();
    note_transcript("[options]");
    mode_ = 0;
    move_mode_ = 0;
    f6bd2_ = f6bd7_ = 0;
    f8433_ = 0;
    menu_timeout_cs_ = 0;
    menu_timeout_key_ = 0;
    f6b74_ = f6b77_ = 0;
    input_->options_open(true);
    int idx = 1;
    for (;;) {
        layout_state0();                               // 015F:00D8: the outer frame
        std::vector<ListNode> list;
        std::vector<int> which;
        for (std::size_t i = 0; i < rows().size(); ++i) {
            const OptionRow& r = rows()[i];
            if (!*r.key) {
                list.push_back({r.label, true});
            } else if (std::string(r.key) == "messages") {
                const int v = v0(0x4BFC);
                list.push_back({row_line(r.label, str_byte(v) + " " + speed_name(v)), false});
            } else if (std::string(r.key) == "anim") {
                list.push_back({row_line(r.label, ds3bee_ ? "On" : "Off"), false});
            } else {
                const std::string v = input_->option(r.key);
                std::string name = v;
                for (const auto& [val, nm] : r.values)
                    if (v == val) name = nm;
                list.push_back({row_line(r.label, name), false});
            }
            which.push_back(static_cast<int>(i));
        }
        bool redraw = true;
        int picked = -1;
        list_menu("", "Change ", 1, 2, 0x26, 0x16, list, true, redraw, idx, picked);
        if (picked < 0) break;
        const OptionRow& r = rows()[static_cast<std::size_t>(which[static_cast<std::size_t>(picked)])];
        if (std::string(r.key) == "anim") {
            ds3bee_ = ds3bee_ ? 0 : 1;                 // CHANGE's Anim (0x45E6B), without its message
        } else if (std::string(r.key) == "messages") {
            v0(0x4BFC) = static_cast<std::uint8_t>((v0(0x4BFC) + 1) % 10);   // as CHANGE keeps it: the game's
        } else if (!r.values.empty()) {
            const std::string v = input_->option(r.key);
            std::size_t n = 0;
            while (n < r.values.size() && v != r.values[n].first) ++n;
            input_->set_option(r.key, r.values[n < r.values.size() ? (n + 1) % r.values.size() : 0].first);
        }
    }
    input_->options_open(false);
    std::tie(mode_, move_mode_, f6bd2_, f6bd7_, f8433_, f6b4b_, f6b4c_, f6b1e_, f8435_, highlight_, menu_sum_,
             list_top_, list_c0_, list_c1_, list_rows_, menu_prompt_fg_, menu_hot_fg_, menu_text_fg_, mouse_x_,
             mouse_y_) = saved;
    std::tie(menu_timeout_cs_, menu_timeout_key_, f6b74_, f6b77_, anim_t0_) = saved_menu;
    if (anim_t0_ >= 0 && opened_cs >= 0 && input_->clock_cs() >= opened_cs) anim_t0_ += input_->clock_cs() - opened_cs;
    screen_ = saved_screen;
    note_transcript("[options closed]");
    options_up_ = false;
}

}  // namespace cotd::game
