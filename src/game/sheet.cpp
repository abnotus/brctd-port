// SPDX-License-Identifier: GPL-3.0-or-later
// The character sheet (ovl18 +0x00E3) and VIEW CHARACTER (ovl18 +0x0F75),
// with the derived values they show: Recalc (ovl23 +0x138D), CalcMove
// (ovl23 +0x166C) and ItemName (ovl23 +0x0124) (docs/notes/frontend.md).
#include <cstdlib>

#include "game/game.hpp"

namespace cotd::game {

namespace {

// Text.PadLeft 0653:15B2: spaces in front up to `width` (StrStore 40).
std::string pad_left(std::string s, std::size_t width) {
    if (s.size() < width) s.insert(0, width - s.size(), ' ');
    if (s.size() > 40) s.resize(40);
    return s;
}

constexpr std::size_t kRaceNames = 0x1224;     // DS: string[13], 14 bytes per race
constexpr std::size_t kGenderNames = 0x12A2;   // DS: string[6], 7 per gender
constexpr std::size_t kClassNames = 0x11D0;    // DS: string[11], 12 per class
constexpr std::size_t kStatusNames = 0x12B0;   // DS: string[12], 13 per status
constexpr std::size_t kAbilityNames = 0x0C7C;  // DS: string[4], 5 per ability
constexpr std::size_t kItemWords = 0x1D9F;     // DS: string[20], 21 per word

}  // namespace

std::string str_byte(int v) { return std::to_string(v & 0xFF); }   // Text.StrByte 0653:1370

// --- derived values ------------------------------------------------------------------------

std::string Game::item_name(Bytes& item, bool ready_prefix) {
    // ItemName ovl23 +0x0124 (0x56034): the words of ITEM0 bytes 3, 2, 1
    // (DS:1D9F + 21*w), each followed by a space; then with a count
    // (+0x38): bit 0x20 of +0x34 appends "(" count ")", else the count
    // comes first and a count above 1 turns the last character into 's';
    // with ready_prefix, CS:0116 " Yes  " or CS:011D " No   " by +0x34
    // bit 7 in front (0x56162). The name is stored in the node (+0x00,
    // string[40]; the bytes past it keep their old values).
    std::string name;
    for (int i = 1; i <= 3; ++i) {
        const std::uint8_t w = item[0x2E + static_cast<std::size_t>(i)];
        if (w) name = text_.ds_str(kItemWords + 21 * static_cast<std::size_t>(w), 20) + text_[Text::ItemSpace] + name;
    }
    const std::uint8_t count = item[0x38];
    if (count > 0) {
        if (item[0x34] & 0x20) {
            name += text_[Text::ItemOpen] + str_byte(count) + text_[Text::ItemClose];
        } else {
            name = str_byte(count) + text_[Text::ItemSpace] + name;
            if (name.size() > 40) name.resize(40);
            if (count > 1 && !name.empty()) name.back() = 's';
        }
    }
    if (name.size() > 40) name.resize(40);
    if (ready_prefix) {
        name = text_[(item[0x34] & 0x80) ? Text::ItYes : Text::ItNo] + name;
        if (name.size() > 40) name.resize(40);
    }
    item[0] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), item.begin() + 1);
    return name;
}

int Game::calc_move(Character& c) {
    // CalcMove ovl23 +0x166C: +0xE5 := +0x36, halved when the load passes
    // DS:0CFA[STR] / 2; the result (DS:698F) adds var 0x7F72 unless +0xD6,
    // then doubles (byte).
    c.b(0xE5) = c.b(0x36);
    // ovl23 +0x07E0 (0x566F0): word DS:0CFA[2 * STR +0x10], the load limit
    if (text_.ds16(0x0CFA + 2 * static_cast<std::size_t>(c.b(0x10))) / 2 < c.w(0xE8))
        c.b(0xE5) = static_cast<std::uint8_t>(c.b(0xE5) / 2);
    std::uint8_t m = c.b(0xE5);
    if (c.b(0xD6) == 0) m = static_cast<std::uint8_t>(m + v1(0x7F72));
    return static_cast<std::uint8_t>(m * 2);
}

