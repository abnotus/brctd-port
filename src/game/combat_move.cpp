// SPDX-License-Identifier: GPL-3.0-or-later
// The player's Move in combat (ovl6 +0x0E85) with its bump attack (ovl6
// +0x124D), the step (ovl11 +0x081E), the guards' free attacks (ovl11
// +0x06A7), StepQuery and Place (ovl30 +0x0E23, +0x1272). Session 19:
// main-session readings of 0x3AB7E..0x3B0FF, 0x4111E..0x41326 and
// 0x5FB93..0x5FCE4; GuardCheck, Place and the hazard from a delegated
// reading (single).
#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr int kDx[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};   // DS:257C
constexpr int kDy[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};   // DS:2585

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }

}  // namespace

void Game::step_query(const Character& c, int dir, int& occ, int& tile, bool& hazard) const {
    // StepQuery ovl30 +0x0E23 (0x5FB93): over the footprint moved by dir,
    // the last other occupant, a tile-2 cell (the hazard), and the tile of
    // the highest cost (0: a cell off the map; 0x17 if no cell counts).
    occ = 0;
    tile = 0x17;
    hazard = false;
    int best = 1;
    const int k = icon_index(&c);
    if (!k) throw Error(c.name() + " has no combat icon (StepQuery would read entry 0)");
    const Icon& ic = icons_[static_cast<std::size_t>(k - 1)];
    for (int i = 0; i < 4; ++i) {
        int dx, dy;
        if (!footprint(ic.size, i, dx, dy)) continue;
        const int x = s8(ic.x + dx + kDx[dir]), y = s8(ic.y + dy + kDy[dir]);
        int o = 0, t = 0;                              // MapAt ovl30 +0x06BC: off the map 0, 0
        if (x >= 0 && x <= 0x31 && y >= 0 && y <= 0x18) {
            o = occupant(x, y);
            t = field_[static_cast<std::size_t>(y * kFieldW + x)];
        }
        if (o == k) o = 0;
        if (o > 0) occ = o;
        if (t == 2) hazard = true;
        if (t == 0 || tile == 0) {
            tile = 0;
            continue;
        }
        const int cost = text_.ds8(0x258A + 4 * static_cast<std::size_t>(t));
        if (cost >= best) {
            best = cost;
            tile = t;
        }
    }
}

bool Game::place_at(Character& c, int x, int y) {
    // Place ovl30 +0x1272 (0x5FFE2) with flag 0: the icon put back at (x, y)
    // if the square is free and passable, else removed (size 0).
    const int k = icon_index(&c);
    if (!k) throw Error(c.name() + " has no combat icon (Place)");
    Icon& ic = icons_[static_cast<std::size_t>(k - 1)];
    ic.size = c.b(0x34) & 0x7F;
    ic.x = s8(x);
    ic.y = s8(y);
    int occ, tile;
    bool hazard;
    step_query(c, 8, occ, tile, hazard);
    if (occ != 0 || tile == 0 || text_.ds8(0x258A + 4 * static_cast<std::size_t>(tile)) == 0xFF) {
        ic.size = 0;
        return false;
    }
    return true;
}

void Game::combat_step(Character& c, int dir) {
    // Step ovl11 +0x081E (0x4111E..0x41326): the main square's cost from
    // the movement points, the icon moved (the view follows: margin 1 for
    // the player, 3 with a pre-scroll for the AI), the attackers forgotten,
    // the guards' chance, and no points left when down or helpless.
    const int k = icon_index(&c);
    Icon& ic = icons_[static_cast<std::size_t>(k - 1)];
    const int ox = ic.x, oy = ic.y;
    const int nx = s8(ox + kDx[dir]), ny = s8(oy + kDy[dir]);
    const int t = field_tile_at(nx, ny);
    const int c0 = text_.ds8(0x258A + 4 * static_cast<std::size_t>(t));
    const int cost = (dir & 1 ? c0 * 3 : c0 * 2) & 0xFF;
    c.cb[3] = static_cast<std::uint8_t>(cost > c.cb[3] ? 0 : c.cb[3] - cost);
    int margin = 1;
    if (c.b(0xD7) != 0) {
        margin = 3;
        if (!in_view(nx - field_ox_, ny - field_oy_) && f6bda_) field_view(ox, oy, 2, 8);
    }
    if (f6bda_) field_erase_icon(0, 0, k);
    ic.x = nx;
    ic.y = ny;
    if (f6bda_) field_view(nx, ny, margin, 8);
    if (f6bda_ && c.b(0xD7) != 0) step_pace();          // the port's pace (D-0064)
    c.cb[0x0D] = 0;
    c.cb[0x0E] = 0;
    sound(text_.ds16(0x21F0));                         // 0x412ED: the step's sound (10)
    guard_check(c);                                    // ovl11 +0x06A7
    if (c.b(0xD5) == 0 || has_effect(c, 1)) c.cb[3] = 0;
}

