// SPDX-License-Identifier: GPL-3.0-or-later
// Saved games: LOAD SAVED GAME (LoadSavedGame ovl17 0x4F1E8, CharLoad
// 0x4E6D9) and SAVE (SaveGame ovl17 +0x220C = 0x4F92C, CharSave 0x4DF60).
// File layout: tools/craft_save.py; pointer and stale bytes: DIVERGENCE.md
// D-0010, D-0011. File names: find_file (D-0016).
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "game/game.hpp"

namespace fs = std::filesystem;

namespace cotd::game {

namespace {

constexpr std::size_t kPtrOffs[5] = {0xB4, 0xF3, 0xF7, 0xFB, 0xFF};
constexpr std::uint32_t kPlaceholder = 0xFFFFFFFF;   // D-0010

std::string join(const std::string& dir, const std::string& name) {
    return (fs::path(dir) / name).string();
}

// A file in the save directory, matched without regard to case (DOS
// ignores case; the original passes mixed-case names such as "savgam" +
// slot + ".dat"). An existing file keeps its name, so it is overwritten
// rather than doubled; a new one is named in upper case, as DOS stores
// names (DIVERGENCE.md D-0016).
// A file name's characters (D-0092, session 78): what InputString types,
// 0x21..0x7A, without < > ", which neither DOS nor Windows takes in a file
// name; the name prompts refuse them and a loaded name holding one is
// rejected, so every name that reaches the disk is one both systems hold.
bool file_name_char(unsigned char ch) {
    // The space too (session 78's night, fuzz keys 1000758): StripChar's
    // quirk keeps one of two adjacent spaces, so a typed "A  B" makes the
    // original's file "A B.WHO", which DOS and Windows both hold.
    return ch >= 0x20 && ch <= 0x7A && !std::strchr("<>\"", ch);
}

std::string find_file(const std::string& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string have;
        try {
            have = e.path().filename().string();
        } catch (const std::exception&) {
            continue;   // a name this process can't convert isn't one the game asks for
        }
        if (upper(have) == upper(name)) return e.path().string();
    }
    return join(dir, upper(name));
}

// 0800:052E (0x1852E): File.Exists: FindFirst, attribute 0, DOS error 0 and a non-empty name
bool file_exists(const std::string& dir, const std::string& name) {
    return fs::exists(find_file(dir, name));
}

// 0800:019E (0x1819E): DeleteIfExists: File.Exists, then Assign, Close, Erase
void delete_if_exists(const std::string& dir, const std::string& name) {
    std::error_code ec;
    fs::remove(find_file(dir, name), ec);
}

std::vector<Bytes> read_nodes(const std::string& path, std::size_t size) {
    // CharLoad reads full nodes while a whole one is read (a short final
    // read is ignored).
    const Bytes b = read_file(path);
    std::vector<Bytes> out;
    for (std::size_t i = 0; i + size <= b.size(); i += size) out.emplace_back(b.begin() + i, b.begin() + i + size);
    return out;
}

// The pointer the original would write: nil for none; otherwise the bytes
// last read from disk, or a placeholder (D-0010).
std::uint32_t pointer(bool present, std::uint32_t disk) {
    if (!present) return 0;
    return disk ? disk : kPlaceholder;
}

}  // namespace

