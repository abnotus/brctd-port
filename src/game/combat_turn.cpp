// SPDX-License-Identifier: GPL-3.0-or-later
// The fight's rounds and the player's turn: Combat.Run and its round
// loop (ovl6), initiative and Battle Tactics (ovl11), the turn list, a
// turn, the player's menu and the end of a round
// (docs/notes/combat_screens.md; main-session readings of session 19).
// Monsters' turns and Quick are the AI's (ovl7, game/combat_ai.cpp).
#include <algorithm>

#include "game/game.hpp"

namespace cotd::game {

namespace {

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }

std::size_t side_of(const Character& c) {
    const int side = s8(c.b(0xD6));
    if (side != 0 && side != 1) throw Error(c.name() + ": side " + std::to_string(side) + " (+0xD6) out of 0..1");
    return static_cast<std::size_t>(side);
}

}  // namespace

// --- Combat.Run ovl6 +0x00EF (0x39EEF..0x3A009) --------------------------------

void Game::combat_run() {
    mode_ = 5;            // DS:3BE8 := 5
    leadership();
    combat_setup();
    // 0x39F08: the counts from SetUp's CountSides.
    bool done = side_count_[0] == 0 || side_count_[1] == 0;
    if (!done) f82f1_ = 1;
    while (!done) {
        count_sides();                                 // 0x39F36
        tactics_round();                               // 0x39F3B
        for (const auto& c : party_) initiative_of(*c);   // 0x39F5A, list order
        v1(0x7ECB) = 0;                                // 0x39F75: surprise is over
        build_turn_list();                             // 0x39F7C
        turn_idx_ = 1;
        Character* cur = turn_list_[1];
        while (cur && !done) {
            combat_turn(*cur);                         // 0x39FA5
            turn_idx_ = static_cast<std::uint8_t>(turn_idx_ + 1);
            cur = turn_idx_ > 0x48 ? nullptr : turn_list_[turn_idx_];
            count_sides();                             // 0x39FD4
            if (side_count_[0] == 0 || side_count_[1] == 0 || round_ >= round_limit_) done = true;
        }
        end_of_round();                                // 0x39FF7
    }
    combat_cleanup();                                  // 0x39FFE
    typing_ = 1;                                       // 0x3A001: DS:3BF8 (the typewriter)
}

void Game::count_sides() {
    // CountSides ovl23 +0x1A6A (0x5797A): the active records per side.
    side_count_ = {0, 0};
    for (const auto& c : party_)
        if (c->b(0xD5) != 0) ++side_count_[side_of(*c)];
}

// --- the round's set-up -------------------------------------------------------

void Game::tactics_round() {
    // Battle Tactics ovl11 +0x45FA (0x44EFA): row 23 cleared; per record in
    // list order that is active, Okay / dodging / jetpacked (set CS:45BB),
    // without effect 0x0E and of a side that hasn't succeeded yet: a
    // SkillCheck (skill 0x1F, difficulty 2); a success marks the side
    // (DS:6BD3 + side) and shows the success message (the name, CS:45DB,
    // CS:45E3 or 45E7 by +0x26, CS:45EB) in the side panel
    // (0x44FA2..0x45084), then Game.Delay. The rule:
    // combat::battle_tactics.
    clear_cells(0, 0x17, 0x27, 0x17);
    tactics_won_ = {0, 0};
    for (const auto& p : party_) {
        Character& r = *p;
        if (r.b(0xD5) == 0) continue;
        const int st = r.b(0xD4);
        if (st != 1 && st != 0x0A && st != 0x0D) continue;
        if (has_effect(r, 0x0E)) continue;
        const std::size_t side = side_of(r);
        if (tactics_won_[side]) continue;
        combat::Rng rng(seed_);
        const int res = combat::skill_check({skill_total(r, 0x1F)}, 2, rng).result;
        seed_ = rng.seed();
        data_note("skill", 0x1F, res);
        if (res < 2) continue;
        std::string msg = r.name() + text_[Text::CbMakes];
        msg = msg.substr(0, 40) + text_[r.b(0x26) == 0 ? Text::CbHis : Text::CbHer];
        msg = msg.substr(0, 40) + text_[Text::CbTacticsRoll];
        msg = msg.substr(0, 40);
        tactics_won_[side] = 1;
        f6bd9_ = 1;
        combat_panel(r);
        name_message(r, msg, 10, false);
        game_delay();   // Game.Delay
    }
}

void Game::initiative_of(Character& c) {
    // Initiative ovl11 +0x0000 (0x40900..0x40A22); the dice: combat::initiative.
    c.cb[0] = 1;
    c.cb[6] = 0;
    c.cb[2] = 2;
    recount_attacks(c);                                // 0x40932
    f698e_ = c.b(0x3C);                                // DS:698E := +0x3C
    effect_hook(c, 0x12, "Initiative (0x40947)");
    const int rate = f698e_;
    c.b(0xDC) = static_cast<std::uint8_t>((rate + (round_ & 1)) >> 1);
    combat::Rng rng(seed_);
    const int adj = s8(text_.ds8(0x0CB9 + c.b(0x11)));   // ovl23 +0x077D: DS:0CB9[DEX]
    const combat::Initiative v = combat::initiative(c.rec, adj, v1(0x7ECB), rng);
    seed_ = rng.seed();
    c.cb[1] = static_cast<std::uint8_t>(v.cb1);
    if (c.b(0xD5) != 0) c.cb[5] = static_cast<std::uint8_t>(v.cb5);
    c.cb[3] = static_cast<std::uint8_t>(calc_move(c));   // 0x40A0E
}

