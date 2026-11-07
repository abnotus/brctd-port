// SPDX-License-Identifier: GPL-3.0-or-later
// After a fight: ovl5 +0x20D8 and its parts (the outcome, the removal of
// the enemies, the medical phase, the XP report, the booty menu). The
// rules are combat_rules.cpp's (xp_total, medical, divvy, pool); this is
// their order and their screens. Session 19: main-session readings of
// 0x39C18..0x39DF5 (after combat), 0x37E25..0x38286 (outcome),
// 0x39AAB..0x39BD6 (removal), 0x382DC..0x3845E (XP report),
// 0x3979A..0x39AAA (booty menu), Medical's entry and close; the rest of
// Medical and XPTotal from a delegated reading (single).
#include "game/game.hpp"

namespace cotd::game {

int Game::cb_0f(const Character& c) {
    // cb+0x0F through +0xF3; with no combat block (no fight) the original
    // reads 0000:000F (D-0029): the reference machine's 0, or what D-0033
    // wrote there.
    if (c.cb.size() == 0x12) return c.cb[0x0F];
    return combat::side_of_record_without_block(low_);
}

void Game::after_combat() {
    // AfterCombat ovl5 +0x20D8 (0x39C18..0x39DF5). In the demo (DS:3BEA)
    // no Outcome, and nothing after Removal (0x39C3A, 0x39C49).
    mode_ = 6;
    xp_share_ = 0;                                     // DS:82EC
    v1(0x7EC7) = 0;
    f3586_ = 0;
    if (!demo_) combat_outcome();                      // ovl5 +0x02E5
    combat_removal();                                  // ovl5 +0x1F6B
    if (demo_) return;
    for (const auto& c : party_) recalc(*c);
    if (party_lost_) {
        // 0x39D3B (ovl5 +0x21FB): two lines, Pause; the common tail; the
        // booty freed only when it holds items (credits alone stay in
        // DS:6980 until CLEAR_MONSTERS: quirk, kept).
        v1(0x7EC7) = 0x80;
        layout_state0();                               // 015F:00D8
        col_ = 2;                                      // DS:5F3E/5F3F, overridden by the clear
        row_ = 6;
        print_in(2, 5, 0x25, 0x16, 0x0A, true, text_[Text::AcWiped1]);
        print_in(2, 7, 0x25, 0x16, 0x0A, true, text_[Text::AcWiped2]);
        pause_continue();                              // Text.Pause 0653:1291
        typing_ = 1;                                   // 0x39DAE: the common tail (session 51's audit)
        v1(0x7F70) = 0;
        v1(0x7F71) = 0;
        v1(0x7F72) = 0;
        v1(0x7EE3) = 0;
        v1(0x7EE6) = 0;
        if (!loot_.empty()) free_booty();
        return;
    }
    if (f3587_) free_booty();
    if (v0(0x4D7C) != 1) {                             // 0x39CA3
        if (v0(0x4B80) == 0) medical_phase();         // ovl5 +0x1449
        v0(0x4B80) = 0;
        if (v0(0x4D7C) == 2) f82f2_ = 1;
        if (f82f2_) ship_repair();                     // ovl5 +0x0CFE
        if (v0(0x4D7C) == 2) v0(0x4D7C) = 0;
        if (xp_share_ != 0) xp_report(xp_share_);      // ovl5 +0x079C
        if (!loot_.empty() || loot_value_ != 0) booty_menu();   // ovl5 +0x1C5A
    }
    free_booty();                                      // 0x39D35
    typing_ = 1;                                       // 0x39DAE: DS:3BF8 (the typewriter)
    // The fight's modifiers reset.
    v1(0x7F70) = 0;
    v1(0x7F71) = 0;
    v1(0x7F72) = 0;
    v1(0x7EE3) = 0;
    v1(0x7EE6) = 0;
}

void Game::free_booty() {
    // FreeBooty ovl5 +0x0000 (0x37B40).
    loot_.clear();
    loot_value_ = 0;
}

void Game::combat_outcome() {
    // Outcome ovl5 +0x02E5 (0x37E25..0x38286): who won, fled or was
    // destroyed; the XP and the booty; the survivors' statuses.
    for (const auto& p : party_)                       // set CS:02A5 = {1, 10, 11, 12}
        if (p->b(0xD4) == 1 || p->b(0xD4) == 10 || p->b(0xD4) == 11 || p->b(0xD4) == 12) {
            p->b(0xD4) = 1;
            p->b(0xD5) = 1;
        }
    f82eb_ = 0;
    party_lost_ = 1;
    f3587_ = 0;
    for (const auto& p : party_) {                     // 0x37E87: the party fled?
        if (cb_0f(*p) == 1 || f3587_) break;
        if (p->b(0xD4) == 6) f3587_ = 1;
    }
    for (const auto& p : party_)                       // 0x37ED3
        if (p->b(0xD5) != 0 && cb_0f(*p) != 1 && p->b(0xD6) == 0) {
            party_lost_ = 0;
            break;
        }
    f82f0_ = 0;
    if (f82ea_) {                                      // SOLO 0x381EB
        Character& c = *cur_;
        if (c.b(0xD5) == 1 && c.b(0xD4) == 1 && c.b(0xD6) != 1) {
            f82f0_ = 1;
            xp_total();
            add_xp(xp_share_);
            v1(0x7EC7) = 0;
        } else {
            v1(0x7EC7) = 0x80;
            if (c.b(0xD4) == 3 || c.b(0xD4) == 4) c.b(0xD4) = 5;   // set CS:02C5
            if (c.b(0xD4) == 6) {
                c.b(0xD4) = 1;
                c.b(0xD5) = 1;
            }
        }
        return;
    }
    for (const auto& p : party_) {                     // 0x37F61
        Character& c = *p;
        if (cb_0f(c) == 1) break;
        if (c.b(0xD4) == 6 || (c.b(0xD4) == 1 && c.b(0xD6) == 0 && c.b(0xE7) < 0x80)) party_lost_ = 0;
        if (c.b(0xD4) == 1) {
            f82f0_ = 1;
            f3587_ = 0;
        }
        if (c.b(0xD5) == 0) f82eb_ = static_cast<std::uint8_t>(f82eb_ + 1);
        for (int i = 1; i <= 5; ++i)                   // DS:2817.. = 2, 0x1D, 0x0E, 0x0D, 1
            effect_remove(c, text_.ds8(0x2816 + static_cast<std::size_t>(i)), "Outcome (0x38004)");
    }
    if (f82f0_) {                                      // 0x38022
        xp_total();
        add_xp(xp_share_);
        // Debug transcript only: who got the share (+0xD5) and who didn't.
        std::string got, missed;
        for (const auto& p : party_) {
            if (cb_0f(*p) == 1) break;
            if (p->b(0xD6) != 0) continue;
            std::string& s = p->b(0xD5) != 0 ? got : missed;
            s += (s.empty() ? "" : ",") + p->name() + (p->b(0xD5) != 0 ? "" : ":" + std::to_string(p->b(0xD4)));
        }
        note_transcript("[xp " + std::to_string(xp_share_) + " each to " + got + "; none to " + missed + "]");
    }
    if (party_lost_) {
        // 0x38182: every record up to the first block with +0x0F = 1 is
        // removed (each the list's head by then). The loop reads the next
        // pointer at 0x381CC from the record RemoveChar has just freed
        // (D-0036): TP 5.5's FreeMem leaves a freed block's bytes alone,
        // so it is the old successor, which the index walk gives.
        bool any = false;
        while (!party_.empty()) {
            Character& c = *party_.front();
            if (cb_0f(c) == 1) break;
            cur_ = &c;
            remove_char(false, true);                  // ovl13 +0x1C47
            any = true;
        }
        if (any)
            combat::diag("D-0036", "Outcome (ovl5 +0x02E5, 0x381CC): the destroyed party's records are walked through "
                                   "the next pointer of a freed record; the reference machine's value (the old "
                                   "successor) is used");
        v1(0x7F3E) = 0;
        return;
    }
    // 0x38054: the survivors (the party's records, up to the first block
    // with +0x0F = 1).
    for (std::size_t i = 0; i < party_.size();) {
        Character& c = *party_[i];
        if (cb_0f(c) == 1) break;
        if (!f3587_) {
            const int st = c.b(0xD4);
            if (st == 6) {
                c.b(0xD4) = 1;
                c.b(0xD5) = 1;
            } else if (st == 4) {
                if (v1(0x7EE6) != 0) {
                    c.b(0xD4) = 1;
                    c.b(0xD5) = 1;
                    c.setw(0xE3, 1);
                } else {
                    c.b(0xD4) = 5;
                }
            } else if (st == 5) {
                if (c.w(0xE3) > 0 || v1(0x7EE6) != 0) {
                    c.b(0xD4) = 1;
                    c.b(0xD5) = 1;
                    if (c.w(0xE3) == 0) c.setw(0xE3, 1);
                }
            }
            ++i;
        } else {                                       // 0x38132: the party fled
            v1(0x7EC7) = 0x81;
            if (c.b(0xD4) == 6) {
                c.b(0xD4) = 1;
                c.b(0xD5) = 1;
                ++i;
            } else {
                cur_ = &c;
                remove_char(false, true);              // the ones who stayed are lost
            }
        }
    }
}

void Game::xp_total() {
    // XPTotal ovl5 +0x005B (0x37B9B..0x37D7B; the rule: combat::xp_total,
    // xp_share, booty_credits): the defeated enemies' +0xB2, their credits
    // and copies of their items (not flagged 0x40; prepended, so the
    // booty list runs backwards) into the booty.
    std::uint32_t total = 0;
    for (const auto& p : party_) {
        Character& c = *p;
        if (c.b(0xD6) != 1 || c.b(0xD4) == 1 || cb_0f(c) == 0 || c.b(0xD4) == 6) continue;
        f3586_ = 1;
        total += c.w(0xB2);
        if (f82ea_) continue;
        if (cb_0f(c) == 1) loot_value_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(loot_value_) + c.d(0x2B));
        if (v1(0x7EE3) == 1) continue;
        for (Bytes& it : c.items) {
            if (it[0x34] & 0x40) continue;
            item_name(it);                             // ovl23 +0x0124: the node's name
            Bytes copy = it;
            copy[0x34] &= 0x7F;
            loot_.insert(loot_.begin(), copy);
        }
    }
    if (f82ea_) xp_share_ = static_cast<std::int32_t>(total);
    else xp_share_ = static_cast<std::int32_t>(combat::xp_share(total, v1(0x7F3E), f82eb_, false));
}