void check_effect_nodes(const std::vector<Bytes>& nodes, const std::string& file) {
    // Effect.Remove calls through DS:699D + 4 type when node+4 is set
    // (0x55246, no bounds test); InitHandlers (ovl10 +0x082B) fills types
    // 1..0x20 only, so another type calls through the data next to the
    // table (type 0: DS:699D itself, which 0x1F's handler writes). 0x1F's
    // handler (ovl10 +0x05AB, a touch attack: SavingThrow, AttackResult)
    // isn't ported, and its hook 0x0E runs for every combatant each round
    // (0x3BF2D), so a 0x1F node is rejected whatever +4 holds (session 62:
    // no shipped file makes one).
    for (std::size_t k = 0; k < nodes.size(); ++k) {
        const Bytes& n = nodes[k];
        if (n.size() < 5) continue;
        const int t = n[0];
        if (t == 0x1F) {
            throw Error(file + " +0x" + hex(static_cast<std::uint32_t>(kSpellSize * k), 2) + ": effect node " +
                        std::to_string(k + 1) + " is type 0x1F, whose handler (ovl10 +0x05AB) is not ported (D-0073)");
        }
        if (!n[4]) continue;
        if (t != 0 && t <= 0x20) continue;
        throw Error(file + " +0x" + hex(static_cast<std::uint32_t>(kSpellSize * k + 4), 2) + ": effect node " +
                    std::to_string(k + 1) + " (type " + hex(static_cast<std::uint32_t>(t), 2) + ") has its removal "
                    "handler flag set, and the type is outside the handler table DS:699D (1..0x20) (D-0073)");
    }
}

namespace {

Bytes nodes_bytes(const std::vector<Bytes>& nodes, std::size_t next_off) {
    Bytes out;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        Bytes n = nodes[i];
        put32(&n[next_off], pointer(i + 1 < nodes.size(), le32(&n[next_off])));
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}

// StripChar 0800:0578 (0x18578): every occurrence of `sub` deleted,
// scanning i = 1 .. len(s) - len(sub) + 1 (computed once); after a deletion
// i still advances, so of two adjacent matches one survives (quirk).
std::string strip_char(std::string s, const std::string& sub) {
    const int n = static_cast<int>(s.size()) - static_cast<int>(sub.size()) + 1;
    for (int i = 1; i <= n; ++i)
        if (static_cast<std::size_t>(i - 1) < s.size() && s.compare(static_cast<std::size_t>(i - 1), sub.size(), sub) == 0)
            s.erase(static_cast<std::size_t>(i - 1), sub.size());
    return s;
}

}  // namespace

std::string Game::make_file_name(const std::string& name) const {
    // CharSave's file name (0x4DF60): MakeFileName 0800:0647 of
    // StripChar(name, '.'): StripChar of each of the ten characters at
    // DS:0EA4..0EAD, then the first 8 characters, upper case.
    std::string s = strip_char(name, ".");
    for (std::size_t k = 0; k < 10; ++k) s = strip_char(s, std::string(1, static_cast<char>(text_.ds8(0x0EA4 + k))));
    if (s.size() > 8) s.resize(8);
    for (char& ch : s)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    return s;
}

bool Game::save_file_exists(const std::string& name) const { return file_exists(save_dir_, name); }

std::string Game::save_path(const std::string& name) const { return find_file(save_dir_, name); }

namespace {

}  // namespace


void Game::set_logbook_from_text(const std::string& name, const std::string& text) {
    logbook_text_ = std::make_unique<LogbookText>(LogbookText::from_text(name, text));
    for (const std::string& d : logbook_text_->diagnostics()) combat::diag("D-0083", d);
}

bool Game::logbook_named(int entry) {
    // The entries the shipped scripts' LOGBOOK calls name (docs/notes/
    // logbook.md: 30 numbers; the 31st call, ECL4 #66 0xA268, names the
    // number the player typed, shown only when it is one of them).
    static const int kNamed[] = {4,  7,  8,  10, 14, 17, 21, 22, 27, 28, 29, 30, 34, 35, 38,
                                 39, 41, 44, 45, 46, 50, 52, 54, 55, 60, 63, 66, 68, 70, 71};
    return std::find(std::begin(kNamed), std::end(kNamed), entry) != std::end(kNamed);
}

