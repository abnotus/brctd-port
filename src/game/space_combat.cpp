// SPDX-License-Identifier: GPL-3.0-or-later
// SPACE_COMBAT (ECL opcode 0x1F, ovl3 +0x3600, 0x37340): the ship fight,
// game state 8; its screens (ovl23 +0x1CF8..+0x2440, +0x31E5..) and the
// ship's repairs after it (ovl5 +0x0CFE). docs/notes/space_combat.md.
// Every routine of ovl3 but three is nested in the handler and shares its
// locals: they are the members of SpaceFight below (H.-x in the notes).
#include <algorithm>

#include "game/game.hpp"

namespace cotd::game {

namespace {

// The layout of state 8 (015F:01B5, the list DS:0E30): (x1, y1, x2, y2,
// start, end).
struct Line { int x1, y1, x2, y2, start, end; };
constexpr Line kSpaceLines[] = {{0, 0, 39, 0, 0, 1},   {0, 0, 0, 22, 0, 3},   {39, 0, 39, 22, 1, 2},
                                {0, 22, 39, 22, 3, 2}, {0, 18, 39, 18, 9, 7}, {25, 0, 25, 18, 6, 8},
                                {25, 13, 39, 13, 9, 7}};

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }
int s16(int v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(v)); }

}  // namespace

// --- the ship record (0x6C bytes) -------------------------------------------------------

