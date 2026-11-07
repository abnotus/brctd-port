// SPDX-License-Identifier: GPL-3.0-or-later
#include "game/game.hpp"

#include "game/combat_rules.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace cotd::game {

std::string str_long(std::int32_t v) {
    // StrLong 0653:13E8: groups of three with ','; ten or more characters
    // keep only the last three groups.
    const std::string d = std::to_string(v);
    const std::size_t L = d.size();
    if (L <= 3) return d;
    if (L <= 6) return d.substr(0, L - 3) + "," + d.substr(L - 3);
    if (L <= 9) return d.substr(0, L - 6) + "," + d.substr(L - 6, 3) + "," + d.substr(L - 3);
    return "," + d.substr(L - 9, 3) + "," + d.substr(L - 6, 3) + "," + d.substr(L - 3);
}

std::string Character::name() const {
    const int n = std::min<int>(rec[0], 15);
    return std::string(rec.begin() + 1, rec.begin() + 1 + n);
}

Game::Game(const GameFiles& files) : files_(files), text_(files) {
    init_screen();
    // ITEMS: loaded verbatim to DS:4A34 (run ecl_t2b: RAM equals the file).
    items_table_ = files_.read("ITEMS");
    // ITEM0.DAX #1: item types 1.. at DS:4EB9 + 16*type (docs/formats/data.md).
    if (auto e = dax_entry("ITEM0.DAX", 1)) {
        item0_ = *e;
        check_item0();
    }
}

void Game::check_item0() const {
    // The start-up loader fills 0xFE0 bytes from DS:4EC9 with 0xFF, then
    // loads the entry there (0x4D48C): 254 records of 16 bytes. A cut record
    // keeps 0xFF in its other bytes, a longer entry writes past the table,
    // and with no type among 1..15 WeaponSpecialize's list is empty: ListMenu
    // returns 0 with no key (0x5B668), and a must-choose call goes round for
    // ever (0x50E49..0x50E55). The shipped entry is 91 whole records with all
    // 15 types. Rejected here, per the quirk policy (D-0090).
    const std::size_t n = item0_.size();
    if (n % 16 != 0 || n > 0xFE0)
        throw Error("D-0090: ITEM0.DAX #1: " + std::to_string(n) +
                    " bytes is not whole 16-byte item records within the table's 254 (0x4D48C)");
    for (std::size_t t = 1; t <= 15; ++t)
        if (const std::uint8_t* r = item_type(t); r && r[0] != 0xFF) return;
    throw Error("D-0090: ITEM0.DAX #1: offset 0: none of item types 1..15 exists; WeaponSpecialize (ovl18 +0x0AA5) "
                "would never read a key");
}

const DaxFile& Game::dax(const std::string& name) {
    const std::string key = upper(name);
    auto it = dax_cache_.find(key);
    if (it == dax_cache_.end())
        it = dax_cache_.emplace(key, std::make_unique<DaxFile>(files_.read(key), key)).first;
    return *it->second;
}

std::optional<Bytes> Game::dax_entry(const std::string& name, int id) {
    // Dax.Load 0800:08FD: SoundSuspend first (0x1892C), SoundResume after.
    dax_cut();
    if (!files_.exists(name)) return std::nullopt;
    auto e = dax(name).load(static_cast<std::uint8_t>(id));
    if (e) data_note(("dax:" + name).c_str(), id, 0);   // tools/coverage_count.py (session 55)
    return e;
}

std::string Game::continue_item() const {
    // DS:0F00 is the one-item menu string, '~' + the prompt; menu() adds
    // the '~' hotkey marks itself.
    const std::string& s = text_[Text::MenuContinue];
    return !s.empty() && s[0] == '~' ? s.substr(1) : s;
}

std::string Game::ecl_file_name(const char* stem) const {
    return std::string(stem) + std::to_string(ecl_file_) + ".DAX";
}

std::uint32_t Game::random(std::uint32_t n) {
    // System.Random 0BE4:1150: RandSeed := RandSeed * 08088405h + 1;
    // result (RandSeed shr 16) mod n, 0 for n = 0 (tools/tprandom.py).
    const std::uint32_t before = seed_;
    seed_ = seed_ * 0x08088405u + 1u;
    const std::uint32_t r = n ? (seed_ >> 16) % n : 0;
    combat::rng_note(before, static_cast<int>(n), static_cast<int>(r));
    return r;
}

namespace {

// The combat speed option (D-0054): a Game.Delay sped up stays at least
// this long (or its own length, if shorter), so a message stays readable.
constexpr int kReadableMs = 150;

}  // namespace

void Game::pause_ms(int ms) {
    // Crt.Delay (0B00:029E): no key is read and the screen doesn't change,
    // so headless runs don't wait; the window shows the screen and waits.
    // In combat (ground fights, state 5; ship fights, state 8) the combat
    // speed option scales the wait (D-0054); 1.0 is the original's.
    if (!input_) return;
    const double f = input_->speed_factor();
    if (f != 1.0 && (mode_ == 5 || mode_ == 8)) ms = static_cast<int>(std::lround(ms * f));
    input_->pause(screen_, ms);
}

