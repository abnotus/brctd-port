// SPDX-License-Identifier: GPL-3.0-or-later
// The AI (ovl7): a computer-controlled combatant's turn (monsters, NPCs
// and Quick party members), morale, targets, weapons, the area weapon's
// aim, movement. Session 20: main-session readings of ovl7
// 0x3B16F..0x3CDC8 (all of it), FastestEnemy (0x4365D), Flee (0x41344),
// RemoveFromCombat (0x55820) and the ready toggle (0x51EB2);
// docs/notes/combat_ai.md.
#include <algorithm>

#include "game/game.hpp"

namespace cotd::game {

namespace {

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }

}  // namespace

// --- AI.Turn ovl7 +0x000F (0x3B16F..0x3B2DC) --------------------------------------

void Game::ai_turn(Character& c) {
    bool done = ai_check_key(c);
    clear_row24();                                     // 0653:1359
    clear_text();
    if (c.b(0xD5) == 0 || c.b(0xD4) == 0x0A || c.b(0xD4) == 0x0B) done = end_turn(c);
    // 0x3B1BB: the movement strategy cb+0x11 kept on a d4 other than 1,
    // else rolled again: d8, 8 -> d2 + 4, else d4.
    int s = c.cb[0x11];
    bool reroll = true;
    if (s != 0 && s <= 4) reroll = roll_dice(1, 4) == 1;
    if (reroll) {
        s = roll_dice(1, 8);
        s = s == 8 ? roll_dice(1, 2) + 4 : roll_dice(1, 4);
    }
    c.cb[0x11] = static_cast<std::uint8_t>(s);
    if (!done) done = ai_morale(c);                    // ovl7 +0x1281
    if (c.cb[0x10] != 0) name_message(c, text_[Text::AiFlees], 10, true);
    if (!done) {
        done = ai_check_key(c);
        while (!done) {
            pick_target(c, true, done);                // ovl7 +0x1A70
            if (!done) {
                if (s8(c.cb[1]) > 0 && c.b(0xD5) != 0) done = ai_act(c);
                else done = ai_idle(c);
            }
        }
    }
}

bool Game::ai_check_key(Character& c) {
    // CheckKey ovl7 +0x11AE (0x3C30E): only when a key is waiting (KEYFEED
    // never answers this poll, the headless input has none waiting): Space
    // takes the party's Quick off (and hands this turn back, cb+1 := 0x14),
    // '-' is the developer's Alt-X; then FlushKeys.
    steps_unread_ = 0;
    if (!input_ || !input_->key_waiting()) return false;
    // GetKey (twice for an extended key: its scan code is the value), so
    // Alt-D (scan 0x20) counts as Space and Alt-X (0x2D) as '-'.
    const int k = next_key("the AI's key poll") & 0xFF;
    bool out = false;
    if (k == ' ') {
        for (const auto& p : party_)
            if (p->b(0xD6) == 0 && p->b(0x4C) == 0 && p->cb.size() == 0x12 && p->cb[0x0F] == 0) p->b(0xD7) = 0;
        if (c.b(0xD7) == 0) {
            c.cb[1] = 0x14;
            out = true;
        }
    }
    // '-': ovl11 +0x4036, a developer's exit that needs a password on the
    // command line (the port has none): nothing.
    flush_keys();                                      // Input.FlushKeys
    return out;
}

bool Game::ai_idle(Character& c) {
    // Idle ovl7 +0x115E (0x3C2BE): helpless or out of actions: EndTurn;
    // else Guard (ovl11 +0x2A32).
    clear_text();
    if (has_effect(c, 1) || c.cb[1] == 0) return end_turn(c);
    return guard_cmd(c);
}