void Game::add_xp(std::int32_t xp) {
    // AddXP ovl5 +0x023E (0x37D7E).
    auto add = [&](Character& c) { c.setd(0x2F, c.d(0x2F) + static_cast<std::uint32_t>(xp)); };
    if (f82ea_) add(*cur_);
    else
        for (const auto& p : party_)
            if (p->b(0xD5) != 0) add(*p);
}

void Game::combat_removal() {
    // Removal ovl5 +0x1F6B (0x39AAB..0x39BD6): the enemies leave the list
    // (those of the other side only while var 0x4BAA is 0); the others'
    // combat blocks are freed.
    v1(0x7EC8) = 0;
    for (std::size_t i = 0; i < party_.size();) {
        Character& c = *party_[i];
        const int f = cb_0f(c);
        if (f == 1 || (c.b(0xD6) == 1 && v0(0x4BAA) == 0)) {
            f3586_ = 1;
            if (c.b(0xD5) != 1) v1(0x7EC8) = static_cast<std::uint8_t>(v1(0x7EC8) + 1);
            if (c.b(0xD4) == 6 && v1(0x7EC7) == 0) v1(0x7EC7) = 1;
            cur_ = &c;
            remove_char(cb_0f(c) == 1, true);          // ovl13 +0x1C47
        } else {
            c.cb.clear();
            c.cb_target = nullptr;
            ++i;
        }
    }
    cur_ = party_.empty() ? nullptr : party_.front().get();
}

