// SPDX-License-Identifier: GPL-3.0-or-later
// TRAIN CHARACTER: TrainCharacter ovl13 +0x2BFD (0x4950D..0x49847), from
// the team menu and, in creation mode, from CreateCharacter. Rules:
// docs/notes/levelup.md (model tools/levelup.py).
#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr std::size_t kClassNames = 0x11D0;    // DS: string[11], 12 per class
constexpr std::size_t kConHpBonus = 0x31B3;    // DS: [CON], signed
constexpr std::size_t kXpTable = 0x31A2;       // DS: dword [class*0x20 + level*4]
constexpr std::size_t kThac0 = 0x3130;         // DS: [class*10 + level]

}  // namespace

std::int32_t Game::xp_for_level(const Character& c, int level) const {
    // DS:31A2 + class*0x20 + level*4 (signed class and level bytes); the
    // level-9 entry is the next class's first (levelup.md).
    const int cls = static_cast<std::int8_t>(c.b(0x28));
    return static_cast<std::int32_t>(text_.ds32(kXpTable + static_cast<std::size_t>(cls * 0x20 + level * 4)));
}

void Game::roll_hp_level(Character& c) {
    // RollHPLevel ovl13 +0x2A72 (0x49382), called after the level went up:
    // below level 9 the better of two dice (d10 Warrior, d8 Engineer, else
    // d6) plus the CON bonus DS:31B3, at most +2 unless a Warrior; at
    // least 1. From level 9 on (0x49424) no dice: +2, +3 for a Warrior,
    // and an unwritten local for a class outside 1..6; TRAIN stops at
    // level 8, so this game never gets there (levelup.md).
    const int cls = c.b(0x28);
    if (c.b(0x29) >= 9) throw Error("D-0090: RollHPLevel from level 9 on (ovl13 0x49424) is unreachable in this game");
    const int d = cls == 3 ? 10 : cls == 4 ? 8 : 6;
    const int a = roll_dice(1, d);
    const int b2 = roll_dice(1, d);
    int bonus = static_cast<std::int8_t>(text_.ds8(kConHpBonus + c.b(0x12)));
    if (cls != 3 && bonus > 2) bonus = 2;
    const int r = std::max(std::max(a, b2) + bonus, 1);
    c.setw(0x45, c.w(0x45) + static_cast<std::uint32_t>(r));
    c.setw(0xE3, c.w(0xE3) + static_cast<std::uint32_t>(r));
}

void Game::train_level(Character& c, bool creation) {
    // TrainCharacter's level-up (0x49730..0x49842): the level, THAC0 from
    // DS:3130, RollHPLevel; outside creation a Warrior at an even level
    // specializes (WeaponSpecialize(1)), the XP is capped one below the
    // next level's and the skills are allocated (40, 20, 15); every third
    // level the eight saves +0x1E..+0x25 go down by one.
    const int cls = c.b(0x28);
    c.b(0x29) = static_cast<std::uint8_t>(c.b(0x29) + 1);
    const int lvl = static_cast<std::int8_t>(c.b(0x29));
    c.b(0x33) = text_.ds8(kThac0 + static_cast<std::size_t>(cls * 10 + lvl));
    roll_hp_level(c);
    if (!creation) {
        if (cls == 3 && lvl % 2 == 0) weapon_specialize(true);   // 0x4977C
        const std::int32_t next = xp_for_level(c, lvl + 1);      // 0x49799
        if (static_cast<std::int32_t>(c.d(0x2F)) >= next) c.setd(0x2F, static_cast<std::uint32_t>(next - 1));
        allocate_skills(40, 20, 15);                             // 0x49807
    }
    if (lvl % 3 == 0)                                            // 0x49815
        for (std::size_t i = 0; i < 8; ++i) c.b(0x1E + i) = static_cast<std::uint8_t>(c.b(0x1E + i) - 1);
}

void Game::train_creation(Character& c) {
    // TrainCharacter in creation mode (DS:3BF7 = 1, 0x496E7): from level 1
    // on, DS:6B75 := 1 when the XP is below the next level's; else a level.
    // The status test (0x4952E) comes first in both modes: a character who
    // isn't okay gets RowMessage and the return (0x49554) without 6B75, so
    // the caller's loop asks again forever: a hang. Creation sets status 1
    // first (chargen.cpp), so no data reaches it; rejected (session 57).
    if (c.b(0xD4) != 1)
        throw Error("D-0090: TrainCharacter (ovl13 +0x2BFD, creation mode) for a character whose status is " +
                    std::to_string(c.b(0xD4)) + ", not okay: the original shows the message at 0x4953A and "
                    "returns without DS:6B75, and creation's loop calls it again forever (a hang); rejected");
    const int lvl = static_cast<std::int8_t>(c.b(0x29));
    if (lvl >= 1 && static_cast<std::int32_t>(c.d(0x2F)) < xp_for_level(c, lvl + 1)) {
        train_done_ = 1;
        return;
    }
    train_level(c, true);
}

void Game::train_character() {
    // TrainCharacter from the team menu (DS:3BF7 = 0): the member picked
    // (none after a click on CurChar's roster row, DS:6B4C); the refusals
    // by RowMessage (row 24, yellow, Game.Delay); else the question.
    if (!f6b4c_ && !pick_character()) return;                   // 0x4951B
    Character& c = *cur_;
    if (c.b(0xD4) != 1) {                                        // 0x4952E
        row_message(text_[Text::TrainConscious], 14, 0);
        return;
    }
    const int lvl = static_cast<std::int8_t>(c.b(0x29));
    if (lvl >= 8) {                                              // 0x49566
        row_message(text_[Text::TrainMaxLevel], 14, 0);
        return;
    }
    if (xp_for_level(c, lvl + 1) > static_cast<std::int32_t>(c.d(0x2F))) {   // 0x495B0
        row_message(text_[Text::TrainNoXp], 14, 0);
        return;
    }
    // 0x49604: the question over a cleared window.
    clear_cells(1, 1, 0x26, 0x16);
    draw_char_name(c, 4, 4);
    draw_string(static_cast<int>(c.name().size()) + 4, 4, 10, 0, text_[Text::TrainWillBecome]);
    draw_string(6, 5, 10, 0,
                text_[Text::TrainALevel] + str_byte(lvl + 1) + text_[Text::TrainSpace] +
                    text_.ds_str(kClassNames + 12 * static_cast<std::size_t>(static_cast<std::int8_t>(c.b(0x28))), 11));
    if (yes_no(text_[Text::TrainAsk], 1) != 'Y') return;        // 0x496C5
    show_message(text_[Text::TrainCongrats]);
    train_level(c, false);
}

}  // namespace cotd::game