bool Game::ai_morale(Character& c) {
    // Morale ovl7 +0x1281 (0x3C3E1): only NPCs and monsters (+0x4C). The
    // limit (+0xE7 & 0x7F)·2 (0 above 0x66) against the HP lost in %; for
    // the enemy the side's HP % DS:6BC9 against 100 - var 0x7EC6; then flee
    // (cb+0x10) when no enemy is faster, else surrender when INT > 5.
    c.cb[0x10] = 0;
    if (c.b(0x4C) == 0) return false;
    int v = ((c.b(0xE7) & 0x7F) * 2) & 0xFF;
    if (v > 0x66) v = 0;
    f699a_ = static_cast<std::uint8_t>(v);
    effect_hook(c, 0x11, "Morale (0x3C42A)");
    const std::uint16_t max = c.w(0x45);
    if (max == 0) throw Error("Morale (ovl7 +0x1281, 0x3C441): " + c.name() + "'s max HP is 0: the original divides by zero");
    const int lost = (100 - static_cast<std::uint16_t>(static_cast<std::uint16_t>(c.w(0xE3) * 100) / max)) & 0xFFFF;
    if (f699a_ >= lost && f699a_ != 0) return false;
    f699a_ = enemy_hp_pct_;
    effect_hook(c, 0x11, "Morale (0x3C475)");
    const int thr = 100 - v1(0x7EC6);
    if (static_cast<int>(f699a_) >= thr && f699a_ != 0 && c.b(0xD6) != 0) return false;
    const int fe = fastest_enemy(c);
    const int cm = (calc_move(c) & 0xFF) / 2;
    if (fe <= cm) {
        c.cb[0x10] = 1;
        return false;
    }
    if (c.b(0x13) > 5) {
        remove_from_combat(c, c.b(0xD6) == 0 ? 6 : 5, text_[Text::AiGivesUp]);
        return end_turn(c);
    }
    return false;
}

int Game::fastest_enemy(Character& c) {
    // FastestEnemy ovl11 +0x2D5D (0x4365D): CalcMove div 2 over the other
    // side's active records (CalcMove rewrites their +0xE5).
    const int other = c.b(0xD6) == 0 ? 1 : 0;
    int best = 0;
    for (const auto& p : party_)
        if (p->b(0xD6) == other && p->b(0xD5) != 0) {
            const int m = ((calc_move(*p) & 0xFF) / 2) & 0xFF;
            if (m > best) best = m;
        }
    return best;
}

void Game::remove_from_combat(Character& c, int status, const std::string& msg) {
    // RemoveFromCombat ovl22 +0x07D0 (0x55820): FocusOn, the message (with
    // its delay), out of the fight with the status (HP 0 unless fled), the
    // icon erased and gone, EndTurn, ClearOnDown.
    if (c.b(0xD5) == 0) return;
    const int k = icon_index(&c);
    focus_on(c, 3, false);
    name_message(c, msg.substr(0, 40), 10, true);
    c.b(0xD5) = 0;
    c.b(0xD4) = static_cast<std::uint8_t>(status);
    if (status != 6) c.setw(0xE3, 0);
    field_erase_icon(0, 0, k);
    field_present();
    if (k) icons_[static_cast<std::size_t>(k - 1)].size = 0;
    if (end_turn(c)) clear_on_down(c);
}

bool Game::flee(Character& c) {
    // Flee ovl11 +0x0A44 (0x41344): away when no enemy is in sight, or none
    // is faster (a tie: RollDice(1, 2) = 1); always EndTurn.
    bool ok = false;
    if (collect_enemies(c, 0xFF) == 0) ok = true;
    else {
        const int mine = ((calc_move(c) & 0xFF) / 2) & 0xFF;
        const int theirs = fastest_enemy(c);
        if (theirs < mine) ok = true;
        else if (theirs == mine && roll_dice(1, 2) == 1) ok = true;
    }
    if (ok) remove_from_combat(c, 6, text_[Text::CbGotAway]);
    else show_message(text_[Text::CbEscapeBlocked]);
    return end_turn(c);
}