// --- the medical phase: ovl5 +0x1449 (0x38F89..0x39712) --------------------------

void Game::medical_phase() {
    if (!f82f1_) return;
    f82f1_ = 0;
    bool nobody_treated = true, poison_heading = false, wound_heading = false, no_injuries = true, no_medics = true;
    layout_state0();
    set_window(1, 1, 0x26, 0x16, 0);
    auto check = [&](Character& m, int skill, int difficulty) {
        combat::Rng rng(seed_);
        const int r = combat::skill_check({skill_total(m, skill)}, difficulty, rng).result;
        seed_ = rng.seed();
        data_note("skill", skill, r);
        return r >= 2;
    };
    // Poison (0x38FEC): each medic (class 2, Okay) tries in turn.
    for (const auto& pp : party_) {
        Character& p = *pp;
        if (p.b(0xD4) != 7) continue;
        no_injuries = false;
        if (!poison_heading) {
            print_line(0, "");
            print_line(0x0D, text_[Text::AcPoisoning]);
            print_line(0, "");
            print_line(0x0F, text_[Text::AcColumns]);
            poison_heading = true;
        }
        bool found = false;
        for (const auto& mp : party_) {
            if (found) break;
            Character& m = *mp;
            if (m.b(0x28) != 2 || m.b(0xD4) != 1) continue;
            no_medics = false;
            draw_string(1, lp_row_, 0x0A, 0, m.name());
            draw_string(0x12, lp_row_, 0x0A, 0, p.name());
            if (check(m, 0x14, 2)) {
                found = true;
                draw_string(0x20, lp_row_, 0x0A, 0, text_[Text::AcCured]);
                p.b(0xD4) = 5;
                p.b(0xD5) = 0;
                p.setw(0xE3, 0);
                nobody_treated = false;
            }
            print_line(0, "");
        }
        if (!found) {
            show_message((p.name() + text_[Text::AcDyingPoison]).substr(0, 40));
            // Nested 0x38D63: the first record in the list carrying an
            // antidote (item type 0x1F) is asked; Yes uses one charge (the
            // last one removes the item). Only that record is asked.
            bool cured = false;
            for (const auto& hp : party_) {
                Character& h = *hp;
                int k = -1;
                for (std::size_t j = 0; j < h.items.size() && k < 0; ++j)
                    if (h.items[j][0x2E] == 0x1F) k = static_cast<int>(j);
                if (k < 0) continue;
                if (yes_no((text_[Text::AcAntidote] + p.name()).substr(0, 255) + text_[Text::AcQuestion], 1) == 'Y') {
                    Bytes& it = h.items[static_cast<std::size_t>(k)];
                    if (it[0x38] <= 1) remove_item(h, k);
                    else it[0x38] = static_cast<std::uint8_t>(it[0x38] - 1);
                    cured = true;
                }
                break;
            }
            if (cured) {                                // 0x391DE
                print_line(0x0A, p.name() + text_[Text::AcIsCured]);
                p.b(0xD4) = 5;
                p.b(0xD5) = 0;
                p.setw(0xE3, 0);
                nobody_treated = false;
            } else {                                    // 0x39227
                print_line(0x0C, p.name() + text_[Text::AcComa]);
                p.b(0xD4) = 8;
                p.b(0xD5) = 0;
            }
        }
    }
    print_line(0, "");                                 // 0x39274
    // Wounds (0x3928C): four tiers; only the successes are drawn.
    for (const auto& pp : party_) {
        Character& p = *pp;
        const int st = p.b(0xD4);
        bool done = !((st == 1 || st == 4 || st == 5) && p.b(0xD6) == 0 && p.w(0xE3) < p.w(0x45));   // CS:139E
        if (!done) no_injuries = false;
        for (int tier = 0; tier <= 3; ++tier) {
            for (const auto& mp : party_) {
                if (done) break;
                Character& m = *mp;
                if (m.b(0xD4) != 1 || m.b(0xD6) == 1 || m.w(0xE3) == 0) continue;
                int skill = 0, difficulty = 2;
                std::uint16_t h = 0;
                const std::uint16_t hp = p.w(0xE3);
                switch (tier) {
                    case 0:
                        skill = 0x0D;
                        h = static_cast<std::uint16_t>(roll_dice(1, 6));
                        if (static_cast<std::int32_t>(hp) + static_cast<std::int16_t>(h) > 6) h = static_cast<std::uint16_t>(6 - hp);
                        break;
                    case 1:
                        skill = 0x13;
                        difficulty = &m == &p ? 4 : 2;
                        h = static_cast<std::uint16_t>(roll_dice(3, 8) + 3);
                        break;
                    case 2:
                        skill = 0x12;
                        h = static_cast<std::uint16_t>(roll_dice(2, 8) + 1);
                        break;
                    default:
                        skill = 0x11;
                        h = static_cast<std::uint16_t>(roll_dice(1, 8));
                        break;
                }
                if (tier != 0) h = static_cast<std::uint16_t>(h << 1);
                const std::uint16_t room = static_cast<std::uint16_t>(p.w(0x45) - hp);
                if (static_cast<std::int32_t>(static_cast<std::int16_t>(h)) > static_cast<std::int32_t>(room)) h = room;
                if (m.b(0x4C + static_cast<std::size_t>(skill)) > 0) no_medics = false;
                if (static_cast<std::int16_t>(h) <= 0) continue;
                if (!check(m, skill, difficulty)) continue;
                if (!wound_heading) {                  // 0x394C3
                    wound_heading = true;
                    print_line(0x0D, text_[Text::AcWounds]);
                    print_line(0, "");
                    print_line(0x0F, text_[Text::AcColumns]);
                    print_line(0, "");
                }
                draw_string(1, lp_row_, 0x0A, 0, m.name());
                draw_string(0x12, lp_row_, 0x0A, 0, p.name());
                draw_string(0x22, lp_row_, 0x0A, 0, text_[Text::AcSpace] + str_byte(h & 0xFF));
                p.setw(0xE3, static_cast<std::uint16_t>(hp + h));
                p.b(0xD4) = 1;
                p.b(0xD5) = 1;
                if (tier != 0) done = true;
                nobody_treated = false;
                print_line(0, "");
            }
        }
        if (has_effect(p, 3)) {                        // 0x39600
            if (p.b(0xD6) == 1) {
                // 0x3962A: 1 through the nil +0xF3 into 0000:000F, which a
                // later fight-less COMBAT reads (D-0029).
                low_.write(0x0F, 1, "D-0033", "Medical (ovl5 +0x1449, 0x3962A): an enemy-side record with effect 3 "
                                              "is marked through its freed combat block (a nil pointer)");
            } else if (p.w(0xE3) < 0x2D) {
                p.b(0xD4) = 1;
                p.b(0xD5) = 1;
                p.setw(0xE3, 0x2D);
            }
        }
    }
    print_line(0x0E, "");                              // 0x39677
    if (no_injuries) print_line(0x0D, text_[Text::AcNoInjuries]);
    else if (no_medics) print_line(0x0D, text_[Text::AcNoMedics]);
    else if (nobody_treated) print_line(0x0D, text_[Text::AcNoneTreated]);
    else print_line(0x0D, text_[Text::AcHealed]);
    pause_continue();                                  // Text.Pause
}