void Game::delete_roster_files(const Character& c) {
    // 0x4DDD2: DeleteIfExists of the member's roster files, found by the
    // record's name, so a roster character who only shares the name goes
    // too (D-0015). With the fix they are moved into SAVE/DELETED instead
    // (an existing copy there keeps its name: the new one gets ~2, ~3 ...),
    // so the roster looks as the original leaves it and nothing is lost.
    const std::string base = make_file_name(c.name());
    for (const char* ext : {".HID", ".WHO", ".STF", ".SFX"}) {
        if (!fixes_.roster_backup || !file_exists(save_dir_, base + ext)) {
            delete_if_exists(save_dir_, base + ext);
            continue;
        }
        const fs::path bak = fs::path(save_dir_) / "DELETED";
        std::error_code ec;
        fs::create_directories(bak, ec);
        fs::path to = bak / (base + ext);
        for (int n = 2; fs::exists(to); ++n) to = bak / (base + "~" + std::to_string(n) + ext);
        fs::rename(find_file(save_dir_, base + ext), to, ec);
        if (ec) delete_if_exists(save_dir_, base + ext);   // as the original, when it can't be kept
    }
}

std::unique_ptr<Character> Game::char_load(const std::string& base, const char* ext) {
    // CharLoad 0x4E6D9: read the record, then nil every pointer field
    // (0x4E7A4..0x4E7E5); the disk values are kept only for writing back.
    auto c = std::make_unique<Character>();
    c->rec = read_file(find_file(save_dir_, base + ext));
    if (c->rec.size() != kRecSize) throw Error(base + ext + ": not a 0x103-byte record");
    // D-0073: the career indexes the tables at DS:31A2 (XP, 32 bytes a
    // career) and on; no record the game makes or ships has one outside
    // 1..5, and past them the reads leave the data segment.
    if (c->rec[0x28] < 1 || c->rec[0x28] > 5)
        throw Error(base + ext + " +0x28: career " + std::to_string(c->rec[0x28]) + ", not 1..5 (D-0073)");
    // The name: a 15-character string (InputString's limit); a longer
    // length reads the next fields as its characters (session 54, N14).
    if (c->rec[0] > 15)
        throw Error(base + ext + " +0x00: name length " + std::to_string(c->rec[0]) + ", more than 15 (D-0073)");
    // D-0092: the file name the name makes (CharSave's strip) holds only
    // what the name prompt types; no record the game writes holds more
    // (session 78: 19,388 records of the recorded runs, NPCs included).
    for (const char ch : make_file_name(c->name()))
        if (!file_name_char(static_cast<unsigned char>(ch)))
            throw Error(base + ext + " +0x01: the name holds a character a file name can't (D-0092)");
    for (std::size_t i = 0; i < 5; ++i) {
        c->disk_ptrs[i] = c->d(kPtrOffs[i]);
        c->setd(kPtrOffs[i], 0);
    }
    // Lists by file existence: .STF, else .HID (0x4E82F, 0x4E957 ->
    // 0x4EAA7: a member with both loses the stash, D-0013; the fix loads
    // both); then .SFX.
    const bool stf = file_exists(save_dir_, base + ".STF");
    if (stf) c->items = read_nodes(find_file(save_dir_, base + ".STF"), kItemSize);
    if ((!stf || fixes_.stash_load) && file_exists(save_dir_, base + ".HID"))
        c->stash = read_nodes(find_file(save_dir_, base + ".HID"), kItemSize);
    if (file_exists(save_dir_, base + ".SFX")) {
        c->effects = read_nodes(find_file(save_dir_, base + ".SFX"), kSpellSize);
        check_effect_nodes(c->effects, base + ".SFX");
    }
    flush_keys();   // 0x4EBF4
    recalc(*c);     // 0x4EBFF
    return c;
}