bool Game::guard_cmd(Character& c) {
    // Guard ovl11 +0x2A32 (0x43332).
    const bool done = end_turn(c);
    if (!weapon_heavy(c)) {
        c.cb[4] = 1;
        show_message(text_[Text::CbGuardMsg]);
    }
    return done;
}

// --- PickTarget ovl7 +0x1A70 (0x3CBD0..0x3CDC8) ----------------------------------

bool Game::pick_target(Character& c, bool allow_swap, bool& done) {
    auto hostile = [&](const Character* t) { return t && (t == &c || t->b(0xD6) != c.b(0xD6)); };   // ovl11 +0x0BDA
    Character* t = c.cb_target;
    if (t && (t->b(0xD6) == c.b(0xD6) || t->b(0xD5) == 0 || !hostile(t))) c.cb_target = nullptr;
    bool found = c.cb_target != nullptr;
    bool pass = false, last = false;
    while (!found && !last) {
        last = pass;
        if (pass) field_open_ = true;
        int tries = 20;
        int n = collect_enemies(c, 0xFF);
        std::vector<int> ks(static_cast<std::size_t>(n) + 1, 0);   // DS:6B7E + i
        for (int i = 1; i <= n; ++i) ks[static_cast<std::size_t>(i)] = scan_[static_cast<std::size_t>(i)].k;
        while (tries > 0 && !found && n > 0) {
            --tries;
            const int k = roll_dice(1, n);
            if (ks[static_cast<std::size_t>(k)] <= 0) continue;
            Character* r = icons_[static_cast<std::size_t>(ks[static_cast<std::size_t>(k)] - 1)].rec;
            if ((allow_swap && field_open_) || hostile(r)) {
                found = true;
                c.cb_target = r;
            } else {
                ks[static_cast<std::size_t>(k)] = 0;
                bool any = false;
                for (int i = 1; i <= n; ++i)
                    if (ks[static_cast<std::size_t>(i)] > 0) any = true;
                if (!any) n = 0;
            }
        }
        pass = true;
    }
    field_open_ = false;
    if (found && allow_swap) choose_weapon(c, true, done);
    return found;
}

// --- ChooseWeapon ovl7 +0x164A (0x3C7AA..0x3CBCD) ---------------------------------

