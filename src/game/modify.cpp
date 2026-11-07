// SPDX-License-Identifier: GPL-3.0-or-later
// MODIFY CHARACTER (ovl13 +0x1156, 0x47A66..0x48117) and PURGE CHARACTER
// (ovl13 +0x186D, 0x4817D..0x4830A), from the team menu. Rules and
// screens: docs/notes/frontend.md "MODIFY CHARACTER" and "PURGE
// CHARACTER".
#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr std::size_t kRaceMod = 0x32CB;       // DS: [race*7 + i], signed
constexpr std::size_t kClassMin = 0x3303;      // DS: [class*7 + i]
constexpr std::size_t kConHpBonus = 0x31B3;    // DS: [CON], signed
constexpr std::size_t kKeypad = 0x3046;        // DS: '1'..'9' -> scan codes

}  // namespace

int Game::min_hp(const Character& c) const {
    // MinHP ovl13 +0x298F (0x4929F): level times (the CON bonus + 1, at
    // least 1, at most 3 unless a Warrior), as a byte.
    int b = static_cast<std::int8_t>(text_.ds8(kConHpBonus + c.b(0x12))) + 1;
    b = static_cast<std::int8_t>(b);
    if (b < 1) b = 1;
    if (b > 3 && c.b(0x28) != 3) b = 3;
    return static_cast<std::uint8_t>(static_cast<std::int8_t>(c.b(0x29)) * b);
}

int Game::max_hp(const Character& c) const {
    // MaxHP ovl13 +0x29EC (0x492FC): level times (the die + the CON bonus,
    // at most +2 unless a Warrior), as a byte.
    int b = static_cast<std::int8_t>(text_.ds8(kConHpBonus + c.b(0x12)));
    const int cls = c.b(0x28);
    if (cls != 3 && b > 2) b = 2;
    if (cls == 3) b += 10;
    else if (cls == 6 || cls == 4) b += 8;
    else if (cls == 1 || cls == 5 || cls == 2) b += 6;
    b = static_cast<std::int8_t>(b);
    return static_cast<std::uint8_t>(static_cast<std::int8_t>(c.b(0x29)) * b);
}

void Game::modify_draw(Character& c, int field, int cur, bool on) {
    // The nested Draw ovl13 +0x0F45 (0x47855).
    if (field >= 0 && field <= 6) {
        sheet_ability(field, on);
    } else if (field == 8) {
        draw_hp(c, 0x1F, 1, on, true);
    } else if (field == 7) {
        if (on) {
            fill_cells(c.b(0) + 8, 1, 0, 1);
            draw_string(8, 1, 13, 0, c.name());
            if (c.b(static_cast<std::size_t>(cur)) == ' ')
                draw_string(7 + cur, 1, 15, 0, text_[Text::ModifyCursor]);
            else
                draw_string(7 + cur, 1, 15, 0, std::string(1, static_cast<char>(c.b(static_cast<std::size_t>(cur)))));
        } else {
            draw_string(8, 1, 10, 0, c.name());   // green, not the roster colour
        }
    }
}