bool Game::char_save_roster(Character& c, bool last) {
    // CharSave(c, '') ovl17 +0x0840 (0x4DF60): <name>.WHO and its lists.
    // An existing file asks: ShowMessage(name + CS:0804), then
    // MenuScreen('', CS:0814): 'O' writes over it; 'R' asks for a new name
    // (InputString CS:0830, max 8, until non-empty), which becomes the
    // file name and the character's name (StrStore 15), then writes
    // without another check; 'A' gives up; other keys ask again.
    if (!save_dir_ready()) return false;
    std::string base = make_file_name(c.name());
    int choice = 0;
    while (file_exists(save_dir_, base + ".WHO") && choice != 'O' && choice != 'R') {
        show_message(base + text_[Text::Exists]);
        const MenuKey mk = menu_screen("", text_[Text::ExistsItems], false);
        choice = mk.key;
        if (choice == 'A') return false;
        if (choice == 'R') {
            std::string n;
            // D-0092: the new file name isn't stripped, so every character
            // DOS or Windows refuses in a file name is refused here.
            while (n.empty()) n = input_string(text_[Text::NewFileName], 10, 0, 8, kEntryLetters, ".*,?/\\:;|<>\"");
            base = n;
            const std::string nm = n.substr(0, 15);
            c.b(0) = static_cast<std::uint8_t>(nm.size());
            std::fill(c.rec.begin() + 1, c.rec.begin() + 16, 0);
            std::copy(nm.begin(), nm.end(), c.rec.begin() + 1);
        }
    }
    // The typed name isn't stripped (InputString allows 0x21..0x7A): with
    // '/' or ':' DOS can't create the file and CharSave's writes fail
    // silently; the game goes on with nothing written (run s54_rename_bad,
    // route rename_bad_name). '\' likewise (inference: the path separator).
    // Since D-0092 the prompts refuse these, so this is the original's
    // path kept, no longer reached by typing.
    if (base.find_first_of("/\\:") != std::string::npos) return true;
    char_save(c, base, last, ".WHO");
    return true;
}