void Game::guard_check(Character& mover) {
    // GuardCheck ovl11 +0x06A7 (0x40FA7): every enemy in sight that guards
    // (cb+4) and isn't helpless fires once at the mover: a melee guard
    // when adjacent, both within its weapon's trace (a mover with status
    // 0x0E is always in line).
    const int n = collect_enemies(mover, 0x3C);
    std::vector<int> ks;
    for (int i = 1; i <= n; ++i) ks.push_back(scan_[static_cast<std::size_t>(i)].k);   // DS:6B7F + i
    for (int k : ks) {
        if (mover.b(0xD5) == 0) continue;
        Character* g = icons_[static_cast<std::size_t>(k - 1)].rec;
        if (!g || g->cb.size() != 0x12 || g->cb[4] == 0) continue;
        if (has_effect(*g, 1)) continue;
        if (!weapon_ranged(*g) && distance(*g, mover) > 1) continue;
        int tx = pos_x(mover), ty = pos_y(mover), r = weapon_range(*g);
        if (mover.b(0xD4) != 0x0E && !trace(pos_x(*g), pos_y(*g), tx, ty, r)) continue;
        field_view(pos_x(mover), pos_y(mover), 2, 8);
        note_attacker(*g, mover);
        bool tmp = false;
        attack_command(*g, &mover, false, tmp);
        g->cb[4] = 0;
    }
}

void Game::move_cmd(Character& c, bool& done) {
    // Move ovl6 +0x0E85 (0x3AC85..0x3B023), always entered with key ' '.
    const std::uint8_t m0 = c.cb[3], f0 = c.cb[7];
    const int x0 = pos_x(c), y0 = pos_y(c);
    int key = 0x20;
    done = false;
    int dir = 8;
    f6bd7_ = 1;                                        // 0x3AC8C .. 0x3B01B: map clicks step
    while (!(key == 0 || key == 0x0D || key == 0x44 || done)) {
        const Text label = c.b(0xD4) == 0x0B ? Text::CbLabelSprint : c.b(0xD4) == 0x0A ? Text::CbLabelDodge : Text::CbMoveAttack;
        clear_cells(0x18, 0x18, 0x27, 0x18);
        if (key == 0x20) {
            const MenuKey mk = menu_screen(text_[label] + text_[Text::CbMoveLeft] + str_byte(c.cb[3] / 2),
                                           text_[Text::CbDone], true);   // 0x3ADC2
            key = mk.key;
        }
        switch (key) {
            case 0: {                                  // Undo 0x3AB7E (Esc)
                c.cb[3] = m0;
                field_erase_icon(0, 0, icon_index(&c));
                done = !place_at(c, x0, y0);
                field_view(pos_x(c), pos_y(c), 0, 8);
                c.cb[7] = f0;
                dir = 8;
                break;
            }
            case 0x49: dir = 1; break;
            case 0x48: dir = 0; break;
            case 0x51: dir = 3; break;
            case 0x4D: dir = 2; break;
            case 0x4F: dir = 5; break;
            case 0x50: dir = 4; break;
            case 0x47: dir = 7; break;
            case 0x4B: dir = 6; break;
            default: dir = 8; break;
        }
        if (dir < 8) {
            face(c, dir, 0, false);                    // 0x3AE48
            int occ, tile;
            bool hazard;
            step_query(c, dir, occ, tile, hazard);
            if (occ > 0) {                             // Bump ovl6 +0x124D (0x3B04D)
                Character& other = *icons_[static_cast<std::size_t>(occ - 1)].rec;
                if (c.b(0xD4) == 0x0B) show_message(text_[Text::CbRunning]);
                else if (weapon_heavy(c) || !can_fire(c)) show_message(text_[Text::CbWrongWeapon]);
                else if (can_attack(c, other)) {
                    c.cb_target = &other;
                    note_attacker(c, other);
                    attack_command(c, &other, false, done);
                }
            } else if (tile == 0) {                    // off the map: Flee?
                const int r = yes_no(text_[Text::CbFlee], 2);
                if (r == 'Y') done = flee(c);         // ovl11 +0x0A44
                if (r == 'N') done = false;
            } else {
                const int c0 = text_.ds8(0x258A + 4 * static_cast<std::size_t>(tile));
                int cost = (dir & 1 ? c0 * 3 : c0 * 2) & 0xFF;
                if (c0 == 0xFF) cost = 0xFF;
                if (cost > c.cb[3]) show_message(text_[Text::CbCantGo]);
                else if (c.b(0xD5) == 0) done = end_turn(c);
                else {
                    if (c.cb[3] > 0) combat_step(c, dir);
                    if (c.b(0xD5) == 0) done = end_turn(c);
                    terrain_hazard(c);                 // ovl22 +0x0436
                    if (has_effect(c, 1)) done = end_turn(c);
                }
            }
        }
        if (key != 0 && key != 0x0D && key != 0x44) key = 0x20;
    }
    if (c.cb[3] < 2) c.cb[3] = 0;                      // 0x3B007
    f6bd7_ = 0;
}


