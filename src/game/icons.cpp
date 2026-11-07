// SPDX-License-Identifier: GPL-3.0-or-later
// The body icon screen: IconSelect ovl13 +0x25BB (0x48ECB), at the end of
// character creation and for ICON SELECT (docs/notes/frontend.md). A 6x6
// grid of CHARS.DAX icons; the choice is record +0xB8.
#include "game/game.hpp"

namespace cotd::game {

void Game::load_icon_table() {
    // LoadIconTable ovl13 +0x1E99 (0x487A9): CHARS.DAX entries 2i / 2i+1
    // by directory position, which on the shipped file are ids i and
    // 0x80+i; the port reads them by id and rejects a file that doesn't
    // have them as 24x24 images (the original reads them blindly; it also
    // reads 2 bytes past its directory buffer: not reproduced).
    icon_ready_.clear();
    icon_action_.clear();
    sound(text_.ds16(0x21DC));              // 0x487BC: Sound.Play(0), before the direct file reads
    fill_rect(0x15, 0x18, 0x27, 0x18, 0);   // 0653:12D2, "Loading..."
    draw_string(0, 0x18, 10, 0, text_[Text::Loading]);
    for (int i = 0; i < 36; ++i) {
        for (int frame = 0; frame < 2; ++frame) {
            const int id = frame ? 0x80 + i : i;
            auto raw = dax_entry("CHARS.DAX", id);
            if (!raw) throw Error("CHARS.DAX: no icon entry " + std::to_string(id));
            Image im = decode_generic(*raw, "CHARS.DAX #" + std::to_string(id));
            if (im.width != 24 || im.height != 24 || im.frames.empty())
                throw Error("CHARS.DAX #" + std::to_string(id) + ": not a 24x24 icon");
            load_palette(im);
            (frame ? icon_action_ : icon_ready_).push_back(im.frames[0]);
        }
    }
    sound(text_.ds16(0x21DE));      // 0x48931: Sound.Play(1) (SetSpeakerEnable(1): nothing)
    flush_keys();                   // 0x4893B
    clear_row24();                  // 0653:1359
    set_dac(0x60, 0x30, pal_908b_);   // DAC 0x60..0x8F
    if (!icon_backdrop_) {
        // COMSPR #20 (combat slot 0x20, loaded at game start by Game.Init
        // 0x3FC2A): the highlighted cell's backdrop.
        auto raw = dax_entry("COMSPR.DAX", 20);
        if (!raw) throw Error("COMSPR.DAX: no entry 20");
        const Image im = decode_generic(*raw, "COMSPR.DAX #20");
        if (im.width != 24 || im.height != 24 || im.frames.empty()) throw Error("COMSPR.DAX #20: not a 24x24 icon");
        icon_backdrop_ = std::make_unique<Bytes>(im.frames[0]);
    }
}

void Game::draw_icon_cell(int cx, int cy, int icon, int frame, bool hi) {
    // DrawIconCell ovl13 +0x2250: the backdrop (COMSPR #20 when
    // highlighted, else colour 8), then the icon's ready (frame 0) or
    // action (frame 1) image, both drawn with colour key 8. Cell (cx, cy)
    // is the screen's (24cx + 8, 24cy + 8).
    const int x = 24 * cx + 8, y = 24 * cy + 8;
    if (hi) screen_.blit(*icon_backdrop_, 24, 24, x, y, 8);
    else fill_rect(3 * cx + 1, 3 * cy + 1, 3 * cx + 3, 3 * cy + 3, 8);
    const auto& set = frame ? icon_action_ : icon_ready_;
    if (icon >= 0 && icon < static_cast<int>(set.size())) screen_.blit(set[static_cast<std::size_t>(icon)], 24, 24, x, y, 8);
}

void Game::icon_select() {
    // IconSelect ovl13 +0x25BB (0x48ECB), keyboard path.
    Character& c = *cur_;
    if (c.b(0x4C) != 0) return;
    f6b74_ = 1;                                        // 0x48EE0 .. 0x49296: grid clicks
    layout_state0();
    const std::uint8_t old = c.b(0xB8);
    std::array<std::uint8_t, 16> colours{};
    for (std::size_t i = 0; i < 16; ++i) colours[i] = c.b(0xC4 + i);
    load_icon_table();
    auto pair = [&](int x, int y) {
        // DrawIconPair ovl13 +0x23AC: ready and action, both highlighted.
        draw_icon_cell(x, y, c.b(0xB8), 0, true);
        draw_icon_cell(x + 3, y, c.b(0xB8), 1, true);
    };
    auto move = [&](int dx, int dy, bool full) {
        // MoveIconCursor ovl13 +0x2410: each axis wraps 0..5 on its own.
        if (full) {
            for (int n = 0; n < 36; ++n) draw_icon_cell(n % 6 + 6, n / 6 + 1, n, 0, n == c.b(0xB8));
            return;
        }
        const int icon = c.b(0xB8);
        draw_icon_cell(icon % 6 + 6, icon / 6 + 1, icon, 0, false);
        int col = icon % 6 + dx, row = icon / 6 + dy;
        if (col < 0) col = 5;
        if (col > 5) col = 0;
        if (row < 0) row = 5;
        if (row > 5) row = 0;
        c.b(0xB8) = static_cast<std::uint8_t>(col + 6 * row);
        draw_icon_cell(col + 6, row + 1, c.b(0xB8), 0, true);
    };
    for (;;) {
        c.b(0xB8) = old;
        for (std::size_t i = 0; i < 16; ++i) c.b(0xC4 + i) = colours[i];
        pair(1, 2);
        draw_string(8, 6, 15, 0, text_[Text::IconOld]);
        draw_string(3, 0x0A, 15, 0, text_[Text::IconReadyAction]);
        draw_string(8, 0x0C, 15, 0, text_[Text::IconNew]);
        draw_string(3, 0x10, 15, 0, text_[Text::IconReadyAction]);
        move(0, 0, true);
        bool done = false;
        while (!done) {
            pair(1, 4);
            const int k = menu_screen(text_[Text::IconPrompt], text_[Text::IconDone], true).key;
            switch (k) {
                case 0x47: move(-1, -1, false); break;
                case 0x48: move(0, -1, false); break;
                case 0x49: move(1, -1, false); break;
                case 0x4B: move(-1, 0, false); break;
                case 0x4D: move(1, 0, false); break;
                case 0x4F: move(-1, 1, false); break;
                case 0x50: move(0, 1, false); break;
                case 0x51: move(1, 1, false); break;
                case 0x21: {
                    // 0x4916E (main): a click on the grid (MenuScreen's
                    // IconGrid): the clicked cell, or Done on the current one.
                    const int dx = (mouse_x_ - 0x98) / 0x18 - c.b(0xB8) % 6;
                    const int dy = (mouse_y_ - 0x20) / 0x18 - c.b(0xB8) / 6;
                    if (dx == 0 && dy == 0) done = true;
                    else move(dx, dy, false);
                    break;
                }
                case 'D': done = true; break;
                case 0x1B: case 0:
                    c.b(0xB8) = old;
                    done = true;
                    break;
                default: break;
            }
        }
        clear_row24();
        if (yes_no(text_[Text::IconOk], 1) == 'Y') break;
    }
    // ovl13 +0x206F (0x4897F, called at 0x49273): when the member's own combat
    // slot (+0xB9) holds an image (0x4925D), the chosen icon's two frames are
    // copied into it, so the next fight shows it (route s77_icon_fight).
    if (const std::size_t slot = c.b(0xB9), id = c.b(0xB8);
        slot < combat_icons_.size() && combat_icons_[slot].loaded && id < icon_ready_.size() && id < icon_action_.size()) {
        combat_icons_[slot].ready = IconFrame{24, 24, icon_ready_[id]};
        combat_icons_[slot].action = IconFrame{24, 24, icon_action_[id]};
    }
    f6b74_ = 0;
}

}  // namespace cotd::game