void Game::game_delay() {
    // Game.Delay (0653:0E3B): Crt.Delay(var 0x4BFC x 100). In a fight it
    // is the message wait: the combat speed option scales it, keeping it
    // at least kReadableMs below 1.0 (D-0054).
    const int chars = msg_chars_;
    msg_chars_ = 0;
    if (!input_) return;
    int ms = (v0(0x4BFC) * 100) & 0xFFFF;
    if (mode_ != 5 && mode_ != 8) {
        input_->pause(screen_, ms);
        return;
    }
    const double f = input_->speed_factor();
    int scaled = ms;
    if (f != 1.0) {
        scaled = static_cast<int>(std::lround(ms * f));
        if (f < 1.0) scaled = std::max(scaled, std::min(ms, kReadableMs));
    }
    message_waits_.emplace_back(chars, scaled);
    input_->pause(screen_, scaled);
}

void Game::step_pace() {
    // Step (ovl11 +0x081E) has no wait: on the machines of its time the
    // view's redraw took the time, on a modern one it takes none. The step
    // pace (D-0064, on in play) waits after each step the player sees,
    // divided by the combat speed's rate (D-0054).
    if (!input_ || mode_ != 5) return;
    const int base = input_->step_pace_ms();
    if (base <= 0) return;
    const int ms = static_cast<int>(std::lround(base * input_->speed_factor()));
    step_waits_.push_back(ms);
    input_->pause(screen_, ms);
}

void Game::message_chars(std::size_t n) {
    // Message text drawn in a fight, for the next message wait (tests).
    if (mode_ == 5 || mode_ == 8) msg_chars_ += static_cast<int>(n);
}

int Game::roll_dice(int count, int sides) {
    // Game.RollDice ovl23 +0x3025: sum of count x (Random(sides) + 1), as a
    // byte; count 0 calls the RNG not at all.
    int sum = 0;
    for (int i = 0; i < count; ++i) sum += static_cast<int>(random(static_cast<std::uint32_t>(sides))) + 1;
    return sum & 0xFF;
}

bool Game::save(Character& c, int type, int bonus) {
    combat::Rng r(seed_);
    const combat::SaveRoll s = combat::saving_throw(c.rec, type, bonus, r, has_effect(c, 0x0D));
    seed_ = r.seed();
    if (s.roll != 1 && s.roll != 20) effect_hook(c, 0x0C, "SavingThrow (ovl22 +0x0679)");
    return s.success;
}

// --- variables ----------------------------------------------------------------

namespace {
int size_of_type(std::uint8_t t) { return t <= 1 ? 1 : t <= 3 ? 2 : 4; }
}  // namespace

std::uint32_t Game::read_var(std::uint32_t addr, int size) {
    auto raw = [&](Bytes& blk, std::uint32_t off) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int i = 0; i < size; ++i) v |= static_cast<std::uint32_t>(off + i < blk.size() ? blk[off + i] : 0) << (8 * i);
        return v;
    };
    if (addr >= 0x4B00 && addr <= 0x4EFF) {
        if (addr == 0x4BEA) return facing_;   // read 0x4BEA -> DS:6A31
        return raw(vars0_, addr - 0x4B00);
    }
    if (addr >= 0x7C00 && addr <= 0x7FFF) {
        // The window on the current character (ovl15 +0x07BF).
        const std::uint32_t k = addr - 0x7C00;
        Character* c = cur_;
        auto rec_bytes = [&](std::size_t off) -> std::uint32_t {
            std::uint32_t v = 0;
            for (int i = 0; i < size; ++i) v |= static_cast<std::uint32_t>(off + i < kRecSize ? c->rec[off + i] : 0) << (8 * i);
            return v;
        };
        if (c) {
            if (k <= 0x0F) return rec_bytes(k + 1);
            if (k >= 0x10 && k <= 0xB9) {
                if (k == 0x4C) return c->b(0x4C) ? 0x80 : 0;
                return rec_bytes(k);
            }
            if (k >= 0xC4 && k <= 0xCD) return rec_bytes(k - 10);
            switch (k) {
                case 0x100: return no_char_ ? 0 : c->b(0xD4);
                case 0x10C: return c->b(0xD6) + (c->b(0x4C) ? 0x40 : 0) + (c->b(0xD7) ? 0x80 : 0);
                case 0x10E: return c->b(0xD8);
                case 0x10F: return c->b(0xD9);
                case 0x110: return c->b(0xDA);
                case 0x119: return c->b(0xE3);   // current HP, low byte only
                case 0x11B: return c->b(0xE5);
                case 0x2B1: return static_cast<std::uint32_t>(std::max(0, index_of(c)));
                default: break;
            }
        }
        if (k == 0x312) return ecl_file_;
        return raw(vars1_, k);
    }
    if (addr >= 0x7A00 && addr <= 0x7BFF) return raw(vars2_, addr - 0x7A00);
    if (addr >= 0x8000 && addr <= 0xA400) {
        std::uint32_t v = 0;
        for (int i = 0; i < size; ++i) v |= static_cast<std::uint32_t>(script_->byte(addr + i)) << (8 * i);
        return v;
    }
    // Region 4 (ovl15 +0x0764).
    switch (addr) {
        case 0x033D: return facing_;
        case 0xC04B: return static_cast<std::uint32_t>(static_cast<std::int32_t>(x_));
        case 0xC04C: return static_cast<std::uint32_t>(static_cast<std::int32_t>(y_));
        case 0xC04D: return v0(0x4BE6) ? facing_ / 2u : facing_;
        case 0xC04E: return wall_ahead_;
        case 0xC04F: return map_byte_;
        default: return 0;
    }
}