void Game::load_save(const std::string& path) {
    // LoadSavedGame (0x4F1E8): the 11 blocks in file order.
    const Bytes s = read_file(path);
    if (s.size() != 2901) throw Error(path + ": not a 2901-byte saved game");
    save_dir_ = fs::path(path).parent_path().string();
    // The tools' start from a save (ecl-run --save) has had no Game.Init: the
    // team menu's enable bytes, as there (DS:0A49 + 0x2A*i), so Create and Add
    // are in a script's team menu as after a launch (fuzz keys 759443).
    for (int i = 0; i < 13; ++i) team_enable_[static_cast<std::size_t>(i)] = text_.ds8(0x0A49 + 0x2A * i);
    ecl_file_ = s[0];
    std::copy(s.begin() + 0x001, s.begin() + 0x401, vars0_.begin());
    std::copy(s.begin() + 0x401, s.begin() + 0x801, vars1_.begin());
    std::copy(s.begin() + 0x801, s.begin() + 0xA01, vars2_.begin());
    x_ = static_cast<std::int8_t>(s[0xA01]);
    y_ = static_cast<std::int8_t>(s[0xA02]);
    facing_ = s[0xA03];
    wall_ahead_ = s[0xA04];
    map_byte_ = s[0xA05];
    prev_mode_ = s[0xA06];
    file_prev_mode_ = s[0xA06];
    mode_ = s[0xA07];
    walls_table_[0] = le16(&s[0xA08]);
    walls_table_[1] = le16(&s[0xA0A]);
    std::copy(s.begin() + 0xA0D, s.begin() + 0xA0D + 0x148, name_table_.begin());
    // D-0073: what no saved game holds is rejected here, naming the
    // offset: the 16 x 16 area's square, a facing (0..7), a state the
    // game saves in (the original's saves hold 3, 4, 5, 9, 10, and 0 from
    // TeamMenu's EXIT, 0x46E9B: D-0077), more
    // members than the name table's 8 entries (0x148 bytes; past them the
    // names would come from the next fields), and a name that isn't a file name CharSave makes
    // (1..8 characters, none of DS:0EA4's ` .*,?/\:;|` or below 0x21).
    auto reject = [&](std::size_t off, const std::string& what) {
        throw Error(path + " +0x" + hex(static_cast<std::uint32_t>(off), 3) + ": " + what + " (D-0073)");
    };
    // A square off the map is one a script can leave (D-0073, runs
    // s75_tech_charge, s75_offmap_*); the original saves, loads and plays
    // on with any byte, wrapping it where the map is read.
    if (s[0xA03] > 7) reject(0xA03, "facing " + std::to_string(s[0xA03]));
    if (s[0xA07] != 0 && s[0xA07] != 3 && s[0xA07] != 4 && s[0xA07] != 5 && s[0xA07] != 9 && s[0xA07] != 10)
        reject(0xA07, "state " + std::to_string(s[0xA07]) + ", not one the game saves in");
    if (s[0xA0C] > 8) reject(0xA0C, std::to_string(s[0xA0C]) + " members, more than the name table holds");
    for (int k = 0; k < s[0xA0C]; ++k) {
        const std::uint8_t* p = &s[0xA0D + 41 * k];
        bool ok = p[0] >= 1 && p[0] <= 8;
        for (int i = 1; ok && i <= p[0]; ++i)
            ok = p[i] != ' ' && file_name_char(p[i]) && !std::strchr(".*,?/\\:;|", p[i]);   // D-0092: < > " too
        if (!ok) reject(0xA0D + 41 * static_cast<std::size_t>(k), "member " + std::to_string(k + 1) + "'s file name");
    }
    loaded_game_ = 1;   // DS:3BF1 := 1 (0x4F3B8)
    ++loads_;           // the port's count (D-0081's script-load)
    menu_sum_ = 0xFF;   // DS:6B48 := 0xFF (0x4F2BA)
    ds3bec_ = static_cast<std::uint8_t>(v0(0x4BFF) / 2);
    ds3bee_ = static_cast<std::uint8_t>(v0(0x4BFF) & 1);
    v1(0x7F3E) = 0;     // joining counts the members again
    const int count = s[0xA0C];
    for (int k = 0; k < count && k < 8; ++k) {
        const std::uint8_t* p = &s[0xA0D + 41 * k];
        const std::string base(p + 1, p + 1 + std::min<int>(p[0], 40));
        if (!file_exists(save_dir_, base + ".SAV")) continue;   // skipped silently
        join_party(char_load(base, ".SAV"));
    }
    // D-0056: DS:387C on a record REMOVE freed before this LOAD: the loaded
    // records take the freed blocks first-fit in their order, so it names
    // the loaded record at the freed one's position (run
    // s80_d0056_after_load: ACE removed, the party removed, LOAD: ACE white
    // at the Trade, then DOC, DUNE; s80_d0056_load_pos3: DUNE, position 3).
    if (f387c_ == &removed_partner_ && removed_pos_ < party_.size()) {
        combat::diag("D-0056", "LOAD (0x4F1E8): DS:387C's freed record reused by the loaded record at its position " +
                                   std::to_string(removed_pos_ + 1) + " (measured)");
        f387c_ = party_[removed_pos_].get();
    }
    freed_next_.clear();
    for (const auto& m : party_) delete_roster_files(*m);   // 0x4F62D
    // 0x4F645..0x4F6AD: each member's combat icon, CurChar walking the list:
    // CHARS for the party (0x4DD7F), CPIC for an NPC (+0xE7 >= 0x80).
    for (const auto& m : party_)
        load_combat_icon(m->b(0xE7) < 0x80 ? "CHARS" : "CPIC", m->b(0xB8), m->b(0xB9));
    ecl_file_ = v1(0x7F12);   // DS:4898 := var 0x7F12 (0x4F6C3)
    // CurChar is the first member afterwards (observed: every run starts
    // with CurChar at index 0; inference about where it is set).
    cur_ = party_.empty() ? nullptr : party_.front().get();
    // 0x4F6C6 (main session, session 26): a station's area type (var
    // 0x4BE6 in CS:1AA8 = {0xA2, 0xA8}) loads its big picture instead;
    // otherwise the area, unless the file's state (DS:3BE8, before it is
    // reset at 0x4F745) is 10, the base's big-picture menus.
    const std::uint8_t area = v0(0x4BE6);
    const bool restore = static_cast<std::int16_t>(walls_table_[0]) > 0 && v0(0x4BC5) != 0x7F && mode_ != 10;
    if (area == 0xA2 || area == 0xA8) {
        // 0x4F6DC..0x4F703 (session 32): the planet's big picture, 0x48 on
        // Venus, 0x47 on Mars, loaded afresh (DS:6B14 := 0xFF), not drawn.
        f6b14_ = 0xFF;
        load_big_pic(area == 0xA2 ? 0x48 : 0x47);
    } else if (restore && fixes_.wipe_load && v0(0x4BC5) == 0 && !dax_entry(ecl_file_name("GEO"), 0)) {
        // D-0081 (wipe-load): a game saved after the party's end (Game.Reset
        // zeroes the variables, GEO 0, and leaves the walls pair: N4) asks
        // for GEO #0, which no GEO file holds: the area isn't restored,
        // and BEGIN starts the game as after the reset.
        combat::diag("D-0081", path + ": a game saved after the party's end (GEO 0): no area restored");
    } else if (restore) {
        // Area restore (0x4F705..0x4F736).
        const std::string file = ecl_file_name("GEO");
        auto g = dax_entry(file, v0(0x4BC5));
        if (!g || g->size() != 0x402) throw Fatal(text_[Text::GeoFatal]);
        std::copy(g->begin() + 2, g->end(), geo_.begin());
        load_walldef(walls_table_[0] & 0xFF, walls_table_[1] & 0xFF);
    }
    flush_keys();   // 0x4F73B
    clear_row24();  // 0x4F740: 0653:1359
    prev_mode_ = mode_;   // 0x4F745: DS:3BE9 := DS:3BE8, DS:3BE8 := 0
    mode_ = 0;
}