void Game::modify_character() {
    // ModifyCharacter ovl13 +0x1156 (0x47A66).
    if (!f6b4c_ && !pick_character()) return;
    Character& c = *cur_;
    const int cls = static_cast<std::int8_t>(c.b(0x28));
    if (c.d(0x2F) != text_.ds32(0x31AA + static_cast<std::size_t>(cls) * 0x20)) {   // 0x47A80: the starting XP
        row_message(c.name() + text_[Text::ModifyRefused], 14, 0);
        return;
    }
    f6b43_ = 0xFF;
    f6b77_ = 1;
    draw_sheet();
    std::array<std::uint8_t, 7> saved_ab{};
    for (std::size_t i = 0; i < 7; ++i) saved_ab[i] = c.b(0x10 + i);
    const std::uint8_t saved_hp = c.b(0x45);                         // its low byte
    const std::string saved_name = c.name();
    int cur = 1;                                                      // [bp-0x3D]
    int field = 8;                                                    // [bp-0x34]
    modify_draw(c, field, cur, false);
    field = 0;
    modify_draw(c, field, cur, true);
    auto cancel = [&] {
        // 'E' (0x47FDA) or 0 (0x4804C): the abilities, max HP from its low
        // byte, current HP := max, the name; Recalc.
        for (std::size_t i = 0; i < 7; ++i) c.b(0x10 + i) = saved_ab[i];
        c.setw(0x45, saved_hp);
        c.setw(0xE3, c.w(0x45));
        c.b(0) = static_cast<std::uint8_t>(saved_name.size());
        std::copy(saved_name.begin(), saved_name.end(), c.rec.begin() + 1);
        recalc(c);
        f6b77_ = 0;
    };
    auto del_char = [&] {
        // DelChar ovl13 +0x106B (0x4797B): the name field only, never empty.
        if (field != 7 || c.b(0) <= 1) return;
        if (cur == c.b(0)) {
            c.b(0) = static_cast<std::uint8_t>(c.b(0) - 1);
            cur = c.b(0);
            return;
        }
        for (int j = cur; j <= c.b(0) - 1; ++j)
            c.b(static_cast<std::size_t>(j)) = c.b(static_cast<std::size_t>(j + 1));
        c.b(0) = static_cast<std::uint8_t>(c.b(0) - 1);
    };
    // The pad's picker over the name field (D-0085, session 68): told the
    // field as it stands at each read; off once MODIFY ends.
    struct EntryOff {
        Input* in;
        ~EntryOff() {
            if (in) in->text_entry(kEntryNone);
        }
    } entry_off{input_};
    for (;;) {
        f6b43_ = 0xFF;                                               // 0x47B48
        int key;
        if (input_) {
            input_->text_entry(field == 7 ? kEntryField : kEntryNone);
            if (field == 7) input_->entry_field(c.name(), cur);
        }
        if (field == 7) {
            // The name reads GetKey itself (0x47B53): digits move as the
            // keypad (DS:3046), '\' is 0x37, an extended key comes as 0
            // and its scan code; Esc is 0.
            f6b1e_ = 0;
            int k = next_key("key");
            if ((k >= '1' && k <= '9') || k == '\\') {
                key = k == '\\' ? 0x37 : text_.ds8(kKeypad + static_cast<std::size_t>(k - '1'));
                f6b1e_ = 1;
            } else if (k >= 0x100) {
                key = k - 0x100;
                f6b1e_ = 1;
            } else {
                key = k;
            }
            if (key == 0x1B) key = 0;
        } else {
            const MenuKey mk = menu_screen(text_[Text::ModifyPrompt], text_[Text::ModifyItems], true);
            key = mk.key;
        }
        modify_draw(c, field, cur, false);
        if (f6b43_ != 0xFF) {                                        // 0x47BF1: a click on a field
            field = f6b43_;
            key = key == kEnter ? 'A' : key == kEsc ? 'S' : 0;
            f6b1e_ = 0;
        }
        if (f6b1e_) {
            if (key == 0x4B || key == 0x53) del_char();
            else if (key == 0x50) field = field >= 8 ? 0 : field + 1;
            else if (key == 0x48) field = field == 0 ? 8 : field - 1;
        } else if (key == 8) {
            del_char();
        } else if (key == 'A' || key == 'S') {
            if (field < 7) {
                // 0x47C82: one ability, wrapping as a byte, then clamped.
                const std::size_t i = static_cast<std::size_t>(field);
                c.b(0x10 + i) = static_cast<std::uint8_t>(c.b(0x10 + i) + (key == 'A' ? 1 : -1));
                const int race = static_cast<std::int8_t>(c.b(0x27));
                const int mod = static_cast<std::int8_t>(text_.ds8(kRaceMod + static_cast<std::size_t>(race * 7 + field)));
                if (c.b(0x10 + i) > mod + 18) c.b(0x10 + i) = static_cast<std::uint8_t>(mod + 18);
                if (c.b(0x10 + i) < mod + 3) c.b(0x10 + i) = static_cast<std::uint8_t>(mod + 3);
                const std::uint8_t mn = text_.ds8(kClassMin + static_cast<std::size_t>(cls * 7 + field));
                if (c.b(0x10 + i) < mn) c.b(0x10 + i) = mn;
                if (field == 2) {
                    // 0x47DC2: CON re-clamps max HP; current := max; then the
                    // name is redrawn green (field 7, Draw(0)).
                    if (min_hp(c) > static_cast<int>(c.w(0x45))) c.setw(0x45, static_cast<std::uint32_t>(min_hp(c)));
                    else if (max_hp(c) < static_cast<int>(c.w(0x45))) c.setw(0x45, static_cast<std::uint32_t>(max_hp(c)));
                    c.setw(0xE3, c.w(0x45));
                    modify_draw(c, 7, cur, false);
                }
                sheet_career_skills();
            } else if (field == 8) {
                // 0x47E52: max HP +-1 (a word), within MaxHP and MinHP.
                c.setw(0x45, (c.w(0x45) + (key == 'A' ? 1u : 0xFFFFu)) & 0xFFFF);
                if (max_hp(c) < static_cast<int>(c.w(0x45))) c.setw(0x45, static_cast<std::uint32_t>(max_hp(c)));
                if (min_hp(c) > static_cast<int>(c.w(0x45))) c.setw(0x45, static_cast<std::uint32_t>(min_hp(c)));
                c.setw(0xE3, c.w(0x45));
            } else {
                cur = cur == c.b(0) ? 1 : cur + 1;                    // 0x47ED1: the name's cursor
            }
        } else if (key == kEnter) {
            field = field >= 8 ? 0 : field + 1;
        } else if (key == '0' || (key >= 0x20 && key <= 0x7A)) {
            if (field == 7) {
                // 0x47F8C: typed at the cursor (no upper case); past the end
                // the name grows by a space under the cursor.
                if (cur <= 15) {
                    c.b(static_cast<std::size_t>(cur)) = static_cast<std::uint8_t>(key);
                    if (++cur > 15) cur = 15;
                    if (cur > c.b(0)) {
                        c.b(0) = static_cast<std::uint8_t>(c.b(0) + 1);
                        c.b(static_cast<std::size_t>(cur)) = ' ';
                    }
                }
            } else if (key == 'E') {
                cancel();
                return;
            }
        } else if (key == 0) {
            cancel();
            return;
        }
        // 0x480B7: after every key.
        recalc(c);
        sheet_combat();
        modify_draw(c, field, cur, true);
        if (!f6b1e_ && key == 'K') {
            // Keep (0x480E4): the second copy of the abilities.
            for (std::size_t i = 0; i < 7; ++i) c.b(0x17 + i) = c.b(0x10 + i);
            f6b77_ = 0;
            return;
        }
    }
}