// --- the XP report: ovl5 +0x079C (0x382DC..0x3845E) ------------------------------

void Game::xp_report(std::int32_t xp) {
    layout_state0();
    if (f3586_ || f82ea_) {
        if (f3587_) {
            draw_string(1, 3, 0x0A, 0, text_[Text::AcFled]);
            xp = 0;
            free_booty();
        } else {
            draw_string(1, 3, 0x0A, 0, text_[Text::AcWon]);
        }
    }
    draw_string(1, 5, 0x0A, 0, text_[Text::AcEach] + std::to_string(xp));   // System.StrInt
    draw_string(1, 7, 0x0A, 0, text_[Text::AcXpPoints]);
    // BootyLeft ovl21 +0x04E4: credits > 0 (signed) or an item.
    const bool left = loot_value_ > 0 || !loot_.empty();
    draw_string(1, 10, 0x0A, 0, text_[Text::AcFound] + (left ? "" : text_[Text::AcNo]) + text_[Text::AcBooty]);
    pause_continue();
}

// --- the booty: ovl5 +0x1C5A (0x3979A..0x39AAA) ----------------------------------

void Game::booty_menu() {
    redraw_screen();                                   // state 6
    bool leave = false;
    while (!leave) {
        std::string items = text_[Text::AcView];
        if (loot_value_ != 0 || !loot_.empty()) items += text_[Text::AcTake];
        if (loot_value_ != 0) items += text_[Text::AcDivvy];
        items += text_[Text::AcPool];
        // "Status " with an enemy ship (DS:4A14): none outside ship fights.
        items += text_[Text::AcExit];
        f8433_ = 1;                                    // 0x398CE .. 0x398EA: roster clicks
        const MenuKey mk = menu_screen("", items, true);   // anim 1: see combat_screens.md
        f8433_ = 0;
        if (mk.ext) {
            roster_row(cur_, false);
            select_char(mk.key);
            roster_row(cur_, true);
            continue;
        }
        switch (mk.key) {
            case 'D':
                divvy_credits();
                break;
            case 'P':
                pool_credits();
                break;
            case 'V':
                view_character(f6b1e_);                // the exit flag DS:6B1E
                redraw_screen();
                break;
            case 'T':
                take_booty();                          // ovl5 +0x0B3F, no redraw after
                break;
            case 'E':
            case 0: {
                const bool left = loot_value_ > 0 || !loot_.empty();   // BootyLeft
                if (!left) {
                    leave = true;
                    break;
                }
                print_in(1, 0x11, 0x26, 0x16, 0x0A, true, text_[Text::AcBootyLeft]);
                print_in(1, 0x11, 0x26, 0x16, 0x0F, false, text_[Text::AcClaim]);
                // Ecl.Menu ovl15 +0x16DD over CS:1C0F (two '~'-marked
                // items): index 1 (the second) leaves.
                std::vector<std::string> answers;
                std::size_t at = 0;
                const std::string& yn = text_[Text::AcYesNo];
                while (at < yn.size()) {
                    std::size_t sp = yn.find(' ', at);
                    if (sp == std::string::npos) sp = yn.size();
                    std::string w = yn.substr(at, sp - at);
                    if (!w.empty() && w[0] == '~') w.erase(0, 1);
                    answers.push_back(w);
                    at = sp + 1;
                }
                if (menu(answers, false) == 1) leave = true;
                else fill_rect(1, 0x11, 0x26, 0x16, 0);   // Text.ClearRect
                break;
            }
            default: break;
        }
    }
    f82f2_ = 0;
}