void Game::recount_attacks(Character& c) {
    // ovl11 +0x0B02 (0x41402): the attacks this round from the readied
    // weapon's rate (ITEMS byte 1) or +0x3B, halved with the odd round's
    // half (DS:6B7D and 1), capped by the ammunition when the weapon's
    // kind isn't 0; +0xDB never grows once the record has attacked this
    // round (cb+6).
    const int old = c.b(0xDB);
    const Bytes* w = weapon_of(c);
    f698e_ = static_cast<std::uint8_t>(w ? item_byte((*w)[0x2E], 1) : c.b(0x3B));   // DS:698E
    effect_hook(c, 0x12, "Recount (0x41456)");
    int rate = f698e_;
    rate = ((rate + (round_ & 1)) >> 1) & 0xFF;
    if (w && item_byte((*w)[0x2E], 2) != 0 && rate > (*w)[0x38]) rate = (*w)[0x38];
    if (c.cb[6] != 0 && rate >= old) rate = old;
    c.b(0xDB) = static_cast<std::uint8_t>(rate);
}

void Game::build_turn_list() {
    // BuildTurnList ovl6 +0x020A (0x3A00A..0x3A184): every record, in
    // list order, inserted before the first entry with a lower cb+1
    // (signed; an empty slot counts as -1) or an equal cb+1 and a lower
    // cb+5 (unsigned), at most at index 0x41; the entries after it move
    // down one until an empty one, an entry pushed past 0x48 is lost.
    turn_list_.fill(nullptr);
    for (const auto& p : party_) {
        Character* r = p.get();
        const int key = s8(r->cb[1]);
        int i = 1;
        while (i < 0x41) {
            const Character* o = turn_list_[static_cast<std::size_t>(i)];
            const int other = o ? s8(o->cb[1]) : -1;
            if (key > other) break;
            if (key == other && o->cb[5] < r->cb[5]) break;
            ++i;
        }
        Character* carry = turn_list_[static_cast<std::size_t>(i)];
        turn_list_[static_cast<std::size_t>(i)] = r;
        for (int j = i; carry && j < 0x48;) {
            ++j;
            std::swap(carry, turn_list_[static_cast<std::size_t>(j)]);
        }
    }
}

// --- a turn: ovl6 +0x0385 (0x3A185..0x3A2F0) -----------------------------------

void Game::combat_turn(Character& c) {
    c.cb[0x0D] = 0;
    c.cb[0x0E] = 0;
    c.cb[4] = 0;                                       // a guard ends
    if (c.b(0xD4) == 0x0A || c.b(0xD4) == 0x0C) c.b(0xD4) = 1;
    effect_hook(c, 7, "Turn (0x3A1E2)");
    effect_remove(c, 2, "Turn (0x3A1F6)");
    if (s8(c.cb[1]) <= 0) return;                      // 0x3A203
    if (c.cb[1] == 0x14) c.cb[1] = 0x13;               // Alt-Q's re-entry
    cur_ = &c;                                         // CurChar DS:4EC1
    note_transcript("[turn " + c.name() + " at " + std::to_string(pos_x(c)) + "," + std::to_string(pos_y(c)) +
                          ", round " + std::to_string(round_) + "]");   // a debugging note
    f6bda_ = (c.b(0xD6) == 0 || icon_visible(&c, false)) ? 1 : 0;
    focus_on(c, 2, true);                              // 0x3A267
    recalc(c);
    f6bd9_ = 1;
    combat_panel(c);
    zero_g(c);                                         // 0x3A28D
    effect_hook(c, 0x0F, "Turn (0x3A29A)");
    if (s8(c.cb[1]) > 0) {
        if (c.b(0xD7) != 0) ai_turn(c);                // ovl7 +0x000F
        else player_menu(c);
    }
    field_redraw_square(pos_x(c), pos_y(c));          // 0x3A2E8, no Present
}

void Game::zero_g(Character& c) {
    // ZeroG ovl11 +0x47CE (0x450CE): only in zero gravity (var 0x4BE6
    // bit 4), for an active record with cb+1 > 1: SkillCheck of skill
    // 0x43 (difficulty 2) for it alone; its message ("made"/"failed" +
    // the record's pronoun by +0x26, 10 characters at most) and the roll's
    // label at (0x17, 0x0C) without a clear, then Delay. A failure: status
    // 0x0C (back to 1 at the round's end), a quarter of the movement left,
    // Recalc.
    if (!(v0(0x4BE6) & 0x10) || c.b(0xD5) == 0 || s8(c.cb[1]) <= 1) return;
    combat::Rng rng(seed_);
    const int res = combat::skill_check({skill_total(c, 0x43)}, 2, rng).result;
    seed_ = rng.seed();
    data_note("skill", 0x43, res);
    const bool ok = res >= 2;
    const int sex = c.b(0x26);
    // 0x45155..0x451CB: "his" or "her" appended to the verb; any other
    // byte stores CS:47B8 in its place, the verb lost (session 51's audit;
    // no shipped record has one: an edited save's quirk, kept).
    const std::string msg = sex == 0 || sex == 1
        ? (text_[ok ? Text::ZgMade : Text::ZgFailed] + text_[sex == 0 ? Text::ZgHis : Text::ZgHer]).substr(0, 10)
        : text_[Text::ZgIts].substr(0, 10);
    name_message(c, msg, 10, false);
    print_in(0x17, 0x0C, 0x26, 0x15, 0x0A, false, text_[Text::ZgRoll]);
    game_delay();             // Game.Delay
    if (!ok) {
        c.b(0xD4) = 0x0C;
        c.cb[3] = static_cast<std::uint8_t>(c.cb[3] >> 2);
        recalc(c);
    }
}