void Game::choose_weapon(Character& c, bool allow_area, bool& out) {
    Character* t = c.cb_target;
    int best = -1, ammo = -1, hand6 = -1;
    bool hand6_ready = false;
    int best_score = (c.b(0x3D) * c.b(0x3F) + 2 * s8(c.b(0x41))) & 0xFF;   // the natural attack
    int p2 = 0, p3 = 0, p4 = 0;
    if (t) {
        int tx = pos_x(*t), ty = pos_y(*t), n = 0xFF;
        trace(pos_x(c), pos_y(c), tx, ty, n);          // the result is unused (0x3C866)
        if (trace_flags_[1] || has_effect(*t, 0x18)) p3 = 3;
        if (trace_flags_[0]) p2 = 3;
        else if ((t->b(0x48) & 0x10) || has_effect(*t, 0x14)) p2 = 2;
        if ((t->b(0x48) & 0x40) || has_effect(*t, 0x12)) p4 = 3;
    }
    auto score = [&](std::size_t i) {                  // WeaponScore ovl7 +0x141B (0x3C57B)
        Bytes& it = c.items[i];
        const int type = it[0x2E];
        if (item_byte(type, 2) > 0 && it[0x38] == 0) return 0;
        int s = (2 * s8(item_byte(type, 6)) + item_byte(type, 4) * item_byte(type, 5)) & 0xFF;
        if (type == 0x0F) s = (t && has_effect(*t, 0x0E)) ? 0 : 12;
        else if (type == 17 || type == 18 || (type >= 34 && type <= 38)) {   // CS:13DB
            const bool heavy = type == 37 || type == 38;                     // CS:13FB
            if (!allow_area) s = 0;
            else if (c.b(0xD6) != 1 && heavy) s = 0;
            else if ((v0(0x4BE6) & 0x10) && heavy) s = 0;
            else {                                     // ScoreArea ovl7 +0x083E (0x3B99E)
                const int saved = c.weapon;
                c.weapon = static_cast<int>(i);
                int v = 0;
                area_target(c, false, v);
                c.weapon = saved;
                s = v & 0xFF;
            }
        }
        s = (s + 3 * spec_count(c, type) + 3 * s8(it[0x32])) & 0xFF;
        if (item_byte(type, 2) == 0)
            s = (s + 2 * s8(text_.ds8(0x0CCF + c.b(0x10))) + 2 * s8(text_.ds8(0x0CE5 + c.b(0x10)))) & 0xFF;
        else
            s = (s + 2 * s8(text_.ds8(0x0CB9 + c.b(0x11)))) & 0xFF;
        int m = item_byte(type, 1) >> 1;
        if (m == 0) m = 1;
        if (!allow_area && item_byte(type, 2) >= 7) return 0;
        return (m * s) & 0xFF;
    };
    for (std::size_t i = 0; i < c.items.size(); ++i) {
        const Bytes& it = c.items[i];
        const int type = it[0x2E];
        const int cls = item_byte(type, 0);
        if (cls == 0) {
            int s = score(i);
            const int kind = item_byte(type, 2);
            if (kind == 2) s >>= p2;
            else if (kind == 3) s >>= p3;
            else if (kind == 4) s >>= p4;
            if (s > best_score) {
                best_score = s;
                best = static_cast<int>(i);
            }
        } else if (cls == 1) {
            if (ammo < 0 ||
                s8(c.items[static_cast<std::size_t>(ammo)][0x32]) + item_byte(c.items[static_cast<std::size_t>(ammo)][0x2E], 2) <
                    s8(it[0x32]) + item_byte(type, 2))
                ammo = static_cast<int>(i);
        } else if (cls == 6) {
            if (it[0x34] & 0x80) hand6_ready = true;
            else hand6 = static_cast<int>(i);
        }
    }
    auto readied = [&](int cls) {                      // FindReadied ovl18 +0x1E8C
        for (std::size_t i = 0; i < c.items.size(); ++i)
            if ((c.items[i][0x34] & 0x80) && item_byte(c.items[i][0x2E], 0) == cls) return static_cast<int>(i);
        return -1;
    };
    const int cur1 = readied(1);
    if (ammo != cur1) {
        if (cur1 >= 0) ready_toggle(c, cur1, out);
        if (ammo >= 0) ready_toggle(c, ammo, out);
    }
    out = false;
    const int cur0 = readied(0);
    if (!(best == cur0 && best == c.weapon) && best >= 0 && item_byte(c.items[static_cast<std::size_t>(best)][0x2E], 3) != 0)
        out = true;
    // 0x3CAE4..0x3CB12: cur0 unreadied, best readied (through nil
    // pointers when either is absent: D-0024).
    const bool hand6_toggle = combat::choose_weapon_tail(
        cur0 < 0, best < 0, best >= 0 ? c.items[static_cast<std::size_t>(best)][0x2E] : 0, hand6 >= 0, hand6_ready, out,
        items_table_, low_);
    if (cur0 >= 0) c.items[static_cast<std::size_t>(cur0)][0x34] &= 0x7F;
    if (best >= 0) {
        c.items[static_cast<std::size_t>(best)][0x34] |= 0x80;
        data_note("ready", c.items[static_cast<std::size_t>(best)][0x2E], c.b(0xD6));   // tools only
    }
    c.weapon = best;
    if (hand6_toggle) ready_toggle(c, hand6, out);
    if (!(cur1 == ammo && cur0 == best && !out)) {     // 0x3CB54
        recalc(c);
        recount_attacks(c);
        combat_panel(c);
        if (out) {
            name_message(c, text_[Text::AiHeavy], 10, false);
            game_delay();
        }
    }
}