void Game::jetpack(Character& c) {
    // Jetpack ovl11 +0x2667 (0x42F67..0x43326; session 32, reading
    // analysis/re/jetpack.md, the roll, the hops and the candidate test
    // checked in the main session). SkillCheck(skill 0x38, difficulty 2)
    // (0x42FAD). Success: Aim in mode 2, a free flight to the square picked
    // with Land (status 0x0E), guards checked at take-off and landing, the
    // icon off the map (x 0x80) between, status 0x0D; the turn goes on.
    // Failure: eleven random hops (a 5 x 5 block one row down, (0, +1)
    // left out), landing on the last free one, then the turn ends.
    note_readied(c, 2, "worn_use");                    // tools only: --data-trace
    const int k = icon_index(&c);
    if (!k) throw Error(c.name() + ": Jetpack without a combat icon");
    auto ic = [&]() -> Icon& { return icons_[static_cast<std::size_t>(k - 1)]; };
    int x = s8(ic().x), y = s8(ic().y);
    combat::Rng rng(seed_);
    const int r = combat::skill_check({skill_total(c, 0x38)}, 2, rng).result;
    seed_ = rng.seed();
    data_note("skill", 0x38, r);
    const bool ok = r >= 2;
    if (ok) {
        bool f = false, chosen = false;
        Character* t = nullptr;
        int rx = 0, ry = 0;
        aim(c, 0, 2, false, false, f, chosen, t, rx, ry, x, y);   // 0x42FE9
        field_redraw_square(aim_x_, aim_y_);                       // 0x42FF4
        if (c.b(0xD4) != 0x0E) return;                             // 0x42FFC: no Land
        guard_check(c);                                            // 0x4300E
        ic().x = -128;                                             // 0x4301C: 0x80, y kept
        c.cb[3] = 0;                                               // 0x43039
        if (c.b(0xD5) != 0) {
            const int dx = s8(aim_x_), dy = s8(aim_y_);
            marker_from(c.b(0xB9), 0, 0, dx < x);                  // 0x43067
            anim_icon(x, y, dx, dy, 1, 15);                        // 0x43086
            ic().x = dx;
            ic().y = dy;
            guard_check(c);                                        // 0x430BD
        }
        if (c.b(0xD5) != 0) c.b(0xD4) = 0x0D;                      // 0x430CE
        return;
    }
    name_message(c, text_[Text::CbJetFails], 10, false);           // 0x430F2
    c.b(0xD4) = 0x0E;
    c.cb[3] = 0;
    int best = -1;
    int px[11], py[11];
    for (int i = 0; i <= 10; ++i) {
        int nx, ny;
        do {                                                       // 0x4311A: off the map, again
            int kk = roll_dice(1, 24) - 1;
            if (kk >= 12) ++kk;
            nx = s8(x + kk % 5 - 2);
            ny = s8(y + kk / 5 - 1);
        } while (nx < 0 || nx > 0x31 || ny < 0 || ny > 0x18);
        px[i] = nx;
        py[i] = ny;
        x = nx;
        y = ny;
        // MapAt ovl30 +0x06BC: free and passable (the grid as before the
        // take-off: the start square still occupied).
        if (occupant(x, y) == 0 && text_.ds8(0x258A + 4 * static_cast<std::size_t>(field_tile_at(x, y))) != 0xFF)
            best = i;
    }
    x = s8(ic().x);                                                // 0x431E4
    y = s8(ic().y);
    ic().x = -128;
    ic().y = -128;
    for (int i = 0; i <= best; ++i) {                              // the hops up to the landing
        marker_from(c.b(0xB9), 0, 0, px[i] < x);                   // 0x43266
        anim_icon(x, y, px[i], py[i], 1, 40);                      // 0x43291
        x = px[i];
        y = py[i];
    }
    ic().x = x;                                                    // 0x432C9
    ic().y = y;
    guard_check(c);                                                // 0x432EB
    if (c.b(0xD5) != 0) c.b(0xD4) = 0x0D;                          // 0x432FC
    end_turn(c);                                                   // 0x43308
    field_view(x, y, 2, 8);                                        // 0x4331E
}

}  // namespace cotd::game