bool Game::end_turn(Character& c) {
    // EndTurn ovl23 +0x1CAE (0x57BBE).
    c.cb[1] = 0;
    c.cb[4] = 0;
    c.cb[3] = 0;
    return true;
}

// --- the player's menu: ovl6 +0x0554 (0x3A354..0x3A6ED) -------------------------

void Game::player_menu(Character& c) {
    auto quick_on = [](Character& r) {   // ovl6 +0x1302 (0x3B102)
        r.b(0xD7) = 1;
        if (r.cb_target && r.cb_target->b(0xD6) == r.b(0xD6)) r.cb_target = nullptr;
    };
    if (c.b(0xD5) == 0) {
        end_turn(c);
        return;
    }
    bool done = false;
    while (!done) {
        highlight_ = 1;                                // DS:6B1D := 1 (0x3A379)
        const int k = combat_menu(c);
        const bool ext = k >= 0x100;
        const int key = k & 0xFF;
        if (!ext) {
            switch (key) {
                case 'M': move_cmd(c, done); break;          // 0x3A3B2
                case 'T': target_cmd(c, 0xFF, 1, can_fire(c), done); break;   // 0x3A3E0
                case 'V': {                                 // 0x3A3EC: ViewCharacter, Recount, redraw
                    // The turn's done flag is VIEW's exit flag: a heavy
                    // weapon readied in Gear ends the turn (ovl18 +0x1F12).
                    std::uint8_t flag = 0;
                    view_character(flag);
                    recount_attacks(c);
                    combat_redraw();
                    if (flag) done = true;
                    break;
                }
                case 'I':                                   // 0x3A40D
                    intimidate(c);
                    done = end_turn(c);
                    break;
                case 'Q':                                   // 0x3A42D
                    quick_on(c);
                    clear_row24();
                    flush_keys();                           // 0x3A43C
                    pause_ms(200);
                    done = true;
                    ai_turn(c);                             // ovl7 +0x000F
                    break;
                case 'A':                                   // 0x3A460
                    aid_cmd(c);
                    done = end_turn(c);
                    break;
                case 'S':                                   // 0x3A480: Sprint, then Move
                    if (c.b(0xD4) != 0x0B) {
                        c.b(0xD4) = 0x0B;
                        c.cb[3] = static_cast<std::uint8_t>(c.cb[3] * 2);
                    }
                    move_cmd(c, done);
                    break;
                case 'D':                                   // 0x3A4C9: Dodge
                    c.b(0xD4) = 0x0A;
                    done = end_turn(c);
                    break;
                case 'J':                                   // 0x3A4E3
                    jetpack(c);
                    done = c.cb[1] == 0;
                    break;
                case 'W': {                                 // 0x3A514: Wait, to the end of the list
                    c.cb[1] = 1;
                    int j = s8(turn_idx_);                  // DS:6EDB := DS:6CFB
                    while (j + 1 <= 0x48 && turn_list_[static_cast<std::size_t>(j + 1)] && j < 0x48) {
                        turn_list_[static_cast<std::size_t>(j)] = turn_list_[static_cast<std::size_t>(j + 1)];
                        ++j;
                    }
                    turn_list_[static_cast<std::size_t>(j)] = &c;
                    turn_idx_ = static_cast<std::uint8_t>(turn_idx_ - 1);
                    done = true;
                    break;
                }
                case 'G': done = guard_cmd(c); break;       // ovl11 +0x2A32
                case 'E': done = end_turn(c); break;
                case ' ':                                   // 0x3A5CF: the party's Quick off
                    for (const auto& p : party_)
                        if (p->b(0xD6) == 0 && p->b(0x4C) == 0 && p->cb.size() == 0x12 && p->cb[0x0F] == 0)
                            p->b(0xD7) = 0;
                    break;
                default: break;
            }
        } else if (key == 0x10 || key == 0x1E) {            // 0x3A639: Alt-Q / Alt-A
            turn_idx_ = static_cast<std::uint8_t>(turn_idx_ - 1);
            c.cb[1] = 0x14;
            for (const auto& p : party_) quick_on(*p);
            clear_row24();
            pause_ms(200);
            done = true;
        }
        // Alt-X (ext 0x2D, ovl11 +0x4036) acts only when ParamStr(1) is a
        // developer's password; the port is launched without one: nothing.
        if (!done) {                                         // 0x3A6B6
            focus_on(c, 2, true);
            f6bd9_ = 1;
            combat_panel(c);
        }
    }
}

