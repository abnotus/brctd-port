// SPDX-License-Identifier: GPL-3.0-or-later
// The area weapon: WeaponSpecial (ovl11 +0x16B0) with its scatter and
// explosion, the area damage (ovl22 +0x094A), EffectMessage's animation
// (ovl23 +0x171A), AreaAttack (ovl11 +0x3270) and the terrain hazard
// (ovl22 +0x0436). Session 20: main-session readings of 0x418D4..0x42AFD,
// 0x55994..0x55B52, 0x5762A..0x5780A and 0x15651 (the marker's four
// frames); docs/notes/combat_ai.md.
#include <cmath>

#include "core/real48.hpp"
#include "game/game.hpp"

namespace cotd::game {

namespace {

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }

}  // namespace

real48::Real Game::random_real() {
    // System.RandomReal 0BE4:1166: the next seed (as Random), its 32 bits
    // as the mantissa of a real with exponent 0x80, normalised: seed / 2^32.
    const std::uint32_t before = seed_;
    seed_ = seed_ * 0x08088405u + 1u;
    combat::rng_note(before, 0, -1);
    return real48::from_seed(seed_);
}

void Game::marker_four(int slot) {
    // 0267:2FE1 (0x15651): the marker's four frames from one combat image.
    marker_from(slot, 0, 0, false);
    marker_from(slot, 0, 1, true);
    marker_from(slot, 1, 2, true);
    marker_from(slot, 1, 3, false);
}

void Game::remove_item(Character& c, int i) {
    // RemoveItem ovl23 +0x0863 (0x56773): the item unlinked from the
    // record's list (+0xF7, next +0x2A) and freed. Its error paths (a nil
    // item, CS:0828; one not on the list, CS:083C) can't be reached from
    // here: the item is the record's own.
    c.items.erase(c.items.begin() + i);
    if (c.weapon == i) c.weapon = -1;
    else if (c.weapon > i) --c.weapon;
    if (c.armour == i) c.armour = -1;
    else if (c.armour > i) --c.armour;
}

void Game::remove_item_of(Character& rec, Character& owner, int i) {
    // RemoveItem(rec, node) where the node is owner's (0x56773..0x56890):
    // not on rec's list, the message CS:083C (0653:0C02, yellow on black),
    // then the party from DS:4EC5 on (+0xFF), the message again for each
    // record before the one holding the node, which loses it. Reached by
    // Gear's Trade and Sell after a roster click moved CurChar (D-0074).
    if (&rec != &owner) {
        press_continue(14, 0, text_[Text::LostItem]);
        for (const auto& m : party_) {
            if (m.get() == &owner) break;
            press_continue(14, 0, text_[Text::LostItem]);
        }
    }
    remove_item(owner, i);
}

void Game::area_attack(Character& c, int x, int y, bool& done) {
    // AreaAttack ovl11 +0x3270 (0x43B70).
    done = true;
    area_x_ = static_cast<std::uint8_t>(x);
    area_y_ = static_cast<std::uint8_t>(y);
    attack_command(c, nullptr, false, done);
}

// --- WeaponSpecial ovl11 +0x16B0 (0x41FB0..0x42AFD) ---------------------------------