// --- Take: ovl5 +0x0B3F (0x3867F..0x387B6) ---------------------------------------

void Game::take_booty() {
    // With the flags BootyLeft computes (ovl21 +0x04E4): credits > 0
    // (signed) and items left. Only credits: TakeCredits and a redraw;
    // no credits: TakeGear; both: a menu until the chosen kind runs out.
    bool cred = loot_value_ > 0, items = !loot_.empty();
    if (!cred) {
        take_gear();
        return;
    }
    if (!items) {
        take_credits();
        redraw_screen();
        return;
    }
    bool done = false;
    while (!done) {
        const MenuKey mk = menu_screen(text_[Text::TkPrompt], text_[Text::TkItems], false);
        const int k = mk.key;
        if (mk.ext || !(k == 0 || k == 'C' || k == 'E' || k == 'G')) {   // set CS:0B1F
            roster_row(cur_, false);
            select_char(k);
            roster_row(cur_, true);
            continue;
        }
        if (k == 'C') {
            take_credits();
            redraw_screen();
            cred = loot_value_ > 0;
            done = !cred;
            draw_roster();                             // ovl25 +0x20F9
        } else if (k == 'G') {
            take_gear();
            items = !loot_.empty();
            done = !items;
            draw_roster();
        } else {
            done = true;
        }
    }
}