void Game::recalc(Character& c) {
    // Recalc ovl23 +0x138D (0x5729D..0x57579).
    auto cls_of = [&](const Bytes& it) -> int {
        const std::size_t t = it[0x2E];
        return 9 * t < items_table_.size() ? items_table_[9 * t] : -1;
    };
    auto item_byte = [&](std::size_t t, std::size_t k) -> std::uint8_t {
        return 9 * t + k < items_table_.size() ? items_table_[9 * t + k] : 0;
    };
    const std::uint8_t str = c.b(0x10), dex = c.b(0x11);
    // 1-2: the load, and the readied weapon / armour (the last wins).
    c.weapon = c.armour = -1;
    for (ForeignSlot& f : foreign_slot_)
        if (f.holder == &c) f = {};                    // D-0074
    std::uint16_t load = 0;
    for (std::size_t i = 0; i < c.items.size(); ++i) {
        const Bytes& it = c.items[i];
        const int cls = cls_of(it);
        const bool ready = it[0x34] & 0x80;
        if (!(cls == 1 && ready)) {
            std::uint16_t w = le16(&it[0x36]);
            if (it[0x38] > 0 && !(it[0x34] & 0x20)) w = static_cast<std::uint16_t>(w * it[0x38]);
            load = static_cast<std::uint16_t>(load + w);
        }
        if (ready && cls == 0) c.weapon = static_cast<int>(i);
        if (ready && cls == 1) c.armour = static_cast<int>(i);
    }
    c.setw(0xE8, load);
    // +0xEB / +0xEF are heap pointers in the original (D-0010).
    auto keep = [&](std::size_t off, bool present) {
        const std::uint32_t v = c.d(off);
        c.setd(off, present ? (v ? v : 0xFFFFFFFFu) : 0);
    };
    keep(0xEB, c.weapon >= 0);
    keep(0xEF, c.armour >= 0);
    // 3-5.
    for (std::size_t k = 0; k < 2; ++k) {
        c.b(0xDD + k) = c.b(0x3D + k);
        c.b(0xDF + k) = c.b(0x3F + k);
        c.b(0xE1 + k) = c.b(0x41 + k);
    }
    c.b(0xD8) = c.b(0x33);
    if (c.weapon < 0) {
        // ovl23 +0x079E (0x566AE): DS:0CCF[STR +0x10], to-hit
        c.b(0xD8) = static_cast<std::uint8_t>(c.b(0xD8) + text_.ds8(0x0CCF + str));
        // ovl23 +0x07BF (0x566CF): DS:0CE5[STR +0x10], damage
        c.b(0xE1) = static_cast<std::uint8_t>(c.b(0xE1) + text_.ds8(0x0CE5 + str));
    } else {
        // 6: WeaponStats ovl23 +0x0000 (0x55F10).
        const Bytes& w = c.items[static_cast<std::size_t>(c.weapon)];
        const std::size_t t = w[0x2E];
        c.b(0xD8) = c.b(0x33);
        c.b(0xE1) = item_byte(t, 6);
        if (item_byte(t, 2) == 0) {
            c.b(0xD8) = static_cast<std::uint8_t>(c.b(0xD8) + text_.ds8(0x0CCF + str));
            c.b(0xE1) = static_cast<std::uint8_t>(c.b(0xE1) + text_.ds8(0x0CE5 + str));
        } else {
            c.b(0xD8) = static_cast<std::uint8_t>(c.b(0xD8) + text_.ds8(0x0CB9 + dex));
        }
        const std::uint8_t b = static_cast<std::uint8_t>(spec_count(c, static_cast<int>(t)) + static_cast<std::int8_t>(w[0x32]));
        if (t != 0x0F) c.b(0xE1) = static_cast<std::uint8_t>(c.b(0xE1) + b);
        c.b(0xD8) = static_cast<std::uint8_t>(c.b(0xD8) + b);
        c.b(0xDD) = item_byte(t, 4);
        c.b(0xDF) = item_byte(t, 5);
    }
    // 7. Zero gravity's failed check -2; the side that won Battle Tactics
    // this round +1 (DS:6BD3 + side, 0x57493; DS:6BD4 is never cleared
    // after a fight, so it carries over: reproduced).
    if (c.b(0xD4) == 12) c.b(0xD8) = static_cast<std::uint8_t>(c.b(0xD8) - 2);
    {
        const int side = static_cast<std::int8_t>(c.b(0xD6));
        if (side != 0 && side != 1) throw Error("D-0090: " + c.name() + ": side " + std::to_string(side) + " (+0xD6) out of 0..1 (Recalc 0x57493)");
        if (tactics_won_[static_cast<std::size_t>(side)]) c.b(0xD8) = static_cast<std::uint8_t>(c.b(0xD8) + 1);
    }
    // ArmourEffect ovl23 +0x12D1 (0x571E1): one readied item's AC and +0x48 bits
    // 8-9: AC and the +0x48 bits per readied item (ArmourEffect +0x12D1).
    c.b(0x48) = 0;
    c.b(0xD9) = c.b(0x37);
    for (const Bytes& it : c.items) {
        if (!(it[0x34] & 0x80)) continue;
        const std::size_t t = it[0x2E];
        const int cls = cls_of(it);
        if (cls == 1) {
            c.b(0xD9) = static_cast<std::uint8_t>(c.b(0xD9) + (item_byte(t, 2) - 0x32) + static_cast<std::int8_t>(it[0x32]));
            c.b(0x48) |= 0xC0 | (t == 0x18 ? 0x20 : 0);
        } else if (cls == 3) c.b(0x48) |= 0x08;
        else if (cls == 4) c.b(0x48) |= 0x80;
        else if (cls == 7) c.b(0x48) |= 0x10;
    }
    // 10-13.
    c.b(0xE5) = static_cast<std::uint8_t>(calc_move(c) / 2);
    if (c.b(0xD4) == 10) {
        const int lvl = static_cast<std::int8_t>(c.b(0x29));
        c.b(0xD9) = static_cast<std::uint8_t>(c.b(0xD9) + (lvl < 4 ? 2 : lvl / 2));
    }
    c.b(0xDA) = static_cast<std::uint8_t>(c.b(0xD9) - 2);
    // ovl23 +0x075C (0x5666C): DS:0CA3[DEX +0x11], added to +0xD9 at 0x57571
    c.b(0xD9) = static_cast<std::uint8_t>(c.b(0xD9) + text_.ds8(0x0CA3 + dex));
}