int Game::combat_menu(Character& c) {
    // CombatMenu ovl6 +0x09A4 (0x3A7A4..0x3AAE9): two rows; returns the
    // key, 0x100 + scan for an extended one. It has no mouse code of its
    // own: clicks come through MenuScreen (screen.cpp, 0x5ADBF).
    clear_cells(0, 0x17, 0x27, 0x18);
    const int st = c.b(0xD4);
    std::string items;
    if (c.cb[3] > 0) items += text_[Text::CbMove];
    items += text_[Text::CbTargetView];
    if (c.b(0x94) > 0 && collect_enemies(c, 1) > 0 && st != 0x0A && st != 0x0C) items += text_[Text::CbMenuIntim];
    items += text_[Text::CbQuick];
    if (can_aid(c)) items += text_[Text::CbAid];
    if (!items.empty()) items.back() = '@';            // 0x3A8EC: the row break
    if (c.cb[3] > 0 && st != 0x0A) {
        if (st != 0x0B) {
            if (st != 0x0D && st != 0x0C) items += text_[Text::CbSprint];
            items += text_[Text::CbDodge];
        }
        // 0x3A986: Jetpack with a readied class-2 item, on a station (var
        // 0x4BE6 in {0xA2, 0xA8}, set CS:092A).
        const int env = v0(0x4BE6);
        if (st != 0x0D && find_readied(c, 2) && (env == 0xA2 || env == 0xA8)) items += text_[Text::CbJetpack];
    }
    items += text_[Text::CbWait];
    if (st != 0x0B && st != 0x0A) items += text_[Text::CbGuard];
    items += text_[Text::CbEnd];
    for (;;) {
        const MenuKey k = menu_screen("", items, true);   // 0x3AA93
        int key = k.key;
        if (k.ext && key != 0x10 && key != 0x13 && key != 0x1E && key != 0x2D && key != 0x32) key = 0;   // CS:0964
        static constexpr char kTaken[] = "ADEGIJMQSTVW ";   // CS:0984 (with 0x10, 0x13, 0x1E, 0x2D, 0x32)
        const bool ok = k.ext ? key != 0 : (key != 0 && std::string(kTaken).find(static_cast<char>(key)) != std::string::npos);
        if (ok) {
            clear_cells(0, 0x17, 0x27, 0x18);
            return k.ext ? 0x100 + key : key;
        }
    }
}

bool Game::can_aid(Character& c) {
    // AidAvailable ovl18 +0x2C18 (0x52BB8): someone on the party side
    // (cb+0x0F = 0) next to the record (in combat) that is dying while it
    // has the skills +0x59 or +0x5D, or is active with effect 0x1D or 0x0E
    // while it has +0x61; the list of them (DS:387F + n) is Aid's.
    aid_list_.clear();
    // The positions count only in combat (state 5); in a ship fight the
    // original reads icon entry 0 for them and ignores the result.
    const int x = mode_ == 5 ? pos_x(c) : 0, y = mode_ == 5 ? pos_y(c) : 0;
    for (const auto& p : party_) {
        Character& r = *p;
        if (r.b(0xD6) != 0 || r.cb.size() != 0x12 || r.cb[0x0F] != 0) continue;
        const bool wants = (r.b(0xD4) == 4 && (c.b(0x59) > 0 || c.b(0x5D) > 0)) ||
                           (r.b(0xD5) != 0 && (has_effect(r, 0x1D) || has_effect(r, 0x0E)) && c.b(0x61) > 0);
        // A record never placed this fight (a COMBAT_SOLO onlooker) keeps
        // its icon entry's square from an earlier fight (icon_xy_).
        if (mode_ == 5) {
            const int dx = s8(pos_x(r) - x), dy = s8(pos_y(r) - y);
            if (!(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1)) continue;
        }
        if (wants) aid_list_.push_back(&r);
    }
    return !aid_list_.empty();
}

void Game::aid_cmd(Character& c) {
    // Aid ovl18 +0x2D90 (0x52D30): every dying neighbour bandaged (status
    // 5, no roll); every neighbour with effect 0x1D or 0x0E treated on a
    // SkillCheck (skill 0x15) of the aider, both effects removed.
    if (!can_aid(c)) return;
    const std::vector<Character*> list = aid_list_;
    for (Character* m : list) {
        // The list holds IndexOf(rec) (0x52CDB) and Aid reads the record
        // back from DS:6745 + 4k (0x52D72). In a ship fight a member with
        // no combat icon (no fight yet since the load) gives k = 0, entry 0
        // is nil, and the original works on 0000:0000: measured (run
        // s65_aid_dump), its +0xD4 is not 4 and no effect is found, so
        // nothing changes (D-0087).
        if (mode_ != 5 && icon_index(m) == 0) {
            combat::diag("D-0087", "Aid (ovl18 +0x2D90, 0x52D72) in a ship fight on " + m->name() +
                                       ", who has no combat icon: the original reads icon entry 0 (nil); nothing done");
            continue;
        }
        if (m->b(0xD4) == 4 && (c.b(0x59) > 0 || c.b(0x5D) > 0)) {
            m->b(0xD4) = 5;
            m->cb[0x0C] = 0;
            name_message(*m, text_[Text::CbBandaged], 10, true);
            game_delay();
        } else if (has_effect(*m, 0x1D) || has_effect(*m, 0x0E)) {
            combat::Rng rng(seed_);
            const int r = combat::skill_check({skill_total(c, 0x15)}, 2, rng).result;
            seed_ = rng.seed();
            data_note("skill", 0x15, r);
            if (r >= 2) {
                name_message(*m, text_[Text::CbTreated], 10, true);
                effect_remove(*m, 0x1D, "Aid (0x52E5A)");
                effect_remove(*m, 0x0E, "Aid (0x52E6C)");
            }
        }
    }
}