int Ship::w(int off) const { return s16(p[off] | p[off + 1] << 8); }
void Ship::setw(int off, int v) {
    p[off] = static_cast<std::uint8_t>(v);
    p[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

// The screens of state 8 (ovl23 +0x1CF8..+0x26BE, +0x31E5..+0x3346): the
// ship fight's and ROCKET's (the travel menu, docs/notes/travel.md). The
// party's ship is DS:4A10 ([3BC8] + 0x200, var 0x4D00), the enemy's
// DS:4A14, nil (p = nullptr) outside SPACE_COMBAT (written only at
// 0x37455, 0x3771F and game init 0x3FD01).
struct ShipScreen {
    Game& g;
    Ship party, enemy;

    explicit ShipScreen(Game& game, Ship p = {}, Ship e = {}) : g(game), party(p), enemy(e) {}

    bool is_party(const Ship& s) const { return s.p == party.p; }
    bool is_enemy(const Ship& s) const { return enemy.p && s.p == enemy.p; }   // ship = DS:4A14
    const GameText& t() const { return g.text_; }
    std::string wshort(int type) const { return t().ds_str(0x1036 + 9 * static_cast<std::size_t>(type)); }

    static bool can_maneuver(const Ship& s) { return s.cur(2) > 0 && s.cur(4) > 0 && s.cur(5) > 0; }   // ovl23 +0x31E5

    int speed(const Ship& s) const {
        // Speed ovl23 +0x321B (0x5912B), a signed byte. DS:4A22 is reset
        // only at a fight's set-up (0x3749E): outside a fight the party's
        // speed keeps the last fight's boosts.
        if (!can_maneuver(s)) return 0;
        int v = (s.p[0x36] + (is_party(s) ? g.f4a22_ : g.f4a23_)) & 0xFF;
        if ((s.max(0) >> 1) > s.cur(0)) v = (static_cast<std::uint16_t>(s8(v)) >> 1) & 0xFF;
        if ((s.max(5) >> 2) > s.cur(5)) v = (v - 3) & 0xFF;
        else if ((s.max(5) >> 1) > s.cur(5)) v = (v - 1) & 0xFF;
        if (s8(v) < 1) v = 1;
        return s8(v);
    }

    void draw_field(int x, int y, int value, const Ship& s, int kind, int fg, int bg) {
        // DrawField ovl23 +0x1CF8 (0x57C08).
        std::string v;
        if (kind == 3 && is_enemy(s) && !g.f4a21_) {
            v = t()[Text::ScFieldUnknown];
            fg = 0x0C;
            bg = 8;
        } else {
            v = std::to_string(s16(value));            // StrInt width 0, string[6]
        }
        // n := byte(5 - Length) and the pad grows while n <> 0 (0x57C65..
        // 0x57CA5, unsigned): a 6-character number (-32768..-10000) runs
        // 255 rounds into the string[6] (StrStore 0x57C9D): 6 spaces
        // (deterministic quirk).
        const std::size_t n = v.size() < 5 ? 5 - v.size() : v.size() == 6 ? 6 : 0;
        std::string pad;
        for (std::size_t i = 0; i < n; ++i) pad += t()[Text::ScFieldPad];
        if (fg < 0) fg = 0x0F;
        if (bg < 0) bg = 8;
        const std::string suffix = kind == 1 ? t()[Text::ScTons] : kind == 2 ? t()[Text::ScFeet] : "";
        g.draw_string(x, y, fg, bg, pad + v + suffix);
    }

    void skippable_delay(int& n) {
        // ovl23 +0x1E24 (0x57D34): a key shortens the blink (not read;
        // KEYFEED never answers this poll), else Crt.Delay(70).
        if (n > 0) {
            if (g.input_ && (g.steps_unread_ = 0, g.input_->key_waiting())) n &= 1;
            else g.pause_ms(70);
        }
        --n;
    }

    void hline(int x1, int x2, int y, int c) {   // 0267:28D6
        // 0267:3E64 (0x164D4): 0267:28D6's VGA path: y byte, x1..x2 in the colour, straight to A000
        for (int x = x1; x <= x2; ++x) g.screen_.row(y)[x] = static_cast<std::uint8_t>(c);
    }
    void vline(int y1, int y2, int x, int c) {   // 0267:2BDD
        // 0267:3E90 (0x16500): 0267:2BDD's VGA path: x, y1..y2 in the colour, straight to A000
        for (int y = y1; y <= y2; ++y) g.screen_.row(y)[x] = static_cast<std::uint8_t>(c);
    }

    void system_bar(const Ship& s, int sys, bool animate) {
        // SystemBar ovl23 +0x1E60 (0x57D70).
        const bool p = is_party(s);
        if (!p && party.cur(1) == 0) return;
        const int col = p ? 0x1D : 3, row = p ? 2 * sys + 1 : sys + 0x0C, off = p ? 5 : 7, bg = p ? 8 : 0;
        const int x0 = p ? 232 : 120, y = p ? 16 * sys + 16 : 8 * sys + 96;
        const int cur = s.cur(sys), max = s.max(sys);
        const int fga = cur == 0 ? 0x0C : (p ? 0x0B : 0x0A), fgb = cur == 0 ? 0 : 0x0C;
        if (p) g.draw_string(0x1D, 2 * sys + 1, fga, 8, t().ds_str(0x1087 + 5 * static_cast<std::size_t>(sys)) + t()[Text::ScColon]);
        int n = animate ? 10 : 0;
        do {
            draw_field(col + off, row, cur, s, 0, (n & 1) == 0 ? fga : fgb, bg);
            skippable_delay(n);
        } while (n >= 0);
        int len = max >= 100 ? ((cur + 15) >> 4) * 80 / (max >> 4) : cur * 80 / max;
        len = std::clamp(len, 0, 79);
        if (len > 0) {
            hline(x0, x0 + len - 1, y + 3, g.text_.ds8(0x0D28 + static_cast<std::size_t>(sys)));
            hline(x0, x0 + len - 1, y + 4, g.text_.ds8(0x0D2E + static_cast<std::size_t>(sys)));
        }
        for (int i = len; i < 80; ++i) vline(y + 3, y + 4, x0 + i, (i & 7) == 0 ? 8 : 0);
    }

    void range_display() {
        // RangeDisplay ovl23 +0x22E2 (0x581F2).
        g.draw_string(1, 1, 0x0C, 0, t().ds_str(0x0D34 + 12 * static_cast<std::size_t>(g.f4a20_)));
        g.draw_string(1, 2, 0x0C, 0, t()[Text::ScRange] + std::to_string(std::max(s8(g.f4a1f_), 0)));
    }

    void ship_panel(const Ship& s) {
        // ShipPanel ovl23 +0x2144 (0x58054).
        if (is_party(s)) {
            for (int k = 0; k < 6; ++k) {
                g.fill_rect(0x1D, 2 * k + 1, 0x26, 2 * k + 1, 8);
                g.fill_rect(0x1D, 2 * k + 2, 0x26, 2 * k + 2, 0);
                for (int i = 0; i < 10; ++i) vline(8 * (2 * k + 2), 8 * (2 * k + 2) + 7, 232 + 8 * i, 8);
            }
            for (int k = 0x11; k <= 0x16; ++k) g.ship_image(k);
            for (int i = 0; i < 6; ++i) system_bar(s, i, false);
        } else {
            if (party.cur(1) == 0) {
                g.fill_rect(1, 0x0C, 0x18, 0x11, 8);
                g.draw_string(4, 0x0E, 0x0C, 8, t()[Text::ScSensorsGone]);
                return;
            }
            for (int k = 0x17; k <= 0x1C; ++k) g.ship_image(k);
            for (int i = 0; i < 6; ++i) system_bar(s, i, false);
        }
    }

    void weapon_list(const Ship& s, bool detail, int hl) {
        // WeaponList ovl23 +0x23F4 (0x58304): the summary (the right panel,
        // slots 0..2) or the detail (ShipStatus's left panel, slots 0..3,
        // with ammo and reloads, the enemy's kind 3: "?" without sensors).
        if (hl < 0 && !detail) g.fill_rect(0x1D, 0x0E, 0x26, 0x11, 8);
        int kind = 0, col = 0x1D, row = 0x0F, last = 2;   // 0x5835D
        if (detail) {                                  // 0x58334..0x58357
            col = 1;
            row = 0x0E;
            last = 3;
            if (!is_party(s)) kind = 3;
        }
        g.draw_string(col, row - 1, 0x0A, 8, t()[Text::ScWeapons]);
        if (detail) {
            g.draw_string(col + 0x0C, row - 1, 0x0A, 8, t()[Text::ScAmmo]);
            g.draw_string(col + 0x11, row - 1, 0x0A, 8, t()[Text::ScReloadsHead]);
        }
        for (int w = 0; w <= last; ++w) {
            const std::uint8_t* n = s.slot(w);
            if ((n[0] & 0x7F) == 0 || n[1] == 0) continue;
            const int fg = n[1] != n[2] ? 0x0E : 0x0A;
            if (detail) {                              // 0x58457..0x584BE: every type, before the name
                draw_field(col + 0x0A, row + w, n[4], s, kind, -1, -1);
                draw_field(col + 0x10, row + w, n[5], s, kind, -1, -1);
            }
            std::string num = std::to_string(n[2]);
            if (num.size() < 2) num = " " + num;
            int c = hl == w ? 10 : 0;
            do {
                const int k = (c & 1) == 0 ? fg : 0x0C;
                g.draw_string(col, row + w, k, 8, wshort(n[0] & 0x7F));
                g.draw_string(col + 8, row + w, k, 8, num);
                skippable_delay(c);
            } while (c >= 0);
        }
        g.ship_image(0x10);                            // SHIPS #26 at (26, 14)
    }

    void ship_status(const Ship& s, bool detail) {
        // ShipStatus ovl23 +0x3346 (0x59256..0x596D4): the ship's record in
        // the left panel, then its detailed weapon list. DS:4A21 (the
        // enemy's numbers shown) := detail and the party's Sensors > 0.
        g.f4a21_ = detail && party.cur(1) > 0;
        g.fill_rect(1, 1, 0x18, 0x11, 8);
        const int lc = is_party(s) ? 0x0B : 0x0E;      // 0x5928A..0x592A1
        auto label = [&](int x, int y, Text k) { g.draw_string(x, y, lc, 8, t()[k]); };
        label(1, 1, Text::SsName);
        g.draw_string(0x0A, 1, 0x0F, 8, s.name());
        label(1, 2, Text::SsType);
        g.draw_string(0x0A, 2, 0x0F, 8, t().ds_str(0x1104 + 18 * static_cast<std::size_t>(s.p[0x39])));
        label(1, 3, Text::SsRace);                     // the race byte signed (cbw, 0x59358)
        g.draw_string(0x0A, 3, 0x0F, 8, t().ds_str(static_cast<std::uint16_t>(0x1224 + 14 * s8(s.p[0x3A]))));
        label(1, 4, Text::SsTonnage);
        draw_field(0x0A, 4, s.w(0x10), s, 1, -1, -1);
        label(1, 5, Text::SsLength);
        draw_field(0x0A, 5, s.w(0x12), s, 2, -1, -1);
        label(1, 6, Text::SsWidth);
        draw_field(0x0A, 6, s.w(0x14), s, 2, -1, -1);
        label(1, 7, Text::SsCargo);
        draw_field(0x0A, 7, s.w(0x2E), s, 1, -1, -1);
        label(1, 9, Text::SsAc);
        draw_field(7, 9, s8(s.p[0x32]), s, 3, -1, -1);
        label(0x0D, 9, Text::SsCrew);
        draw_field(0x14, 9, s.w(0x30), s, 3, -1, -1);
        label(1, 0x0A, Text::SsSpeed);
        draw_field(7, 0x0A, speed(s), s, 3, -1, -1);
        label(0x0D, 0x0A, Text::SsReact);
        draw_field(0x14, 0x0A, s8(s.p[0x37]), s, 3, -1, -1);
        if (is_enemy(s)) {
            // 0x59593..0x59679: the range shown (DS:4A1F, a signed byte).
            label(1, 0x0B, Text::SsRange);
            const int r = s8(g.f4a1f_);
            if (r < 1) g.draw_string(0x0A, 0x0B, 0x0E, 8, t()[Text::SsRamming]);
            else
                g.draw_string(0x0A, 0x0B, 0x0F, 8,
                              t()[r < 3 ? Text::SsShort : r < 5 ? Text::SsMedium : r < 7 ? Text::SsLong : Text::SsExtreme]);
        } else {
            // 0x5967B..0x596BE: the medical supplies, [3BC8] + 0x163.
            label(1, 0x0B, Text::SsMedical);
            draw_field(0x14, 0x0B, g.v0(0x4C63), s, 3, -1, -1);
        }
        weapon_list(s, true, -1);                      // 0x596CE
    }

    void main_view() {
        // MainView ovl23 +0x2358 (0x58268). Outside a fight (DS:4A14 nil,
        // 0x58270..0x58277) the party's ship's status (0x582D6..0x582E2).
        if (!enemy.p) {
            ship_status(party, true);
            return;
        }
        g.backbuffer_image(0, 11, g.ship_images_[0]);  // SHIPS #10, the enemy panel
        if (g.ships_bg_) g.backbuffer_image(0, 0, *g.ships_bg_);   // the enemy's picture DS:6B10
        g.bb_present();
        range_display();
        ship_panel(enemy);
    }

    void redraw() {
        // Screen.Redraw, state 8 (ovl23 0x586CC): LayoutSpace 015F:01B5,
        // MainView, the party's panel and weapons.
        g.clear_cells(1, 1, 0x26, 0x15);
        for (const Line& l : kSpaceLines) g.border_line(l.x1, l.y1, l.x2, l.y2, l.start, l.end);
        g.clear_cells(0, 0x17, 0x27, 0x18);
        main_view();
        ship_panel(party);
        weapon_list(party, false, -1);
        g.lp_used_ = false;
    }
};

struct SpaceFight : ShipScreen {
    int type = 0;                       // H.-1: a1 - 1
    bool done = false;                  // H.-6
    int range = 8;                      // H.-0x47 (signed byte)
    bool enemy_first = false;           // H.-0x48
    bool closed = false;                // H.-0x4E: range < 4 at a round's end
    bool took_command = false;          // H.-5
    bool enemy_rigged = false;          // H.-7
    std::optional<bool> fired;          // H.-8 (not set at set-up)
    std::optional<int> fired_dmg;       // H.-0xA
    std::optional<int> fired_slot;      // H.-0xB
    int best_slot = -1;                 // H.-0xC
    bool verbose = true;                // H.-0xD
    int rig_sys[2][6] = {};             // H.-0x25
    int rig_w[2][8] = {};               // H.-0x35
    int shots[2][8] = {};               // H.-0x45
    bool life_hurt = false;             // H.-0x49

    explicit SpaceFight(Game& game) : ShipScreen(game) {}

    int side(const Ship& s) const { return is_party(s) ? 0 : 1; }
    int wtype(const Ship& s, int w) const { return s.slot(w)[0] & 0x7F; }
    int wbyte(int type, int k) const { return t().ds8(0x22BA + 0x22 * static_cast<std::size_t>(type) + static_cast<std::size_t>(k)); }
    int damage(int type) const { return wbyte(type, 3); }
    std::string wname(int type) const { return t().ds_str(0x22C3 + 0x22 * static_cast<std::size_t>(type)); }
    std::string sysname(int i) const { return t().ds_str(0x10AA + 18 * static_cast<std::size_t>(i)); }
    int colour(const Ship& s) const { return is_party(s) ? 0x0B : 0x0E; }   // ovl3 +0x0000

    // --- helpers (ovl3 +0x004E .. +0x257F) -------------------------------------------

    std::string name(Character* c, const Ship& s) {
        // ShipName ovl3 +0x004E (0x33D8E): a character's name, the party
        // ship's, or the enemy's by RollDice(1,4) (0x33DE0).
        if (c) return c->name();
        if (is_party(s)) return s.name();
        const int r = g.roll_dice(1, 4);
        if (r == 1) return t()[Text::ScEnemyShip];
        if (r == 2)
            return t().ds_str(0x1224 + 14 * static_cast<std::size_t>(s.p[0x3A])) + " " +
                   t().ds_str(0x1104 + 18 * static_cast<std::size_t>(s.p[0x39]));
        return s.name();
    }

    bool check(int skill, int diff, const Ship& s, Character* c, int bonus) {
        // Check ovl3 +0x0130 (0x33E70): Percent of a skill value scaled by
        // the difficulty.
        int v = 0;
        if (c) v = g.skill_total(*c, skill);
        else if (skill == 3 || skill == 6) v = s.p[0x34];
        else if (skill == 0x33) v = s.p[0x33];
        v = (v + bonus) & 0xFF;
        switch (diff) {
            case 0: v = (v * 4) & 0xFF; break;
            case 1: v = (v * 2) & 0xFF; break;
            case 3: v /= 2; break;
            case 4: v /= 4; break;
            default: break;
        }
        const bool ok = g.percent(v);
        data_note("ship_check", skill, ok ? 1 : 0);    // tools only: --data-trace
        return ok;
    }

    int weapon_status(const Ship& s, int w, bool crew) const {
        // WeaponStatus ovl3 +0x031D (0x3405D): 0 = can fire.
        const std::uint8_t* n = s.slot(w);
        const int type = n[0] & 0x7F;
        if (type == 0) return 7;
        if (crew && n[0] < 0x80) return 1;
        if (!crew && n[0] >= 0x80) return 2;
        if (n[2] == 0) return 3;
        if (type > 2 && n[4] == 0) return 4;
        if (shots[side(s)][w] >= n[2]) return 5;
        if (s8(g.f4a1f_) > s8(wbyte(type, 0))) return 6;
        return 0;
    }

    int best_weapon(const Ship& s, bool crew) const {
        // BestWeapon ovl3 +0x0422 (0x34162): the first of the most damage.
        int best = -1, most = 0;
        for (int w = 0; w < 8; ++w)
            if (weapon_status(s, w, crew) == 0 && damage(wtype(s, w)) > most) {
                most = damage(wtype(s, w));
                best = w;
            }
        return best;
    }

    int to_hit(Character* c, int w, const Ship& target, const Ship& attacker) const {
        // ToHit ovl3 +0x0E82 (0x34BC2): the d20 to reach, at most 20.
        int v;
        if (c) {
            v = s8(g.text_.ds8(0x0CB9 + c->b(0x11))) + c->b(0x33);   // InitiativeAdj ovl23 +0x077D: DS:0CB9[DEX]
            if (c->b(0x28) == 1) v += c->b(0x29);
        } else {
            v = attacker.p[0x35];
        }
        if (is_party(attacker)) v += 3;
        v += s8(wbyte(wtype(attacker, w), 4));
        if (s8(g.f4a1f_) > 4) v -= 5;
        else if (s8(g.f4a1f_) > 2) v -= 2;
        if (attacker.cur(2) == 0) v -= 5;
        if (target.cur(2) == 0) v += 3;
        v += target.p[0x38];
        int r = s8((60 - target.p[0x32] - v) & 0xFF);
        return r > 20 ? 20 : r;
    }

    int strength(const Ship& s) const {
        // Strength ovl3 +0x242C (0x3616C).
        int st = s.cur(0) + (can_maneuver(s) ? (s.cur(2) + s.cur(4) + s.cur(5)) / 3 : 0);
        int f = 0;
        for (int w = 0; w < 8; ++w)
            if (s.slot(w)[0] != 0) f += s.slot(w)[2] * damage(wtype(s, w));
        f &= 0xFFFF;
        return f == 0 ? 0 : st + f;
    }

    bool needs_reload(const Ship& s, int w) const {
        const std::uint8_t* n = s.slot(w);
        return (n[0] & 0x7F) > 2 && n[5] > 0 && n[4] == 0;   // ovl3 +0x1FC6
    }
    // AnyNeedsReload ovl3 +0x201B (0x35D5B): any of the 8 weapon slots needs Reload (+0x1FC6).
    bool any_needs_reload(const Ship& s) const {
        for (int w = 0; w < 8; ++w)
            if (needs_reload(s, w)) return true;
        return false;
    }
    bool weapon_riggable(const Ship& s) const {
        // ovl3 +0x140D (0x3514D).
        for (int w = 0; w < 8; ++w)
            if (s.slot(w)[2] < s.slot(w)[1] && rig_w[side(s)][w] < s.slot(w)[1]) return true;
        return false;
    }
    bool can_jury_rig(Character* c, const Ship& s) const {
        // CanJuryRig ovl3 +0x230A (0x3604A).
        if (side(s) == 0 && !(c && s8(g.skill_total(*c, 6)) > 0)) return false;
        for (int i = 0; i < 6; ++i)
            if (i != 4 && s.cur(i) < s.max(i) && rig_sys[side(s)][i] == 0) return true;
        return weapon_riggable(s);
    }
    int enemy_firepower() const {
        // ovl3 +0x24EF (0x3622F): a byte.
        int f = 0;
        for (int w = 0; w < 8; ++w)
            if (weapon_status(enemy, w, false) == 0) f = (f + ((damage(wtype(enemy, w)) * enemy.slot(w)[2]) & 0xFF)) & 0xFF;
        return f;
    }
    bool shoot_better_than_ram() {
        // ovl3 +0x257F (0x362BF).
        const int bw = best_weapon(enemy, false);
        if (bw < 0) return false;
        const int es = speed(enemy), ps = speed(party);
        int total;
        if (g.pilot_) {
            total = g.skill_total(*g.pilot_, 51);
        } else {
            // 0x3631D: SkillTotal(DS:4A18 = nil, 51) reads the record's bytes
            // at 0000:0000: the skill +0x7F, +0xD5, and (SkillBonus: skill
            // 51 has no prerequisite) DEX +0x11, the reference machine's
            // 0xC0, 0xCA, 0x00 (D-0042).
            const int pts = g.low_.read(0x7F, "D-0042", "ShootBetterThanRam (ovl3 +0x257F, 0x3631D): no pilot; "
                                                        "SkillTotal reads a nil record");
            total = pts && g.low_.peek(0xD5) ? (pts + g.low_.peek(0x11)) & 0xFF : 0;
        }
        const int p = s8(total) + (ps - es) * 10;
        if (p >= 90) return true;
        // 0x36337: the firepower byte sign-extended (cbtw) before the idiv,
        // so 128 and more count as negative (session 50: met with a Warrior
        // as the pilot, whose Pilot Rocket total 0 leaves p below 90).
        const std::int32_t l = static_cast<std::int16_t>((21 - to_hit(nullptr, bw, party, enemy)) * 5 * (s8(enemy_firepower()) / 10));
        const std::int32_t r = static_cast<std::int32_t>(static_cast<std::uint16_t>(enemy.w(0x10)) / 10) * (100 - p);
        return l > r;
    }

    // --- output -----------------------------------------------------------------------

    void say(int fg, const std::string& s) { g.print_line(fg, s); }   // Text.PrintLine 0653:118A

    void explosion(int size) {
        // Explosion ovl3 +0x04C0 (0x34200), over the enemy's picture.
        const int a = g.roll_dice(1, 8) - 1;
        int x = a + 9, y = 7 - a / 2;
        x += g.roll_dice(1, 3) - 2;
        y += g.roll_dice(1, 3) - 2;
        const int k = size == 1 || size == 2 ? 2 : size == 6 ? 1 : 3;
        auto ent = [&](int e, int f) { return g.text_.ds8(0x23EC + 6 * static_cast<std::size_t>(e) + static_cast<std::size_t>(f)); };
        x -= ent(k, 1) / 2;
        y -= ent(k, 0) / 16;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        const int over = ent(k, 0) / 8 + y - 11;
        if (over > 0) y -= over;
        for (int i = 4; i >= 0; --i) {
            const int e = 3 * i + k;
            const Image& im = g.ship_images_[static_cast<std::size_t>(e)];
            if (im.frames.empty()) continue;
            // DrawImage(…, flags 5): the backbuffer under it saved into
            // DS:3C7E, the frame drawn masked; Present; Delay(60); the saved
            // part drawn back (no Present until the next frame).
            const int px = 8 * (x + s8(ent(e, 4))), py = 8 * (y + s8(ent(e, 5)));
            Bytes under(static_cast<std::size_t>(im.width * im.height), 0);
            for (int r = 0; r < im.height; ++r)
                for (int c = 0; c < im.width; ++c) {
                    const int X = px + c, Y = py + r;
                    if (X >= 0 && X < 320 && Y >= 0 && Y < 200)
                        under[static_cast<std::size_t>(r * im.width + c)] = g.bb_[static_cast<std::size_t>(Y * 320 + X)];
                }
            g.bb_draw(im.frames[0], im.width, im.height, px, py, true);
            g.bb_present();
            g.pause_ms(60);
            g.bb_draw(under, im.width, im.height, px, py, false);
        }
        g.bb_present();
    }

    // --- the fight's actions ---------------------------------------------------------

    void crew_hit(int chance, bool all) {
        // CrewHit ovl3 +0x0762 (0x344A2).
        chance &= 0xFF;
        for (auto& m : g.party_) {
            Character& p = *m;
            if (p.b(0xD4) != 1) continue;
            if (g.percent(chance) || all) {
                if (g.save(p, 2, 0) && !all && chance < 100) {
                    say(0x0B, p.name() + t()[Text::ScLeapsClear]);
                    continue;
                }
                const int dmg = all ? (g.roll_dice(5, 20) + p.b(0x45)) & 0xFF : g.roll_dice(1, 9) + 1;
                g.take_damage(p, dmg);
                say(0x0B, p.name() + t()[Text::ScTakes] + std::to_string(dmg) + t()[Text::ScPointsDmg]);
                if (!all) {
                    if (p.b(0xD4) == 3) say(0x0C, p.name() + t()[Text::ScIsDead]);
                    else if (p.b(0xD4) == 4) say(0x0E, p.name() + t()[Text::ScIsDying]);
                    else if (p.b(0xD4) == 5) say(0x0E, p.name() + t()[Text::ScUnconscious]);
                    else if (p.w(0xE3) < 10) say(0x0E, p.name() + t()[Text::ScInjured]);
                }
                if (&p == g.pilot_ && p.b(0xD4) != 1) {
                    p.b(0xEA) = 0;
                    g.pilot_ = nullptr;
                    took_command = false;
                }
            }
        }
    }

    void enemy_fire_report() {
        // EnemyFireReport ovl3 +0x09B3 (0x346F3).
        if (!fired || !fired_dmg || !fired_slot) {
            // Before the enemy's first turn H.-8, H.-0xA and H.-0xB were never
            // written (a party system destroyed in the party's own first
            // turn: a failed boost): the reference machine's stale values
            // are fired, 0 damage, slot 0 (D-0043, run s23_boost).
            combat::diag("D-0043", "EnemyFireReport (ovl3 +0x09B3, 0x346F3) before the enemy's first turn reads "
                                   "unwritten locals; the reference machine's values (fired, 0, slot 0) are used");
            if (!fired) fired = true;
            if (!fired_dmg) fired_dmg = 0;
            if (!fired_slot) fired_slot = 0;
        }
        if (!*fired) return;
        // The slot is a signed byte: before the enemy's first shot of a turn
        // it is -1, and the type comes from enemy+0x36 (0x3C - 6), as in the
        // original (deterministic).
        std::string s = t()[Text::ScFires] + wname(wtype(enemy, s8(*fired_slot))) + t()[Text::ScSAnd] +
                        (*fired_dmg > 0 ? t()[Text::ScHitsFor] + std::to_string(*fired_dmg) + t()[Text::ScPointsDmg2]
                                        : t()[Text::ScMisses]);
        say(0x0E, name(nullptr, enemy) + s);
        crew_hit(*fired_dmg / 5, false);
        fired_dmg = 0;
        fired = false;
    }

    void damage_system(int amount, int sys, Ship& s) {
        // DamageSystem ovl3 +0x0B2D (0x3486D).
        bool crew_kill = false;
        if (sys == 6) {
            int n = 0;
            for (int w = 0; w < 8; ++w) n = (n + s.slot(w)[2]) & 0xFF;
            if (s8(n) <= 0) return;
            int r = g.roll_dice(1, s8(n));
            for (int w = 0; w < 8 && r > 0; ++w) {
                r -= s.slot(w)[2];
                if (r < 1) {
                    if (is_party(s)) {
                        enemy_fire_report();
                        weapon_list(party, false, w);
                    }
                    say(colour(s), wname(wtype(s, w)) + t()[Text::ScDestroyedOn] + name(nullptr, s) + t()[Text::ScBang]);
                    s.slot(w)[2] = static_cast<std::uint8_t>(s.slot(w)[2] - 1);
                    if (is_party(s)) weapon_list(party, false, -1);
                }
            }
        } else {
            if (s.cur(sys) == 0) sys = 0;
            if (amount > s.cur(sys)) amount = s.cur(sys);
            if (amount == 0) return;
            system_bar(s, sys, true);
            s.set_cur(sys, s.cur(sys) - amount);
            system_bar(s, sys, false);
            if (s.cur(sys) == 0) {
                if (is_party(s)) enemy_fire_report();
                say(colour(s), sysname(sys) + t()[Text::ScDestroyed]);
                if (sys == 0) {
                    done = true;
                    if (!is_party(s)) {
                        g.v0(0x4BBA) = 0;
                        g.sound(t().ds16(0x21F2));             // 0x34B00: the enemy's hull (11)
                        for (int i = 0; i < 6; ++i) explosion(6);
                    } else {
                        g.v0(0x4BBA) = 1;
                        crew_kill = true;
                    }
                }
                if (sys == 3 && is_party(s) && !life_hurt) {
                    crew_hit(100, false);
                    amount = 0;
                    life_hurt = true;
                }
                if (sys == 1 && is_party(s)) main_view();
            }
        }
        if (crew_kill) crew_hit(100, true);
        if (g.percent(amount >> 1) && s.w(0x30) > 0) s.setw(0x30, s.w(0x30) - 1);
    }

    void hit(int sys, int type, Ship& target) {
        // Hit ovl3 +0x0F7D (0x34CBD).
        const int dmg = damage(type);
        if (!is_party(target)) explosion(type);
        if (sys == 7) {
            int r = g.roll_dice(1, 100);
            for (int i = 0; i <= 6; ++i) {
                r -= g.text_.ds8(0x00BA + static_cast<std::size_t>(i));
                if (r <= 0) {
                    damage_system(dmg, i, target);
                    break;
                }
            }
        } else {
            damage_system(dmg, sys, target);
        }
    }

    void fire(int sys, Character* c, int w, Ship& target, Ship& attacker) {
        // Fire ovl3 +0x105C (0x34D9C).
        const int type = wtype(attacker, w);
        if (verbose) {
            if (g.lp_used_) g.pause_continue();
            g.line_home();
        }
        fired = true;
        bool hitp = false;
        std::string s = t()[Text::ScMisses2];
        if (sys == 7 || g.percent(50)) {
            const int need = to_hit(c, w, target, attacker);
            if (g.roll_dice(1, 20) >= need) {
                hitp = true;
                const int dmg = damage(type);
                s = t()[Text::ScHitsFor2] + std::to_string(dmg) + t()[Text::ScPointsDmg3];
                fired_dmg = (fired_dmg.value_or(0) + dmg) & 0xFFFF;
            }
        }
        // 0x34EB3: the weapon's sound on the party's turn (H.-0xD), or the
        // enemy's first shot of its turn (H.-0xB still 0xFF): lasers (19),
        // the K-cannon (16), any other (11).
        if (verbose || (fired_slot && (*fired_slot & 0xFF) == 0xFF))
            g.sound(t().ds16(type == 1 || type == 2 ? 0x2204 : type == 6 ? 0x2200 : 0x2202));
        if (verbose) say(colour(attacker), name(c, attacker) + t()[Text::ScAttacksWith] + wname(type) + t()[Text::ScAnd] + s);
        if (hitp) hit(sys, type, target);
        if (type > 2) attacker.slot(w)[4] = static_cast<std::uint8_t>(attacker.slot(w)[4] - 1);
        shots[side(attacker)][w] += 1;
    }

    void speed_boost(Character* c, Ship& s) {
        // SpeedBoost ovl3 +0x12BA (0x34FFA).
        std::string m = name(c, s) + t()[Text::ScBoost];
        int dmg = 0;
        if (check(3, 3, s, c, 0)) {
            m += t()[Text::ScSucceeds];
            (is_party(s) ? g.f4a22_ : g.f4a23_) += 1;
        } else {
            m += t()[Text::ScFails];
            dmg = g.roll_dice(1, 100);
        }
        say(colour(s), m);
        if (dmg > 0) damage_system(dmg, 5, s);
    }

    void close(Ship& s) {
        // Close ovl3 +0x1EDE (0x35C1E).
        say(colour(s), name(nullptr, s) + t()[Text::ScClosing]);
        range = s8(range - speed(s));
        if (!is_party(s)) g.f4a20_ = 0;
        range_display();
    }

    void withdraw(int n, Ship& s) {
        // The retreat, ovl3 +0x1F5E (0x35C9E).
        say(colour(s), name(nullptr, s) + t()[Text::ScRetreating]);
        range = s8(range + n);
        if (!is_party(s)) g.f4a20_ = 1;
        range_display();
    }

    bool dodge(Ship& victim, Ship& rammer, Character* pilot) {
        // Dodge ovl3 +0x1C67 (0x359A7).
        if (speed(victim) <= 0) return false;
        if (!is_party(victim)) {
            if (!(victim.cur(0) < rammer.cur(0))) return false;
        } else if (g.yes_no(t()[Text::ScTryDodge], 1) != 'Y') {
            return false;
        }
        const int rs = speed(rammer);
        const int bonus = ((speed(victim) - rs) * 10) & 0xFF;
        return check(0x33, 2, victim, pilot, bonus);
    }

    void ram(Character* c, Ship& victim, Ship& rammer) {
        // Ram ovl3 +0x1D97 (0x35AD7).
        Character* pilot = is_party(victim) ? g.pilot_ : nullptr;
        say(colour(rammer), name(c, rammer) + t()[Text::ScAttemptsRam]);
        if (dodge(victim, rammer, pilot)) {
            const std::string nm = pilot ? t()[Text::ScYourShip] : name(nullptr, victim);
            say(colour(victim), nm + t()[Text::ScDodges]);
        } else {
            say(colour(rammer), t()[Text::ScRamOk]);
            damage_system(victim.w(0x10), 0, rammer);
            damage_system(rammer.w(0x10), 0, victim);
        }
    }

    bool reload(Character* c, Ship& s) {
        // Reload ovl3 +0x20A8 (0x35DE8).
        int sel = -1;
        if (is_party(s)) {
            // 0x35E0A: the menu of the weapons to reload; a key matching a
            // slot's type letter (DS:1037 + 9 type, every slot, the last
            // wins) picks it, even one that needs no reload (quirk), until
            // 0, Esc or 'Q' (CS:207A).
            say(0x0D, t()[Text::ScSelectReload]);
            std::string items;
            for (int w = 0; w < 8; ++w)
                if (needs_reload(s, w)) items += wshort(wtype(s, w)) + " ";
            items += t()[Text::ScQuit];
            int k;
            do {
                k = g.menu_screen("", items, true).key;
                if (k != 0)
                    for (int w = 0; w < 8; ++w)
                        if (g.text_.ds8(0x1037 + 9 * static_cast<std::size_t>(wtype(s, w))) == k) {
                            sel = w;
                            k = 'Q';
                        }
            } while (k != 0 && k != kEsc && k != 'Q');
        } else {
            for (int w = 0; w < 8; ++w)
                if (needs_reload(s, w)) sel = w;
        }
        if (sel < 0) return false;
        std::uint8_t* n = s.slot(sel);
        n[5] = static_cast<std::uint8_t>(n[5] - 1);
        n[4] = static_cast<std::uint8_t>(n[4] + wbyte(n[0] & 0x7F, 2));
        say(colour(s), name(c, s) + t()[Text::ScReloads] + wname(n[0] & 0x7F) + t()[Text::ScDot]);
        return true;
    }

    bool jury_rig_party(Character& c) {
        // JuryRig ovl3 +0x150F (0x3524F..0x3598A), a member's path (session
        // 33; reading analysis/re/juryrig.md). The systems' menu: Weap when
        // a weapon can be rigged, each damaged system not yet rigged (Fuel
        // never), Quit; until a key of CS:14BD (any other shows it again).
        // Weap's menu: each damaged mount's short name, Quit; a key naming
        // a mount's type (its hotkey byte DS:1037 + 9 t; the last match
        // wins) picks it, 0 or Q leaves, anything else asks again. An
        // extended key comes back as its scan code (Up 'H', Del 'S', F9
        // 'C', Shift-F4 'W': kept). Then SkillCheck(6, 2): success restores
        // 2d10 (to the maximum) or a mount; the turn is used either way.
        Ship& s = party;
        std::string items;
        if (weapon_riggable(s)) items += t()[Text::ScJrWeap];
        for (int i = 0; i <= 5; ++i)
            if (s.cur(i) < s.max(i) && rig_sys[0][i] == 0 && i != 4) items += t().ds_str(0x1087 + 5 * static_cast<std::size_t>(i)) + " ";
        items += t()[Text::ScJrQuit];
        auto in14bd = [](int k) {
            return k == 0 || k == 0x1B || k == 'C' || k == 'E' || k == 'F' || k == 'H' || k == 'L' || k == 'Q' ||
                   k == 'S' || k == 'W';
        };
        int key = 0, sel = -1;
        do {
            key = g.menu_screen("", items, false).key;
            if (key == 'W') {
                std::string wi;
                for (int w = 0; w < 8; ++w) {
                    const std::uint8_t* n = s.slot(w);
                    if (n[2] < n[1] && rig_w[0][w] < n[1]) wi += wshort(wtype(s, w)) + " ";
                }
                wi += t()[Text::ScJrQuit];
                do {
                    const int k = g.menu_screen("", wi, false).key;
                    key = (k == 'Q' || k == 0) ? k : 'x';
                    for (int w = 0; w < 8; ++w)
                        if (g.text_.ds8(0x1037 + 9 * static_cast<std::size_t>(wtype(s, w))) == k) {
                            key = 'W';
                            sel = w;
                        }
                } while (key != 0 && key != 'Q' && key != 'W');
            }
        } while (!in14bd(key));
        g.clear_row24();
        int sys;
        switch (key) {
            case 'H': sys = 0; break;
            case 'S': sys = 1; break;
            case 'C': sys = 2; break;
            case 'L': sys = 3; break;
            case 'F': sys = 4; break;
            case 'E': sys = 5; break;
            case 'W': sys = 6; break;
            default: return false;
        }
        const std::string nm = sys == 6 ? wname(wtype(s, sel)) : sysname(sys);
        if (check(6, 2, s, &c, 0)) {
            say(colour(s), name(&c, s) + t()[Text::ScJrAttempts] + nm + t()[Text::ScJrSucceeds]);
            if (sys == 6) {
                rig_w[0][sel] = (rig_w[0][sel] + 1) & 0xFF;
                s.slot(sel)[2] = static_cast<std::uint8_t>(s.slot(sel)[2] + 1);
                weapon_list(s, false, -1);
            } else {
                int a = g.roll_dice(2, 10);
                if (s.cur(sys) + a > s.max(sys)) a = (s.max(sys) - s.cur(sys)) & 0xFF;
                s.set_cur(sys, s.cur(sys) + a);
                rig_sys[0][sys] += a;
                system_bar(s, sys, false);
                if (sys == 1 && s.cur(1) == a) g.redraw_screen();
            }
        } else {
            say(colour(s), name(&c, s) + t()[Text::ScJrAttempts] + nm + t()[Text::ScJrFails]);
        }
        return true;
    }

    bool jury_rig_enemy() {
        // JuryRig ovl3 +0x150F (0x3524F), the enemy's path.
        int sys = 7, best = 0, sel = -1;
        if (weapon_riggable(enemy)) {
            // 0x355AA..0x35639: a damaged slot unless BP-2 = 4; BP-2 isn't
            // written on this path (stale stack): not 4 on the reference
            // machine (D-0041).
            combat::diag("D-0041", "JuryRig (ovl3 +0x150F, 0x355E8): the enemy's weapon choice tests an "
                                   "uninitialised local; the reference machine's outcome (not 4) is used");
            for (int w = 0; w < 8; ++w) {
                const std::uint8_t* n = enemy.slot(w);
                if (n[2] < n[1] && damage(n[0] & 0x7F) > best) {
                    sel = w;
                    best = damage(n[0] & 0x7F);
                }
            }
            if (sel >= 0) sys = 6;
        }
        if (sys == 7)
        for (int i = 0; i <= 5; ++i)
            if (enemy.cur(i) != enemy.max(i) && rig_sys[1][i] == 0) {
                int sc = s8((10 - enemy.cur(i) * 10 / enemy.max(i)) & 0xFF);
                if (enemy.cur(i) == 0) sc += 5;
                sc *= 2;
                if (sc > best) {
                    best = sc;
                    sys = i;
                }
            }
        if (sys == 7) return false;   // BP-1 uninitialised: the caller's loop does nothing more either way
        if (check(6, 2, enemy, nullptr, 0)) {
            enemy_rigged = true;
            if (sys == 6) {
                rig_w[1][sel] += 1;
                enemy.slot(sel)[2] = static_cast<std::uint8_t>(enemy.slot(sel)[2] + 1);
                return true;
            }
            int a = g.roll_dice(2, 10);
            if (enemy.cur(sys) + a > enemy.max(sys)) a = (enemy.max(sys) - enemy.cur(sys)) & 0xFF;
            enemy.set_cur(sys, enemy.cur(sys) + a);
            rig_sys[1][sys] += a;
            system_bar(enemy, sys, false);
        }
        return true;
    }

    // --- the turns ---------------------------------------------------------------------

    void enemy_turn() {
        // EnemyTurn ovl3 +0x269F (0x363DF).
        if (done) return;
        const int ps = strength(party), es = strength(enemy);
        const int mood = es * 2 < ps ? 0 : es > ps ? 2 : 1;
        bool boost = false;
        if (can_maneuver(enemy)) {
            if (s8(g.f4a1f_) <= 1 && !shoot_better_than_ram() && enemy.cur(0) > party.cur(0) && enemy.cur(0) > party.w(0x10))
                ram(nullptr, party, enemy);
            else if (mood == 0 && closed)
                withdraw(speed(enemy), enemy);
            else if (mood == 1 && s8(g.f4a1f_) <= 2)
                withdraw(std::min(speed(enemy), 3), enemy);
            else if (mood == 2 || s8(g.f4a1f_) >= 5)
                close(enemy);
            if (mood == 0 && speed(enemy) < speed(party) && g.percent(enemy.p[0x34]) && closed) boost = true;
        }
        fired_slot = 0xFF;
        fired_dmg = 0;
        verbose = false;
        fired = false;
        do {
            best_slot = best_weapon(enemy, false);
            if (s8(*fired_slot) >= 0 && s8(*fired_slot) != best_slot) enemy_fire_report();
            if (best_slot >= 0) fire(7, nullptr, best_slot, party, enemy);
            fired_slot = best_slot & 0xFF;
        } while (best_slot >= 0 && !done);
        const int crew = enemy.w(0x30);
        int actions = s8(((crew + 9) / 10) & 0xFF);
        int gunners = s8((crew - actions) & 0xFF);
        while (gunners > 0 && !done) {
            --gunners;
            if (any_needs_reload(enemy)) {
                reload(nullptr, enemy);
            } else {
                best_slot = best_weapon(enemy, true);
                if (s8(*fired_slot) >= 0 && s8(*fired_slot) != best_slot) enemy_fire_report();
                if (best_slot >= 0) fire(7, nullptr, best_slot, party, enemy);
                else {
                    // 0x3672E: a byte add into the byte BP-3, which the
                    // loop below tests signed (0x3675F): a crew of 128 or
                    // more with no gun in range (SHIPS #42, crew 135, at
                    // range 5..8) leaves it negative, and the enemy does
                    // nothing more this turn (session 51's audit).
                    actions = s8((actions + gunners) & 0xFF);
                    gunners = 0;
                }
                fired_slot = best_slot & 0xFF;
            }
        }
        verbose = true;
        if (s8(*fired_slot) >= 0) enemy_fire_report();
        while (actions > 0 && !done) {
            --actions;
            if (boost) {
                speed_boost(nullptr, enemy);
                boost = false;
            } else if (!jury_rig_enemy()) {
                actions = 0;
            }
        }
        if (enemy_rigged) say(0x0E, t()[Text::ScEnemyRigs]);
        enemy_rigged = false;
    }

    std::string build_menu(Character& c, bool can_move) {
        // BuildMenu ovl3 +0x2B37 (0x36877).
        const bool is_pilot = &c == g.pilot_;
        if (!g.lp_used_ || g.lp_row_ > 0x15 || is_pilot) {
            if (g.lp_used_) g.pause_continue();
            g.line_home();
        }
        if (is_pilot) {
            say(0x0D, c.name() + t()[Text::ScIsPilot]);
            say(0, "");
        }
        while (g.lp_row_ < 0x15) say(0, "");
        say(0x0D, t()[Text::ScWhatDoes] + c.name() + t()[Text::ScWantToDo]);
        g.lp_used_ = false;
        std::string items;
        if (best_weapon(party, !is_pilot) >= 0) items += t()[Text::ScFireTarget];
        if (!is_pilot) {
            if (any_needs_reload(party)) items += t()[Text::ScLoad];
            if (can_jury_rig(&c, party)) items += t()[Text::ScJuryrig];
        }
        items += t()[Text::ScQuitView];
        if (is_pilot) {
            if (can_move) {
                if (s8(g.f4a1f_) > 1) items += t()[Text::ScClose];
                items += t()[Text::ScWdr];
                if (s8(g.f4a1f_) <= 1) {
                    items += t()[Text::ScRam];
                    if (speed(enemy) == 0) items += t()[Text::ScBoard];
                }
            }
        } else {
            if (!took_command) items += t()[Text::ScCommand];
            if (g.can_aid(c)) items += t()[Text::ScAid];
            if (s8(g.skill_total(c, 3)) > 0 && party.cur(5) > 0 && g.f4a22_ < 2) items += t()[Text::ScBoostEngines];
            if (party.cur(1) > 0 && c.b(0x56) > 0) items += t()[Text::ScSensors];
        }
        return items;
    }

    void party_turn() {
        // PartyTurn ovl3 +0x305D (0x36D9D).
        bool can_move = can_maneuver(party);
        took_command = false;
        for (std::size_t i = 0; i < g.party_.size() && !done; ++i) {
            Character& c = *g.party_[i];
            if (c.b(0xD4) != 1) continue;
            bool acted = false;
            do {
                g.clear_cells(0, 0x17, 0x27, 0x18);
                const std::string items = build_menu(c, can_move);
                const Game::MenuKey mk = g.menu_screen("", items, true);
                g.clear_cells(0, 0x17, 0x27, 0x18);
                const bool is_pilot = &c == g.pilot_;
                switch (mk.key) {
                    case 'A':
                        g.aid_cmd(c);
                        acted = true;
                        break;
                    case 'B':
                        if (is_pilot) {
                            g.v0(0x4D7C) = 1;
                            g.v0(0x4BBA) = 0;
                            done = true;
                            say(0x0B, t()[Text::ScBoardMsg]);
                        } else {
                            speed_boost(&c, party);
                        }
                        acted = true;
                        break;
                    case 'C':
                        if (is_pilot) {
                            close(party);
                            can_move = false;
                        } else {
                            say(0x0B, c.name() + t()[Text::ScTakesOver]);
                            g.pilot_ = &c;
                            c.b(0xEA) = 1;
                            took_command = true;
                        }
                        break;
                    case 'F':
                    case 'T': {
                        int sys = 7, key = mk.key;
                        if (mk.key == 'T') {
                            std::string its;
                            for (int k = 0; k <= 6; ++k) its += t().ds_str(0x1087 + 5 * static_cast<std::size_t>(k)) + " ";
                            say(0x0D, t()[Text::ScTargetWhich]);
                            const Game::MenuKey tk = g.menu_screen("", its, false);
                            for (int k = 0; k <= 6; ++k)
                                if (g.text_.ds8(0x1088 + 5 * static_cast<std::size_t>(k)) == tk.key) sys = k;
                            key = tk.key;
                        }
                        // 0x3701C: only 0x1B leaves without firing, which
                        // MenuScreen never returns (Esc comes back as 0):
                        // Esc, like any key naming no system, fires with
                        // the default 7 (0x36F45), as Fire does (session
                        // 33, run s33_tesc; route c_space_tesc).
                        if (mk.key == 'T' && key == 0x1B) break;
                        g.line_home();
                        int w;
                        do {
                            w = best_weapon(party, &c != g.pilot_);
                            best_slot = w;
                            if (w >= 0) fire(sys, &c, w, enemy, party);
                        } while (w >= 0 && !done && &c == g.pilot_);
                        acted = true;
                        break;
                    }
                    case 'J':                                  // 0x370BD
                        acted = jury_rig_party(c);
                        break;
                    case 'L':
                        acted = reload(&c, party);
                        break;
                    case 'Q':
                        if (!mk.ext) acted = true;
                        break;
                    case 'R':
                        if (g.yes_no(t()[Text::ScRamQ] + name(nullptr, enemy) + t()[Text::ScQ], 2) == 'Y') {
                            ram(g.pilot_, enemy, party);
                            acted = true;
                        }
                        break;
                    case 'S': {
                        // 0x37188..0x37232: SkillCheck(skill 0x0A, difficulty
                        // 1); the line; the enemy's status, its numbers only
                        // on a success; the wait; the screen again.
                        combat::Rng rng(g.seed_);
                        const int r = combat::skill_check({g.skill_total(c, 0x0A)}, 1, rng).result;
                        g.seed_ = rng.seed();
                        data_note("skill", 0x0A, r);
                        const bool ok = r >= 2;
                        say(0x0B, c.name() + t()[Text::ScSensorSpace] + t()[ok ? Text::ScSensorOk : Text::ScSensorFail] +
                                      t()[Text::ScSensorTail]);
                        ship_status(enemy, ok);
                        g.pause_continue();                    // Text.Pause 0653:1291
                        g.redraw_screen();
                        acted = true;
                        break;
                    }
                    case 'V': {
                        // 0x3723C..0x372CB: the menu CS:302E (remap 0) until
                        // 0, Esc, 'C' or 'S' (the set CS:303D; DS:6B49, the
                        // extended flag untested). Character: CurChar := the
                        // member, ViewCharacter with the turn's flag, which is
                        // then cleared; Ship: the party's status. Neither ends
                        // the turn.
                        int k;
                        do {
                            k = g.menu_screen("", t()[Text::ScViewItems], false).key;
                            if (k == 'C') {
                                g.cur_ = &c;                   // DS:4EC1
                                std::uint8_t flag = 0;
                                g.view_character(flag);        // ovl18 +0x0F75
                                g.redraw_screen();
                                acted = false;
                            } else if (k == 'S') {
                                ship_status(party, true);
                                g.pause_continue();
                                g.redraw_screen();
                            }
                        } while (k != 0 && k != kEsc && k != 'C' && k != 'S');
                        break;
                    }
                    case 'W':
                        withdraw(speed(party), party);
                        can_move = false;
                        break;
                    default:
                        break;
                }
            } while (!acted && !done);
        }
    }

    // --- the handler ------------------------------------------------------------------

    void run(int a1, int a3, int a4) {
        type = (a1 - 1) & 0xFF;
        static constexpr int kXp[5] = {300, 3000, 12600, 2500, 4600};   // 0x37380..0x373B7
        if (type <= 4) g.xp_5ea9_ = static_cast<std::uint16_t>(kXp[type]);
        g.v0(0x4D7C) = 0;
        const int saved_mode = g.mode_;
        g.mode_ = 8;
        g.load_ship_images();                          // ovl24 +0x016A
        g.pilot_ = nullptr;                            // DS:4A18
        for (auto& m : g.party_) {
            Character& p = *m;
            if (p.b(0xEA) == 0) continue;
            if (g.pilot_) p.b(0xEA) = 0;
            else if (p.b(0xD5) != 0) g.pilot_ = &p;
        }
        g.f4a20_ = 0;
        g.enemy_ship_.assign(0x6C, 0);                 // GetMem(0x6C)
        enemy = Ship{g.enemy_ship_.data()};
        g.load_ship(enemy.p, type);                    // ovl17 +0x26F3
        g.set_dac(0x20, 0xE0, g.pal_908b_);
        enemy.p[0x33] = static_cast<std::uint8_t>(a3);
        enemy.p[0x35] = static_cast<std::uint8_t>(60 - a4);
        g.f4a22_ = 0;
        g.f4a23_ = 0;
        for (auto& m : g.party_) m->cb.assign(0x12, 0);   // GetMem(0x12) per member
        done = false;
        life_hurt = false;
        g.f4a1f_ = 8;
        range = 8;
        g.redraw_screen();                             // Screen.Redraw (0x3752D), state 8
        g.set_window(1, 0x13, 0x26, 0x15, 0);
        const int r1 = g.roll_dice(1, 10), r2 = g.roll_dice(1, 10);   // Initiative ovl3 +0x02C7
        enemy_first = s8((r2 - r1 + party.p[0x37] - enemy.p[0x37]) & 0xFF) > 0;
        do {
            for (auto& s : shots)
                for (int& v : s) v = 0;
            if (!enemy_first) party_turn();
            enemy_turn();
            if (enemy_first) party_turn();
            if (!done) {
                if (s8(range) < 0) range = 0;
                else if (s8(range) < 4) closed = true;
                else if (s8(range) > 8) {
                    say(0x0E, t()[Text::ScRecedes]);
                    done = true;
                    g.xp_5ea9_ = 0;
                    g.v0(0x4BBA) = 1;
                    g.pause_continue();
                }
            }
            g.dying_round();                           // Combat.Dying ovl23 +0x30F3
            g.f4a1f_ = static_cast<std::uint8_t>(range);
            range_display();
        } while (!done);
        // RemoveJuryRigs ovl3 +0x01F0 (0x33F30), the party's only.
        for (int i = 0; i < 6; ++i)
            if (rig_sys[0][i] < party.cur(i)) party.set_cur(i, party.cur(i) - rig_sys[0][i]);
        for (int w = 0; w < 8; ++w) {
            std::uint8_t* n = party.slot(w);
            n[2] = rig_w[0][w] > n[2] ? 0 : static_cast<std::uint8_t>(n[2] - rig_w[0][w]);
        }
        g.v0(0x4B80) = 0;
        if (g.v0(0x4D7C) == 0) {
            g.f82f2_ = 1;
            g.f82f1_ = 1;
        }
        if (g.xp_5ea9_ == 0) {
            for (std::uint32_t a = 0x4D6C; a <= 0x4D77; ++a) g.v0(a) = 0;
            for (std::uint32_t a = 0x4D7D; a <= 0x4D82; ++a) g.v0(a) = 0;
        } else {
            for (int k = 0; k < 12; ++k) g.v0(0x4D6C + static_cast<std::uint32_t>(k)) = enemy.p[0x16 + k];
            g.v0(0x4D78) = enemy.p[0x39];
            g.v0(0x4D7A) = enemy.p[0x31];
            g.v0(0x4D79) = enemy.p[0x30];
            g.v0(0x4D7B) = enemy.p[0x35];
            int n = 0;
            for (int w = 0; w < 8; ++w) n = (n + enemy.slot(w)[2]) & 0xFF;
            g.v0(0x4D81) = static_cast<std::uint8_t>(n);
            g.v0(0x4D82) = 0;
        }
        g.ships_bg_.reset();                           // FreeImage DS:6B10
        g.big_pic_.reset();
        g.free_ship_images();                          // ovl24 +0x00CC
        g.after_combat();                              // ovl5 +0x20D8
        g.enemy_ship_.clear();                         // FreeMem; DS:4A14 := nil
        g.mode_ = saved_mode;
        g.redraw_screen();
    }
};

// --- Game: images, the ship's record, the opcode, the repairs -------------------------------

void Game::load_ship_images() {
    // ovl24 +0x016A (0x5984A): the travel images freed, the event picture
    // freed, DS:6B10 freed, then SHIPS #10..#38 (DS:4A25), row 24 cleared.
    if (f4a25_) return;
    travel_free();
    free_picture();                                    // ovl27 +0x0B04
    ships_bg_.reset();
    big_pic_.reset();                                  // the same DS:6B10
    fill_rect(0x15, 0x18, 0x27, 0x18, 0);              // Text.LoadingLine
    draw_string(0, 0x18, 10, 0, text_[Text::Loading]);
    f4a25_ = 1;
    ship_images_.assign(29, Image{});
    for (int k = 0; k <= 0x1C; ++k) {
        auto raw = dax_entry("SHIPS.DAX", 10 + k);
        if (!raw) throw Fatal(text_[Text::WallUnable] + std::to_string(10 + k));
        ship_images_[static_cast<std::size_t>(k)] = decode_generic(*raw, "SHIPS.DAX #" + std::to_string(10 + k));
        load_palette(ship_images_[static_cast<std::size_t>(k)]);
        flush_keys();                                  // Gfx.LoadImage's 0x12BED
    }
    clear_row24();
}

void Game::free_ship_images() {
    // ovl24 +0x00CC (0x597AC).
    if (!f4a25_) return;
    f4a25_ = 0;
    ship_images_.clear();
}

void Game::ship_image(int k) {
    // Gfx.DrawImage 0267:159A of SHIPS #(10+k) at its cell (DS:23F0 + 6k,
    // DS:23F1 + 6k), straight to the screen.
    const Image& im = ship_images_.at(static_cast<std::size_t>(k));
    if (im.frames.empty()) return;
    const int x8 = static_cast<std::int8_t>(text_.ds8(0x23F0 + 6 * static_cast<std::size_t>(k)));
    const int y8 = static_cast<std::int8_t>(text_.ds8(0x23F1 + 6 * static_cast<std::size_t>(k)));
    screen_.blit(im.frames[0], im.width, im.height, 8 * x8, 8 * y8);
}

void Game::redraw_ship_screen() {
    // Screen.Redraw, state 8 (ovl23 0x586CC..0x586F7): the fight's screen,
    // or ROCKET's with no enemy (DS:4A14 nil).
    ShipScreen s(*this, Ship{&vars0_.at(0x200)}, enemy_ship_.empty() ? Ship{} : Ship{enemy_ship_.data()});
    s.redraw();
}

// 0267:3A56 (0x160C6): DrawImage's VGA path: clipped to the destination, opaque, or keyed (flags bit 0) with the background saved (bit 2)
void Game::bb_draw(const Bytes& px, int w, int h, int x, int y, bool masked) {
    // DrawImage 0267:0D06 into the backbuffer: masked skips colour 0
    // (flags 5's key; the explosions); rows below 168 get their dirty span.
    for (int r = 0; r < h; ++r) {
        const int Y = y + r;
        if (Y < 0 || Y >= 200) continue;
        for (int c = 0; c < w; ++c) {
            const int X = x + c;
            if (X < 0 || X >= 320) continue;
            const std::uint8_t v = px[static_cast<std::size_t>(r * w + c)];
            // 0267:3C46 (0x162B6): keyed rows: the key skipped per pixel; optional background save (flag -0x1A); dirty span as 0267:3BE7
            if (masked && v == 0) continue;
            bb_[static_cast<std::size_t>(Y * 320 + X)] = v;
        }
        // 0267:3BE7 (0x16257): opaque rows, each row's dirty span widened (DS:3C92, 3D3A, 3E8A, 3FDA)
        if (Y >= 168) continue;
        const int x0 = std::max(x, 0), x1 = std::min(x + w - 1, 319);
        auto& d = bb_dirty_[static_cast<std::size_t>(Y)];
        int& a = bb_x0_[static_cast<std::size_t>(Y)];
        int& b = bb_x1_[static_cast<std::size_t>(Y)];
        if (!d) {
            a = x0;
            b = x1;
            d = true;
        } else {
            a = std::min(a, x0);
            b = std::max(b, x1);
        }
    }
}

void Game::bb_present() {
    // Present 0267:183A (0x13EAA..0x13F26): each dirty row's span to the
    // screen at (8, 8) + its place; the marks cleared.
    for (int y = 0; y < 168; ++y) {
        if (!bb_dirty_[static_cast<std::size_t>(y)]) continue;
        bb_dirty_[static_cast<std::size_t>(y)] = false;
        const int a = bb_x0_[static_cast<std::size_t>(y)], b = bb_x1_[static_cast<std::size_t>(y)];
        for (int x = a; x <= b && x + 8 < 320 && y + 8 < 200; ++x)
            screen_.row(y + 8)[x + 8] = bb_[static_cast<std::size_t>(y * 320 + x)];
    }
}

void Game::backbuffer_image(int x8, int y8, const Image& im) {
    // DrawImage 0267:0D06 (opaque) of an image's frame 0 at cell (x8, y8)
    // of the backbuffer.
    if (im.frames.empty()) return;
    bb_draw(im.frames[0], im.width, im.height, 8 * x8, 8 * y8, false);
}

void Game::load_ship(std::uint8_t* dest, int idx) {
    // LoadShip ovl17 +0x26F3 (0x4FE13): SHIPS #(idx + 0x28), 0x6C bytes.
    fill_rect(0x15, 0x18, 0x27, 0x18, 0);              // Text.LoadingLine
    draw_string(0, 0x18, 10, 0, text_[Text::Loading]);
    auto raw = dax_entry("SHIPS.DAX", idx + 0x28);
    if (!raw || raw->size() < 0x6C) throw Fatal(text_[Text::WallUnable] + std::to_string(idx + 0x28));
    std::copy(raw->begin(), raw->begin() + 0x6C, dest);
    const bool is_enemy = dest == enemy_ship_.data();
    data_note("ship", idx, is_enemy ? 1 : 0);          // tools only: --data-trace
    if (is_enemy) {
        free_picture();                                // ovl27 +0x0B04 (0x4FE86)
        auto pic = dax_entry("SHIPS.DAX", idx % 5);    // 192x88, palette 0x40..0xFF
        if (!pic) throw Fatal(text_[Text::WallUnable] + std::to_string(idx % 5));
        ships_bg_ = std::make_unique<Image>(decode_generic(*pic, "SHIPS.DAX #" + std::to_string(idx % 5)));
        load_palette(*ships_bg_);
        flush_keys();                                  // Gfx.LoadImage's 0x12BED
        set_dac(0x40, 0xC0, pal_908b_);
        f6b14_ = 0xFF;
    }
    std::string name;
    if (idx > 4) {
        name = text_[Text::ScPartyShip];
    } else {
        const int c = v0(0x4D91 + static_cast<std::uint32_t>(idx));
        name = text_.ds_str(0x0F2C + 0x37 * static_cast<std::size_t>(idx) + 0x0B * static_cast<std::size_t>(c % 5), 15);
        if (c > 1) name = (name + " " + std::to_string(c)).substr(0, 15);
        v0(0x4D91 + static_cast<std::uint32_t>(idx)) = static_cast<std::uint8_t>(c + 1);
    }
    name = name.substr(0, 15);
    dest[0] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), dest + 1);
}