// --- the sheet ----------------------------------------------------------------------------------

void Game::draw_char_name(const Character& c, int col, int row, bool possessive) {
    // Screen.DrawCharName ovl25 +0x22BE: the name in its roster colour;
    // possessive: CS:22BB "'s" after it in the same colour (0x5BE9A).
    const int fg = roster_colour(c);
    draw_string(col, row, fg, 0, c.name());
    if (possessive) draw_string(col + static_cast<int>(c.name().size()), row, fg, 0, text_[Text::NamePossessive]);
}

void Game::draw_ac(const Character& c, int col, int row) {
    // Screen.DrawAC ovl25 +0x2399.
    const int raw = c.b(0xD9);
    std::string s = str_byte(std::abs(raw - 60));
    if (raw > 60) s = text_[Text::RosterMinus] + s;
    draw_string(col, row, 15, 0, s);
}

void Game::draw_hp(const Character& c, int col, int row, bool hilite, bool with_max) {
    // Screen.DrawHP ovl25 +0x2424: max HP shown as its low byte (quirk).
    const int fg = hilite ? 13 : c.w(0xE3) < c.w(0x45) ? 14 : 15;
    std::string s = std::to_string(c.w(0xE3));
    if (with_max) s = pad_left(s + text_[Text::HpSlash] + str_byte(c.b(0x45)), 7);
    draw_string(col, row, fg, 0, s);
}

void Game::sheet_ability(int i, bool hilite) {
    // Sheet.AbilityRow ovl18 +0x09A9 (0x50949).
    clear_cells(7, 0xF + i, 9, 0xF + i);
    draw_string(7, 0xF + i, hilite ? 13 : 15, 0, pad_left(str_byte(cur_->b(0x10 + static_cast<std::size_t>(i))), 2));
}