void Game::weapon_special(Character& c, int sp) {
    // sp: the weapon's +0x3C (set CS:2200, 5..12); the row DS:08AA + 5 sp:
    // effect type, blast radius, a lingering area, save type, damage kind.
    f6993_ = 1;
    const int wi = c.weapon;
    Bytes& w = c.items.at(static_cast<std::size_t>(wi));
    const int type = w[0x2E];
    data_note("throw", type, c.b(0xD6));               // tools only: --data-trace
    auto row = [&](int i) { return static_cast<int>(text_.ds8(0x08AA + 5 * static_cast<std::size_t>(sp) + i)); };
    int dmg = (roll_dice(item_byte(type, 4), item_byte(type, 5)) + s8(item_byte(type, 6)) + s8(w[0x32])) & 0xFF;
    f698d_ = 0x0F;
    int dur = 0xFF;
    const int radius = row(1);
    if (sp == 5) {
        dur = (roll_dice(1, 6) + 1) & 0xFF;
        f698d_ = 0x17;
    } else if (sp == 7 || sp == 0x0C) {
        dur = dmg;
        f698d_ = 0x11;
        dmg = 0;
    } else if (sp == 0x0A) {
        f698d_ = 0x10;
    }
    const int kind = item_byte(type, 2);
    bool missile = true;
    if (kind == 7 || kind == 9) {
        w[0x34] &= 0x7F;
        missile = combat::area_message_missile(kind, false);   // D-0027
    } else if (kind == 8) {
        Bytes* l = find_readied(c, 6);
        if (l) data_note("throw", (*l)[0x2E], c.b(0xD6));   // tools only: the launcher fired (session 56)
        if (l) (*l)[0x34] &= 0x7F;
        missile = l != nullptr;
    }
    std::string msg = text_[missile ? Text::WsLaunch : Text::WsThrow].substr(0, 20);
    name_message(c, msg, 10, false);
    msg.clear();
    recalc(c);
    effect_hook(c, 0x0A, "WeaponSpecial (0x4213F)");
    const int sx = pos_x(c), sy = pos_y(c);
    const int aim_x = s8(area_x_), aim_y = s8(area_y_);   // for the dump's [area] line
    // 0x42160: the distance (max + min div 2) and the hit: d20 + to-hit
    // - the range band > 50, else up to four scatters.
    {
        const int ax = std::abs(s8(area_x_) - s8(sx)) & 0xFF, ay = std::abs(s8(area_y_) - s8(sy)) & 0xFF;
        const int d = (ax > ay ? ax + (ay >> 1) : (ax >> 1) + ay) & 0xFF;
        const int wr = weapon_range(c) & 0xFF;
        const int band = wr <= 1 ? 0 : d > ((3 * wr) >> 2) ? 5 : d > (wr >> 1) ? 2 : 0;
        const int roll = roll_dice(1, 20);
        if (roll + c.b(0xD8) - band <= 0x32) {
            int tries = 0, lim = 4, nx = 0, ny = 0;
            auto scatter = [&](int& v) {             // nested 0x418D4
                const int a = roll_dice(1, 4) >> 1;
                const int b = roll_dice(1, 2) * 2 - 3;
                const int sv = s8(v);
                if (sv < -8 || sv > 8) v = s8(v + b * a);
                else if (sv < 0) v = s8(v - a);
                else if (sv > 0) v = s8(v + a);
                else v = s8(v + b);
            };
            for (;;) {
                nx = s8(area_x_ - sx);
                ny = s8(area_y_ - sy);
                scatter(nx);
                scatter(ny);
                nx = s8(nx + sx);
                ny = s8(ny + sy);
                ++tries;
                if (nx >= 0 && nx <= 0x31 && ny >= 0 && ny <= 0x18) {
                    int tx = nx, ty = ny;
                    const bool ok = trace(sx, sy, tx, ty, lim);
                    nx = tx;
                    ny = ty;
                    if (ok) break;
                }
                if (tries >= 4) break;
            }
            if (tries < 4) {
                area_x_ = static_cast<std::uint8_t>(nx);
                area_y_ = static_cast<std::uint8_t>(ny);
                msg = text_[Text::WsAstray].substr(0, 20);
            }
        }
    }
    // 0x42299: the line to the square stops at the first tile 1 (chaff).
    trace_flags_[3] = 1;
    {
        int lim = 0xFF, tx = s8(area_x_), ty = s8(area_y_);
        if (!trace(sx, sy, tx, ty, lim) && trace_flags_[0]) {
            // An empty message gets the joiner, a set one is replaced:
            // the original's order, kept.
            msg = msg.empty() ? (msg + text_[Text::WsAnd]).substr(0, 20) : text_[Text::WsIt].substr(0, 20);
            msg = (msg + text_[Text::WsChaff]).substr(0, 20);
            area_x_ = static_cast<std::uint8_t>(tx);
            area_y_ = static_cast<std::uint8_t>(ty);
        }
    }
    projectile(sx, sy, s8(area_x_), s8(area_y_), w);
    field_view(s8(area_x_), s8(area_y_), 0xFF, 8);
    // 0x4238F: for the types of set CS:1661 (5, 6, 10, 11), the first
    // active record (the list DS:4EC5) holding effect 3, its icon and
    // square (0x423FA); then for those but 5 the blast's terrain (nested
    // 0x41960), which also finds the holder's cover (BP-0xE5/-0xE6, 0xFF
    // for none).
    Character* holder = nullptr;
    int hk = 0, hx = -1, hy = -1, cx = -1, cy = -1;
    if (sp == 5 || sp == 6 || sp == 0x0A || sp == 0x0B) {
        for (const auto& p : party_)
            if (p->b(0xD5) != 0 && has_effect(*p, 3)) {
                holder = p.get();
                hk = icon_index(holder);
                hx = pos_x(*holder);
                hy = pos_y(*holder);
                break;
            }
        if (sp != 5) blast_terrain(radius, holder, hx, hy, cx, cy);
    }
    scan(s8(area_x_), s8(area_y_), radius, 0xFF, 1);
    std::vector<Character*> victims;                   // BP-0xDF + 4 i, up to 40
    for (int i = 1; i <= scan_n_; ++i) {
        Character* v = icons_.at(static_cast<std::size_t>(scan_[static_cast<std::size_t>(i)].k - 1)).rec;
        if (!v || v->b(0xD5) == 0) continue;
        if (v == holder && cx != -1) {
            // 0x42502: the holder leaps for cover instead: the panel, the
            // message, the icon off the field (0xFF, 0xFF), the view, the
            // marker facing the leap, the icon animated to the cover
            // (1, 0x1E) and set there, the view again.
            combat_panel(*v);
            name_message(*v, text_[Text::WsCover], 0x0A, false);
            Icon& ic = icons_.at(static_cast<std::size_t>(hk - 1));
            ic.x = -1;
            ic.y = -1;
            field_view(s8(area_x_), s8(area_y_), 0xFF, 8);
            marker_from(v->b(0xB9), 0, 0, cx < hx);
            anim_icon(hx, hy, cx, cy, 1, 0x1E);
            ic.x = cx;
            ic.y = cy;
            field_view(s8(area_x_), s8(area_y_), 0xFF, 8);
            continue;
        }
        if (victims.size() >= 40)
            throw Error("WeaponSpecial (ovl11 0x425F7): more than 40 victims overrun its list");
        victims.push_back(v);
    }
    {
        // The engine's dump only (no screen): the shot for the plan's
        // measurements (party_c_fullrun.md Q1): the thrower and its side
        // (+0xD6), the weapon type, the square aimed at (+ where it
        // landed: scattered, or at the chaff), every record in the blast.
        std::string t = "[area " + c.name() + " (side " + std::to_string(c.b(0xD6)) + ") type " +
                        std::to_string(type) + " aimed " + std::to_string(aim_x) + "," + std::to_string(aim_y) +
                        " lands " + std::to_string(s8(area_x_)) + "," + std::to_string(s8(area_y_)) + ";";
        for (Character* v : victims) t += " " + v->name() + " (side " + std::to_string(v->b(0xD6)) + ")";
        note_transcript(t + "]");
    }
    explosion(sp, radius);
    if (!msg.empty()) {
        clear_cells(0x17, 0x0A, 0x26, 0x15);
        print_in(0x17, 0x0A, 0x26, 0x15, 0x0A, true, msg);
        game_delay();         // Game.Delay
        clear_text();
    }
    msg.clear();
    if (row(0) > 0)
        msg = (text_[Text::WsIs] + text_.ds_str(0x2AF3 + 0x29 * static_cast<std::size_t>(row(0)), 40)).substr(0, 20);
    if (row(2) != 0) {
        // 0x426D6: a lingering area of nine cells round the square, laid
        // where the tile underneath allows (DS:258A + 4 u below 0xFF), for
        // 4 rounds (1 in zero gravity), newest first in the list DS:6B4F.
        AreaFx a;
        a.x = s8(area_x_);
        a.y = s8(area_y_);
        a.dur = (v0(0x4BE6) & 0x10) ? 1 : 4;
        if (sp == 7) a.tile = 2;
        else if (sp == 8) a.tile = 1;
        else if (sp == 9) a.tile = 4;
        else   // the original's fatal error (CS:1696); no type with an area reaches it (rows 7..9 only)
            throw Error("WeaponSpecial (ovl11 0x4276E): a lingering area for type " + std::to_string(sp));
        for (int i = 1; i <= 9; ++i) {
            int x, y;
            area_cell(a, i, x, y);
            if (x < 0 || x > 0x31 || y < 0 || y > 0x18) {
                a.on[static_cast<std::size_t>(i)] = false;
                continue;
            }
            const int u = field_underlying(x, y, field_tile_at(x, y));
            a.on[static_cast<std::size_t>(i)] = u > 0 && text_.ds8(0x258A + 4 * static_cast<std::size_t>(u)) < 0xFF;
            a.under[static_cast<std::size_t>(i)] = u;
            if (a.on[static_cast<std::size_t>(i)]) field_[static_cast<std::size_t>(y * kFieldW + x)] = static_cast<std::uint8_t>(a.tile);
        }
        if (below_site()) a.heap = below_alloc();      // GetMem(0x1A) at 0x426DA
        areas_.insert(areas_.begin(), a);
        field_view(s8(area_x_), s8(area_y_), 0xFF, 8);
        game_delay();         // Game.Delay
    }
    for (Character* v : victims) {
        if (v->b(0xD5) == 0) continue;
        if (sp == 4 && (pos_x(*v) < s8(area_x_) || pos_y(*v) < s8(area_y_))) continue;
        bool saved;
        if (sp == 5) note_readied(*v, 3, "worn_use");         // tools only: --data-trace
        if (sp == 7) note_readied(*v, 4, "worn_use");
        if ((sp == 5 && (v->b(0x48) & 0x08)) || (sp == 7 && (v->b(0x48) & 0x80))) saved = true;
        else saved = save(*v, row(3), 0);
        if (dmg > 0) area_damage(*v, dmg, row(4), saved);
        if (v->b(0xD5) != 0 && row(0) > 0) apply_effect(*v, row(0), dur, 0, 0, row(4) == 1, saved, msg);
    }
    end_turn(c);
    if (w[0x38] > 0) {
        w[0x38] = static_cast<std::uint8_t>(w[0x38] - 1);
        if (w[0x38] == 0 && item_byte(type, 8) == 1) {
            remove_item(c, wi);
            c.weapon = -1;                             // +0xEB := nil
        }
    }
}

