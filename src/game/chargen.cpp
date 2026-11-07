// SPDX-License-Identifier: GPL-3.0-or-later
// CREATE NEW CHARACTER: CreateCharacter ovl13 +0x06FE (0x4700E). Rules:
// docs/notes/chargen.md (model tools/chargen.py); flow and screens:
// docs/notes/frontend.md.
#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr std::size_t kRaceNames = 0x1224;     // DS: string[13], 14 per race
constexpr std::size_t kGenderNames = 0x12A2;   // DS: string[6], 7 per gender
constexpr std::size_t kClassNames = 0x11D0;    // DS: string[11], 12 per class
constexpr std::size_t kClassOfRace = 0x3175;   // DS: [race*7] count, [race*7 + k] class
constexpr std::size_t kRaceMod = 0x32CB;       // DS: [race*7 + i], signed
constexpr std::size_t kClassMin = 0x3303;      // DS: [class*7 + i]
constexpr std::size_t kThac0 = 0x3130;         // DS: [class*10 + level]
constexpr std::size_t kSaveBase = 0x32CA;      // DS: [i], i = 0..7
constexpr std::size_t kSaveRaceMod = 0x3282;   // DS: [race*8 + i]

}  // namespace

void Game::set_default_icon(Character& c) {
    // SetDefaultIcon ovl13 +0x1D8A (0x4869A): +0xB8; RollDice(1,4) for a
    // Desert Runner (race 6).
    const int race = c.b(0x27);
    if (race == 5) {
        c.b(0xB8) = 0x11;
    } else if (race == 6) {
        static constexpr std::uint8_t kRunner[4] = {0x10, 0x17, 0x18, 0x1F};
        c.b(0xB8) = kRunner[(roll_dice(1, 4) - 1) & 3];
    } else {
        c.b(0xB8) = text_.ds8(0x0C41 + static_cast<std::size_t>(c.b(0x26)) * 5 + c.b(0x28));
    }
}

void Game::give_starting_items(Character& c) {
    // GiveStartingItems ovl14 +0x03F9 (0x4A7A9): for k = 1, 2 a node with
    // ITEM0 type DS:0C4B[k], +0x38 := DS:0C4D[k], readied, prepended to the
    // list. Its bytes 0..0x28 are uninitialised heap in the original; the
    // port writes 0, as every observed file has (D-0022).
    for (std::size_t k = 1; k <= 2; ++k) {
        Bytes node(kItemSize, 0);
        const std::size_t type = text_.ds8(0x0C4B + k);
        const std::uint8_t* r = item_type(type);
        if (!r) throw Error("ITEM0.DAX: no starting item type " + std::to_string(type));
        std::copy(r, r + 16, node.begin() + 0x2E);
        node[0x38] = text_.ds8(0x0C4D + k);
        node[0x29] = 0;
        node[0x34] |= 0x80;
        data_note("ready", node[0x2E], c.b(0xD6));     // tools only: --data-trace
        c.items.insert(c.items.begin(), node);
    }
}

int Game::pick_from_list(Text header, const std::vector<std::string>& names) {
    // CreateCharacter's race / gender / class menus (0x470A6, 0x471BD,
    // 0x472D6): node 0 the heading, then "  " + name; ListMenu('',
    // 'Select', 1, 2, 0x26, 0x16, Exit shown) until 'S'; 0 (Esc, Exit)
    // aborts the creation. Returns the index (1-based), or -1.
    std::vector<ListNode> list;
    list.push_back({text_[header], true});
    for (const std::string& n : names) list.push_back({(text_[Text::ItemIndent] + n).substr(0, 40), false});
    int index = 1, picked = -1;
    bool redraw = true;
    for (;;) {
        const int k = list_menu("", text_[Text::CgSelect], 1, 2, 0x26, 0x16, list, true, redraw, index, picked);
        if (k == 0) return -1;
        if (k == 'S') return index;
    }
}