void Game::sheet_credits() {
    // Sheet.Credits ovl18 +0x05C4 (0x50564).
    draw_string(8, 7, 15, 0, pad_left(str_long(static_cast<std::int32_t>(cur_->d(0x2B))), 10));
}

void Game::sheet_combat() {
    // Sheet.Combat ovl18 +0x0620 (0x505C0).
    Character& c = *cur_;
    recalc(c);
    clear_cells(0x1F, 1, 0x26, 1);
    draw_hp(c, 0x1F, 1, false, true);
    const int ac = c.b(0xD9);
    const int col = (ac >= 1 && ac <= 50) ? 0x24 : (ac >= 51 && ac <= 60) ? 0x25 : (ac >= 61 && ac <= 69) ? 0x24 : 0x23;
    clear_cells(0x1F, 2, 0x26, 2);
    draw_ac(c, col, 2);
    draw_string(0x23, 3, 15, 0, pad_left(std::to_string(60 - c.b(0xD8)), 3));
    // Text.StrWord 0653:13AC (0x178DC): Str of a word, unsigned decimal, no width
    draw_string(0x21, 9, 15, 0, pad_left(std::to_string(c.w(0xE8)), 5));
    calc_move(c);   // shown without var 0x7F72 (quirk)
    draw_string(8, 9, 15, 0, pad_left(str_byte(c.b(0xE5)), 7));
    std::string dmg = str_byte(c.b(0xDD)) + text_[Text::DmgD] + str_byte(c.b(0xDF));
    const int bonus = static_cast<std::int8_t>(c.b(0xE1));
    if (bonus > 0) dmg += text_[Text::DmgPlus] + str_byte(bonus);
    if (bonus < 0) dmg += text_[Text::DmgMinus] + str_byte(-bonus);
    draw_string(8, 0xA, 15, 0, pad_left(dmg, 7));
}

void Game::sheet_career_skills() {
    // Sheet.CareerSkills ovl18 +0x08A9 (0x50849).
    const Character& c = *cur_;
    for (int i = 1; i <= 8; ++i) {
        const int k = career_skill(c.b(0x28), i);
        draw_string(0xC, 0xE + i, skill_prereq_ok(c, k) ? 10 : 14, 0, skill_name(k));
        draw_string(0x22, 0xE + i, 15, 0, pad_left(str_byte(skill_total(c, k)), 3));
    }
}

void Game::draw_sheet() {
    // Sheet.Draw ovl18 +0x00E3 (0x50083), of CurChar.
    Character& c = *cur_;
    layout_sheet();
    struct Label { Text t; int col, row; };
    static constexpr Label kLabels[] = {
        {Text::SheetLabel0, 1, 1},     {Text::SheetLabel1, 0x1C, 1},  {Text::SheetLabel2, 1, 2},
        {Text::SheetLabel3, 0x1C, 2},  {Text::SheetLabel4, 1, 3},     {Text::SheetLabel5, 0x19, 3},
        {Text::SheetLabel6, 1, 4},     {Text::SheetLabel7, 0x19, 4},  {Text::SheetLabel8, 1, 6},
        {Text::SheetLabel9, 0x14, 6},  {Text::SheetLabel10, 1, 7},    {Text::SheetLabel11, 0x1B, 7},
        {Text::SheetLabel12, 1, 9},    {Text::SheetLabel13, 0x13, 9}, {Text::SheetLabel14, 1, 0xA},
        {Text::SheetLabel15, 1, 0xB},  {Text::SheetLabel16, 1, 0xC},  {Text::SheetLabel17, 1, 0xE},
        {Text::SheetLabel18, 0xC, 0xE}};
    for (const Label& l : kLabels) draw_string(l.col, l.row, 10, 0, text_[l.t]);
    draw_char_name(c, 8, 1);
    draw_string(8, 2, 15, 0, text_.ds_str(kRaceNames + 14 * static_cast<std::size_t>(c.b(0x27)), 13));
    draw_string(8, 3, 15, 0, text_.ds_str(kGenderNames + 7 * static_cast<std::size_t>(c.b(0x26)), 6));
    draw_string(8, 4, 15, 0, text_.ds_str(kClassNames + 12 * static_cast<std::size_t>(c.b(0x28)), 11));
    draw_string(0x23, 4, 15, 0, pad_left(str_byte(c.b(0x29)), 3));
    draw_string(8, 6, 15, 0, text_.ds_str(kStatusNames + 13 * static_cast<std::size_t>(c.b(0xD4)), 12));
    draw_string(0x23, 7, 15, 0, pad_left(str_byte(c.b(0x38)), 3));
    draw_string(0x1F, 6, 15, 0, pad_left(str_long(static_cast<std::int32_t>(c.d(0x2F))), 7));
    for (int i = 0; i < 7; ++i) {
        draw_string(2, 0xF + i, 10, 0, text_.ds_str(kAbilityNames + 5 * static_cast<std::size_t>(i), 4));
        sheet_ability(i, false);
    }
    sheet_credits();
    sheet_combat();
    sheet_career_skills();
    if (c.weapon >= 0) draw_string(8, 0xB, 10, 0, item_name(c.items[static_cast<std::size_t>(c.weapon)]));
    if (c.armour >= 0) draw_string(8, 0xC, 10, 0, item_name(c.items[static_cast<std::size_t>(c.armour)]));
}