void Game::blast_terrain(int radius, const Character* holder, int hx, int hy, int& cx, int& cy) {
    // Nested 0x41960 (types 6, 10, 11): with the terrain open, every cell
    // of the 7 x 7 square round the target that a trace from the target
    // reaches within the radius turns into its broken tile (DS:258C + 4 u);
    // the tiles 0xFA..0xFD: a quarter of the time 0x92 with 0x93 or 0x99
    // below it, else 0xFA -> 0x98, 0xFB -> 0x95, 0xFC -> 0x96. A cell the
    // trace doesn't reach, with effect 3's holder present (0x41AC2): free
    // and passable (MapAt ovl30 +0x06BC, DS:258A + 4 t not 0xFF) and
    // reached by a trace from the holder (limit 0xFF), the cover with the
    // smallest count so far (BP-9 from 0xFF; ties keep the first).
    field_open_ = true;                                // [DS:68A2]+6
    int best = 0xFF;
    for (int i = 0; i <= 6; ++i)
        for (int j = 0; j <= 6; ++j) {
            int x = s8(s8(area_x_) + i - 3), y = s8(s8(area_y_) + j - 3);
            if (x < 0 || x > 0x31 || y < 0 || y > 0x18) continue;
            int lim = radius;
            if (!trace(s8(area_x_), s8(area_y_), x, y, lim)) {
                if (!holder) continue;
                x = s8(s8(area_x_) + i - 3);
                y = s8(s8(area_y_) + j - 3);
                lim = 0xFF;
                if (occupant(x, y) != 0 || text_.ds8(0x258A + 4 * static_cast<std::size_t>(field_tile_at(x, y))) == 0xFF)
                    continue;
                if (trace(hx, hy, x, y, lim) && lim < best) {
                    best = lim;
                    cx = x;
                    cy = y;
                }
                continue;
            }
            int t = text_.ds8(0x258C + 4 * static_cast<std::size_t>(field_underlying(x, y, field_tile_at(x, y))));
            if (t == 0) continue;
            int extra = 0;
            if (t >= 0xFA && t <= 0xFD) {
                if (percent(25)) {
                    t = 0x92;
                    extra = percent(25) ? 0x93 : 0x99;
                } else if (t == 0xFA) {
                    t = 0x98;
                } else if (t == 0xFB) {
                    t = 0x95;
                } else if (t == 0xFC) {
                    t = 0x96;
                }
            }
            field_set_tile(x, y, t);
            if (extra && y + 1 < kFieldH) {
                field_set_tile(x, y + 1, extra);
            } else if (extra) {
                // 0x41AAC..0x41ABA: SetTile(x, y + 1) with no row check;
                // on the bottom row it writes past the field's block
                // (GetMem 0x4E9, 0x3E49E): D-0072, not written here.
                combat::diag("D-0072", "WeaponSpecial's blast (ovl11 0x41AAC): SetTile (ovl30 +0x1647) of row 25, "
                             "column " + std::to_string(x) + ", past the battlefield: not written");
            }
        }
    field_open_ = false;
}