void Game::space_combat(int a1, int a3, int a4) {
    SpaceFight f(*this);
    f.party = Ship{&vars0_.at(0x200)};                 // DS:4A10 = [3BC8] + 0x200
    f.run(a1, a3, a4);
}

void Game::ship_repair() {
    // ShipRepair ovl5 +0x0CFE (0x3883E..0x38D4E).
    Ship party{&vars0_.at(0x200)};
    int active = 0;
    for (const auto& p : party_)
        if (p->b(0xD5) != 0) ++active;
    xp_share_ = 0;
    if (active) {
        xp_share_ = xp_5ea9_ / active;
        add_xp(xp_share_);                             // ovl5 +0x023E
    }
    xp_5ea9_ = 0;
    free_booty();                                      // ovl5 +0x0000
    f82f2_ = 0;
    layout_state0();                                   // 015F:00D8 (its byte parameter unused)
    set_window(1, 1, 0x26, 0x15, 0);
    print_line(0x0D, text_[Text::SrRepairing]);
    print_line(0, "");
    print_line(0x0F, text_[Text::SrHeader]);
    print_line(0, "");
    auto okay_check = [&](Character& p, int skill) {
        Character* one = &p;
        combat::Rng rng(seed_);
        const int r = combat::skill_check({skill_total(*one, skill)}, 2, rng).result;
        seed_ = rng.seed();
        data_note("skill", skill, r);
        return r >= 2;
    };
    for (int i = 0; i <= 5; ++i) {
        if (i == 4 || party.cur(i) >= party.max(i)) continue;
        for (auto& m : party_) {
            Character& p = *m;
            if (p.b(0xD4) != 1) continue;
            if (!okay_check(p, text_.ds8(0x0572 + static_cast<std::size_t>(i)))) continue;
            int r = roll_dice(text_.ds8(0x0579 + static_cast<std::size_t>(i)), 10);
            if (r + party.cur(i) > party.max(i)) r = (party.max(i) - party.cur(i)) & 0xFF;
            party.set_cur(i, party.cur(i) + r);
            const std::string n = std::to_string(r);       // StrInt width 0 (0x38A56)
            draw_string(1, lp_row_, 0x0A, 0, p.name());
            draw_string(0x12, lp_row_, 0x0A, 0, text_.ds_str(0x10AA + 18 * static_cast<std::size_t>(i)));
            draw_string(0x22, lp_row_, 0x0A, 0, text_[Text::SrSpace] + n);
            print_line(0, "");
            break;
        }
    }
    for (int w = 0; w < 8; ++w) {
        std::uint8_t* n = party.slot(w);
        int missing = (n[1] - n[2]) & 0xFF;
        while (missing > 0) {
            --missing;
            for (auto& m : party_) {
                Character& p = *m;
                if (p.b(0xD4) != 1) continue;
                if (!okay_check(p, text_.ds8(0x0578))) continue;
                draw_string(1, lp_row_, 0x0A, 0, p.name());
                draw_string(0x12, lp_row_, 0x0A, 0, text_.ds_str(0x22C3 + 0x22 * static_cast<std::size_t>(n[0] & 0x7F)));
                draw_string(0x20, lp_row_, 0x0A, 0, text_[Text::SrFixed]);
                print_line(0, "");
                n[2] = static_cast<std::uint8_t>(n[2] + 1);
                break;
            }
        }
    }
    if (xp_share_ != 0) {
        // The enemy's fuel (word +0x1E of DS:4A14, 0x38C4F..0x38C53). After
        // a capture DS:4A14 is nil (0x3771F): the word at 0000:001E, the
        // reference machine's 0xF000 (D-0066).
        int fuel;
        if (enemy_ship_.empty()) {
            fuel = low_.read(0x1E, "D-0066", "ShipRepair (ovl5 +0x0CFE, 0x38C4F): a captured ship's fuel read through "
                                             "the nil DS:4A14 (0000:001E); the reference machine's 0xF000 is used") |
                   low_.read(0x1F, "D-0066", "") << 8;
        } else {
            fuel = Ship{enemy_ship_.data()}.w(0x1E);
        }
        int f = (((roll_dice(2, 4) + 7) * fuel) & 0xFFFF) >> 4;
        if (f > 0) f += roll_dice(1, 16);
        const std::int32_t room = party.w(0x2A) - party.w(0x1E);
        if (static_cast<std::int32_t>(static_cast<std::uint16_t>(f)) > room) f = room & 0xFFFF;
        if (static_cast<std::uint16_t>(f) > 0) {
            const std::string n = std::to_string(static_cast<std::uint16_t>(f));   // StrInt width 0 (0x38CC5)
            print_line(0, "");
            print_line(0x0A, n + text_[Text::SrPlundered]);
            party.setw(0x1E, party.w(0x1E) + f);
        }
    }
    // 0x38D0E: the enemy's fuel := 0, through the nil pointer after a
    // capture (the word at 0000:001E: a later capture plunders nothing).
    if (!enemy_ship_.empty()) {
        Ship{enemy_ship_.data()}.setw(0x1E, 0);
    } else {
        low_.write(0x1E, 0, "D-0066", "ShipRepair (0x38D0E): a write through the nil DS:4A14 (0000:001E)");
        low_.write(0x1F, 0, "D-0066", "");
    }
    print_line(0, "");
    print_line(0x0D, text_[Text::SrDone]);
    pause_continue();
}

}  // namespace cotd::game