Bytes& Game::foreign_node(ForeignSlot& f) {
    for (const auto& m : party_)
        if (m.get() == f.owner)
            for (Bytes& it : m->items)
                if (it == f.item) return it;
    return f.item;
}

void Game::ready_toggle(Character& owner, int i, bool& flag) {
    // ovl18 +0x1F12 (0x51EB2): the item is a node pointer ([BP+0Ah], the
    // list's owner's), the slots and the class test CurChar's (DS:4EC1):
    // unready, or ready unless an item of the class (0, 1: the weapon /
    // armour slot; 2..7: FindReadied) is readied already; readying a heavy
    // item in combat ends the turn. The owner is CurChar except in ViewGear
    // after a trade's SelectMember moved CurChar by a roster click
    // (session 51's fuzzing, F8: run s51_f8_gear).
    Character& c = *cur_;
    flag = false;
    Bytes& it = owner.items[static_cast<std::size_t>(i)];
    const int cls = item_byte(it[0x2E], 0);
    if (it[0x34] & 0x80) {
        // 0x51F03: unreadied; the class's slot +0xEB/+0xEF is cleared for
        // classes 0 and 1, whichever item it held.
        it[0x34] ^= 0x80;
        if (cls == 0) c.weapon = -1;
        else if (cls == 1) c.armour = -1;
        if (cls <= 1 && foreign_slot_[cls].holder == &c) foreign_slot_[cls] = {};
        return;
    }
    int other = -1;
    Bytes* foreign = nullptr;                          // D-0074: another record's node in the slot
    if (cls <= 1) {
        other = cls == 0 ? c.weapon : c.armour;
        if (other < 0 && foreign_slot_[cls].holder == &c) foreign = &foreign_node(foreign_slot_[cls]);
    }
    else if (cls <= 7)
        for (std::size_t j = 0; j < c.items.size(); ++j)
            if ((c.items[j][0x34] & 0x80) && item_byte(c.items[j][0x2E], 0) == cls) {
                other = static_cast<int>(j);
                break;
            }
    if (cls <= 1 && other < 0 && !foreign) {
        // 0x51F62: CurChar's slot := the node; another record's node is
        // held as D-0074's foreign slot (measured: run s52_d0074_unarmed).
        if (&owner != &c) foreign_slot_[cls] = {&c, &owner, it};
        else (cls == 0 ? c.weapon : c.armour) = i;
    }
    if (other >= 0 || foreign) {
        // 0x51FAE: another item of the class is readied: its name
        // (ItemName ovl23 +0x0124) after CS:1F03 as a message; nothing
        // readied.
        show_message(text_[Text::RdRefused] + item_name(foreign ? *foreign : c.items[static_cast<std::size_t>(other)]));
        return;
    }
    it[0x34] |= 0x80;
    data_note("ready", it[0x2E], c.b(0xD6));           // tools only: --data-trace
    flag = item_byte(it[0x2E], 3) != 0 && mode_ == 5;
    if (flag) flag = end_turn(c);
}

// --- Act ovl7 +0x0DA4 (0x3BF04..0x3C2BB) ------------------------------------------