void Game::intimidate(Character& c) {
    // The I command ovl11 +0x426A (0x44B6A): a SkillCheck (skill 0x48); on a
    // success every adjacent enemy in sight that isn't helpless saves
    // (type 0) or gets effect 2 (10 rounds: it loses its turn).
    const int n = collect_enemies(c, 1);
    if (n == 0) return;
    std::vector<int> ks;
    for (int i = 1; i <= n; ++i) ks.push_back(scan_[static_cast<std::size_t>(i)].k);   // DS:6B7F + i
    combat::Rng rng(seed_);
    const int r = combat::skill_check({skill_total(c, 0x48)}, 2, rng).result;
    seed_ = rng.seed();
    data_note("skill", 0x48, r);
    if (r < 2) {
        show_message(c.name() + text_[Text::CbFailsIntim]);
        return;
    }
    show_message(c.name() + text_[Text::CbMenacing]);
    for (int k : ks) {
        Character* e = icons_[static_cast<std::size_t>(k - 1)].rec;
        if (!e || has_effect(*e, 1)) continue;
        const bool saved = save(*e, 0, 0);
        apply_effect(*e, 2, 10, 0x0C, 0, true, saved, text_[Text::CbIntimMsg]);
    }
}

void Game::apply_effect(Character& c, int type, int dur, int p3, int p4, bool only_if_failed, bool saved,
                        const std::string& msg) {
    // Effect.Apply ovl22 +0x0B13 (0x55B63): hook 9 may make the record
    // immune; a save with only_if_failed: the immune message (CS:0B05); an existing
    // effect of the type gets the longer duration (a permanent one stays),
    // else a node is appended; then the message (EffectMessage ovl23
    // +0x171A, k = 0: NameMessage with its delay) and ClearText.
    f698b_ = static_cast<std::uint8_t>(type);          // DS:698B (0x55B7F)
    effect_hook(c, 9, "Effect.Apply (0x55B75)");
    type = f698b_;
    if (type == 0 || (saved && only_if_failed)) {
        name_message(c, text_[Text::CbImmuneMsg], 10, true);
        return;
    }
    data_note("effect", type, c.b(0xD6));              // tools only: --data-trace
    bool found = false;
    for (Bytes& e : c.effects)
        if (!e.empty() && e[0] == type) {
            const int d = e[1] | (e[2] << 8);
            if (d != 0 && dur > d) {
                e[1] = static_cast<std::uint8_t>(dur);
                e[2] = static_cast<std::uint8_t>(dur >> 8);
            }
            found = true;
            break;
        }
    if (!found)
        // Effect.Add ovl22 +0x0720 (0x55770): a 9-byte node at the list's end: type, duration, +3, +4, next nil.
        c.effects.push_back(Bytes{static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(dur),
                                  static_cast<std::uint8_t>(dur >> 8), static_cast<std::uint8_t>(p3),
                                  static_cast<std::uint8_t>(p4), 0, 0, 0, 0});
    if (!msg.empty()) {
        effect_message(c, type == 0x0E ? 0x11 : 0, msg.substr(0, 40));   // 0x55C1D
        clear_text();
    }
}

// --- the end of a round: ovl6 +0x0CEC (0x3AAEC..0x3AB7D) ------------------------

void Game::end_of_round() {
    for (const auto& p : party_)
        if (p->b(0xD4) == 0x0B || p->b(0xD4) == 0x0D) p->b(0xD4) = 1;   // sprint, jetpack end
    pass_time(1, 1);                                   // 0x3AB3D, with TimedEffects
    round_ = static_cast<std::uint8_t>(round_ + 1);    // 0x3AB42
    enemy_hp_percent();
    area_age();                                        // ovl22 +0x0CC3
    dying_round();                                     // ovl23 +0x30F3
    field_view(s8(field_ox_ + 3), s8(field_oy_ + 3), 0xFF, 8);   // 0x3AB75
}

void Game::dying_round() {
    // Dying ovl23 +0x30F3 (0x59003): each dying party-side record (cb+0x0F
    // = 0) counts a round (cb+0x0C); past 9 it falls into a coma (status
    // 8) with a message; then one message for those still dying.
    int dying = 0;
    for (const auto& p : party_) {
        if (p->b(0xD4) != 4 || p->cb.size() != 0x12 || p->cb[0x0F] != 0) continue;
        p->cb[0x0C] = static_cast<std::uint8_t>(p->cb[0x0C] + 1);
        if (p->cb[0x0C] > 9) {
            p->b(0xD4) = 8;
            show_message(p->name() + text_[Text::DyComa]);   // ovl23 +0x0A18
        } else {
            ++dying;
        }
    }
    if (dying > 0)
        show_message(text_[Text::DyHead] + text_[dying > 1 ? Text::DyPlural : Text::DySingular] + text_[Text::DyTail]);
}

void Game::enemy_hp_percent() {
    // ovl11 +0x2CD7 (0x435D7): the enemies' HP left, in steps of 5 %, into
    // DS:6BC9 (words; the product's low word divided).
    std::uint16_t cur = 0, max = 0;
    for (const auto& p : party_) {
        if (p->b(0xD6) != 1) continue;
        if (p->b(0xD5) != 0) cur = static_cast<std::uint16_t>(cur + p->w(0xE3));
        max = static_cast<std::uint16_t>(max + p->w(0x45));
    }
    if (max > 0) enemy_hp_pct_ = static_cast<std::uint8_t>(((static_cast<std::uint16_t>(cur * 20) / max) * 5) & 0xFF);
}