void Game::char_save(Character& c, const std::string& base, bool last, const char* ext) {
    // CharSave 0x4DF60: the record as in memory, with its pointer fields
    // per D-0010; then the item, stash and effect lists.
    Bytes rec = c.rec;
    const bool present[5] = {!c.stash.empty(), false, !c.items.empty(), !c.effects.empty(), !last};
    for (std::size_t i = 0; i < 5; ++i) {
        // +0xF3 is nil outside combat (the loader nils it; nothing outside
        // the combat state sets it).
        const std::uint32_t live = c.d(kPtrOffs[i]);
        put32(&rec[kPtrOffs[i]], pointer(present[i] || (i == 1 && live), live ? live : c.disk_ptrs[i]));
    }
    // Each file keeps the name it already has (whatever its case), even
    // when the original deletes it before writing it again (D-0016).
    const std::string stf = find_file(save_dir_, base + ".STF");
    const std::string hid = find_file(save_dir_, base + ".HID");
    const std::string sfx = find_file(save_dir_, base + ".SFX");
    write_file(find_file(save_dir_, base + ext), rec);
    delete_if_exists(save_dir_, base + ".STF");                       // 0x4E3FA
    if (!c.items.empty()) {
        delete_if_exists(save_dir_, base + ".HID");                   // 0x4E436
        write_file(stf, nodes_bytes(c.items, 0x2A));
    }
    if (!c.stash.empty()) write_file(hid, nodes_bytes(c.stash, 0x2A));
    // With both lists empty the old .HID stays and the next load takes it
    // (D-0014); the fix deletes it.
    else if (fixes_.stale_stash) delete_if_exists(save_dir_, base + ".HID");
    delete_if_exists(save_dir_, base + ".SFX");                       // 0x4E620
    if (!c.effects.empty()) write_file(sfx, nodes_bytes(c.effects, 5));
}