void Game::create_character() {
    // CreateCharacter ovl13 +0x06FE.
    auto rec = std::make_unique<Character>();
    Character& c = *rec;
    for (std::size_t i = 0; i < 16; ++i) c.b(0xC4 + i) = static_cast<std::uint8_t>(i);
    c.b(0x37) = 0x32;
    c.b(0x33) = 0x28;
    c.b(0xD4) = 1;
    c.b(0xD5) = 1;
    c.b(0x34) = 1;
    c.b(0xB9) = 0x0A;
    c.setd(0x2B, 1000);
    // Race, gender, career.
    std::vector<std::string> races;
    for (std::size_t r = 1; r <= 6; ++r) races.push_back(text_.ds_str(kRaceNames + 14 * r, 13));
    const int race = pick_from_list(Text::PickRace, races);
    if (race < 0) return;
    c.b(0x27) = static_cast<std::uint8_t>(race);
    std::vector<std::string> genders;
    for (std::size_t g = 0; g < 2; ++g) genders.push_back(text_.ds_str(kGenderNames + 7 * g, 6));
    const int gender = pick_from_list(Text::PickGender, genders);
    if (gender < 0) return;
    c.b(0x26) = static_cast<std::uint8_t>(gender - 1);
    std::vector<std::string> classes;
    const std::size_t n = text_.ds8(kClassOfRace + 7 * static_cast<std::size_t>(race));
    for (std::size_t k = 1; k <= n; ++k)
        classes.push_back(text_.ds_str(kClassNames + 12 * static_cast<std::size_t>(text_.ds8(kClassOfRace + 7 * static_cast<std::size_t>(race) + k)), 11));
    const int ci = pick_from_list(Text::PickClass, classes);
    if (ci < 0) return;
    const int cls = text_.ds8(kClassOfRace + 7 * static_cast<std::size_t>(race) + static_cast<std::size_t>(ci));
    c.b(0x28) = static_cast<std::uint8_t>(cls);
    c.setd(0x2F, text_.ds32(0x31AA + static_cast<std::size_t>(cls) * 0x20));
    c.b(0x33) = text_.ds8(kThac0 + static_cast<std::size_t>(cls) * 10 + c.b(0x29));
    for (std::size_t i = 0; i < 8; ++i)   // InitSaves ovl13 +0x05A2
        c.b(0x1E + i) = static_cast<std::uint8_t>(text_.ds8(kSaveBase + i) - static_cast<std::int8_t>(text_.ds8(kSaveRaceMod + static_cast<std::size_t>(race) * 8 + i)));
    c.b(0x38) = static_cast<std::uint8_t>(18 + roll_dice(1, 6));
    const CurChar saved = cur_;
    cur_ = &c;
    c.b(0x29) = 2;
    c.b(0xD9) = 0x32;
    c.setw(0xE3, 0);
    c.setw(0x45, 0);
    draw_sheet();
    int rerolls = 0;
    for (;;) {
        // The roll (0x47501): each ability the best of six 3d6, the race
        // modifier (byte add), at least 1, at least the career minimum.
        for (std::size_t i = 0; i < 7; ++i) {
            c.b(0x10 + i) = 0;
            for (int t = 0; t < 6; ++t) {
                const int v = roll_dice(3, 6);
                if (v > c.b(0x10 + i)) c.b(0x10 + i) = static_cast<std::uint8_t>(v);
            }
            c.b(0x10 + i) = static_cast<std::uint8_t>(c.b(0x10 + i) + text_.ds8(kRaceMod + static_cast<std::size_t>(race) * 7 + i));
            if (c.b(0x10 + i) < 1) c.b(0x10 + i) = 1;
            const std::uint8_t mn = text_.ds8(kClassMin + static_cast<std::size_t>(cls) * 7 + i);
            if (c.b(0x10 + i) < mn) c.b(0x10 + i) = mn;
            c.b(0x17 + i) = c.b(0x10 + i);
            sheet_ability(static_cast<int>(i), false);
        }
        c.b(0x3D) = 1;
        c.b(0x3F) = (race == 6 || cls == 3) ? 3 : 2;
        c.b(0x41) = race == 6 ? 1 : 0;
        c.b(0x3B) = cls == 3 ? 4 : 2;
        c.b(0x36) = race <= 4 ? 12 : race == 5 ? 9 : 15;
        c.b(0x29) = 0;
        c.setw(0x45, 0);
        c.setw(0xE3, 0);
        const std::uint8_t t = v1(0x7EA8);   // CreationMode DS:3BF7 = 1
        v1(0x7EA8) = 0xFF;
        train_done_ = 0;
        while (!train_done_) train_creation(c);
        v1(0x7EA8) = t;
        sheet_combat();
        init_career_skills(c);
        sheet_career_skills();
        sheet_credits();
        int k;
            k = yes_no(text_[Text::Reroll], 2);
        if (k == 'N') break;
        ++rerolls;
    }
    draw_sheet();
    do {
        // D-0092: the name becomes the roster file's (CharSave strips
        // . * , ? / \ : ; |), so only what the strip leaves is refused.
        std::string name = input_string(text_[Text::CharName], 0x0D, 0, 15, kEntryLetters, "<>\"");
        if (name.size() > 15) name.resize(15);
        c.b(0) = static_cast<std::uint8_t>(name.size());
        std::copy(name.begin(), name.end(), c.rec.begin() + 1);
    } while (c.b(0) == 0);
    if (cls == 3) weapon_specialize(true);
    init_skills(c);
    allocate_skills(80, 40, 30);
    set_default_icon(c);
    icon_select();
    if (yes_no(text_[Text::SaveChar] + c.name() + text_[Text::SaveCharEnd], 1) == 'N') {
        cur_ = saved;
        return;
    }
    give_starting_items(c);
    char_save_roster(c, true);
    cur_ = saved;
}



}  // namespace cotd::game