void Game::combat_cleanup() {
    // Cleanup ovl6 +0x0020 (0x39E20): the area effects freed, the
    // statuses 0x0A..0x0E back to 1 (the set at ovl6 CS:0000), the
    // battlefield freed, DS:6BD3 := 0 (not DS:6BD4), the backbuffer back
    // to 200 x 40.
    areas_.clear();
    below_on_ = false;
    for (const auto& p : party_)
        if (p->b(0xD4) >= 0x0A && p->b(0xD4) <= 0x0E) p->b(0xD4) = 1;
    tactics_won_[0] = 0;
    for (std::size_t k = 0; k < icons_.size() && k + 1 < icon_xy_.size(); ++k)
        icon_xy_[k + 1] = {icons_[k].x, icons_[k].y};   // DS:60B3 keeps them
}

// --- effects (ovl22) -------------------------------------------------------------

bool Game::has_effect(const Character& c, int type) const {
    for (const Bytes& e : c.effects)
        if (!e.empty() && e[0] == type) return true;
    return false;
}

void Game::effect_hook(Character& c, int event, const char* where) {
    // Effect.Hook ovl22 +0x021D: the types each event checks
    // (docs/notes/combat.md "Effects") and their handlers (ovl10); a type
    // whose handler isn't ported stops the engine (no shipped monster
    // holds one: 0x1F; 0x11's is empty, session 61).
    static const std::vector<std::pair<int, std::vector<int>>> kTypes = {
        {3, {0x1C, 0x1E}}, {5, {0x14, 0x19, 0x12, 0x17, 0x18, 0x1A, 0x1B, 3}}, {6, {0x17, 3}}, {7, {1, 2, 0x0E}},
        {9, {0x16, 0x15, 0x13, 0x10, 3}}, {10, {0x0D}}, {0x0B, {0x0D}}, {0x0C, {0x0D}}, {0x0E, {0x20, 0x1F, 0x11}},
        {0x0F, {0x0E, 2}}, {0x12, {0x1D}}, {0x16, {0x1D}}};
    for (const auto& [ev, types] : kTypes) {
        if (ev != event) continue;
        for (int t : types) {
            // Effect.HookType ovl22 +0x01B8 (0x55208): the first node of type t, then its handler.
            if (!has_effect(c, t)) continue;
            effect_handler(c, t, event, where, nullptr);
        }
    }
}