Game::MenuKey Game::modify_pick(bool& done, int key) {
    // ModifyPick ovl25 +0x0B58 (0x5A698): with DS:6B77, a click on row 1
    // (the name, columns 8..0x18; HP, 0x1F..0x26) or on an ability's row
    // (rows 0x0F..0x15, columns 5..0x0A) names the field in DS:6B43; other
    // cells of those rows end the menu without one; any other row is
    // dropped (the menu goes on). DS:8435 := 0, DS:6B1E := 1 always.
    done = true;
    if (row_click_ == 1) {
        if (col_click_ >= 8 && col_click_ <= 0x18) f6b43_ = 7;
        else if (col_click_ >= 0x1F && col_click_ <= 0x26) f6b43_ = 8;
    } else if (row_click_ >= 0x0F && row_click_ <= 0x15) {
        if (col_click_ >= 5 && col_click_ <= 0x0A) f6b43_ = static_cast<std::uint8_t>(row_click_ - 0x0F);
    } else {
        done = false;
    }
    f8435_ = 0;
    return {key, true};
}

void Game::purge_character() {
    // PurgeCharacter ovl13 +0x186D (0x4817D): the member picked (none after
    // a click on CurChar's roster row); two questions, No first; on Yes the
    // message, two Game.Delays, the roster files deleted, RemoveChar(0, 1),
    // the roster. On No the message and the party-lost recount.
    if (!f6b4c_ && !pick_character()) return;
    if (cur_) {
        Character& c = *cur_;
        menu_prompt_fg_ = 14;
        int a = yes_no(text_[Text::PurgeDrop] + c.name() + text_[Text::PurgeForever], 2);
        if (a == 'Y') {
            menu_prompt_fg_ = 14;
            a = yes_no(text_[Text::PurgeSure], 2);
        }
        if (a == 'Y') {                                              // 0x48209
            if (c.b(0xD5) == 0) show_message(text_[Text::PurgeIncinerate] + c.name() + text_[Text::PurgeDot]);
            else show_message(c.name() + text_[Text::PurgeHitches]);
            game_delay();                   // Game.Delay twice
            game_delay();
            delete_roster_files(c);
            remove_char(false, true);
            draw_roster();
            return;
        }
        show_message(c.name() + text_[Text::PurgeRelief]);
    }
    // 0x482AD: lost unless a member is on the party side, active and not
    // an NPC (+0x4C).
    party_lost_ = 1;
    for (const auto& m : party_)
        if (m->b(0xD6) == 0 && m->b(0xD5) != 0 && m->b(0x4C) == 0) {
            party_lost_ = 0;
            break;
        }
}

}  // namespace cotd::game