void Game::divvy_credits() {
    // Divvy ovl21 +0x02C4: the pool shared out (combat::divvy).
    std::vector<std::uint8_t> e7;
    for (const auto& p : party_) e7.push_back(p->b(0xE7));
    const std::vector<std::uint32_t> got = combat::divvy(loot_value_, e7);
    if (loot_value_ > 0) {
        for (std::size_t i = 0; i < party_.size(); ++i) party_[i]->setd(0x2B, party_[i]->d(0x2B) + got[i]);
        loot_value_ = 0;
    }
}

void Game::pool_credits() {
    // Pool ovl21 +0x0205: every member's credits into the pool (combat::pool).
    std::vector<std::uint8_t> e7;
    std::vector<std::uint32_t> credits;
    for (const auto& p : party_) {
        e7.push_back(p->b(0xE7));
        credits.push_back(p->d(0x2B));
    }
    loot_value_ = combat::pool(loot_value_, e7, credits);
    for (std::size_t i = 0; i < party_.size(); ++i) party_[i]->setd(0x2B, credits[i]);
}

void Game::take_credits() {
    // TakeCredits ovl21 +0x0416 (0x54F36): the credits left, then how many;
    // GiveCredits (ovl21 +0x0396) to CurChar, at most what is left, no cap
    // on the purse. DS:6B54 (credits left) has no reader: not modelled.
    // System.StrInt (0x54F52, main session 26): plain digits, no commas
    // (the shop's route shop_a showed 5350 where StrLong gives 5,350).
    draw_string(1, 0x11, 15, 0, text_[Text::TkCreditsLabel] + std::to_string(loot_value_) + text_[Text::TkCreditsPad]);
    std::int32_t n = input_credits(loot_value_, text_[Text::TkHowMany], 0x0A);
    if (n > 0) {
        if (n > loot_value_) n = loot_value_;
        loot_value_ -= n;
        cur_->setd(0x2B, cur_->d(0x2B) + static_cast<std::uint32_t>(n));
    }
}