void Game::effect_handler(Character& c, int t, int event, const char* where, const Bytes* node) {
    // A type's handler (ovl10, table DS:699D), from Effect.Hook or from
    // Effect.Remove (node: the node removed, whose +3 0x1C and 0x1E read;
    // the hook's is the first of its type). The handlers don't see the
    // event; it names the hook in the unported message only.
    auto bonus_of = [&](int type) {
        if (node) return static_cast<int>(static_cast<std::int8_t>((*node)[3]));
        for (const Bytes& e : c.effects)
            if (!e.empty() && e[0] == type) return static_cast<int>(static_cast<std::int8_t>(e[3]));
        return 0;
    };
    if (t == 0x0D) {
        // ovl10 +0x00BB (0x4001B): 4 off the attack roll DS:6995 and
        // the save total DS:698A (combat_rules.cpp applies those
        // where they are read) and off the holder's AC +0xD9/+0xDA.
        c.b(0xD9) = static_cast<std::uint8_t>(c.b(0xD9) - 4);
        c.b(0xDA) = static_cast<std::uint8_t>(c.b(0xDA) - 4);
        return;
    }
    if (t == 3) {
        // ovl10 +0x0084 (0x3FFE4): the type being applied DS:698B
        // := 0 (Effect.Apply then finds it cancelled), and the
        // damage DS:698C capped at the holder's HP (its low byte).
        f698b_ = 0;
        if (dmg_ > c.w(0xE3)) dmg_ = static_cast<std::uint8_t>(c.w(0xE3));
        return;
    }
    if (t == 0x1C) {
        // ovl10 +0x0460 (0x403C0), on the attacker after a hit:
        // its target (cb+8, DS:699B) saves (type 1, bonus = the
        // node's +3) against effect 0x1D for good (only if failed),
        // with its message, Delay; then the 0x1D message if it
        // holds 0x1D now.
        Character* t2 = c.cb_target;
        if (!t2) throw Error("effect 0x1C (ovl10 +0x0460) on " + c.name() + " without a target");
        const int bonus = bonus_of(0x1C);
        const bool saved = save(*t2, 1, bonus);
        apply_effect(*t2, 0x1D, 0, 0, 0, true, saved, text_[Text::FxPoisonA]);
        game_delay();     // Game.Delay
        if (has_effect(*t2, 0x1D)) name_message(*t2, text_[Text::FxDizzy], 10, true);
        return;
    }
    if (t == 0x1E) {
        // ovl10 +0x0547 (0x404A7), on the attacker after a hit: its
        // target (cb+8, DS:699B) saves (type 1, bonus = the node's
        // +3); a failure kills it with status 7 (ovl22 +0x0000).
        Character* t2 = c.cb_target;
        if (!t2) throw Error("effect 0x1E (ovl10 +0x0547) on " + c.name() + " without a target");
        const int bonus = bonus_of(0x1E);
        if (!save(*t2, 1, bonus)) kill_with(*t2, 7, text_[Text::FxPoisoned]);
        return;
    }
    if (t == 0x1D) {
        // ovl10 +0x050E (0x4046E), unless on a removal: the attack
        // rate DS:698E and the movement DS:698F halved (signed;
        // DS:698F is CalcMove's last result, written again before
        // it is read).
        f698e_ = static_cast<std::uint8_t>(f698e_ / 2);
        return;
    }
    if (t == 1 || t == 2 || t == 0x0E || (t >= 5 && t <= 0x0C)) {
        // ovl10 +0x0044 (0x3FFA4), +0x0060 (0x3FFC0, not on a
        // removal), +0x00EC (0x4004C): EndTurn. The handlers don't
        // see the event: hooks 7 and 0x0F at a turn's start. Types
        // 5..0x0C have type 1's handler (InitHandlers fills DS:69B1..
        // 69CD with 0082:003E, 0x407C2..0x407EE); no hook checks them.
        // ovl10 +0x0026 (0x3FF86): the helper behind +0x0044 and +0x00EC: EndTurn on the holder (ovl23 +0x1CAE).
        end_turn(c);
        return;
    }
    if (t == 0x11) {
        // ovl10 +0x0128 (0x40088): an empty handler (push bp .. lret 0Ah); forced
        // by route s77_effect_11. Types 4 and 0x0F have one too, but Effect.Hook
        // tests neither (0x5526D..): dead.
        return;
    }
    if (t == 0x13) {
        // ovl10 +0x0174 (0x400D4): effect 1 being applied is
        // cancelled (ovl10 +0x0000 with 0: DS:698C, 698B, 6995 := 0).
        if (f698b_ == 1) {
            dmg_ = 0;
            f698b_ = 0;
        }
        return;
    }
    if (t == 0x10) {
        // ovl10 +0x0111 (0x40071, main session 30): effect 2 being
        // applied (the combat menu's I command) is cancelled, as 0x13's.
        if (f698b_ == 2) {
            dmg_ = 0;
            f698b_ = 0;
        }
        return;
    }
    if (t == 0x15 || t == 0x16) {
        // Hook 9 with CurChar the one applying: its readied weapon
        // (record +0xEB) decides. 0x15 (ovl10 +0x01CE, 0x4012E): item
        // type 0x24; 0x16 (+0x0204, 0x40164): a kind-6 weapon or item
        // type 0x23. Either: cancelled (0x3FF60 with 0).
        Character* a = cur_;
        const Bytes* w = a ? weapon_of(*a) : nullptr;
        const int type = w ? (*w)[0x2E] : -1;
        const bool hit = t == 0x15 ? type == 0x24 : (w && (item_byte(type, 2) == 6 || type == 0x23));
        if (hit) {
            dmg_ = 0;
            f698b_ = 0;
        }
        return;
    }
    if (t == 0x12 || t == 0x14 || t == 0x17 || t == 0x18 || t == 0x19 || t == 0x1A || t == 0x1B) {
        // Hook 5 (DamageRoll's target, CurChar the attacker): the
        // handlers read CurChar's readied weapon's kind (ITEMS
        // byte 2). Their cancel (ovl10 +0x0000 with 0) zeroes the
        // damage DS:698C, DS:698B and the roll DS:6995.
        Character* a = cur_;
        const Bytes* w = a ? weapon_of(*a) : nullptr;
        const int kind = w ? item_byte((*w)[0x2E], 2) : -1;
        if (t == 0x12 || t == 0x14 || t == 0x17 || t == 0x18) {
            // A weapon kind does nothing: 0x12 kind 4 (ovl10 +0x0131,
            // 0x40091), 0x14 kind 2 (+0x018B, 0x400EB), 0x17 kinds 5
            // and 9 (+0x0251, 0x401B1), 0x18 kind 3 (+0x02A0, 0x40200).
            const bool hit = t == 0x12 ? kind == 4 : t == 0x14 ? kind == 2 : t == 0x18 ? kind == 3
                                                                                 : (kind == 5 || kind == 9);
            if (hit) {
                dmg_ = 0;
                f698b_ = 0;
            }
        } else if (t == 0x1A) {
            // ovl10 +0x037F (0x402DF): kinds 1..5 from beyond one
            // square: half the time no damage (DS:698C only).
            if (kind >= 1 && kind <= 5 && distance(*a, c) > 1 && percent(50)) dmg_ = 0;
        } else if (t == 0x1B) {
            // ovl10 +0x03FE (0x4035E): a kind-0 weapon: half the time
            // no damage.
            if (kind == 0 && percent(50)) dmg_ = 0;
        } else if (kind == 1 && distance(*a, c) > 1 && percent(50)) {
            // ovl10 +0x02ED (0x4024D): from beyond one square, a
            // kind-1 weapon is dodged half the time; one hit less.
            name_message(c, text_[Text::FxDodge], 10, true);
            dmg_ = 0;
            slot_hits_[1] = static_cast<std::uint8_t>(slot_hits_[1] - 1);   // DS:6998
        }
        return;
    }
    if (t == 0x20) {
        // ovl10 +0x0664 (0x405C4..0x40788), hook 0x0E (Act's start):
        // the acid spit (MON0SPC: monsters 35 and 40). Only at the
        // holder's target (cb+8, DS:699B) when it is set, within 12
        // squares, active and on the other side. DS:6EDA := 1 is
        // dead: its one reader (0x116A6) clears it first (0x115F6).
        Character* t2 = c.cb_target;
        if (!t2 || distance(c, *t2) > 12 || t2->b(0xD5) == 0 || t2->b(0xD6) == c.b(0xD6)) return;
        combat_panel(c);
        c.cb[7] = static_cast<std::uint8_t>(dir_rec(c, *t2));
        face(c, c.cb[7], 1, false);
        name_message(c, text_[Text::FxSpit], 10, false);
        sound(text_.ds16(0x21E2));                 // 0x406C7 (ovl10 +0x0767): 3
        marker_four(0x12);
        anim_icon(pos_x(c), pos_y(c), pos_x(*t2), pos_y(*t2), 4, 0x2D);
        // Percent(30): RollDice(2, 8), TakeDamage, then AttackResult
        // with hit = 1 takes it again: the damage counts twice and
        // shows once (a deterministic quirk, reproduced).
        if (percent(30)) {
            dmg_ = static_cast<std::uint8_t>(roll_dice(2, 8));
            take_damage(*t2, dmg_);
        } else {
            dmg_ = 0;
        }
        behind_ = 0;                               // DS:6BD5
        attack_result(c, *t2, dmg_, dmg_, dmg_ > 0);
        return;
    }
    if (event < 0)
        throw Error("removing effect " + hex(static_cast<std::uint32_t>(t), 2) + " from " + c.name() + " (" + where +
                    "): its handler's removal (ovl22 0x55246, type " + hex(static_cast<std::uint32_t>(t), 2) +
                    ") is not implemented in this engine yet");
    throw Error("effect hook " + hex(static_cast<std::uint32_t>(event), 2) + " for effect type " +
                hex(static_cast<std::uint32_t>(t), 2) + " on " + c.name() + " (" + where +
                ") is not implemented in this engine yet");
}