bool Game::ai_act(Character& c) {
    auto hostile = [&](const Character* t) { return t && (t == &c || t->b(0xD6) != c.b(0xD6)); };
    f358a_ = 8;
    f358b_ = 0;
    int iter = 0;
    bool cont = true, done = false;
    effect_hook(c, 0x0E, "Act (0x3BF2D)");
    if (c.b(0xD6) == 0 && !f82ea_ && can_aid(c)) {
        aid_cmd(c);
        c.cb[1] = 0;
    }
    if (c.cb[1] == 0) cont = false;
    while (!done && cont) {
        while (c.cb[0x10] != 0 && c.cb[3] > 0 && s8(c.cb[1]) > 0 && s8(c.cb[1]) < 0x14) ai_move_step(c);
        if (c.cb[1] == 0 || c.cb[1] == 0x14) cont = false;
        else done = false;
        if (done || !cont) continue;
        if (++iter > 20) {
            done = true;
            cont = !ai_idle(c);
        }
        f6bd8_ = 0;
        Character* t = c.cb_target;
        // 0x3C04F..0x3C05F: cb+8's +0xD5 with no nil test. With no target
        // (PickTarget found none in sight) the original reads 0000:00D5,
        // the INT 35h vector's high offset byte (0xCA in every memory dump;
        // no routine writes it): not 0, so the target stays nil (D-0039).
        if (t ? t->b(0xD5) == 0
              : low_.read(0xD5, "D-0039", "Act (ovl7 +0x0DA4, 0x3C052): no target; its active byte is read through "
                                          "a nil pointer") == 0)
            t = nullptr;
        int wr = weapon_range(c);
        if (t && hostile(t)) {
            int tx = pos_x(*t), ty = pos_y(*t), n = wr;
            field_open_ = false;
            if (trace(pos_x(c), pos_y(c), tx, ty, n) && n / 2 <= wr && (c.cb[3] < 4 || wr > n || weapon_heavy(c)))
                f6bd8_ = 1;
        }
        if (!f6bd8_) {
            if (c.cb[3] > 4) wr = 1;
            const int n = collect_enemies(c, wr);
            if (n == 0) {
                if (pick_target(c, false, done) && !done) ai_move_step(c);
                else done = ai_idle(c);
            } else {
                const int k = roll_dice(1, n);
                t = icons_[static_cast<std::size_t>(scan_[static_cast<std::size_t>(k)].k - 1)].rec;
                if (distance(c, *t) <= wr || hostile(t)) f6bd8_ = 1;
            }
        }
        if (f6bd8_) {
            if (weapon_heavy(c)) {
                int tmp = 0;
                done = area_target(c, true, tmp);
            } else {
                field_view(pos_x(c), pos_y(c), 2, dir_rec(c, *t));
                note_attacker(c, *t);
                attack_command(c, t, false, done);
            }
            if (done) cont = false;
            else if (t && t->b(0xD5) == 0) done = true;
        }
    }
    return !cont;
}

// --- MoveStep ovl7 +0x09BF (0x3BB1F..0x3BF01), CanStep +0x089D (0x3B9FD) -----------

bool Game::ai_can_step(Character& c, int i, int base, bool& off) {
    off = false;
    const int t = text_.ds8(0x057A + 5 * static_cast<std::size_t>(c.cb[0x11]) + static_cast<std::size_t>(i));
    const int d = (t + base) % 8;
    int occ, tile;
    bool hazard;
    step_query(c, d, occ, tile, hazard);
    if (tile == 0) {
        off = true;
        return false;
    }
    const int c0 = text_.ds8(0x258A + 4 * static_cast<std::size_t>(tile));
    if (c0 == 0xFF) return false;
    const int cost = (d & 1 ? c0 * 3 : c0 * 2) & 0xFF;
    return occ == 0 && cost < c.cb[3] && c.cb[3] >= cost;   // the AI can't spend its exact last points
}