std::int32_t Game::input_credits(std::int32_t max, const std::string& prompt, int fg) {
    // InputNumber ovl21 +0x0000 (0x54B20..0x54CF3): digits on row 24 after
    // the prompt; a value past `max` snaps to it; Backspace takes the last
    // digit back; Enter gives the value ('' is 0), Esc -1. GetKey reads an
    // extended key as 0 then its scan code, so Alt-B..Alt-M (scan 0x30..
    // 0x32) count as digits. Leading zeros move the cursor on past column
    // 39, where FillCells' first cell is clipped (0653:03D9).
    const std::string p = prompt.substr(0, 40);
    fill_cells(0, 0x18, 0, 0x28);
    draw_string(0, 0x18, fg, 0, p);
    const int col = static_cast<int>(p.size());
    int cur = col;
    const std::string maxs = std::to_string(max);      // Str(max) (System.StrInt, 0x54B80): no commas
    std::string buf;
    auto val = [](const std::string& s) -> std::int32_t {
        // System.ValInt (0BE4:1341, session 74, main session): a sign, then
        // digits; its digit loop (0x1D09E) fails before a *10 from
        // 0x10000000 on and on a sign-bit carry, and any failure (no digit,
        // another character) makes ValInt return 0 (0x1D1A4). So a value
        // past 2^31 - 1 reads as 0: under max, the digits stay typed, and
        // Enter gives 0. Reached only with a purse or a pool over
        // 214,748,364 credits; one of 2^31 or more makes max negative
        // (a longint, compared signed at 0x54C15), and its Str starts "-".
        std::size_t i = 0;
        bool neg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
        if (i == s.size()) return 0;
        std::int64_t v = 0;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9' || v >= 0x10000000) return 0;
            v = v * 10 + (s[i] - '0');
            if (v > 0x7FFFFFFF) return 0;
        }
        return static_cast<std::int32_t>(neg ? -v : v);
    };
    int k = 0;
    if (input_) input_->text_entry(kEntryDigits);   // the pad's picker (D-0085)
    do {
        fill_cells(cur, 0x18, 15, 1);
        fill_cells(cur + 1, 0x18, 0, 2);
        k = next_key("a number") ;
        if (k >= 0x100) k &= 0xFF;                     // the 0 is ignored, the scan code read next
        if (k >= '0' && k <= '9') {
            buf = (buf + static_cast<char>(k)).substr(0, 15);
            if (val(buf) <= max) {
                ++cur;
            } else {
                buf = maxs;
                cur = col + static_cast<int>(maxs.size());
            }
            draw_string(col, 0x18, 15, 0, buf);
        } else if (k == 8 && !buf.empty()) {
            buf.pop_back();
            --cur;
        }
    } while (k != 13 && k != 27);
    if (input_) input_->text_entry(kEntryNone);
    fill_cells(0, 0x18, 0, 0x28);
    return k == 27 ? -1 : (buf.empty() ? 0 : val(buf));
}