void Game::kill_with(Character& c, int status, const std::string& msg) {
    // ovl22 +0x0000 (0x55050): the message (NameMessage, fg 10); unless
    // the record is dead already (status 2 or 3): status := `status`,
    // inactive, HP 0, ClearOnDown (ovl22 +0x089D), hook 0x0D, the death
    // animation if still inactive; Delay, ClearText, the roster outside
    // combat (state 5).
    name_message(c, msg, 10, false);
    if (c.b(0xD4) == 3 || c.b(0xD4) == 2) return;
    c.b(0xD4) = static_cast<std::uint8_t>(status);
    c.b(0xD5) = 0;
    c.setw(0xE3, 0);
    clear_on_down(c);
    effect_hook(c, 0x0D, "ovl22 +0x0000 (0x550C1)");
    if (c.b(0xD5) == 0) death_anim(c);
    game_delay();             // Game.Delay
    clear_text();
    if (mode_ != 5) draw_roster();
}

void Game::removal_handler(Character& c, const Bytes& node, const char* where) {
    // Effect.Remove's handler call (0x5515A: node+4 set; only an edited
    // .SFX or MON*SPC sets it, every Effect.Apply passes 0: 0x40407,
    // 0x416E9, 0x42A76, 0x44C56, 0x55513). Dispatch 0x55246 calls the
    // type's ovl10 handler (DS:699D + 4 type, no bounds test) with the
    // removal flag as its last argument: types 2, 0x0D and 0x1D return at
    // once when it is set (0x3FFC6, 0x4001E, 0x40471); the others run as
    // their hook does, on the removed node's holder (session 61). A type
    // outside the table (0, past 0x20) or with 0x1F's handler (not ported)
    // is rejected when its file is read (check_effect_nodes).
    const int type = node[0];
    if (type == 0x02 || type == 0x0D || type == 0x1D) return;
    // Outside a fight the holder's +0xF3 is nil: EndTurn's writes go to
    // 0000:0001, 0003, 0004 (0x57BC4..0x57BE6), 0x1C, 0x1E and 0x20 read
    // their target at 0000:0008, and Distance (0x19, 0x1A) traces on a
    // freed field. Undefined; no shipped data sets node+4: rejected
    // (the quirk policy), naming the node.
    const bool end_turn_type = type == 1 || type == 0x0E || (type >= 5 && type <= 0x0C);
    const bool target_type = type == 0x1C || type == 0x1E || type == 0x20;
    bool distance_read = false;
    if (type == 0x19 || type == 0x1A) {
        const Bytes* w = cur_ ? weapon_of(*cur_) : nullptr;
        const int kind = w ? item_byte((*w)[0x2E], 2) : -1;
        distance_read = type == 0x19 ? kind == 1 : kind >= 1 && kind <= 5;
    }
    if ((end_turn_type || target_type || distance_read) &&
        (c.cb.size() != 0x12 || (distance_read && (!cur_ || cur_->cb.size() != 0x12))))
        throw Error(where + std::string(": removing ") + c.name() + "'s effect node of type " +
                    hex(static_cast<std::uint32_t>(type), 2) + " (+0x04 set, which only an edited .SFX or MON0SPC.DAX "
                    "node holds) outside a fight: its handler (ovl22 0x55246, DS:699D + 4 type) reads or writes "
                    "the combat block through nil (D-0073)");
    effect_handler(c, type, -1, where, &node);
}

void Game::effect_remove(Character& c, int type, const char* where) {
    // Effect.Remove ovl22 +0x00AF (0x550FF) with a nil node: the first
    // effect of the type is unlinked and freed; when its node+4 is set its
    // handler (0x55246) runs first with the removal flag.
    for (auto it = c.effects.begin(); it != c.effects.end(); ++it) {
        if (it->empty() || (*it)[0] != type) continue;
        if (it->size() > 4 && (*it)[4] != 0) removal_handler(c, *it, where);
        c.effects.erase(it);
        return;
    }
}

}  // namespace cotd::game