void Game::field_set_tile(int x, int y, int t) {
    // SetTile ovl30 +0x1647 (0x603B7): under an area's laid cell or a body
    // (DS:6862 + 7 j) the tile beneath changes; elsewhere the map's.
    bool found = false;
    for (AreaFx& a : areas_)
        for (int i = 1; i <= 9; ++i) {
            int cx, cy;
            area_cell(a, i, cx, cy);
            if (a.on[static_cast<std::size_t>(i)] && cx == x && cy == y) {
                a.under[static_cast<std::size_t>(i)] = t;
                found = true;
            }
        }
    for (Body& b : bodies_)
        if (b.x == x && b.y == y) {
            b.old = t;
            found = true;
        }
    if (!found) field_[static_cast<std::size_t>(y * kFieldW + x)] = static_cast<std::uint8_t>(t);
}

namespace {
// The explosion's CPU time per image on the reference machine (above).
constexpr long kExplosionImageUs = 440;
}  // namespace

void Game::explosion(int sp, int radius) {
    // Nested 0x41BC0: rings of images at random angles round the square
    // (RandomReal, Cos, Sin; four animated frames by d4 for 5, 6, 10, 11,
    // else the tile image of the lingering area), drawn masked into the
    // backbuffer in thirds of a square, each ring presented; first its
    // sound by the special (0x41BD1..0x41C43). The angles, Cos and Sin in TP's 6-byte reals,
    // as the original computes them (core/real48: 0x41DCF..0x41E6B).
    int slot = 0;
    bool anim = false;
    if (sp == 5) {
        sound(text_.ds16(0x21EC));                     // Dazzle (8)
        slot = 0x24;
        anim = true;
    } else if (sp == 7) {
        sound(text_.ds16(0x21E4));                     // Gas (4)
        slot = text_.ds8(0x2595);
    } else if (sp == 8) {
        sound(text_.ds16(0x21E4));                     // Chaff
        slot = text_.ds8(0x2591);
    } else if (sp == 9 || sp == 0x0C) {
        sound(text_.ds16(0x21E4));                     // Aerosol Mist, Stun
        slot = text_.ds8(0x259D);
    } else if (sp == 0x0A || sp == 0x0B || sp == 6) {
        sound(text_.ds16(0x21F2));                     // the explosives, launchers (11)
        slot = 0x1C;
        anim = true;
    } else {
        return;
    }
    if (anim) {
        marker_from(slot, 0, 0, false);
        marker_from(slot, 0, 1, true);
        marker_from(slot, 1, 2, false);
        marker_from(slot, 1, 3, true);
    } else {
        marker_from((slot >> 1) + 0x21, slot & 1, 0, false);
    }
    const int tx = s8(area_x_), ty = s8(area_y_);
    int adx = 0, ady = 0;
    if (tx < radius) adx = s8((tx - radius) * 3);
    else if (tx > 0x31 - radius) adx = s8((tx - 0x31 + radius) * 3);
    if (ty < radius) ady = s8((ty - radius) * 3);
    else if (ty > 0x18 - radius) ady = s8((ty - 0x18 + radius) * 3);
    const int n = (radius * 3 + 1) & 0xFF;
    const real48::Real two{0x0082, 0, 0};              // CX:SI:DI = 0082 0000 0000
    const real48::Real pi{0x2182, 0xDAA2, 0x490F};     // 2182 DAA2 490F
    // The rings take CPU time in the original, no timer: on the reference
    // machine (DOSBox-X, 30000 cycles) 0.44 ms per image, measured with
    // time-stamped Random calls (run s25_timing_area1, docs/notes/timing.md);
    // given to each ring before its Present.
    long cpu_us = 0;
    for (int i = 1; i <= n; ++i) {
        const real48::Real ir = real48::from_long(i);  // IntToReal 0x41DCF
        for (int j = (2 * i - 2) & 0xFF; j >= 0; --j) {
            cpu_us += kExplosionImageUs;
            const real48::Real th = real48::mul(real48::mul(random_real(), two), pi);   // 0x41DF6..0x41E10
            const int x = s8(real48::trunc(real48::mul(real48::cos(th), ir)) + adx);   // 0x41E27..0x41E3A
            const int y = s8(real48::trunc(real48::mul(real48::sin(th), ir)) + ady);   // 0x41E58..0x41E6B
            if (x < -9 || x > 9 || y < -9 || y > 9) continue;
            const int f = anim ? roll_dice(1, 4) - 1 : 0;
            const Bytes& img = marker_.frames[static_cast<std::size_t>(f & 3)];
            if (!img.empty()) field_blit(img, marker_.w, marker_.h, (x + 9) * 8, (y + 9) * 8, true, 8, false);
        }
        field_present();
        pause_ms(static_cast<int>(cpu_us / 1000));
        cpu_us %= 1000;
    }
    field_view(tx, ty, 0xFF, 8);
}

