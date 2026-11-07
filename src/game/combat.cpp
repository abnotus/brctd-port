// SPDX-License-Identifier: GPL-3.0-or-later
// The combat engine: the COMBAT opcode, the fight's set-up, the
// battlefield and the placement (docs/notes/combat.md "The battlefield";
// addresses are ovl8 offsets unless noted). The rules themselves are in
// game/combat_rules.cpp.
#include <algorithm>

#include "core/real48.hpp"
#include "game/game.hpp"

namespace cotd::game {

namespace {

// The eight directions (DS:257C / DS:2585), 8 = none.
constexpr int kDx[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};
constexpr int kDy[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }

}  // namespace

// --- COMBAT ---------------------------------------------------------------------

void Game::combat_op(bool solo) {
    // COMBAT ovl2 +0x198E (0x321DE), COMBAT_SOLO (DS:82EA := 1, then the same
    // handler; ecl.md). Main-session reading, session 19.
    if (solo) f82ea_ = 1;
    // +0x186B AnyEnemy (0x320BB), inlined: is any enemy (+0xD6 = 1) in the list?
    if (!f82fd_) {   // 0x321E4: is any enemy (+0xD6 = 1) in the list?
        for (const auto& c : party_)
            if (c->b(0xD6) == 1) f82fd_ = 1;
    }
    prev_mode_ = mode_;   // 0x321F3
    f3bed_ = 0;
    const int saved_move = move_mode_;   // DS:8303, restored at the end
    move_mode_ = 0;
    const std::uint32_t site = static_cast<std::uint32_t>(ecl_file_) << 24 | std::uint32_t{block_id_} << 16 | pc_;
    pc_ += 1;
    if (!f82fd_ && !f82ea_) {   // 0x32211: no fight
        f959b_ = 1;
        after_combat();
        combat_end(saved_move);
        return;
    }
    // 0x3222B: the encounter distance is clamped to the steps ahead.
    const int ahead = steps_ahead(x_, y_, facing_, site);
    if (v1(0x7EC1) > ahead) v1(0x7EC1) = static_cast<std::uint8_t>(ahead);
    // ovl2 0x32134 (main-session reading, session 19): under COMBAT_SOLO the
    // other party-side records sit out (+0xD5 := 0); afterwards +0xD5 is
    // set again from the status (1 for 1, 0x0A, 0x0B, 0x0C: CS:18C4).
    auto solo_members = [&](bool on) {
        for (const auto& c : party_) {
            if (c->b(0xD6) != 0 || c.get() == cur_) continue;
            if (on) c->b(0xD5) = 0;
            else {
                const int st = c->b(0xD4);
                c->b(0xD5) = (st == 1 || st == 0x0A || st == 0x0B || st == 0x0C) ? 1 : 0;
            }
        }
    };
    if (f82ea_) solo_members(true);
    combat_run();
    f959b_ = 1;                                        // 0x32271
    if (f82ea_) solo_members(false);
    after_combat();                                    // ovl5 +0x20D8
    combat_end(saved_move);
}

void Game::combat_end(int saved_move) {
    // COMBAT's tail (ovl2 0x3228A..0x322FC).
    f82ea_ = 0;
    mode_ = prev_mode_;                                // DS:3BE8 := DS:3BE9
    v1(0x7ECA) &= 1;
    f82dc_ = 0;
    f82dd_ = 0;
    f82f7_ = 0;
    if (mode_ == 3) {
        // 0x322CC: the map's picture loaded again (DS:6B14 := 0xFF first:
        // a miss, so the loading line and row 24 cleared), drawn, the cursor.
        const int k = f6b14_;
        f6b14_ = 0xFF;
        load_big_pic(k);
        draw_big_pic();
        draw_map_cursor();
    } else {
        f6b14_ = 0xFF;                                 // DS:6B14: the big picture to redraw
        redraw_screen();
    }
    move_mode_ = static_cast<std::uint8_t>(saved_move);   // DS:8303
}

void Game::leadership() {
    // Combat.Leadership ovl11 +0x43A9 (0x44CA9..0x44EBA); the rule (the
    // same loop, without the screen): combat::leadership. Each success
    // prints "<leader> leads <NPC>." with the line printer; the first one
    // first sets up the screen (LayoutState0, the window 1,1..0x26,0x16,
    // a heading in white); a Pause ends it when anything was shown.
    for (const auto& c : party_) c->b(0xD7) = static_cast<std::uint8_t>(c->b(0xD6) == 0 ? c->b(0x4C) : 1);
    bool shown = false;
    for (const auto& lp : party_) {
        Character& l = *lp;
        if (l.b(0xD6) != 0 || l.b(0x4C) != 0 || l.b(0x95) == 0 || l.b(0xD4) != 1) continue;
        for (const auto& np : party_) {
            Character& n = *np;
            if (n.b(0xD6) != 0 || n.b(0x4C) == 0 || n.b(0xD7) == 0 || n.b(0xD4) != 1 || n.b(0x13) <= 3 ||
                has_effect(n, 3))
                continue;
            combat::Rng rng(seed_);
            const int res = combat::skill_check({skill_total(l, 0x49)}, 2, rng).result;   // 0x44DD0
            seed_ = rng.seed();
            data_note("skill", 0x49, res);
            if (res < 2) continue;
            if (!shown) {                              // 0x44DE2
                shown = true;
                layout_state0();
                set_window(1, 1, 0x26, 0x16, 0);
                print_line(0x0F, text_[Text::CbUsingLeader]);
                print_line(0, "");
            }
            print_line(0x0A, l.name() + text_[Text::CbLeads] + n.name() + text_[Text::CbPeriod]);
            print_line(0, "");
            n.b(0xD7) = 0;                             // 0x44E80
        }
    }
    if (shown) pause_continue();                       // Text.Pause 0653:1291
}

// --- the set-up: Combat.SetUp ovl8 +0x242A (0x3F20A) -----------------------------

void Game::combat_setup() {
    // 0x3F210..0x3F278: DS:3BF8 := 0 (Text.Print's typewriter off), the
    // view's images released (ovl27 +0x0B04, ovl24: nothing drawn), the
    // tile image allocated, the backbuffer DS:3C7A set to 168 x 21.
    typing_ = 0;                                       // 0x3F210
    msg_chars_ = 0;
    free_picture();                                    // ovl27 +0x0B04 (0x3F215)
    big_pic_.reset();                                  // DS:6B10 freed (0x3F22E)
    clear_row24();                                     // 0653:1359
    draw_string(0, 0x18, 0x0A, 0, text_[Text::CbBegins]);   // 0x3F298
    round_ = 0;          // DS:6B7D (0x3F29D)
    round_limit_ = 15;   // DS:6B7E (0x3F2A2)
    bodies_.clear();     // DS:68A1, DS:6869..
    v1(0x7F33) = 0;      // [DS:3BCC]+0x333
    field_select();      // 0x3F2D5
    alloc_blocks();      // 0x3F2DE
    place_all();         // 0x3F2E7
    // 0x3F303: the marker image DS:6BCB (24 x 24, 4 frames).
    marker_ = {};
    // 0x3F308..0x3F391: the view origin from the first active party member's
    // icon minus 3 (unclamped), the combat palette to the DAC (0x20..0xFF)
    // and the combat screen; with no active member none of it.
    for (std::size_t k = 0; k < icons_.size(); ++k) {
        const Character* c = icons_[k].rec;
        if (c && c->b(0xD5) != 0 && c->b(0xD6) == 0) {
            field_ox_ = s8(icons_[k].x - 3);
            field_oy_ = s8(icons_[k].y - 3);
            set_dac(0x20, 0xE0, pal_8a8b_);            // Gfx.SetDacRange
            combat_redraw();                           // ovl23 +0x2AF9
            break;
        }
    }
    enemy_hp_percent();                                // 0x3F393
    f6bd7_ = f6bd2_ = 0;                               // DS:6BD7 / 6BD2 (0x3F398)
    move_mode_ = 0;                                    // DS:8303 (0x3F3A2)
    if (debug_)                                        // 0x3F3B3..0x3F3EA: every record's +0xD4, +0xD5
        for (const auto& c : party_) debug_show(c->name(), c->b(0xD4), c->b(0xD5), 0);
}

void Game::field_select() {
    // ovl8 +0x16B8 (0x3E498): the tile set and the generator by var 0x4BE6.
    const int env = v0(0x4BE6);
    if (env == 0xA8 || env == 0xA2) {
        // Mars and Venus (0x3E4D8, 0x3E4FF): MARSCOM / VENUSCOM, 0x34 frames
        // into slots 1.. (slot 0 keeps the zero-filled frame the tile image
        // was made with, 0x149AB); the wild generator on the main stream.
        // The files hold 42 / 44 frames: the slots past them get the bytes
        // past the image (load_field_tiles), which no wild tile draws
        // (D-0063).
        load_field_tiles(env == 0xA8 ? "MARSCOM.DAX" : "VENUSCOM.DAX", 1, 0x34);
        field_tiles_[0] = Bytes(24 * 24, 0);
        field_wild(env);
    } else {
        // ovl31 +0x0022 (0x3E56D): the tiles, 0x35 frames from frame 0.
        load_field_tiles(env == 0x88 || env == 0x82 ? "LOTEK.DAX" : "HITEK.DAX", 0, 0x35);
        field_ground(env);
    }
    // The working palette's 0x20..0x3F into DS:8A8B (0x3E57A).
    for (int i = 0x20; i < 0x40; ++i) pal_8a8b_[static_cast<std::size_t>(i)] = pal_908b_[static_cast<std::size_t>(i)];
}

void Game::field_wild(int env) {
    // The wild generator ovl8 +0x126F (0x3E04F), on the main stream (no
    // SeedRandomTemp): every cell 6 (unfilled), features by the zone, then
    // the floor. The zone is var 0x4BB7 (the ECL's per-square table), +10
    // on Mars (var 0x4BE6 & 0x0F = 8). Session 32 reading
    // (docs/notes/combat.md "The wild battlefield").
    field_calls_.clear();                              // no temporary stream here
    field_.fill(6);                                    // 0x3E066
    int k = v0(0x4BB7);
    if ((env & 0x0F) == 8) k = (k + 10) & 0xFF;        // 0x3E077..0x3E086
    wild_radius_ = 0xFF;                               // DS:37BC (0x3E08A)
    auto f = [&](std::initializer_list<std::pair<int, int>> list) {
        for (const auto& [kind, count] : list) field_feature(kind, count);
    };
    switch (k) {
        case 1: case 8: case 9: f({{6, 6}, {1, 8}, {3, 1}, {4, 2}, {5, 2}, {8, 2}}); break;          // 0x3E09E
        case 2: f({{1, 6}, {3, 1}, {4, 1}, {5, 1}, {7, 5}, {8, 1}}); break;                         // 0x3E0E1
        case 3: f({{2, 1}, {4, 3}, {7, 10}, {10, 1}, {9, 5}, {0x16, 1}}); break;                    // 0x3E124
        case 4: f({{10, 10}, {2, 1}, {3, 1}, {4, 1}, {7, 3}, {9, 10}, {0x16, 2}}); break;           // 0x3E167
        case 5: f({{10, 10}, {2, 1}, {3, 1}, {4, 1}, {7, 10}, {9, 15}, {0x16, 3}}); break;          // 0x3E1B4
        case 6: case 7: f({{1, 1}, {2, 2}, {3, 1}, {4, 1}, {7, 2}, {8, 1}, {9, 1}}); break;         // 0x3E205
        case 0x0B: case 0x14: f({{0xD, 2}, {0xE, 1}, {0x10, 1}, {0xE, 2}, {0x12, 3}, {0xC, 2}}); break;   // 0x3E256
        case 0x0C: case 0x12: f({{0xB, 4}, {0xC, 4}, {0xD, 2}, {0x12, 2}, {0xF, 0xC}}); break;      // 0x3E29D
        case 0x0D: case 0x13: f({{0xB, 3}, {0xC, 3}, {0xD, 2}, {0xE, 2}, {0x12, 4}, {0xF, 8}}); break;    // 0x3E2DA
        case 0x0E: case 0x15:                                                                      // 0x3E321
            for (int i = 1; i <= 6; ++i) {             // DS:6EDB
                wild_radius_ = static_cast<std::uint8_t>(roll_dice(1, 4) + 2);
                f({{0xE, 1}, {0x10, 1}, {0x13, 1}});
            }
            wild_radius_ = 0xFF;
            f({{0x14, 4}, {0x15, 4}, {0xC, 1}, {0xD, 1}});
            break;
        case 0x0F: f({{0xC, 2}, {0xD, 4}, {0x11, 3}, {0x12, 7}, {0xB, 1}}); break;                 // 0x3E398
        case 0x10: f({{0xD, 1}, {0x11, 1}, {0x12, 8}}); break;                                     // 0x3E3D0
        case 0x11: f({{0xB, 2}, {0xC, 4}, {0xD, 2}, {0x12, 4}, {0xF, 6}}); break;                  // 0x3E3F4
        default:
            // 0x3E428: the developers' message (0653:0007): the text, "->",
            // StrInt(0), "...", drawn at (0, 0) in 0 on 0x0F; Text.Pause.
            // Mars zone 0 (7 squares of #64's table) reaches it.
            draw_string(0, 0, 0, 0x0F, text_[Text::CbWildUnknown] + text_[Text::DbgArrow] + "0" + text_[Text::DbgDots]);
            pause_continue();
            break;
    }
    if (k < 10) field_fill(0x6C, 2);                   // 0x3E43F
    else if (k == 0x0F || k == 0x10) field_fill(0x5B, 0);   // set CS:124F
    else field_fill(0x59, 2);
}

void Game::field_feature(int kind, int count) {
    // Feature ovl8 +0x0F29 (0x3DD09): the 11-byte record DS:0691 + 11 kind:
    // chance, cluster dice (count, sides), clearance, radius, radius die,
    // flags (1 the second tile below, else right; 2 tiles once a cluster;
    // 4 the second tile with the first's variant; 8 variants doubled; 0x10
    // the last centre kept; 0x20 the radius exact), the two tiles (6 none)
    // and their variant dice.
    const auto r = [&](int i) { return static_cast<int>(text_.ds8(0x0691 + 11 * static_cast<std::size_t>(kind) + static_cast<std::size_t>(i))); };
    const real48::Real two{0x0082, 0, 0};              // CX:SI:DI = 0082 0000 0000 (0x3DF0F)
    const real48::Real pi{0x2182, 0xDAA2, 0x490F};     // 0x3DF1D
    const int flags = r(6);
    for (int i = 1; i <= count; ++i) {                 // 0x3DD2B
        if (!percent(r(0))) continue;                  // 0x3DD44: one Random(100) always
        int n = roll_dice(r(1), r(2));                 // 0x3DD60
        bool first = true;
        int radius = r(4);
        const int clear = r(3);
        if (!(flags & 0x10)) {                         // 0x3DD87
            wild_cx_ = static_cast<std::uint8_t>(roll_dice(1, 50) - 1);
            wild_cy_ = static_cast<std::uint8_t>(roll_dice(1, 25) - 1);
        }
        if (r(5) > 0) radius = (radius + roll_dice(1, r(5))) & 0xFF;   // 0x3DDB2: no -1
        if (static_cast<std::int8_t>(wild_radius_) > 0) radius = wild_radius_;   // 0x3DDCC (signed)
        int t1 = 0, t2 = 0;
        for (; n > 0; n = (n - 1) & 0xFF) {            // 0x3DDD9; every attempt counts
            if (first || !(flags & 2)) {               // 0x3DDE2: the tiles
                int v = r(9) > 0 ? roll_dice(1, r(9)) - 1 : 0;
                if (flags & 8) v = (v * 2) & 0xFF;
                t1 = (r(7) + v) & 0xFF;
                t2 = r(8);
                if (t2 != 6) {
                    if (!(flags & 4)) {
                        v = r(10) > 0 ? roll_dice(1, r(10)) - 1 : 0;
                        if (flags & 8) v = (v * 2) & 0xFF;
                    }
                    t2 = (t2 + v) & 0xFF;
                }
                first = false;
            }
            int x = wild_cx_, y = wild_cy_;
            if (radius > 0) {                          // 0x3DEB5
                // The distance before the angle's RandomReal (0x3DED2 / 0x3DEF5).
                const real48::Real rr = (flags & 0x20) ? real48::from_long(radius)
                                                       : real48::from_long(roll_dice(1, (radius + 1) & 0xFF) - 1);
                const real48::Real th = real48::mul(real48::mul(random_real(), two), pi);
                x = (real48::trunc(real48::mul(real48::cos(th), rr)) + wild_cx_) & 0xFF;   // 0x3DF34..0x3DF73
                y = (real48::trunc(real48::mul(real48::sin(th), rr)) + wild_cy_) & 0xFF;
            }
            // Fits (nested ovl8 +0x0DE7, 0x3DBC7): the clearance square
            // all free, or the cell and the second tile's cell free.
            bool fits = true;
            if (clear > 0) {
                for (int a = -clear; a <= clear; ++a)
                    for (int b = -clear; b <= clear; ++b)
                        if (fits) fits = field_free(s8(x + a), s8(y + b));
            } else if (!field_free(s8(x), s8(y))) {
                fits = false;
            } else if (t2 != 6) {
                fits = flags & 1 ? field_free(s8(x), s8(y + 1)) : field_free(s8(x + 1), s8(y));
            }
            if (!fits) continue;
            field_[static_cast<std::size_t>(y * kFieldW + x)] = static_cast<std::uint8_t>(t1);   // 0x3DFB2
            if (t2 != 6) {
                if (flags & 1) y = (y + 1) & 0xFF;
                else x = (x + 1) & 0xFF;
                if (y <= 0x18 && x <= 0x31) field_[static_cast<std::size_t>(y * kFieldW + x)] = static_cast<std::uint8_t>(t2);
            }
        }
    }
}

bool Game::field_free(int x, int y) const {
    // ovl8 +0x0D89 (0x3DB69): on the map and an open floor tile (set
    // CS:0D69: 6 unfilled, 0x59..0x5B Mars, 0x6C/0x6D Venus).
    if (x < 0 || x > 0x31 || y < 0 || y > 0x18) return false;
    const int t = field_[static_cast<std::size_t>(y * kFieldW + x)];
    return t == 6 || t == 0x59 || t == 0x5A || t == 0x5B || t == 0x6C || t == 0x6D;
}

void Game::field_fill(int base, int range) {
    // Fill ovl8 +0x0CE1 (0x3DAC1): every unfilled cell, column by column
    // (x outer, y inner), base + RollDice(1, range) - 1.
    for (int x = 0; x <= 0x31; ++x)
        for (int y = 0; y <= 0x18; ++y) {
            std::uint8_t& c = field_[static_cast<std::size_t>(y * kFieldW + x)];
            if (c == 6) c = static_cast<std::uint8_t>(base + (range > 0 ? roll_dice(1, range) - 1 : 0));
        }
}

int Game::field_cell(int x, int y, int d) const {
    // ovl8 +0x06F2 (0x3D4D2): 0 open, 1 a wall that blocks, 3 a wall that
    // doesn't (a door); off the map: open only east/west on the party's row.
    if (0 <= x && x <= 15 && 0 <= y && y <= 15) {
        if (!passable(x, y, d)) return 1;
        return wall_at(d, y, x) != 0 ? 3 : 0;
    }
    return y == y_ && (d == 2 || d == 6) ? 0 : 1;
}

int Game::field_edge(int x, int y, int d) const {
    // ovl8 +0x0774 (0x3D554): both sides of the edge.
    x = s8(x);
    y = s8(y);
    return field_cell(x, y, d) | field_cell(s8(x + kDx[d]), s8(y + kDy[d]), (d + 4) % 8);
}

void Game::field_ground(int env) {
    // The ground generator ovl8 +0x0BC3 (0x3D9A3): a 13 x 5 window of map
    // squares around the party, drawn skewed into the 50 x 25 tiles; its
    // decoration is random under Game.SeedRandomTemp(byte DS:82F4) (0x3D9A9),
    // and the main stream is restored afterwards (0x3DAB8). Session 19
    // reading; the call sequence matches every traced fight, the tiles four
    // memory dumps (docs/notes/combat.md).
    // ovl8 +0x0022 (0x3CE02, at 0x3DAB8): RandSeed restored: r goes out of scope, seed_ was never touched
    // ovl8 +0x0000 (0x3CDE0): temporary RandSeed := byte DS:82F4 (a local Rng; seed_ stays as it was)
    combat::Rng r(block_id_);
    field_.fill(static_cast<std::uint8_t>(env == 0x88 || env == 0x82 ? 7 : 0x2D));
    int dx = 0, dy = 0;
    auto at = [&](int px, int py, int& X, int& Y) {
        X = s8(5 * dy + 6 * dx + 0x15 + px);
        Y = s8(5 * dy + 0x0A + py);
        return 0 <= X && X <= 0x31 && 0 <= Y && Y <= 0x18;
    };
    auto put = [&](int px, int py, int t) {   // ovl8 +0x0036 (0x3CE16)
        int X, Y;
        if (at(px, py, X, Y)) field_[static_cast<std::size_t>(Y * kFieldW + X)] = static_cast<std::uint8_t>(t + 1);
    };
    auto put_floor = [&](int px, int py, int t) {   // ovl8 +0x00DA: only on untouched floor (CS:00BA = {7, 0x2D})
        int X, Y;
        if (!at(px, py, X, Y)) return;
        const int v = field_[static_cast<std::size_t>(Y * kFieldW + X)];
        if (v == 7 || v == 0x2D) put(px, py, t);
    };
    auto R = [&](int n, const char* site) { return r.random(n, site); };
    if (env & 0x40) {   // ovl8 +0x0B55 (0x3D935)
        for (dy = -2; dy <= 2; ++dy)
            for (dx = -6; dx <= 6; ++dx)
                if (R(2, "ovl8 +0x0B77") == 0) {
                    put(4, 2, 0x37);
                    put(5, 2, 0x38);
                    put(5, 3, 0x39);
                    put(6, 3, 0x3A);
                }
    }
    for (dy = -2; dy <= 2; ++dy) {
        for (dx = -6; dx <= 6; ++dx) {
            const int x = s8(x_ + dx), y = s8(y_ + dy);
            const int W = field_edge(x, y, 6), N = field_edge(x, y, 0), E = field_edge(x, y, 2);
            if (W == 1) {   // WestWall 0x3D5C8
                for (int rr = 2; rr <= 4; ++rr) {
                    put(rr - 1, rr, 0x1B);
                    put(rr, rr, 0x1C);
                    put(rr + 1, rr, 0x1A);
                }
            } else if (W == 3) {
                put(1, 2, 0x1B); put(2, 2, 0x1D); put(3, 2, 0x22); put(2, 3, 0x25);
                put(3, 3, 0x26); put(4, 4, 0x23); put(5, 4, 0x24);
            }
            if (N == 1) {   // NorthWall 0x3D67E
                put(3, 0, 0x18); put(4, 0, 0x18); put(3, 1, 0x19); put(4, 1, 0x19);
            }
            // Corners 0x3D6C2: tables DS:061F / DS:0657 (the overlapping
            // 4c + 2row + col indexing as coded).
            int c = (N != 0) + 2 * (W != 0) + 4 * (field_edge(x - 1, y, 0) != 0) + 8 * (field_edge(x, y - 1, 6) != 0);
            if (c)
                for (int row = 0; row < 2; ++row)
                    for (int col = 1; col <= 2; ++col) {
                        const int t = text_.ds8(0x061F + static_cast<std::size_t>(4 * c + 2 * row + col));
                        if (t < 0xFF) put(col, row, t);
                    }
            const int ne = field_edge(x, y - 1, 2);
            c = (field_edge(x + 1, y, 0) != 0) + 2 * (E != 0) + 4 * (N != 0) + 8 * (ne != 0);
            if (c)
                for (int row = 0; row < 2; ++row)
                    for (int col = 5; col <= 6; ++col) {
                        const int t = text_.ds8(0x0657 + static_cast<std::size_t>(4 * c + 2 * row + col));
                        if (t < 0xFF) put(col, row, t);
                    }
            if (N == 3) {
                put(2, 0, 0x1E);
                put(2, 1, W == 0 ? 0x20 : 0x2A);
                put(5, 0, ne == 0 ? 0x1F : 0x2B);
                put(5, 1, 0x21);
            }
            if (ne == 3) put(5, 0, N == 0 ? 0x27 : 0x29);
            const int F = map_byte(x, y) & 0x40;
            // ScatterTiles ovl8 +0x0171 (0x3CF51): decoration along solid north
            // and east edges; the call sequence depends on N, E, F and env only.
            if (!(env & 8)) {
                if (N == 1)
                    for (int i = 4; i <= 6; ++i)
                        if (R(10, "ovl8 +0x019C") < 3) put_floor(i, 2, R(2, "ovl8 +0x01B1") + 0x33);
                if (E == 1) {
                    if (F) {
                        for (int j = 2; j <= 4; ++j) {
                            if (R(10, "ovl8 +0x01E6") < 5) {
                                const int sel = R(3, "ovl8 +0x01F7");
                                static const char* a[3] = {"ovl8 +0x0205", "ovl8 +0x0253", "ovl8 +0x02A0"};
                                static const char* b[3] = {"ovl8 +0x0228", "ovl8 +0x0276", "ovl8 +0x02C3"};
                                static const int tile[3] = {0x2E, 0x31, 0x32};
                                if (R(10, a[sel]) < 8) {
                                    put_floor(j + 4, j, tile[sel]);
                                    if (R(10, b[sel]) < 5) put_floor(j + 3, j, 0x2F);
                                }
                            } else {
                                const int sel = R(6, "ovl8 +0x02E9");
                                static const char* a[6] = {"ovl8 +0x02F7", "ovl8 +0x0322", "ovl8 +0x034D",
                                                           "ovl8 +0x0377", "ovl8 +0x03A1", "ovl8 +0x03CB"};
                                static const int tile[6] = {0x2D, 0x2E, 0x30, 0x31, 0x35, 0x36};
                                if (R(10, a[sel]) < 8) put_floor(j + 4, j, tile[sel]);
                            }
                        }
                    } else {
                        for (int j = 2; j <= 4; ++j)
                            if (R(10, "ovl8 +0x0402") < 8)
                                put_floor(j + 4, j, R(2, "ovl8 +0x0410") == 0 ? 0x2D : 0x36);
                    }
                }
            } else {
                if (N == 1) {
                    if (F) {
                        static const int tile[6] = {0x3B, 0x3C, 0x3E, 0x3F, 0x42, 0x44};
                        for (int i = 4; i <= 6; ++i)
                            if (R(10, "ovl8 +0x0475") < 8) put_floor(i, 2, tile[R(6, "ovl8 +0x0486")]);
                    } else {
                        static const int tile[3] = {0x3E, 0x42, 0x43};
                        for (int i = 4; i <= 6; ++i)
                            if (R(10, "ovl8 +0x0525") < 8) put_floor(i, 2, tile[R(3, "ovl8 +0x0533")]);
                    }
                }
                if (E == 1) {
                    if (F) {
                        static const int tile[6] = {0x3D, 0x3E, 0x41, 0x42, 0x43, 0x3F};
                        for (int j = 2; j <= 4; ++j)
                            if (R(10, "ovl8 +0x059F") < 8) {
                                const int sel = R(6, "ovl8 +0x05B0");
                                put_floor(j + 4, j, tile[sel]);
                                if (sel == 5 && R(10, "ovl8 +0x0661") < 5) put_floor(j + 3, j, 0x40);
                            }
                    } else {
                        // Quirk (0x3D466, reproduced): this branch draws on the
                        // north row again.
                        static const int tile[3] = {0x3E, 0x42, 0x43};
                        for (int i = 4; i <= 6; ++i)
                            if (R(10, "ovl8 +0x0698") < 8) put_floor(i, 2, tile[R(3, "ovl8 +0x06A6")]);
                    }
                }
            }
        }
    }
    field_calls_ = r.calls();
}

// --- combat blocks: Combat.AllocBlocks ovl8 +0x17AF (0x3E58F) -----------------

void Game::alloc_blocks() {
    for (std::size_t n = 1; n <= party_.size(); ++n) {
        Character& c = *party_[n - 1];
        recalc(c);                    // 0x3E5B6
        c.cb.assign(0x12, 0);         // GetMem(0x12), FillChar 0
        if (n > v1(0x7F3E)) c.cb[0x0F] = 1;   // past the party: the enemy's block flag
        const int e7 = c.b(0xE7) & 0x7F;
        if (c.b(0xD6) == 0 && c.cb[0x0F] == 1 && (e7 == 0 || e7 >= 0x67))   // 0x3E638
            c.b(0xE7) = static_cast<std::uint8_t>(v1(0x7EC6) + 0x80);
    }
}

// --- placement: Combat.Place ovl8 +0x2089 (0x3EE69) ----------------------------

void Game::place_all() {
    // Place, Origins (+0x1EBA), PlaceOne (+0x1A30), TryCell (+0x18B6) and the
    // footprint test (ovl30 +0x0E23). Session 19 reading; positions and
    // facings match two memory dumps (docs/notes/combat.md). No RNG.
    // Tables (DS): 05B4 / 05C4 per direction group, 05D4 facings, 05D8 /
    // 05E0 row starts, 05E8 formation rows (min, max), 258A terrain, 27EE
    // footprints.
    auto ds = [&](std::size_t off) { return static_cast<int>(text_.ds8(off)); };
    auto terrain = [&](int t) { return ds(0x258A + 4 * static_cast<std::size_t>(t & 0xFF)); };
    struct Side {
        int ox = 0, oy = 0;     // DS:379E.. / 37A0..: the side's square offset
        int dirn = 0;           // DS:37A4 / 37A5
        int rowlen = 0;         // DS:37A2 / 37A3
        std::array<std::array<std::array<int, 11>, 6>, 4> grid{};   // DS:358E + side * 0x108
    };
    std::array<Side, 2> side{};
    int cur_side = 0;           // DS:37AC (also Origins' loop variable)
    const int saved = facing_;
    int fac = facing_, last = 0xFF;
    const int dist0 = v1(0x7EC1);
    const int dirs = approach_mask_;
    // CountSides (ovl23 +0x1A6A): the records with +0xD5 != 0, per side;
    // the fight's first test reads these counts (before monsters that
    // don't fit leave).
    count_sides();
    const int cnt[2] = {side_count_[0], side_count_[1]};
    side[0].rowlen = (cnt[0] + 1) / 2;
    int g = 0;
    for (int b = 0; b < 4; ++b) g += (dirs >> b) & 1;
    if (g == 0) g = 1;
    side[1].rowlen = (cnt[1] + 2 * g - 1) / (2 * g);
    auto origins = [&] {        // ovl8 +0x1EBA (0x3EC9A)
        int dist = dist0;
        if (dist == 0 && fac != saved) dist = 1;
        if (fac == last) return;
        last = fac;
        side[0].dirn = fac / 2;
        side[1].dirn = ((fac + 4) % 8) / 2;
        side[1].ox = s8(side[0].ox + dist * kDx[fac]);
        side[1].oy = s8(side[0].oy + dist * kDy[fac]);
        for (int sd = 0; sd < 2; ++sd)
            for (int layer = 0; layer < 4; ++layer) {
                const int grp = layer == 1 ? 4 : side[static_cast<std::size_t>(sd)].dirn;
                for (int row = 0; row < 6; ++row) {
                    const int lo = s8(ds(0x05E8 + static_cast<std::size_t>(12 * grp + 2 * row)));
                    const int hi = s8(ds(0x05E9 + static_cast<std::size_t>(12 * grp + 2 * row)));
                    for (int col = 0; col < 11; ++col)
                        side[static_cast<std::size_t>(sd)].grid[static_cast<std::size_t>(layer)]
                            [static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] = lo <= col && col <= hi;
                }
            }
        // Quirk (oracle): DS:37AC is Origins' loop variable and stays 1, so the
        // record being placed takes side 1's origin, grid and direction.
        cur_side = 1;
    };
    // The footprint cells DS:27EE + 8 size: up to 4 (dx, dy), dx < 0 ends.
    auto foot = [&](int size, int i, int& fx, int& fy) {
        fx = s8(ds(0x27EE + static_cast<std::size_t>(8 * size + 2 * i)));
        fy = ds(0x27EF + static_cast<std::size_t>(8 * size + 2 * i));
        return fx >= 0;
    };
    // The occupancy grid DS:6267 (ovl30 +0x059E) over the icons placed so far.
    auto occupant = [&](int x, int y, std::size_t upto) {
        int occ = 0;
        for (std::size_t k = 0; k < upto && k < icons_.size(); ++k) {
            const Icon& ic = icons_[k];
            if (!ic.size) continue;
            for (int i = 0; i < 4; ++i) {
                int fx, fy;
                if (foot(ic.size, i, fx, fy) && ic.x + fx == x && ic.y + fy == y) occ = static_cast<int>(k + 1);
            }
        }
        return occ;
    };
    auto try_cell = [&](std::size_t k, int pc, int pr, int cx, int cy, int layer) {   // TryCell, ovl8 +0x18B6 (0x3E696)
        Side& sd = side[static_cast<std::size_t>(cur_side)];
        if (pc < 0 || pc > 10 || pr < 0 || pr > 5) return 0;
        int& cell = sd.grid[static_cast<std::size_t>(layer)][static_cast<std::size_t>(pr)][static_cast<std::size_t>(pc)];
        if (!cell) return 0;
        Icon& ic = icons_[k];
        ic.x = s8(5 * cy + 6 * cx + pc + 0x16);   // written before the test (quirk)
        ic.y = s8(5 * cy + pr + 0x0A);
        // ovl30 +0x0E23 (0x5FB93), direction 8: the last other occupant, and
        // the footprint's tile with the highest terrain value (0: off the map).
        int occ = 0, tile = 0x17, best = 1;
        for (int i = 0; i < 4 && ic.size; ++i) {
            int fx, fy;
            if (!foot(ic.size, i, fx, fy)) continue;
            const int x = s8(ic.x + fx), y = s8(ic.y + fy);
            int o = 0, t = 0;
            if (0 <= x && x <= 0x31 && 0 <= y && y <= 0x18) {
                o = occupant(x, y, k);
                t = field_[static_cast<std::size_t>(y * kFieldW + x)];
            }
            if (o > 0) occ = o;
            if (t == 0 || tile == 0) tile = 0;
            else if (terrain(t) >= best) {
                best = terrain(t);
                tile = t;
            }
        }
        if (occ == 0 && tile > 0 && terrain(tile) < 0xFF) {
            if (ic.rec->b(0xD5) != 0 || !f82ea_) cell = 0;
            return 1;
        }
        return 0;
    };
    auto place_one = [&](std::size_t k) {   // +0x1A30 (0x3E810)
        Side& sd = side[static_cast<std::size_t>(cur_side)];
        int first = 1, fail = 0, st = 1, n = 0, layer = 0, ok = -1;
        int cx = sd.ox, cy = sd.oy, pc = 0, pr = 0, c0 = 0, r0 = 0, m = 0, cnt_row = 0;
        for (;;) {
            const int di = ds(0x05C4 + static_cast<std::size_t>(sd.dirn * 4 + layer)) / 2;
            const int L = layer > 0 ? 1 : 0;
            if (st == 1) {
                const int a = ds(0x05D4 + static_cast<std::size_t>((di + 2) % 4));
                c0 = s8(n * kDx[a] + s8(ds(0x05D8 + static_cast<std::size_t>(L * 4 + di))));
                r0 = s8(n * kDy[a] + s8(ds(0x05E0 + static_cast<std::size_t>(L * 4 + di))));
                pc = c0; pr = r0; m = 1; st = 2; cnt_row = 1;
            } else if (st == 2) {
                const int a = ds(0x05D4 + static_cast<std::size_t>((di + 1) % 4));
                pc = s8(c0 + m * kDx[a]); pr = s8(r0 + m * kDy[a]); st = 3; ++cnt_row;
            } else if (st == 3) {
                const int a = ds(0x05D4 + static_cast<std::size_t>((di + 3) % 4));
                pc = s8(c0 + m * kDx[a]); pr = s8(r0 + m * kDy[a]); st = 2; ++m; ++cnt_row;
            }
            const bool out = !(0 <= pc && pc <= 10 && 0 <= pr && pr <= 5);
            // ovl8 +0x1885 (0x3E665): 1 only if col is outside 0..10 and row outside 0..5
            const bool both = !(0 <= pc && pc < 11) && !(0 <= pr && pr < 6);   // +0x1885
            if (st > 1 && ((out && !both) || (first && cnt_row >= sd.rowlen) || (!first && cnt_row > 11))) {
                ++n;
                if (cur_side == 0 && (sd.dirn & 1) && layer == 0 && n == 1) {
                    const int x = s8(x_ + sd.ox), y = s8(y_ + sd.oy);
                    bool open = false;
                    for (int i = 1; i <= 3; ++i)
                        if (mode_ == 3 || field_edge(x, y, ds(0x05B4 + static_cast<std::size_t>(sd.dirn * 4 + i))) == 0)
                            open = true;
                    if (open) ++n;
                }
                st = 1;
                first = 0;
            }
            if (out && both) {
                ok = 0;
                st = 0;
                while (layer < 3 && st != 1) {
                    ++layer;
                    const int x = s8(x_ + sd.ox), y = s8(y_ + sd.oy);
                    const int dd = ds(0x05B4 + static_cast<std::size_t>(sd.dirn * 4 + layer));
                    const bool open = mode_ == 3 || field_edge(x, y, dd) != 1;
                    if (open) {
                        cx = s8(kDx[dd] + sd.ox);
                        cy = s8(kDy[dd] + sd.oy);
                        n = 0;
                        st = 1;
                    }
                }
                if (st != 1) fail = 1;
            }
            if (!out) ok = try_cell(k, pc, pr, cx, cy, layer);
            if (ok < 0)   // D-0028: unreachable from shipped data (session 19)
                throw Error("Place (ovl8 +0x1A30, 0x3EC72): the placement result is read before it is written "
                            "(an uninitialised local) [D-0028]");
            if (ok > 0 || fail) return fail ? 0 : 1;
        }
    };

    origins();
    icons_.clear();
    int d = 0;
    std::vector<Character*> order;
    for (const auto& c : party_) order.push_back(c.get());
    for (Character* c : order) {
        cur_side = c->b(0xD6);   // DS:37AC := +0xD6
        Icon ic;   // x/y: DS:60B3's old contents until TryCell writes them
        if (icons_.size() + 1 < icon_xy_.size()) {
            ic.x = icon_xy_[icons_.size() + 1].first;
            ic.y = icon_xy_[icons_.size() + 1].second;
        }
        ic.size = c->b(0x34) & 7;
        ic.rec = c;
        icons_.push_back(ic);
        const std::size_t k = icons_.size() - 1;
        if (dirs) {
            if (c->b(0xD6) == 0) fac = saved;
            else {
                do d = (d + 1) & 3; while (!((dirs >> d) & 1));
                fac = 2 * d;
            }
            origins();
        }
        int f = fac;
        if (c->b(0xD6) == 1) f = (f + 4) % 8;
        if (c->cb.size() == 0x12) c->cb[7] = static_cast<std::uint8_t>(ds(0x05D4 + static_cast<std::size_t>(f / 2)));   // 0x3F040
        const bool placed = (f82ea_ && c->b(0xD5) == 0) ? false : place_one(k) != 0;
        if (k + 1 < icon_xy_.size()) icon_xy_[k + 1] = {icons_[k].x, icons_[k].y};   // DS:60B3 + 4k as written
        if (placed) {
            if (c->b(0xD5) == 0) {   // a member down: a body, no icon
                icons_[k].size = 0;
                if (c->cb.size() == 0x12 && c->cb[0x0F] == 0) {
                    const std::size_t i = static_cast<std::size_t>(icons_[k].y * kFieldW + icons_[k].x);
                    // 0x3F0CF..0x3F0EE: the count first, then UnderTile
                    // (ovl30 +0x14F2) of the square as its floor: an earlier
                    // body's floor there, not tile 5 (session 51's audit;
                    // the new slot, counted before it is written, is left
                    // out: inference).
                    bodies_.push_back({c, icons_[k].x, icons_[k].y, field_underlying(icons_[k].x, icons_[k].y, field_[i])});
                    field_[i] = 5;
                }
            }
        } else {
            icons_[k].size = 0;
            if (c->cb.size() == 0x12 && c->cb[0x0F] == 1) {   // an enemy that doesn't fit leaves
                icons_.pop_back();
                cur_ = c;
                remove_char(true, false);
            }
        }
    }
    facing_ = static_cast<std::uint8_t>(saved);
}




}  // namespace cotd::game
