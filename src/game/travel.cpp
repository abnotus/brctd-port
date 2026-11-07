// SPDX-License-Identifier: GPL-3.0-or-later
// The travel screen, game state 9 (SET_MODE 2; var 0x4BE6 = 0x70): its
// map (ovl24), the travel move (ovl12 +0x0C76), the input (AdventureInput
// state 9) and Rest (ovl20 +0x042D). docs/notes/travel.md.
#include <algorithm>

#include "game/combat_rules.hpp"
#include "game/game.hpp"

namespace cotd::game {

namespace {

// The view frame, the divider and the line over the text window: the
// list at DS:0D58 (015F:0000), as (x1, y1, x2, y2, start, end).
struct Line { int x1, y1, x2, y2, start, end; };
constexpr Line kTravelLines[] = {{2, 2, 14, 2, 0, 1},   {2, 2, 2, 14, 0, 3},   {14, 2, 14, 14, 1, 2},
                                 {2, 14, 14, 14, 3, 2}, {16, 0, 16, 23, 6, 8}, {0, 16, 16, 16, 9, 7}};

}  // namespace

// The ship record R = var 0x4B85 ([3BC8] + 0x85, DS:4A26): R+2i the
// bodies' squares, R+0x34 the step count, R+0x36 the orbit phase, R+0x38
// the ship's tile, R+0x39/0x3A its square.
std::uint8_t& Game::ship(int off) { return v0(0x4B85 + static_cast<std::uint32_t>(off)); }

void Game::screen_tile(int col, int row, int n) {
    // View.DrawTile ovl33 +0x019B with toBuf 0 (0x60FBD..0x61014): the
    // first set whose top DS:256E[k] covers n, frame n - DS:2560[k],
    // copied opaque to the screen at the cell; nothing for tile 0, a set
    // past 6, or a set not loaded (0x60F4C..0x60F70).
    if (n == 0) return;
    int k = 0;
    while (k < 7 && text_.ds16(0x256E + 2 * static_cast<std::size_t>(k)) < n) ++k;
    if (k > 6 || !tile_sets_[static_cast<std::size_t>(k)]) return;
    const Image& im = *tile_sets_[static_cast<std::size_t>(k)];
    const int f = n - text_.ds16(0x2560 + 2 * static_cast<std::size_t>(k));
    if (f < 0 || f >= static_cast<int>(im.frames.size())) return;
    screen_.blit(im.frames[static_cast<std::size_t>(f)], im.width, im.height, col * 8, row * 8);
}

void Game::travel_load() {
    // ovl24 +0x0109 (0x597E9), once until freed (DS:4A24): the ROCKET
    // images freed (+0x00CC, DS:4A25), the loading line, tile set 5 :=
    // 8X8D0 #1 (DS:4898 forced to 0 for the load, 0x59806..0x5981F),
    // the ship's 8x8 save-under (0267:22D7), row 24 cleared.
    if (f4a24_) return;
    f4a25_ = 0;
    fill_rect(0x15, 0x18, 0x27, 0x18, 0);              // Text.LoadingLine 0653:12D2
    draw_string(0, 0x18, 10, 0, text_[Text::Loading]);
    f4a24_ = 1;
    debug_slot(5);                                     // 0x60DE6
    auto raw = dax_entry("8X8D0.DAX", 1);              // LoadTileSet ovl33 +0x004F
    if (!raw) throw Fatal(text_[Text::WallUnable] + "1");
    tile_sets_[5] = std::make_unique<Image>(decode_generic(*raw, "8X8D0.DAX #1"));
    load_palette(*tile_sets_[5]);
    flush_keys();                                      // Gfx.LoadImage's 0x12BED
    ship_under_.assign(64, 0);
    clear_row24();
}

void Game::travel_free() {
    // ovl24 +0x0099 (0x59779): tile set 5, the save-under and SHIPS #128.
    if (!f4a24_) return;
    f4a24_ = 0;
    tile_sets_[5].reset();
    ships_bg_.reset();
}

void Game::travel_frame() {
    // ovl24 +0x01FE (0x598DE).
    travel_load();
    // Gfx.LoadImage("ships", 0x80, …, DS:6B10) on every redraw; its
    // palette into DS:908B, then SetDacRange(0x20, 0xE0, DS:908B).
    auto raw = dax_entry("SHIPS.DAX", 0x80);
    if (!raw) throw Fatal(text_[Text::WallUnable] + std::to_string(0x80));
    ships_bg_ = std::make_unique<Image>(decode_generic(*raw, "SHIPS.DAX #128"));
    load_palette(*ships_bg_);
    flush_keys();                                      // Gfx.LoadImage's 0x12BED
    set_dac(0x20, 0xE0, pal_908b_);
    layout_state0();                                   // 015F:00D8
    for (const Line& l : kTravelLines) border_line(l.x1, l.y1, l.x2, l.y2, l.start, l.end);
    draw_view_area();
    left_ = 1, top_ = 0x11, right_ = 0x0F, bottom_ = 0x16;   // DS:5F40..5F43
    f6b14_ = 0xFF;
    travel_map();
    set_window(1, 0x11, 0x0F, 0x16, 0);                // Text.SetWindow
}

void Game::travel_map() {
    // ovl24 +0x0294 (0x59974): SHIPS #128 at cell (0x11, 1), the Sun, the
    // five planets and eight asteroids from the orbit table DS:249A (x, y,
    // tile) by the phase R+0x36, each body's square written to R+2i.
    if (ships_bg_ && !ships_bg_->frames.empty())
        screen_.blit(ships_bg_->frames[0], ships_bg_->width, ships_bg_->height, 0x11 * 8, 8);
    screen_tile(0x1C, 0x0B, 0x128);
    const int t = ship(0x36);
    auto body = [&](int i, int e, int base) {
        const std::size_t p = 0x249A + 3 * static_cast<std::size_t>(e);
        ship(2 * i) = text_.ds8(p);
        ship(2 * i + 1) = text_.ds8(p + 1);
        screen_tile(text_.ds8(p) + 0x11, text_.ds8(p + 1) + 1, base + text_.ds8(p + 2));
    };
    for (int i = 0; i < 5; ++i) {                      // 0x599A9..0x59A66
        const int len = text_.ds8(0x254D + static_cast<std::size_t>(i));
        body(i, text_.ds8(0x2548 + static_cast<std::size_t>(i)) + (text_.ds8(0x255A + static_cast<std::size_t>(i)) + t) % len,
             0x128);
    }
    for (int i = 0; i < 8; ++i) {                      // 0x59A69..0x59B25
        const int e = text_.ds8(0x254C) +
                      (text_.ds8(0x255E) + text_.ds8(0x2552 + static_cast<std::size_t>(i)) + t) % text_.ds8(0x2551);
        body(i + 5, e, 0x10C);
    }
    ship_x_ = -1;                                      // DS:4A2E := 0xFF: no restore
    travel_ship();
}

void Game::travel_ship() {
    // ovl24 +0x0000 (0x596E0): the saved cell restored at the old square
    // (DS:4A2E/4A2F, when DS:4A2E >= 0), the new square's cell saved
    // (GetImage 0267:12E6), the ship's tile 0x128 + R+0x38 drawn there.
    auto at = [&](int x, int y, std::size_t i) -> std::uint8_t& {
        return screen_.row((y + 1) * 8 + static_cast<int>(i / 8))[(x + 0x11) * 8 + static_cast<int>(i % 8)];
    };
    if (ship_x_ >= 0)
        for (std::size_t i = 0; i < 64; ++i) at(ship_x_, ship_y_, i) = ship_under_[i];
    ship_x_ = static_cast<std::int8_t>(ship(0x39));
    ship_y_ = static_cast<std::int8_t>(ship(0x3A));
    ship_under_.resize(64);
    for (std::size_t i = 0; i < 64; ++i) ship_under_[i] = at(ship(0x39), ship(0x3A), i);
    screen_tile(ship(0x39) + 0x11, ship(0x3A) + 1, 0x128 + ship(0x38));
}

void Game::draw_band() {
    // DrawPicture ovl27 +0x0023 in state 9 (0x5C29B..0x5C3F6): a band
    // round the view frame in the place's colour, its name on row 15
    // (var 0x4BA5: the five labels at ovl27 CS:0000..0019 by place).
    static constexpr int kColour[5] = {5, 12, 2, 1, 4}, kCol[5] = {0, 1, 2, 2, 3};
    static constexpr Text kName[5] = {Text::Count, Text::BandMercury, Text::BandVenus, Text::BandEarth, Text::BandMars};
    const int p = v0(0x4BA5);
    const int c = p < 5 ? kColour[p] : 8, k = p < 5 ? kCol[p] : 0;
    const std::string name = p == 0 ? std::string() : p < 5 ? text_[kName[p]] : text_[Text::BandAst];
    fill_rect(1, 1, 15, 1, c);
    fill_rect(1, 2, 1, 14, c);
    fill_rect(15, 2, 15, 14, c);
    fill_rect(1, 15, k + 3, 15, c);
    fill_rect(static_cast<int>(name.size()) + k + 4, 15, 15, 15, c);
    draw_string(k + 4, 15, 15, c, name);
}

void Game::rest(int n) {
    // Rest ovl20 +0x042D (0x54A7D): n days pass (PassTime unit 4), then
    // every record of the party: status 5 becomes 1; an Okay record is
    // active again and heals n, up to its maximum.
    pass_time(n, 4);
    for (auto& m : party_) {
        if (m->b(0xD4) == 5) m->b(0xD4) = 1;
        if (m->b(0xD4) == 1) {
            m->b(0xD5) = 1;
            m->setw(0xE3, static_cast<std::uint32_t>(std::min<int>(m->w(0xE3) + n, m->w(0x45))));
        }
    }
}

void Game::travel_move() {
    // TravelMove(0) ovl12 +0x0C76 (0x45ED6..0x461CF).
    ship_x_ = static_cast<std::int8_t>(ship(0x39));    // DS:4A2E/4A2F
    ship_y_ = static_cast<std::int8_t>(ship(0x3A));
    // The fuel prompt (ovl12 CS:0C5D) + Str(the fuel word, var 0x4D1E) + CS:0C6F, item CS:0C71.
    const std::int16_t fuel = static_cast<std::int16_t>(v0(0x4D1E) | v0(0x4D1F) << 8);
    const MenuKey mk =
        menu_screen(text_[Text::TravelFuel] + std::to_string(fuel) + text_[Text::TravelSpace], text_[Text::TravelDone], true);
    int dir = 8;
    switch (mk.key) {   // DS:6B49, the extended flag untested
        case 0x48: dir = 0; break;
        case 0x47: dir = 1; break;
        case 0x4B: dir = 2; break;
        case 0x4F: dir = 3; break;
        case 0x50: dir = 4; break;
        case 0x51: dir = 5; break;
        case 0x4D: dir = 6; break;
        case 0x49: dir = 7; break;
        case 0x44: case 0: case 0x0D: case 0x1B:
            // 0x45FCC (main): a click on the map (y <= 0xB0, x >= 0x88)
            // steers toward its cell (map cell (x, y) is text cell
            // (x + 0x11, y + 1); Combat.Dir negated: the travel directions
            // turn the other way); else the move mode ends.
            if (f8435_ && mouse_y_ <= 0xB0 && mouse_x_ >= 0x88)
                dir = -dir_to(ship(0x39), ship(0x3A), mouse_x_ / 8 - 0x11, mouse_y_ / 8 - 1) & 7;
            else
                move_mode_ = 0;
            break;
        default: break;
    }
    if (dir >= 8) return;
    // The turn (0x46039..0x460D2): one tile per 100 ms toward 0x57 + dir.
    const int tgt = 0x57 + dir;
    const int cur0 = ship(0x38);
    const int step = (tgt < cur0 && tgt >= cur0 - 4) || tgt >= cur0 + 4 ? -1 : 1;
    while (ship(0x38) != tgt) {
        int c = ship(0x38) + step;
        if (c < 0x57) c = 0x5E;
        else if (c > 0x5E) c = 0x57;
        ship(0x38) = static_cast<std::uint8_t>(c);
        travel_ship();
        pause_ms(100);                                 // Crt.Delay 0B00:029E
    }
    // The step (0x460D4..0x4617A): clamped to 0..0x15, undone on the Sun.
    const int dx = static_cast<std::int8_t>(text_.ds8(0x0A10 + static_cast<std::size_t>(dir)));
    const int dy = static_cast<std::int8_t>(text_.ds8(0x0A18 + static_cast<std::size_t>(dir)));
    auto add = [&](int off, int d) {
        std::uint8_t v = static_cast<std::uint8_t>(ship(off) + d);
        if (v == 0xFF) v = 0;
        else if (v > 0x15) v = 0x15;
        ship(off) = v;
    };
    add(0x39, dx);
    add(0x3A, dy);
    if (ship(0x39) == 0x0B && ship(0x3A) == 0x0A) {
        ship(0x39) = static_cast<std::uint8_t>(ship(0x39) - dx);
        ship(0x3A) = static_cast<std::uint8_t>(ship(0x3A) - dy);
    }
    travel_ship();
    ship(0x34) = static_cast<std::uint8_t>(ship(0x34) + 3);
    rest(3);                                           // ovl20 +0x042D
    if (ship(0x34) > 0x1F) {                           // 0x46197: the planets move on
        ship(0x34) = static_cast<std::uint8_t>(ship(0x34) - 0x20);
        if (ship(0x36) == 0) ship(0x36) = 0xE0;
        ship(0x36) = static_cast<std::uint8_t>(ship(0x36) - 1);
        travel_map();
    }
}

int Game::travel_input() {
    // AdventureInput ovl12 +0x0FE4, state 9 (0x464F5..0x4659B).
    if (move_mode_) {
        // 0x46503: TravelMove(1) returns at once, and the routine returns
        // its local BP-3 without writing it (D-0040): the 'M' the menu
        // left, which RunLoop tests only for 'E'.
        combat::diag("D-0040", "AdventureInput (ovl12 +0x0FE4, 0x4650A): travel move mode returns an unwritten "
                               "local; the reference machine's outcome (not 'E') is used");
        return 'M';
    }
    for (;;) {
        const MenuKey mk = menu_screen("", text_[Text::TravelMenu], false, true);
        const int key = mk.ext ? 0 : mk.key;
        switch (key) {
            case 'M':
                move_mode_ = 1;
                return 'M';
            case 'R':
                // ROCKET (0x4655B..0x4657E): state 8, the ship images, the
                // party's ship (no enemy), one wait; then state 9 and the
                // travel screen again (its view area empty: load_ship_images
                // freed the picture), and the menu. No DAC change: the icons
                // show in the travel screen's colours (0x20..0x3F).
                mode_ = 8;
                load_ship_images();                    // ovl24 +0x016A
                redraw_screen();                       // ovl23 +0x26BE
                pause_continue();                      // Text.Pause 0653:1291
                mode_ = 9;
                travel_load();                         // ovl24 +0x0109
                redraw_screen();
                break;
            case 'C':
                change_menu();                         // 0x46585: CHANGE, then the travel menu again
                break;
            case 'S':
                save_menu();
                break;
            default:
                break;
        }
    }
}

}  // namespace cotd::game