bool Game::room_to_save(char slot) const {
    // ovl17 +0x2034 (0x4F754): the party's size: 0x103 a record, 0x3E an item
    // of +0xF7, or of the stash +0xB4 when +0xF7 is empty, 9 an effect; a word.
    // SaveGame 0x4F9CF..0x4FA15: + 0xA08 unless SAVGAMx.DAT exists, against
    // DiskFree. A free space the system won't tell: as room (the write decides).
    std::uint32_t need = file_exists(save_dir_, std::string("SAVGAM") + slot + ".DAT") ? 0 : 0xA08;
    std::uint16_t party = 0;
    for (const auto& m : party_)
        party = static_cast<std::uint16_t>(party + 0x103 + 0x3E * (m->items.empty() ? m->stash.size() : m->items.size()) + 9 * m->effects.size());
    need += party;
    if (free_bytes_) return static_cast<long long>(need) <= *free_bytes_;
    std::error_code ec;
    const auto sp = fs::space(save_dir_, ec);
    return ec || need <= sp.available;
}

void Game::save_game(char slot) {
    // SaveGame 0x4F92C after the slot menu.
    std::error_code ec;
    fs::create_directories(save_dir_, ec);   // 0x4DB68: MkDir if missing
    if (!room_to_save(slot)) {               // 0x4FA2D: the message, and out past DS:499B
        press_continue(0x0E, 0, text_[Text::NoRoom]);
        return;
    }
    // 0653:1320 (0x17850, called at 0x4FB3D): row 24's right part cleared, the line in green.
    fill_rect(0x14, 0x18, 0x27, 0x18, 0);
    draw_string(0, 0x18, 10, 0, text_[Text::Saving]);
    v0(0x4BFF) = static_cast<std::uint8_t>(ds3bee_ + 2 * ds3bec_);   // 0x4FB42
    v1(0x7F12) = ecl_file_;                                            // 0x4FB5B
    Bytes s;
    s.push_back(ecl_file_);
    s.insert(s.end(), vars0_.begin(), vars0_.end());
    s.insert(s.end(), vars1_.begin(), vars1_.end());
    s.insert(s.end(), vars2_.begin(), vars2_.end());
    for (std::uint8_t b : {static_cast<std::uint8_t>(x_), static_cast<std::uint8_t>(y_), facing_, wall_ahead_, map_byte_})
        s.push_back(b);
    s.push_back(prev_mode_);
    s.push_back(mode_);
    for (int i = 0; i < 2; ++i) {
        s.push_back(static_cast<std::uint8_t>(walls_table_[i]));
        s.push_back(static_cast<std::uint8_t>(walls_table_[i] >> 8));
    }
    // Every record in the list is a member (monsters and NPCs too), named
    // "CHRDAT" + slot + n; StrStore writes only length and characters
    // into the table (D-0011).
    const std::size_t count = std::min<std::size_t>(party_.size(), 8);
    s.push_back(static_cast<std::uint8_t>(count));
    Bytes table = name_table_;
    std::vector<std::string> names;
    for (std::size_t n = 1; n <= count; ++n) {
        const std::string name = std::string("CHRDAT") + slot + std::to_string(n);
        names.push_back(name);
        std::uint8_t* e = &table[41 * (n - 1)];
        e[0] = static_cast<std::uint8_t>(name.size());
        std::copy(name.begin(), name.end(), e + 1);
    }
    s.insert(s.end(), table.begin(), table.end());
    if (s.size() != 2901) throw Error("save: SAVGAMx.DAT came out at the wrong size");
    write_file(find_file(save_dir_, std::string("SAVGAM") + slot + ".DAT"), s);
    for (std::size_t n = 0; n < count; ++n) {
        char_save(*party_[n], names[n], n + 1 == party_.size());
        delete_roster_files(*party_[n]);   // 0x4DDD2
    }
    saved_ = 1;   // DS:499B
    clear_row24();   // 0x4FDED: 0653:1359
}

}  // namespace cotd::game