void Game::take_gear() {
    // TakeGear ovl5 +0x09EF (0x3852F..0x38645): the booty's items in a list
    // (Loot.SelectMenu ovl5 +0x092D: their names, "Take" highlighted, the
    // page redrawn each time); Take or Enter gives the item to CurChar
    // (GiveItem); one that doesn't fit stays. Then Screen.Redraw.
    layout_state0();
    int index = 0;
    for (;;) {
        std::vector<ListNode> list;
        for (Bytes& it : loot_) list.push_back({item_name(it), it.size() > 0x29 && it[0x29] != 0});
        highlight_ = 1;                                // DS:6B1D
        bool redraw = true;
        int picked = -1;
        const int key = list_menu(text_[Text::TkGearPrompt], text_[Text::TkGearSelect], 1, 1, 0x26, 0x16, list, true,
                                  redraw, index, picked);
        if (key != 'T' && key != 13) break;
        if (picked < 0 || picked >= static_cast<int>(loot_.size()))
            throw Error("TakeGear (ovl5 +0x09EF): Take with no item picked");
        if (!give_item(*cur_, loot_[static_cast<std::size_t>(picked)])) continue;
        loot_.erase(loot_.begin() + picked);
        if (loot_.empty()) break;
    }
    redraw_screen();
}

bool Game::give_item(Character& c, const Bytes& item) {
    // GiveItem ovl14 +0x02F7 (0x4A6A7): a copy appended to the record's
    // items, merged into a stack unless +0x34 bit 5; more than 16 items:
    // the message, and the copy removed again (after the merge: the
    // stack's partial merge stays, a quirk kept); Recalc.
    Bytes copy = item;
    c.items.push_back(copy);
    if (!(copy[0x34] & 0x20)) merge_stack(c, c.items.size() - 1);
    bool ok = true;
    // ItemCount ovl18 +0x0000 (0x4FFA0): the +0xF7 list's length, here c.items.size().
    if (c.items.size() > 16) {
        show_message(text_[Text::TkOverload]);
        ok = false;
        remove_item(c, static_cast<int>(c.items.size()) - 1);
    }
    recalc(c);
    return ok;
}

void Game::merge_stack(Character& c, std::size_t i) {
    // MergeStack ovl18 +0x22C9 (0x52269..0x52441): the new stack takes in
    // every matching one (bytes +0x2E..+0x33, word +0x36, +0x3B..+0x3D),
    // up to 100; the older stack goes, or keeps the part that doesn't fit and
    // becomes the one counted on.
    auto same = [&](const Bytes& a, const Bytes& b) {
        for (std::size_t k : {0x2Fu, 0x30u, 0x31u, 0x2Eu, 0x32u, 0x33u, 0x36u, 0x37u, 0x3Bu, 0x3Cu, 0x3Du})
            if (a[k] != b[k]) return false;
        return true;
    };
    if (c.items[i][0x34] & 0x20) return;
    if (c.items[i][0x38] == 0) c.items[i][0x38] = 1;
    const Bytes* cur = &c.items[i];
    std::size_t cur_i = i;
    int cnt = c.items[i][0x38];
    for (std::size_t j = 0; j < c.items.size();) {
        if (j == cur_i || !same(c.items[j], *cur)) {
            ++j;
            continue;
        }
        Bytes& p = c.items[j];
        if (p[0x38] == 0) p[0x38] = 1;
        if (p[0x38] + cnt <= 100) {
            cnt += p[0x38];
            remove_item(c, static_cast<int>(j));
            if (cur_i > j) --cur_i;
            cur = &c.items[cur_i];
            continue;
        }
        c.items[cur_i][0x38] = 100;
        const int pc = p[0x38];
        p[0x38] = static_cast<std::uint8_t>(pc - (100 - cnt));
        cnt = p[0x38];
        cur_i = j;
        cur = &c.items[cur_i];
        ++j;
    }
    c.items[cur_i][0x38] = static_cast<std::uint8_t>(cnt);
}

}  // namespace cotd::game