void Game::ai_move_step(Character& c) {
    draw_string(0, 0x18, 0x0A, 0, text_[Text::AiMoveLeft] + str_byte(c.cb[3] / 2) + text_[Text::CbRangeSp]);
    if (ai_check_key(c)) return;
    if (c.cb[3] / 2 <= 0 || s8(c.cb[1]) <= 0) {
        ai_idle(c);
        return;
    }
    if (c.b(0xE7) >= 0x80) {                           // 0x3BBCA
        const int r = roll_dice(1, 100);
        if (enemy_hp_pct_ > r + f699a_ && c.b(0xD6) != 1) {
            ai_idle(c);
            return;
        }
    }
    if (c.cb[0x10] == 0 && c.armour < 0 && c.b(0x28) == 2) {   // +0xEF nil, class 2
        ai_idle(c);
        return;
    }
    int base;
    if (c.cb[0x10] == 0) {
        if (!c.cb_target) throw Error("MoveStep (ovl7 +0x09BF, 0x3BC49) toward no target: the original reads icon entry 0");
        base = dir_rec(c, *c.cb_target);
    } else {
        c.cb[0x11] = static_cast<std::uint8_t>(roll_dice(1, 2));
        const int f = facing_;                         // DS:6A31
        base = f - ((f + 2) % 4) / 2;
        if (c.b(0xD6) == 0) base += 4;
        base %= 8;
    }
    bool off = false, stop = false;
    int i = 1;
    while (i < 6 && !stop) {
        if (ai_can_step(c, i, base, off)) break;
        if (c.cb[0x10] != 0 && off) stop = flee(c);
        else ++i;
    }
    if (stop) {                                        // fled
        c.cb[3] = 0;
        c.cb[0x10] = 0;
        end_turn(c);
        return;
    }
    const int d = (text_.ds8(0x057A + 5 * static_cast<std::size_t>(c.cb[0x11]) + static_cast<std::size_t>(i)) + base) % 8;
    if (i == 6 || (d + 4) % 8 == f358a_) {             // stuck or turning back
        ++f358b_;
        c.cb[0x11] = static_cast<std::uint8_t>(c.cb[0x11] % 6 + 1);
        if (f358b_ > 1) {
            c.cb_target = nullptr;
            if (f358b_ > 2) {
                c.cb[3] = 0;
                stop = true;
            } else if (!pick_target(c, true, stop) && !stop) {
                stop = ai_idle(c);
            }
        }
    }
    if (i < 6) f358a_ = static_cast<std::uint8_t>(d);
    else stop = true;
    if (stop) return;
    f6bda_ = (f6bd8_ || icon_visible(&c, false) || c.b(0xD6) == 0) ? 1 : 0;
    face(c, d, 0, false);
    if (c.b(0xD5) == 0) {
        end_turn(c);
        return;
    }
    if (c.cb[3] > 0) combat_step(c, c.cb[7]);
    if (c.b(0xD5) == 0 || has_effect(c, 1)) end_turn(c);
    terrain_hazard(c);
}

// --- AreaTarget ovl7 +0x019F (0x3B2FF..0x3B99B) -----------------------------------