void Game::view_character(std::uint8_t& exit_flag) {
    // ViewCharacter ovl18 +0x0F75 (0x50F15): DS:8433 := 0 (no roster
    // clicks), the exit flag := 0, DS:387C := CurChar; the sheet, then a
    // menu of [G] S [T] [W] E items (CS:0F11..0F50) until 0 or 'E'
    // (CS:0EF1) or the exit flag is set (0x50F5F). The key is compared by
    // its value alone (DS:6B49), extended or not. A set flag also skips
    // the sheet's redraw after the dispatch (0x510ED). The flag is the
    // caller's: combat's turn's done (Gear's heavy weapon), the booty
    // menu's DS:6B1E (MenuScreen's extended-key flag: an extended key ends
    // VIEW), a local elsewhere.
    if (!cur_) {
        // No party (CHANGE's PURGE of the last member): the original shows a
        // sheet of the record at 0000:0000, the interrupt table, whose bytes
        // depend on where DOS loaded the game (D-0053).
        combat::diag("D-0053", "ViewCharacter (ovl18 +0x0F75, 0x50F15) with no party reads a record at 0000:0000 "
                               "(the interrupt table): nothing is shown");
        return;
    }
    f8433_ = 0;                                        // 0x50F1C
    exit_flag = 0;                                     // 0x50F34
    f387c_ = cur_;                                     // 0x50F3E
    int key = ' ';
    draw_sheet();
    while (key != 0 && key != 'E' && !exit_flag) {
        // The menu reads DS:3878, the member Sheet.Draw last drew (0x50090): a
        // roster click in Gear after a Trade can move CurChar (D-0074).
        Character& c = *cur_;
        std::string items;
        highlight_ = 2;
        if (!c.items.empty()) {
            items += text_[Text::ViewGear];
            ++highlight_;
        }
        items += text_[Text::ViewSkills];
        if ((c.b(0xE7) < 0x80 || c.b(0xD5) == 0) && mode_ != 5 && mode_ != 8) {
            items += text_[Text::ViewTrade];
            ++highlight_;
        }
        if (c.b(0x28) == 3) items += text_[Text::ViewWpn];
        items += text_[Text::ViewExit];
        const MenuKey mk = menu_screen("", items, false);
        key = mk.key;
        if (key == 'G') view_gear(exit_flag);
        if (key == 'T') view_trade();
        if (key == 'S') allocate_skills(0, 0, 0);
        if (key == 'W') {
            weapon_specialize(false);
            draw_sheet();
        }
        if (exit_flag) break;
        if (key == 'G' || key == 'S' || key == 'T') draw_sheet();
    }
}

}  // namespace cotd::game