void Game::set_var(std::uint8_t type, std::uint32_t value, std::uint32_t addr) {
    if (type >= 0x80) throw Error("D-0090: SetVar with a string type has no size (uninitialised in the original)");
    const int size = size_of_type(type);
    auto store = [&](Bytes& blk, std::uint32_t off) {
        for (int i = 0; i < size; ++i)
            if (off + i < blk.size()) blk[off + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    if (addr >= 0x4B00 && addr <= 0x4EFF) {
        if (addr == 0x4BE6 && value != v0(0x4BE6)) {
            // ovl15 0x4C1CA..0x4C232: the area type sets the game state.
            prev_mode_ = mode_;
            const std::uint8_t lo = static_cast<std::uint8_t>(value);
            mode_ = (lo == 0xA2 || lo == 0xA8) ? 3 : value == 0x70 ? 9 : 4;
        }
        if (addr == 0x4BFD || addr == 0x4BFE) f82fe_ = 1;
        store(vars0_, addr - 0x4B00);
        return;
    }
    if (addr >= 0x7C00 && addr <= 0x7FFF) {
        const std::uint32_t k = addr - 0x7C00;
        store(vars1_, k);
        Character* c = cur_;
        if (k == 0 && value == 0) f82e7_ = 1;
        if (!c) return;
        // Mirror into the record (ovl15 +0x0A2D).
        if (k >= 0x10 && k <= 0xB9) {
            for (int i = 0; i < size; ++i)
                if (k + i < kRecSize) c->b(k + i) = static_cast<std::uint8_t>(value >> (8 * i));
        } else if (k == 0x100) {
            c->b(0xD4) = value & 0x7F;
            c->b(0xD5) = c->b(0xD4) == 1;
            roster_row(c, false);   // 0x4C0BA: RosterRow(CurChar, 0)
        } else if (k == 0x10C) {
            // 0x4C0C2..0x4C111: the bits as they are (0x40, 0x80, not 1:
            // route c_turned's save, session 32); an enemy (+0xD6 = 1)
            // makes the next COMBAT a fight (DS:82FD := 1).
            c->b(0xD6) = value & 1;
            c->b(0x4C) = value & 0x40;
            c->b(0xD7) = value & 0x80;
            if (c->b(0xD6) == 1) f82fd_ = 1;
        } else if (k == 0x10E) {
            c->b(0xD8) = static_cast<std::uint8_t>(value);
        } else if (k == 0x119) {
            c->setw(0xE3, value);
        } else if (k == 0x11B) {
            c->b(0xE5) = static_cast<std::uint8_t>(value);
        } else if (k == 0x312) {
            // 0800:0A99 (0x18A99): DS:4899 := DS:4898; DS:4898 := the value,
            // the ECL file number the next LOAD_ECL and the area's files use.
            ecl_file_prev_ = ecl_file_;
            ecl_file_ = static_cast<std::uint8_t>(value);
        }
        return;
    }
    if (addr >= 0x7A00 && addr <= 0x7BFF) {
        store(vars2_, addr - 0x7A00);
        return;
    }
    if (addr >= 0x8000 && addr <= 0xA400) {
        for (int i = 0; i < size; ++i) {
            const std::uint32_t a = addr + i;
            if (a - 0x8000 >= ecl::kArea)
                throw ecl::EclError("D-0007", script_->where(), a, "write past the script area");
            script_->mem()[a - 0x8000] = static_cast<std::uint8_t>(value >> (8 * i));
        }
        return;
    }
    if (addr <= 0xBF68) return;   // region 4 writes need addr > 49000
    switch (addr) {
        case 0xC04B:
        case 0xC04C:
            // The party's square: a byte the map, the view and WallAt index
            // as 0..15. Off the map (ECL1 #16 0x98D7 writes 16, the
            // technician's CHARGE) the original plays on: WallAt, MapByte
            // and the move test wrap it as signed bytes, the status line
            // prints it unsigned (runs s75_tech_charge, s75_offmap_ff,
            // s75_offmap_3f: 16, 255, 63). Taken as it is, with D-0073's
            // diagnostic.
            if ((value & 0xFF) > 15)
                combat::diag("D-0073", script_->where() + " at " + hex(op_at_, 4) + ": " +
                                           (addr == 0xC04B ? "x" : "y") + " := " + std::to_string(value & 0xFF) +
                                           ", off the area's 16 x 16 (the map, the view and a step wrap it, as "
                                           "the original's)");
            (addr == 0xC04B ? x_ : y_) = static_cast<std::int8_t>(value);
            f82fc_ = 1;
            break;
        case 0xC04D: {
            std::uint32_t v = value;
            while (v > 3) v -= 4;   // loops value -= 4 until 0..3
            facing_ = static_cast<std::uint8_t>(2 * v);
            f82fc_ = 1;
            break;
        }
        case 0xC059: c6cfc_ = static_cast<std::uint8_t>(value); f82fb_ = 1; break;
        case 0xC05F: c6cfd_ = static_cast<std::uint8_t>(value); f82fb_ = 1; break;
        default: break;
    }
}

std::string Game::get_string_var(std::uint32_t addr) {
    // GetStringVar ovl15 +0x1340 (0x4C920): by region (VarRegion 0x4BD44),
    // the raw bytes of the region's block up to a 0 byte: [DS:3BC8]
    // (0x4B00..), [DS:3BCC] (0x7C00..), [DS:3BD0] (0x7A00..), the script
    // (0x8000..0xA400); at most 255 characters (StrStore 0xFF). Address
    // 0x7C00 itself is CurChar's name string (0x4C9CB). Elsewhere: empty.
    if (addr == 0x7C00) {
        if (cur_.detached && !detached_name_.empty()) {
            // D-0055: CurChar is the block TeamMenu freed, whose first node of
            // the menu's list now lies there (run s80_detach_pick_enter).
            combat::diag("D-0055", "CurChar's name (0x7C00) read from the freed record: the team menu's first item "
                                   "(measured)");
            return detached_name_;
        }
        return cur_ ? cur_->name() : std::string();
    }
    std::string s;
    for (std::uint32_t a = addr; s.size() < 255; ++a) {
        std::uint8_t c = 0;
        if (addr >= 0x4B00 && addr <= 0x4EFF) c = a - 0x4B00 < vars0_.size() ? vars0_[a - 0x4B00] : 0;
        else if (addr >= 0x7C00 && addr <= 0x7FFF) c = a - 0x7C00 < vars1_.size() ? vars1_[a - 0x7C00] : 0;
        else if (addr >= 0x7A00 && addr <= 0x7BFF) c = a - 0x7A00 < vars2_.size() ? vars2_[a - 0x7A00] : 0;
        else if (addr >= 0x8000 && addr <= 0xA400) {
            if (a - 0x8000 >= ecl::kArea) throw ecl::EclError("D-0007", script_->where(), a, "string read past the script area");
            c = script_->mem()[a - 0x8000];
        }
        if (!c) break;
        s += static_cast<char>(c);
    }
    return s;
}

void Game::set_string_var(std::uint32_t addr, const std::string& s) {
    // SetStringVar ovl15 +0x0F97: characters, then a 0 byte (run ecl_t1).
    if (addr == 0x7C00) {   // the current character's name
        if (cur_) {
            const std::string n = s.substr(0, 15);
            cur_->b(0) = static_cast<std::uint8_t>(n.size());
            std::copy(n.begin(), n.end(), cur_->rec.begin() + 1);
        }
        return;
    }
    const bool region03 = (addr >= 0x4B00 && addr <= 0x4EFF) || (addr >= 0x7C00 && addr <= 0x7FFF) ||
                          (addr >= 0x7A00 && addr <= 0x7BFF) || (addr >= 0x8000 && addr <= 0xA400);
    if (!region03) return;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        const std::uint32_t a = addr + static_cast<std::uint32_t>(i);
        const std::uint8_t c = i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0;
        if (a >= 0x4B00 && a <= 0x4EFF) vars0_[a - 0x4B00] = c;
        else if (a >= 0x7C00 && a <= 0x7FFF) vars1_[a - 0x7C00] = c;
        else if (a >= 0x7A00 && a <= 0x7BFF) vars2_[a - 0x7A00] = c;
        else if (a >= 0x8000 && a - 0x8000 < ecl::kArea) script_->mem()[a - 0x8000] = c;
        else throw ecl::EclError("D-0007", script_->where(), a, "string write past the script area");
    }
}

// --- party --------------------------------------------------------------------

Character* Game::member(int index) {
    if (index < 0 || index >= static_cast<int>(party_.size())) return nullptr;
    return party_[static_cast<std::size_t>(index)].get();
}

int Game::index_of(const Character* c) const {
    for (std::size_t i = 0; i < party_.size(); ++i)
        if (party_[i].get() == c) return static_cast<int>(i);
    return -1;
}


void Game::join_party(std::unique_ptr<Character> c) {
    // JoinParty ovl17 +0x194D: slot 0xFF, append, CurChar := it; slot :=
    // the lowest of 0..7 no member uses (8 if none); var 0x7F3E += 1.
    c->b(0xB9) = 0xFF;
    Character* p = c.get();
    party_.push_back(std::move(c));
    cur_ = p;
    bool used[8] = {};
    for (const auto& m : party_)
        if (m->b(0xB9) < 8) used[m->b(0xB9)] = true;
    std::uint8_t slot = 8;
    for (std::uint8_t s = 0; s < 8; ++s)
        if (!used[s]) { slot = s; break; }
    p->b(0xB9) = slot;
    v1(0x7F3E) = static_cast<std::uint8_t>(v1(0x7F3E) + 1);
}

void Game::remove_char(bool keep_count, bool free_icon) {
    // RemoveChar ovl13 +0x1C47.
    if (party_.empty() || !cur_) return;
    const int i = index_of(cur_);
    if (i < 0) return;
    // ovl31 +0x00ED (0x6084D): the record's combat slot freed, with the images
    // any other record shares there (route s77_removed_copy: copies go undrawn).
    if (const std::size_t slot = cur_->b(0xB9); free_icon && slot < combat_icons_.size()) combat_icons_[slot] = CombatIcon{};
    // The record is freed: nothing of the fight may point at it any more.
    for (const auto& p : party_)
        if (p->cb_target == cur_) p->cb_target = nullptr;
    for (Icon& ic : icons_)
        if (ic.rec == cur_) ic.rec = nullptr;
    for (Body& b : bodies_)
        if (b.rec == cur_) b.rec = nullptr;
    for (auto& t : turn_list_)
        if (t == cur_) t = nullptr;
    if (saved_cur_ == cur_)   // D-0055: DS:4A30's record freed; Down from it reads its +0xFF
        detached_next_ = static_cast<std::size_t>(i) + 1 < party_.size() ? party_[static_cast<std::size_t>(i) + 1].get()
                                                                          : nullptr;
    // DS:387C still points at the freed record: its +0xFF, the successor,
    // is what SelectMember's Down reads (D-0056, run s69_d0056_down).
    freed_next_[cur_] = static_cast<std::size_t>(i) + 1 < party_.size() ? party_[static_cast<std::size_t>(i) + 1].get()
                                                                        : nullptr;
    if (f387c_ == cur_) {
        removed_pos_ = static_cast<std::size_t>(i);
        f387c_ = &removed_partner_;
        removed_next_ = static_cast<std::size_t>(i) + 1 < party_.size() ? party_[static_cast<std::size_t>(i) + 1].get()
                                                                         : nullptr;
    }
    if (i == 0) {
        party_.erase(party_.begin());
        cur_ = party_.empty() ? nullptr : party_.front().get();   // the successor
    } else {
        party_.erase(party_.begin() + i);
        cur_ = party_[static_cast<std::size_t>(i - 1)].get();      // the predecessor
    }
    if (!keep_count) v1(0x7F3E) = static_cast<std::uint8_t>(v1(0x7F3E) - 1);
    if (!cur_ && !party_.empty()) cur_ = party_.front().get();
}


std::unique_ptr<Character> Game::load_monster(int id) {
    // LoadMonster ovl17 +0x15FD: MON0CHA.DAX entry id (0x103 bytes used),
    // list pointers nil; effects from MON0SPC, items from MON0ITM, both
    // appended in file order.
    auto rec = dax_entry("MON0CHA.DAX", id);
    if (!rec || rec->empty()) throw Fatal(text_[Text::MonsterFatal]);
    if (rec->size() < kRecSize) throw Error("MON0CHA.DAX #" + std::to_string(id) + ": shorter than a record");
    data_note("monster", id);                          // tools only: --data-trace
    // ovl15 +0x04EE NewMonsterRecord (0x4BACE): a zeroed record, LoadMonster, its list heads (here the vectors).
    auto c = std::make_unique<Character>();
    std::copy(rec->begin(), rec->begin() + kRecSize, c->rec.begin());
    for (std::size_t off : {0xF7u, 0xB4u, 0xFBu, 0xFFu, 0xF3u}) c->setd(off, 0);
    if (auto spc = dax_entry("MON0SPC.DAX", id))
        for (std::size_t i = 0; i + kSpellSize <= spc->size(); i += kSpellSize)
            c->effects.emplace_back(spc->begin() + i, spc->begin() + i + kSpellSize);
    check_effect_nodes(c->effects, "MON0SPC.DAX #" + std::to_string(id));
    if (auto itm = dax_entry("MON0ITM.DAX", id))
        for (std::size_t i = 0; i + kItemSize <= itm->size(); i += kItemSize)
            c->items.emplace_back(itm->begin() + i, itm->begin() + i + kItemSize);
    flush_keys();                                      // 0x4F042
    return c;
}

void Game::take_damage(Character& c, int dmg) {
    // TakeDamage ovl23 +0x1968 (0x5788B..0x5794B), unsigned compares. In
    // combat (state 5) a dying record keeps the excess in cb+0x0C
    // (0x578F6), and a record knocked out leaves its side's count
    // DS:6988 + side and its turn (cb+1 := 0, 0x5792D).
    const std::uint16_t hp = c.w(0xE3);
    const std::uint16_t d = static_cast<std::uint16_t>(dmg);
    std::optional<std::uint8_t> status;
    if (hp >= d) {
        c.setw(0xE3, hp - d);
        if (hp == d) status = 5;
    } else {
        const int excess = d - hp;
        status = excess > 29 ? 3 : excess >= 10 ? 8 : 4;
        if (*status == 4 && mode_ == 5 && c.cb.size() == 0x12) c.cb[0x0C] = static_cast<std::uint8_t>(excess);
    }
    if (status) {
        c.b(0xD4) = *status;
        c.b(0xD5) = 0;
        c.setw(0xE3, 0);
        if (mode_ == 5) {
            const int side = static_cast<std::int8_t>(c.b(0xD6));
            if (side != 0 && side != 1) throw Error("D-0090: TakeDamage: side " + std::to_string(side) + " (+0xD6) out of 0..1");
            side_count_[static_cast<std::size_t>(side)] = static_cast<std::uint8_t>(side_count_[static_cast<std::size_t>(side)] - 1);
            if (c.cb.size() == 0x12) c.cb[1] = 0;
        }
    }
}

void Game::apply_damage(Character& c, int dmg) {
    // ApplyDamage ovl15 +0x1D2C (0x4D30C..0x4D45C).
    if (c.b(0xD4) != 1) return;
    const std::uint16_t hp = c.w(0xE3);
    std::string msg = text_[Text::DmgIndent] + c.name();
    if (static_cast<std::uint16_t>(hp + 10) < static_cast<std::uint16_t>(dmg))   // 0x4D342
        msg += text_[Text::DmgDies];
    else
        msg += text_[Text::DmgHitFor] + std::to_string(dmg) + text_[Text::DmgPoints];
    bool clear = false;
    if (row_ > bottom_) {   // 0x4D3CC: window full
        row_ = top_;
        clear = true;
        press_continue(15, 0);   // white on black (0x4D3E0)
    }
    col_ = 0x26;
    print(msg, clear, 15);   // white (0x4D40C)
    take_damage(c, dmg);
    // Unless DS:3BE8 is 3 or 9: Text.ClearRect(0x11, 1, 0x26, 0x0F) (the
    // roster panel and the status line) and the roster (0x4D43E..0x4D457).
    if (mode_ != 3 && mode_ != 9) {
        fill_rect(0x11, 1, 0x26, 0x0F, 0);
        draw_roster();
    }
}

// --- area -----------------------------------------------------------------------

std::uint8_t Game::map_byte(int x, int y) const {
    // ovl28 +0x05E3 (0x5D5B3): plane 2 (buffer +0x200) at y*16 + x
    // (ecl.md 0x42). Off the map (0x5D350's test): 0 in blocks 0 and 0x50,
    // else each coordinate wrapped as a signed byte (> 15: 0, < 0: 15).
    x = static_cast<std::int8_t>(x);
    y = static_cast<std::int8_t>(y);
    if (x < 0 || x > 15 || y < 0 || y > 15) {
        if (block_id_ == 0 || block_id_ == 0x50) return 0;
        x = x > 15 ? 0 : x < 0 ? 15 : x;
        y = y > 15 ? 0 : y < 0 ? 15 : y;
    }
    return geo_[0x200 + y * 16 + x];
}

int Game::wall_at(int d, int y, int x) const {
    // View.WallAt ovl28 +0x04E0 (docs/formats/view3d.md); its arguments
    // are signed bytes (D-0073).
    x = static_cast<std::int8_t>(x);
    y = static_cast<std::int8_t>(y);
    if (!(0 <= x && x < 16 && 0 <= y && y < 16)) {
        if (block_id_ == 0 || block_id_ == 0x50) return 0;
        x = x < 0 ? 15 : x >= 16 ? 0 : x;
        y = y < 0 ? 15 : y >= 16 ? 0 : y;
    }
    const int i = y * 16 + x;
    switch (d) {
        case 0: return geo_[i] >> 4;
        case 2: return geo_[i] & 0xF;
        case 4: return geo_[0x100 + i] >> 4;
        case 6: return geo_[0x100 + i] & 0xF;
        default: return 0;
    }
}

void Game::load_walldef(int id, int slot) {
    // LoadWalldef ovl28 +0x0ED1 (0x5DEA1..): slot > 3 does nothing; the
    // entry's 0x30C chunks go to [DS:6A22] + (slot + c) * 0x30C; a missing
    // entry, or one that doesn't fit, is fatal (0x5DF26: the message on the
    // text screen, a key, Halt).
    if (slot > 3) return;
    const std::string file = ecl_file_name("WALLDEF");
    auto e = dax_entry(file, id);
    const std::size_t size = e ? e->size() : 0;
    if (size == 0 || slot + size / 0x30C > 4)
        throw Fatal(text_[Text::WallUnable] + std::to_string(id) + text_[Text::WallFrom] +
                    std::to_string(ecl_file_) + text_[Text::WallEnd]);
    for (std::size_t off = 0, c = 0; off < size; off += 0x30C, ++c)
        std::copy(e->begin() + off, e->begin() + std::min(size, off + 0x30C),
                  walls_.begin() + (slot + c) * 0x30C);
    // Then the wall tiles (ovl33 +0x004F) and backgrounds (ovl28 +0x0E1C)
    // for this set: graphics only, not part of the headless state.
    // LoadTileSet ovl33 +0x004F: 8X8D<n> #id into tile set `slot` (key 13);
    // DS:959B := 1; LoadBack(id); DS:908B 0x20..0xB3 copied to DS:8D8B.
    {
        debug_slot(slot);                              // 0x60DE6
        const std::string tf = ecl_file_name("8X8D");
        auto raw = dax_entry(tf, id);
        if (!raw) throw Fatal(text_[Text::WallUnable] + std::to_string(id));
        tile_sets_[static_cast<std::size_t>(slot)] =
            std::make_unique<Image>(decode_generic(*raw, tf + " #" + std::to_string(id)));
        load_palette(*tile_sets_[static_cast<std::size_t>(slot)]);
        flush_keys();                                  // Gfx.LoadImage's 0x12BED
        f959b_ = 1;
        load_back(id);
        for (int i = 0x20; i < 0x20 + 0x94; ++i) pal_8d8b_[static_cast<std::size_t>(i)] = pal_908b_[static_cast<std::size_t>(i)];
    }
    walls_table_[2 * slot] = static_cast<std::uint16_t>(id);
    walls_table_[2 * slot + 1] = static_cast<std::uint16_t>(slot);
}

void Game::refresh_map_byte() {
    // RefreshMapByte ovl15 +0x1F79.
    map_byte_ = map_byte(x_, y_);
    if (f82f7_ || f82f9_ || f82fb_ || f82fc_ || f82fe_) {
        // DS:82DC := 0, DS:82DD := 0 (0x4D58F); 6B70 := 1; view redraw
        // (ovl26 +0x00A6), status line (ovl23 +0x2820).
        f82dc_ = f82dd_ = 0;
        f6b70_ = 1;
        draw_view_area();
        draw_status();
        f82f7_ = f82f9_ = f82fb_ = f82fc_ = f82fe_ = 0;
        wall_ahead_ = static_cast<std::uint8_t>(wall_at(facing_, y_, x_));
    }
}


void Game::pass_time(int n, int unit) {
    // ovl20 +0x0388: n of `unit` added to the clock vars 0x4BC6.., carried
    // by the radices DS:3104; the last unit's overflow ages the party.
    static constexpr int R[7] = {10, 10, 6, 24, 30, 12, 100};   // DS:3104
    int t[7];
    for (int k = 0; k < 7; ++k) t[k] = v0(0x4BC6 + k);
    for (int i = 0; i < n; ++i) {
        t[unit] += 1;
        for (int k = 0; k < 7; ++k) {   // Normalize ovl20 +0x02C1
            if (t[k] < R[k]) continue;
            if (k < 6) {
                t[k + 1] += 1;
                t[k] -= R[k];
            } else {
                for (auto& m : party_) m->b(0x38) += 1;
                t[6] -= 100;
            }
        }
    }
    for (int k = 0; k < 7; ++k) v0(0x4BC6 + k) = static_cast<std::uint8_t>(t[k]);
    timed_effects(n, unit);
}

void Game::timed_effects(int n, int unit) {
    // TimedEffects ovl20 +0x0000 (0x54650..0x5490E). Outside mode 2 every
    // record's flag DS:388D + i is set; in mode 2 only the flags left set
    // (a record that still had a timed effect) count, and none set: out.
    static constexpr int R[7] = {10, 10, 6, 24, 30, 12, 100};   // DS:3104
    if (mode_ != 2) {
        for (int i = 1; i <= 0x48; ++i) f388d_[static_cast<std::size_t>(i)] = 1;
    } else {
        bool any = false;
        const int count = v1(0x7F3E);
        for (int i = 1; !any; ++i) {
            if (f388d_[static_cast<std::size_t>(i & 0xFF)]) any = true;
            if (i + 1 > count) break;
        }
        if (!any) return;
    }
    std::uint16_t total = static_cast<std::uint16_t>(n & 0xFF);
    for (int u = unit; u > 1; --u) total = static_cast<std::uint16_t>(total * R[u - 1]);
    while (total > 0) {
        const int step = total > 10 ? 10 : total;
        int i = 1;
        for (auto& m : party_) {
            std::uint8_t& flag = f388d_.at(static_cast<std::size_t>(i));
            ++i;
            if (!flag) continue;
            flag = 0;
            // The record's nodes as the linked list they are (+0xFB, next
            // +5), walked as the original does (0x5472F): `pred` (BP-0x13)
            // is the current node's predecessor (the node itself at the
            // head); after a removal the walk goes on from the head when
            // `pred` is the head, else from the second node as it was at
            // the start (BP-0x17), even when that one is gone. A freed
            // node keeps its bytes: FreeMem (0BE4:0341, 0x1C318..0x1C3EF)
            // writes only HeapPtr or a free-list entry at FreePtr - 8,
            // never the block, and nothing is allocated before the walk
            // reads it (D-0070, measured: tools/te_trap.py).
            struct Node {
                Bytes b;
                int next;
                bool freed;
            };
            std::vector<Node> v;
            for (std::size_t k = 0; k < m->effects.size(); ++k)
                v.push_back({m->effects[k], k + 1 < m->effects.size() ? static_cast<int>(k + 1) : -1, false});
            int head = v.empty() ? -1 : 0;
            const int last = v.empty() ? -1 : static_cast<int>(v.size()) - 1;
            const int second = head < 0 ? -1 : v[0].next;
            auto freed = [&](int k) { return v[static_cast<std::size_t>(k)].freed; };
            int node = head, pred = head;
            bool done = false;
            while (node >= 0 && !done) {
                // No freed node is met here: the removal below goes on after
                // the removed node instead (D-0070, session 75).
                if (freed(node))
                    throw Error("TimedEffects (ovl20 0x547C6): " + m->name() +
                                "'s walk met a freed effect node (an internal error: D-0070's step past it missed)");
                Node& nd = v[static_cast<std::size_t>(node)];
                if (node == last) done = true;
                const int dur = le16(&nd.b[1]);
                if (dur == 0) {
                    node = nd.next;
                } else if (step < dur) {
                    put16(&nd.b[1], dur - step);
                    flag = 1;
                    node = nd.next;
                } else {
                    // Effect.Remove ovl22 +0x00AF with the node: its handler
                    // first when node+4 is set.
                    if (nd.b[4] != 0) removal_handler(*m, nd.b, "TimedEffects");
                    if (head == node) {
                        head = nd.next;
                    } else {
                        int p = head;
                        while (v[static_cast<std::size_t>(p)].next != node) p = v[static_cast<std::size_t>(p)].next;
                        v[static_cast<std::size_t>(p)].next = nd.next;
                    }
                    nd.freed = true;
                    node = pred == head ? head : second;   // 0x54831
                    if (!done && node >= 0 && freed(node)) {
                        // The second node was freed earlier in the walk:
                        // the original's next pass Removes it again (not
                        // on the list: the unlink writes its link through
                        // nil, FreeMem frees it again) and lands on it
                        // once more, for ever (measured: run s75_te_first,
                        // 32,768 loads of the node and no key read again).
                        // A hang can't be the reference value (rule 4):
                        // the walk goes on after the node just removed.
                        combat::diag("D-0070", "GAME.OVR ovl20 +0x0176 (0x547C6), TimedEffects: " + m->name() +
                                                   "'s walk would remove a freed effect node again (the original "
                                                   "hangs there); it goes on after the node it removed");
                        node = nd.next;
                    }
                }
                pred = head;                                // 0x54864
                while (pred >= 0 && pred != node && v[static_cast<std::size_t>(pred)].next != node)
                    pred = v[static_cast<std::size_t>(pred)].next;
            }
            for (; node >= 0; node = v[static_cast<std::size_t>(node)].next) {   // 0x548B2
                if (freed(node))
                    combat::diag("D-0070", "GAME.OVR ovl20 +0x026D (0x548BD), TimedEffects: " + m->name() +
                                           "'s walk goes on from an effect node it has freed; its duration and link "
                                           "are read as FreeMem left them");
                if (le16(&v[static_cast<std::size_t>(node)].b[1]) > 0) flag = 1;
            }
            m->effects.clear();
            for (int k = head; k >= 0; k = v[static_cast<std::size_t>(k)].next)
                m->effects.push_back(v[static_cast<std::size_t>(k)].b);
        }
        total = total > 10 ? static_cast<std::uint16_t>(total - 10) : 0;
    }
}

void Game::step_forward() {
    // Step ovl15 +0x1A58: one square in the facing direction, wrapping at
    // the map edges (signed compares); no wall check, no RNG.
    switch (facing_) {
        case 0: y_ = y_ <= 0 ? 15 : y_ - 1; break;
        case 2: x_ = x_ >= 15 ? 0 : x_ + 1; break;
        case 4: y_ = y_ >= 15 ? 0 : y_ + 1; break;
        case 6: x_ = x_ <= 0 ? 15 : x_ - 1; break;
        default: break;
    }
    map_byte_ = map_byte(x_, y_);
    wall_ahead_ = static_cast<std::uint8_t>(wall_at(facing_, y_, x_));
    f82fc_ = 1;
}

// --- dumps ----------------------------------------------------------------------





}  // namespace cotd::game