bool Game::area_target(Character& c, bool fire, int& value) {
    // The area weapon's aim: a 19 x 19 grid of weights (the other side
    // counts for, one's own side against) over the blast shapes (DS:05A6),
    // the best cell (ties: Percent(25)) that a trace back to the thrower
    // reaches; fire: AreaAttack there (ovl11 +0x3270) and EndTurn, or with
    // nothing worth it ChooseWeapon(no area); else the cell's value.
    value = 0;
    const Bytes* w = weapon_of(c);
    if (!w) return false;
    const int type = (*w)[0x2E];
    const int kind = item_byte(type, 2);
    int k;
    if (kind == 7) k = 2;
    else if (kind == 8) k = 1;
    else if (kind == 9) k = 3;
    else return false;
    const bool gas = type == 0x25 || type == 0x26;     // CS:017F
    Character* centre = nullptr;
    int R = (weapon_range(c) + k) & 0xFF;
    if (R > 9) {
        R = 9;
        if (!gas) centre = c.cb_target;
    }
    const int r2 = (R - k) & 0xFF;
    int wt0, wt1;
    if (gas) {
        wt0 = c.b(0xD6) == 0 ? 4 : 1;
        wt1 = c.b(0xD6) == 0 ? 1 : 4;
    } else {
        wt0 = c.b(0xD6) == 0 ? -12 : 1;
        wt1 = c.b(0xD6) == 0 ? 1 : -2;
    }
    if (!centre) centre = &c;
    const int cx = pos_x(*centre), cy = pos_y(*centre);
    std::array<std::int8_t, 0x169> g{};
    auto cell = [&](int x, int y) -> std::int8_t& {
        const int i = ((x - cx) * 19 + (y - cy) + 0xB4);
        if (i < 0 || i >= 0x169) throw Error("AreaTarget (ovl7 +0x019F): a cell outside its grid");
        return g[static_cast<std::size_t>(i)];
    };
    for (const auto& p : party_) {
        Character& m = *p;
        const int mx = pos_x(m), my = pos_y(m);
        if (m.b(0xD5) == 0 || mx < s8(cx - R) || mx > s8(cx + R) || my < s8(cy - R) || my > s8(cy + R)) continue;
        int wt = m.b(0xD6) == 0 ? wt0 : m.b(0xD6) == 1 ? wt1 : 0x7FFF;
        if (wt == 0x7FFF) throw Error("AreaTarget (ovl7 +0x019F): side " + std::to_string(m.b(0xD6)) + " reads another local");
        if (gas) {
            const int t = field_tile_at(mx, my);       // MapAt ovl30 +0x06BC
            if (t == 1 || t == 4) wt = -1;
        } else if (type == 0x23) {
            if (has_effect(m, 0x0E) || has_effect(m, 1)) wt = 0;
        } else if (type == 0x22) {
            if (has_effect(m, 1)) wt = 0;
        }
        int jlo = -k, jhi = k;
        if (my + jlo < s8(cy - r2)) jlo = s8(cy - r2) - my;
        if (my + jhi > s8(cy + r2)) jhi = s8(cy + r2) - my;
        for (int j = jlo; j <= jhi; ++j) {
            const int h = s8(text_.ds8(0x05A6 + static_cast<std::size_t>(3 * j + k)));
            int a = mx - h, b = mx + h;
            if (a < s8(cx - r2)) a = s8(cx - r2);
            if (b > s8(cx + r2)) b = s8(cx + r2);
            for (int i = a; i <= b; ++i) cell(i, my + j) = static_cast<std::int8_t>(cell(i, my + j) + wt);
        }
    }
    const int sx = pos_x(c), sy = pos_y(c);
    int best = 0, bx = 0, by = 0;
    for (int dx = -r2; dx <= r2; ++dx)
        for (int dy = -r2; dy <= r2; ++dy) {
            const int s = g[static_cast<std::size_t>(dx * 19 + dy + 0xB4)];
            bool take = s > best;
            if (!take && s == best && s > 0) take = static_cast<int>(random(100)) < 25;   // Percent(25)
            if (!take) continue;
            int tx = sx, ty = sy, n = weapon_range(c);
            trace_flags_[3] = 1;                       // DS:38D9: tile 1 stops the line
            if (trace(cx + dx, cy + dy, tx, ty, n) && trace_flags_[0] == 0) {
                best = s;
                bx = dx;
                by = dy;
            }
        }
    if (fire) {
        if (best > 0) {
            bool tmp = false;
            area_attack(c, cx + bx, cy + by, tmp);     // ovl11 +0x3270
            return end_turn(c);
        }
        bool tmp = false;
        choose_weapon(c, false, tmp);
        return tmp;
    }
    if (best > 0) {
        int base;
        switch (type) {
            case 0x25: base = roll_dice(1, 6) + 1; break;
            case 0x26: base = roll_dice(1, 6); break;
            case 0x21: base = 0x28; break;
            case 0x22: base = 5; break;
            case 0x24: base = roll_dice(1, 10) + 10; break;
            case 0x23: base = roll_dice(1, 10) + 5; break;
            case 0x11: base = 0x19; break;
            case 0x12: base = 0x1E; break;
            default:
                throw Error("AreaTarget (ovl7 +0x019F, 0x3B95C): area type " + std::to_string(type) +
                            " reads an uninitialised base [D-0026]");
        }
        const int v = (best * base) & 0xFFFF;
        value = v > 0x80 ? ((v >> 4) + 0x78) & 0xFF : v & 0xFF;
    }
    return false;
}

}  // namespace cotd::game