// --- the area effects: DS:6B4F ------------------------------------------------------

void Game::area_cell(const AreaFx& a, int i, int& x, int& y) const {
    const int d = text_.ds8(0x303B + static_cast<std::size_t>(i));   // 8 (the centre), 2, 3, .., 0, 1
    x = s8(a.x + s8(text_.ds8(0x257C + static_cast<std::size_t>(d))));
    y = s8(a.y + s8(text_.ds8(0x2585 + static_cast<std::size_t>(d))));
}

void Game::area_age() {
    // ovl22 +0x0CC3 (0x55D13), at a round's end: an area with one round
    // left goes (its cells back to the tile under them, or 5 where a body
    // lies: DS:6862 + 7 j), the others lose a round; after a removal the
    // remaining areas are laid again (0x55C4C) and a message shown.
    bool changed = false;
    for (std::size_t n = 0; n < areas_.size();) {
        AreaFx& a = areas_[n];
        if (a.dur > 1) {
            --a.dur;
            ++n;
            continue;
        }
        for (int i = 1; i <= 9; ++i) {
            if (!a.on[static_cast<std::size_t>(i)]) continue;
            int x, y;
            area_cell(a, i, x, y);
            bool body = false;
            for (const Body& b : bodies_)
                if (b.rec && b.x == x && b.y == y) body = true;
            field_[static_cast<std::size_t>(y * kFieldW + x)] = static_cast<std::uint8_t>(body ? 5 : a.under[static_cast<std::size_t>(i)]);
        }
        changed = true;
        below_free(a);                                  // FreeMem(node, 0x1A) at 0x55EB1
        areas_.erase(areas_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if (changed) {
        area_reapply();
        show_message(text_[Text::FxAirMsg]);           // ovl23 +0x0A18
    }
}

// --- D-0075: the area nodes below a reused field block (session 71) -------------
//
// Each lingering area is a heap node (GetMem(0x1A) at 0x426DA; FreeMem at
// 0x55EB1 when it ends): +0 the next node (far, nil for the oldest), +3 + i
// the tile under cell i, +0x0C + i cell i laid, +0x16 x, +0x17 y, +0x18
// rounds, +0x19 tile. Where a fight's field block reuses a free block of
// the heap (its place set by every allocation before the fight: D-0075),
// the nodes fill the rest of that block, and Trace's cells below the field
// read them. TP 5.5's heap (HEAP.OBJ, GetMem 0BE4:0329 / 0x1C25D, FreeMem
// 0BE4:0341 / 0x1C318): the free list is scanned from FreePtr, the newest
// entry first, and a block is taken from the first entry big enough, at
// its start (an emptied entry gets FreePtr's, 0x1C3EF); a freed block
// merges with every adjacent entry it meets and goes in as the newest; its
// bytes are left as they were. Measured per COMBAT site (KEYFEED's trap at
// 0x5E61C and a RAM dump in the fight); `field_lin` the field block's
// linear address there (the nodes' far pointers).

const Game::BelowSite* Game::below_site() const {
    static const BelowSite kSites[] = {
        // c2_34_mars's flight70_A: five nodes from the field's end, the
        // combat blocks 36 K further on (run s70_d0075_flightA2, RAM at keys
        // 518 and 519; the trap's 21 reads below the field all match).
        {4, 65, 0x8E2D, 0x696E2, "s70_d0075_flightA2"},
    };
    if (demo_) return nullptr;
    for (const BelowSite& b : kSites)
        if (traced_file_ == b.file && block_id_ == b.block && op_at_ == b.pc) return &b;
    return nullptr;
}

void Game::below_start() {
    // The block's rest after the field is one free entry (its end not
    // measured: at least the five nodes' 130 bytes; taken as large). The
    // other entries of the free list are not known: none newer took the
    // first node (it lies at the field's end).
    if (below_on_) return;
    below_on_ = true;
    below_free_.assign(1, {0, 0x8000});
    below_img_.assign(0x8000, -1);
}

int Game::below_alloc() {
    below_start();
    for (std::size_t i = 0; i < below_free_.size(); ++i) {
        auto& e = below_free_[i];
        if (e.second - e.first < 0x1A) continue;
        const int at = e.first;
        e.first += 0x1A;
        if (e.first == e.second) {                     // 0x1C3EF: FreePtr's entry moves into the slot
            below_free_[i] = below_free_[0];
            below_free_.erase(below_free_.begin());
        }
        return at;
    }
    throw Error("D-0075: the area nodes' block below the field is full (not measured)");
}

void Game::below_free(const AreaFx& a) {
    if (a.heap < 0 || !below_on_) return;
    for (std::size_t n = 0; n < areas_.size(); ++n)
        if (&areas_[n] == &a)
            for (int k = 0; k < 0x1A; ++k) below_img_[static_cast<std::size_t>(a.heap + k)] = area_node_byte(n, k);
    int from = a.heap, to = a.heap + 0x1A;
    for (std::size_t i = 0; i < below_free_.size();) {   // 0x1C350..0x1C39F
        const auto e = below_free_[i];
        bool merge = false;
        if (from >= e.first) {
            if (from <= e.second) {
                from = e.first;
                merge = true;
            }
        } else if (to >= e.first) {
            merge = true;
        }
        if (!merge) {
            ++i;
            continue;
        }
        if (to <= e.second) to = e.second;
        below_free_[i] = below_free_[0];                  // the slot gets FreePtr's entry, not looked at again
        below_free_.erase(below_free_.begin());
    }
    below_free_.insert(below_free_.begin(), {from, to});
}

int Game::area_node_byte(std::size_t n, int k) const {
    const AreaFx& a = areas_[n];
    if (k < 4) {                                        // +0: the next node, a normalised far pointer
        if (n + 1 >= areas_.size() || !below_site()) return 0;
        const int lin = below_site()->field_lin + 7 + kFieldW * kFieldH + areas_[n + 1].heap;
        const int w[2] = {lin & 0x0F, lin >> 4};
        return (w[k / 2] >> (8 * (k % 2))) & 0xFF;
    }
    if (k <= 0x0C) return a.under[static_cast<std::size_t>(k - 3)] & 0xFF;
    if (k <= 0x15) return a.on[static_cast<std::size_t>(k - 0x0C)] ? 1 : 0;
    switch (k) {
        case 0x16: return a.x & 0xFF;
        case 0x17: return a.y & 0xFF;
        case 0x18: return a.dur & 0xFF;
        default: return a.tile & 0xFF;
    }
}

void Game::area_reapply() {
    // 0x55C4C: every area's laid cells get its tile again.
    for (const AreaFx& a : areas_)
        for (int i = 1; i <= 9; ++i) {
            if (!a.on[static_cast<std::size_t>(i)]) continue;
            int x, y;
            area_cell(a, i, x, y);
            field_[static_cast<std::size_t>(y * kFieldW + x)] = static_cast<std::uint8_t>(a.tile);
        }
}

// --- AreaDamage ovl22 +0x094A (0x55994..0x55B52) ----------------------------------

void Game::area_damage(Character& v, int dmg, int kind, bool saved) {
    // The kind: 1 a save cancels it, 2 a save halves it.
    dmg_ = static_cast<std::uint8_t>(dmg);
    effect_hook(v, 6, "AreaDamage (0x559B0)");
    if (saved) {
        if (kind == 1) dmg_ = 0;
        else if (kind == 2) dmg_ = static_cast<std::uint8_t>(dmg_ / 2);
    }
    if (dmg_ == 0 || v.b(0xD5) == 0) return;
    std::string msg = dmg_ != 1 ? text_[Text::AreaDmg1] + str_byte(dmg_) + text_[Text::AreaDmg2] : text_[Text::AreaDmgOne];
    effect_message(v, f698d_, msg);
    take_damage(v, dmg_);
    if (v.b(0xD5) == 0) {
        msg = text_[Text::AreaDownMsg];
        if (v.cb.size() > 0x0F && v.cb[0x0F] == 0) {
            if (v.b(0xD4) == 4) msg += text_[Text::AreaDyingMsg];
            if (v.b(0xD4) == 2 || v.b(0xD4) == 3) msg = text_[Text::AreaDeadMsg];
        }
        name_message(v, msg, (row_ + 1) & 0xFF, false);
        if (mode_ != 5) {
            game_delay();
        } else {
            clear_on_down(v);
            effect_hook(v, 0x0D, "AreaDamage (0x55B3B)");
            death_anim(v);
        }
    }
    clear_text();
}

// --- EffectMessage ovl23 +0x171A (0x5762A..0x5780A) -------------------------------

void Game::effect_message(Character& c, int k, const std::string& msg) {
    // In a fight with k > 0: the combat image k + 0x0D's four frames over
    // the record's icon, (Delay factor div 2) + 1 times (once for k 15,
    // 16: set CS:16FA), 70 ms a frame, the background put back each time.
    if (mode_ != 5 || k == 0) {
        name_message(c, msg, 10, true);
        return;
    }
    marker_four(k + 0x0D);
    if (!icon_visible(&c, true)) field_view(pos_x(c), pos_y(c), 3, 8);
    const bool once = k == 15 || k == 16;
    sound(text_.ds16(once ? 0x21F2 : 0x21E0));         // 0x576C8 (11) / 0x576D3 (2)
    name_message(c, msg, 10, false);
    const int reps = once ? 0 : (v0(0x4BFC) >> 1) & 0xFF;
    const int ki = icon_index(&c);
    const Icon& ic = icons_.at(static_cast<std::size_t>(ki - 1));
    const int X = s8(ic.x - field_ox_) * 3 * 8, Y = s8(ic.y - field_oy_) * 3 * 8;   // DS:61D6 / 621E
    for (int r = 0; r <= reps; ++r)
        for (int f = 0; f <= 3; ++f) {
            Bytes under;
            for (int yy = 0; yy < marker_.h; ++yy)
                for (int xx = 0; xx < marker_.w; ++xx) {
                    const int px = X + xx, py = Y + yy;
                    under.push_back(px >= 0 && px < kViewPx && py >= 0 && py < kViewPx
                                        ? field_buf_[static_cast<std::size_t>(py * kViewPx + px)]
                                        : 0);
                }
            const Bytes& img = marker_.frames[static_cast<std::size_t>(f)];
            if (!img.empty()) field_blit(img, marker_.w, marker_.h, X, Y, true, 8, false);
            field_present();
            pause_ms(70);                              // Crt.Delay(70)
            field_blit(under, marker_.w, marker_.h, X, Y, false, 0, false);
        }
    field_present();
    if (reps == 0) game_delay();
    else msg_chars_ = 0;   // no message wait: its text counts toward none
}

// --- TerrainHazard ovl22 +0x0436 (0x55486..0x55562) -------------------------------

void Game::terrain_hazard(Character& c) {
    // A record on tile 2 saves (type 1) or is held for d6 + 1 rounds
    // (effect 1, whose handler ends its turn). The duration is rolled
    // before Effect.Apply's immunity hook.
    if (c.b(0xD5) == 0) return;
    int occ = 0, tile = 0;
    bool hazard = false;
    step_query(c, 8, occ, tile, hazard);
    if (!hazard) return;
    if (!has_effect(c, 1)) note_readied(c, 4, "worn_use");   // tools only: --data-trace
    if (has_effect(c, 1) || (c.b(0x48) & 0x80)) return;
    if (save(c, 1, 0)) return;
    const int d = roll_dice(1, 6) + 1;
    apply_effect(c, 1, d, 0, 0, false, false, text_[Text::HazardMsg]);
    if (has_effect(c, 1)) end_turn(c);                 // ovl10 +0x0044 via ovl22 +0x01F6
}

}  // namespace cotd::game
