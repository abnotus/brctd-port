// SPDX-License-Identifier: GPL-3.0-or-later
// The ECL interpreter: run loop, dispatch and opcode handlers
// (docs/formats/ecl.md; handler addresses are ovl2 offsets unless noted).
#include <algorithm>
#include <cctype>

#include "core/real48.hpp"
#include "game/game.hpp"

namespace cotd::game {

namespace {

using ecl::Operand;

int size_of(std::uint8_t t) { return t <= 1 ? 1 : t <= 3 ? 2 : 4; }

[[noreturn]] void throw_ecl(const char* code, const std::string& where, std::uint16_t at, const std::string& msg) {
    throw ecl::EclError(code, where, at, msg);
}

[[noreturn]] void throw_ni(const std::string& where, std::uint16_t at, std::uint8_t op, const char* name) {
    throw Error(where + " at " + hex(at, 4) + ": opcode 0x" + hex(op, 2) + " (" + name +
                ") is not implemented in this engine yet");
}

// SkillTotal (ovl23 +0x2E3F, 0x58D4F) adds the skill sign-extended to the
// record (0x58D55: cbtw): from 0x80 it reads before the record, and any
// skill past InitSkills' 85 reads the skill table's next data. The shipped
// scripts' operands are constants, 1..84 (D-0090).
void check_skill_operand(int skill, const std::string& where, std::uint16_t at) {
    if (skill > 85)
        throw Error("D-0090: " + where + " at " + hex(at, 4) + ": skill " + std::to_string(skill) + " past the table's 85");
}

#define REJECT(code, msg) throw_ecl(code, where, at, msg)
#define NOT_IMPLEMENTED(name) throw_ni(where, at, op, name)

}  // namespace

// --- blocks and the run loop ---------------------------------------------------

void Game::load_block(int id) {
    // LoadEcl ovl15 +0x042A: 'ECL' + Str(DS:4898) + '.dax' entry id; the
    // first 2 bytes are dropped, the rest copied over 0x2401 zero bytes.
    const std::string file = ecl_file_name("ECL");
    steps_unread_ += 100;   // D-0079: a load weighs as 100 steps (a script reloading its block)
    auto raw = dax_entry(file, id);
    if (!raw || raw->size() < 2) {
        // D-0081: a LOAD in a script's TEAM_MENU left the old block running
        // with the loaded game's file, and that block's LOAD_ECL names a
        // block this file lacks: LoadEcl's retry never ends in the original
        // (run s76_keys759443b). Rule 4: the run starts again with the game
        // the LOAD gave, as --fixes script-load does at the LOAD itself. The
        // old block has written var 0x4BF2 (LOAD_ECL, +0x0CC7) and may have
        // changed the state; the LOAD's are put back. Only once: the flag is
        // down, so a block the restart itself lacks stops as below.
        if (script_menu_load_) {
            script_menu_load_ = false;
            combat::diag("D-0081", file + ": no block " + std::to_string(id) +
                                       " after a LOAD in a script's TEAM_MENU (ovl2 +0x23F3): LoadEcl (ovl15 +0x042A) "
                                       "would retry forever; the run starts again with the loaded game");
            v0(0x4BF2) = script_load_block_;
            mode_ = script_load_mode_;
            prev_mode_ = script_load_prev_mode_;
            throw RestartRun{};
        }
        throw Error(file + ": no block " + std::to_string(id) + " (the original retries forever)");
    }
    script_menu_load_ = false;   // a block that exists: the run has left the old one
    auto s = std::make_unique<ecl::Script>(*raw, file + " #" + std::to_string(id));
    traced_file_ = ecl_file_;   // tools only: the trace names the block loaded (session 59)
    // Load-time half of the quirk policy (D-0003..D-0007); a block that
    // passed once isn't checked again (a script reloading its own block).
    if (load_checks_ && !validated_.count(file + " #" + std::to_string(id) + ":" + std::string(raw->begin(), raw->end()))) {
        const auto errors = s->validate();
        if (!errors.empty()) throw errors.front();
        validated_.insert(file + " #" + std::to_string(id) + ":" + std::string(raw->begin(), raw->end()));
    }
    script_ = std::move(s);
    clear_row24();   // 0x4BAB8: 0653:1359, after LoadEcl's loading line (0x4BA15)
}

void Game::ecl_init() {
    // EclInit ovl15 +0x02DB.
    f82f7_ = f82f8_ = f82e6_ = f82e7_ = 0;
    f82dc_ = f82dd_ = 0;   // 0x4B8E4
    c6cfc_ = 0x41;
    c6cfd_ = 9;
    f82fb_ = 1;
    icon_next_ = 8;
    pc_ = 0x8000;
    gosub_.clear();
    flags_.fill(0);
    v1(0x7ED2) = 0;
    v1(0x7ED3) = 0;
    v0(0x4BE5) = 0;
    entries_ = script_->entries();
    if (!loaded_game_) {
        for (std::uint32_t a = 0x4C00; a <= 0x4C1F; ++a) v0(a) = 0;
        for (std::uint32_t a = 0x7F79; a <= 0x7F82; ++a) v1(a) = 0;
    }
}

void Game::run_ecl(std::uint16_t addr) {
    // RunEcl ovl2 +0x30CB.
    pc_ = addr;
    stop_ = 0;
    while (!stop_ && !party_lost_) step();
    stop_ = 0;
}

void Game::enter_block() {
    // EnterBlock ovl2 +0x3117, after LOAD_ECL.
    for (;;) {
        free_picture();                                // ovl27 +0x0B04 (0x3396A)
        new_block_ = 0;
        map_byte_ = map_byte(x_, y_);
        wall_ahead_ = static_cast<std::uint8_t>(wall_at(facing_, y_, x_));
        f82fc_ = 1;
        v1(0x7ED5) = 0;
        saved_cur_ = cur_;
        run_ecl(entries_[4]);
        if (!new_block_) v0(0x4BF2) = block_id_;
        if (new_block_) continue;
        // 0x339E0..0x33A03: the view area drawn, in state 4 from state 4,
        // or with DS:3581 set unless state 4 was left.
        const bool was4 = prev_mode_ == 4, is4 = mode_ == 4;
        if ((was4 && is4) || (area_flags_[0] && !(was4 && !is4))) draw_view_area();
        new_block_ = 0;
        run_ecl(entries_[0]);
        if (new_block_) continue;
        run_ecl(entries_[1]);
        if (new_block_) continue;
        restore_cur();                       // 0x33A2B: CurChar := DS:4A30,
        draw_roster();                                 // Screen.Roster(CurChar)
        prev_mode_ = mode_;
        return;
    }
}


void Game::run_with_loads() {
    for (;;) {
        try {
            begin_run_loop();
            return;
        } catch (const RestartRun&) {
            // D-0081 (script-load): as after a LOAD from the start, the
            // main program's Screen.Redraw (0x101F6), then RunLoop.
            redraw_screen();
        }
    }
}

void Game::begin_run_loop() {
    // Ecl.RunLoop ovl2 +0x3209.
    saved_cur_ = cur_;
    f6b70_ = 1;
    f82f8_ = 0;
    set_char_ = 0;
    area_flags_.fill(0);
    first_area_ = 1;
    new_block_ = 0;
    if (v0(0x4BF2) == 0) {
        first_area_ = 0;
        mode_ = 4;
        if (demo_) {
            block_id_ = 0x12;                          // 0x33AAC: the demo's block, no roster
        } else {
            block_id_ = 0x10;
            draw_roster();
        }
    } else {
        block_id_ = v0(0x4BF2);
    }
    load_block(block_id_);
    ecl_init();
    run_ecl(entries_[4]);
    if (demo_) {
        // 0x33AF7: the demo's party goes, RemoveChar(1, 1) from the head on,
        // and the run loop is over.
        while (!party_.empty()) {
            cur_ = party_.front().get();
            remove_char(true, true);
        }
        return;
    }
    if (new_block_) enter_block();
    else v0(0x4BF2) = block_id_;
    if (mode_ != 3 && loaded_game_) {
        if (first_area_ == 1) redraw_screen();
        f6b70_ = 1;
        draw_view_area();
    }
    loaded_game_ = 0;
    move_mode_ = 0;   // DS:8303 := 0 before the main loop (0x33B80)
    run_loop();
}

// --- operands --------------------------------------------------------------------

namespace {

struct Args {
    std::vector<Operand> ops;
};

}  // namespace


void Game::ecl_exit() {
    // EXIT (ovl2 +0x0000, 0x30850): with the SET_CHAR flag DS:3580,
    // CurChar := DS:4A30 and the flag cleared (0x30853..0x30867); then
    // FillChar(DS:82DC, 2, 0), DS:82F7 := 0, the stop, PC + 1, the GOSUB
    // list freed, the cursor home (col 1, row 0x11), DS:82F6 := 0.
    if (set_char_) {
        restore_cur();
        set_char_ = 0;
    }
    f82dc_ = 0;
    f82dd_ = 0;
    f82f7_ = 0;
    stop_ = 1;
    pc_ += 1;
    gosub_.clear();
    col_ = 1;
    row_ = 0x11;
    f82f6_ = 0;
}

void Game::step() {
    if (!script_) throw Error("no script loaded");
    prev_op_ = op_;
    op_ = script_->byte(pc_);   // D-0007 checked by the fetch
    const std::uint8_t op = op_;
    const std::string where = script_->where();
    const std::uint16_t at = pc_;
    op_at_ = at;
    // D-0079: a script that runs this long without the game reading,
    // polling or waiting for a key loops forever in the original (the
    // recorded routes' longest run is under 2048 steps).
    if (++steps_unread_ > kStepsUnread)
        REJECT("D-0079", std::to_string(kStepsUnread) + " steps without a key (an instruction 1, a block load 100): the original loops forever");
    // The trace names the block's own file: a script's write to var 0x7F12
    // changes ecl_file_ for the next load, not the block running (session
    // 59: the instruction after such a SAVE was counted under the new file).

    // DecodeArgs (ovl15 +0x0000): from PC on the opcode; afterwards PC is
    // on the next opcode. String operands fill the string slots in order.
    auto decode = [&](int n) {
        std::uint32_t pc = pc_;
        auto ops = script_->decode_args(pc, n);
        pc_ = static_cast<std::uint16_t>(pc + 1);
        str_slots_.clear();
        for (const Operand& a : ops) {
            if (!(a.type <= 5 || a.type == 0x80 || a.type == 0x81))
                REJECT("D-0006", "operand type 0x" + hex(a.type, 2) + " has no value");
        }
        return ops;
    };
    // GetArgValue ovl15 +0x019A.
    auto value = [&](const Operand& a) -> std::uint32_t {
        switch (a.type) {
            case 0: return a.lo;
            case 1: return read_var(*a.word(), 1);
            case 2: return *a.word();
            case 3: return read_var(*a.word(), 2);
            case 4: {
                const std::int32_t low = static_cast<std::int16_t>(a.lo | *a.hi << 8);   // CWD: Q3
                return static_cast<std::uint32_t>((a.b3 << 24) + (a.b2 << 16) + low);
            }
            case 5: return read_var(*a.word(), 4);
            case 0x81: return *a.word();
            case 0x80: return 0;
            default: REJECT("D-0006", "operand type 0x" + hex(a.type, 2) + " has no value");
        }
        return 0;
    };
    // The string slots: text literals and string variables, in order.
    auto strings = [&](const std::vector<Operand>& ops) {
        std::vector<std::string> out;
        for (const Operand& a : ops) {
            if (a.type == 0x80) out.push_back(a.text);
            else if (a.type == 0x81) out.push_back(get_string_var(*a.word()));
        }
        return out;
    };
    auto dest = [&](const Operand& a) -> std::uint32_t {
        if (!a.word()) REJECT("D-0004", "destination operand without a high byte");
        return *a.word();
    };
    auto jump_target = [&](const Operand& a) -> std::uint16_t {
        if (!a.word()) REJECT("D-0004", "jump through an operand without a high byte");
        return *a.word();
    };
    // Compare (ovl15 +0x1934): signed 32-bit X vs Y into the six flags.
    auto compare = [&](std::int32_t x, std::int32_t y) {
        flags_ = {x == y, x != y, x < y, x > y, x <= y, x >= y};
    };
    // var(k) opcodes: decode k, n = value of operand k, PC -= 1, decode n.
    auto decode_var = [&](int k) {
        auto first = decode(k);
        // DecodeArgs takes the count as a byte (0x4B5EA); past 63 it
        // writes outside its operand arrays (D-0078).
        const std::uint32_t n = value(first.back()) & 0xFF;
        if (n > ecl::kMaxOperands)
            REJECT("D-0078", "operand count " + std::to_string(n) + " past the original's " +
                                 std::to_string(ecl::kMaxOperands) + "-operand arrays");
        pc_ = static_cast<std::uint16_t>(pc_ - 1);
        auto rest = decode(static_cast<int>(n));
        return std::make_pair(first, rest);
    };

    switch (op) {
        case 0x00: ecl_exit(); break;   // EXIT (+0x0000)
        case 0x01: {   // GOTO, ovl2 +0x0088 (0x308D8)
            auto a = decode(1);
            pc_ = jump_target(a[0]);
            break;
        }
        case 0x02: {   // GOSUB, ovl2 +0x00A7 (0x308F7)
            auto a = decode(1);
            // ovl15 +0x19CE Gosub (0x4CFAE): push the PC on the list DS:6D04, jump to operand k's hi:lo (+ line 738).
            gosub_.push_back(pc_);
            pc_ = jump_target(a[0]);
            break;
        }
        case 0x03: {   // COMPARE (+0x00BE)
            auto a = decode(2);
            const bool s0 = a[0].is_string(), s1 = a[1].is_string();
            if (s0 != s1) REJECT("D-0005", "COMPARE of a string with a non-string");
            if (s0) {
                auto s = strings(a);
                // ovl15 +0x185E CompareStr (0x4CE3E): the six flags DS:6D0C from StrCompare.
                const int c = s[0].compare(s[1]);   // TP StrCmp: bytewise, then length
                compare(c < 0 ? -1 : c > 0 ? 1 : 0, 0);
            } else {
                compare(static_cast<std::int32_t>(value(a[0])), static_cast<std::int32_t>(value(a[1])));
            }
            break;
        }
        case 0x04: case 0x05: case 0x06: case 0x07: {   // ADD SUB DIV MUL (+0x0147)
            auto a = decode(3);
            const std::int32_t x = static_cast<std::int32_t>(value(a[0]));
            const std::int32_t y = static_cast<std::int32_t>(value(a[1]));
            std::int32_t r = 0;
            if (op == 0x04) r = static_cast<std::int32_t>(static_cast<std::uint32_t>(x) + static_cast<std::uint32_t>(y));
            if (op == 0x05) r = static_cast<std::int32_t>(static_cast<std::uint32_t>(y) - static_cast<std::uint32_t>(x));
            if (op == 0x07) r = static_cast<std::int32_t>(static_cast<std::uint32_t>(x) * static_cast<std::uint32_t>(y));
            if (op == 0x06) {
                if (y == 0)   // Q7: the original exits with TP runtime error 200
                    throw Stopped(where + " at " + hex(at, 4) + " (entry byte 0x" + hex(at - 0x8000 + 2, 4) +
                                  "): DIV by zero [Q7]");
                r = x / y;
                const std::int32_t rem = x % y;
                for (int i = 0; i < 4; ++i) v1(0x7F3F + i) = static_cast<std::uint8_t>(rem >> (8 * i));
            }
            set_var(a[2].type, static_cast<std::uint32_t>(r), dest(a[2]));
            break;
        }
        case 0x08: {   // RANDOM (+0x0242)
            auto a = decode(2);
            std::uint32_t v = value(a[0]) & 0xFF;
            if (v < 0xFF) v += 1;
            // Random(n) through IntToReal and Trunc, then a byte (0x30ACA..
            // 0x30ADB): exact for a word, kept as the original does it.
            std::uint32_t r = static_cast<std::uint8_t>(real48::trunc(real48::from_long(static_cast<std::int32_t>(random(v)))));
            set_var(a[1].type, r, dest(a[1]));
            break;
        }
        case 0x09: {   // SAVE (+0x02A7)
            auto a = decode(2);
            if (a[0].is_string()) set_string_var(dest(a[1]), strings(a)[0]);
            else set_var(a[1].type, value(a[0]), dest(a[1]));
            break;
        }
        case 0x0A: {   // SET_CHAR (+0x02FF)
            set_char_ = 1;
            auto a = decode(1);
            const std::uint32_t v = value(a[0]) & 0xFF;
            Character* p = member(static_cast<int>(v & 0x7F));
            if (p) {
                cur_ = p;
                no_char_ = 0;
            } else {
                no_char_ = 1;
            }
            if ((v & 0x80) && f82e6_ && f82e7_) {
                if (saved_cur_ == cur_) set_char_ = 0;
                remove_char(false, true);
                f82e6_ = f82e7_ = 0;
            }
            break;
        }
        case 0x0B: {   // ADD_MONSTERS id, count, picture (+0x047E)
            auto a = decode(3);
            const CurChar keep = cur_;
            const int count = static_cast<int>(value(a[1]) & 0xFF);
            if (combatants_ >= 0x3F || count == 0) break;
            auto rec = load_monster(static_cast<int>(value(a[0]) & 0xFF));
            if (rec->b(0xD6) == 1 && v0(0x4BB6) < 5) {   // HP by difficulty, var 0x4BB6
                std::uint32_t hp = static_cast<std::uint16_t>((v0(0x4BB6) + 2u) * rec->w(0xE3)) / 4u;
                if (!hp) hp = 1;
                rec->setw(0xE3, hp);
                rec->setw(0x45, hp);
            }
            // 0x30E15: the picture into the slot DS:6D17 (CPIC + the area digit).
            load_combat_icon("CPIC", static_cast<int>(value(a[2]) & 0xFF), icon_next_);
            const Character tmpl = *rec;
            rec->b(0xB9) = icon_next_;
            party_.push_back(std::move(rec));
            ++combatants_;
            for (int i = 2; i <= count && combatants_ < 0x3F; ++i) {
                auto c = std::make_unique<Character>();
                c->rec = tmpl.rec;
                c->b(0xB9) = icon_next_;
                // Copies get the template's lists prepended node by node,
                // i.e. reversed (0x30E7E..0x310C9).
                c->items.assign(tmpl.items.rbegin(), tmpl.items.rend());
                c->effects.assign(tmpl.effects.rbegin(), tmpl.effects.rend());
                party_.push_back(std::move(c));
                ++combatants_;
            }
            ++icon_next_;
            f82fd_ = 1;
            cur_ = keep;
            break;
        }
        case 0x0C: {   // SPRITE_START sprite, max distance, pic, mask (+0x03D9, 0x30C29)
            auto a = decode(4);
            sprite_id_ = static_cast<std::uint8_t>(value(a[0]));
            v1(0x7EC0) = static_cast<std::uint8_t>(value(a[1]));
            sprite_pic_ = static_cast<std::uint8_t>(value(a[2]));
            approach_mask_ = static_cast<std::uint8_t>(value(a[3]));
            v1(0x7EC1) = static_cast<std::uint8_t>(steps_ahead(
                x_, y_, facing_, static_cast<std::uint32_t>(ecl_file_) << 24 | std::uint32_t{block_id_} << 16 | at));
            if (v1(0x7EC0) < v1(0x7EC1)) v1(0x7EC1) = v1(0x7EC0);
            sprite_step(sprite_id_, sprite_pic_, v1(0x7EC1));
            break;
        }
        case 0x0D: {   // SPRITE_ADVANCE (+0x0895, 0x310E5): one square nearer
            if (v1(0x7EC1) > 0) {
                --v1(0x7EC1);
                sprite_step(sprite_id_, sprite_pic_, v1(0x7EC1));
            }
            pc_ += 1;
            break;
        }
        case 0x0E: {   // PICTURE (+0x099D)
            auto a = decode(1);
            picture_ = static_cast<int>(value(a[0]) & 0xFF);
            show_picture(picture_);
            break;
        }
        case 0x0F: {   // INPUT_NUMBER (+0x09B9, 0x31209): InputNumber("", 10, 0)
            // InputNumber 0653:0ADE (0x1700E): InputString with max 10, then
            // asked again unless every character is a digit (set CS:0ABE) and
            // System.ValInt (0x1D09E, 32-bit, error past 0x7FFFFFFF) takes it;
            // an empty line is 0. The first operand isn't used.
            auto a = decode(2);
            std::uint32_t n = 0;
            for (;;) {
                const std::string s = input_string("", 0x0A, 0, 10, kEntryDigits);
                if (s.empty()) break;
                const bool digits = std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
                if (!digits) continue;
                std::uint64_t v = 0;
                for (char c : s) v = std::min<std::uint64_t>(v * 10 + static_cast<unsigned>(c - '0'), 0x80000000u);
                if (v > 0x7FFFFFFF) continue;
                n = static_cast<std::uint32_t>(v);
                break;
            }
            set_var(a[1].type, n, dest(a[1]));
            break;
        }
        case 0x10: {   // INPUT_STRING (+0x0A09, 0x31259): InputString("", 10, 0, 0x28)
            auto a = decode(2);
            if (passcodes_ && input_) {
                // D-0084: the shipped passcode prompts (docs/notes/logbook.md
                // "Passcodes"): the answer is the text operand of the COMPARE
                // that follows, read from the running block, typed for the
                // player (no Enter).
                struct Site { int file, block; std::uint16_t input, compare; };
                static const Site kSites[] = {
                    {2, 33, 0x9239, 0x923F}, {3, 49, 0x911E, 0x9124}, {6, 96, 0xA1B1, 0xA1B7}, {6, 97, 0x9A4E, 0x9A54}};
                for (const Site& site : kSites) {
                    if (site.file != traced_file_ || site.block != block_id_ || site.input != at) continue;
                    std::uint32_t cpc = site.compare;
                    if (script_->byte(cpc) != 0x03) break;
                    for (const Operand& o : script_->decode_args(cpc, 2))
                        if (o.type == 0x80 && !o.text.empty()) {
                            input_->offer_code(o.text);   // F1 types it (D-0084)
                            break;
                        }
                    break;
                }
            }
            std::string s = input_string("", 0x0A, 0, 0x28);
            if (input_) input_->offer_code("");
            if (s.empty()) s = " ";   // CS:0A07
            set_string_var(dest(a[1]), s);
            break;
        }
        case 0x11: case 0x12: {   // PRINT, PRINT_CLEAR (+0x0A81)
            auto a = decode(1);
            std::string s;
            const std::uint8_t t = a[0].type;
            if (a[0].is_string()) {
                s = strings(a)[0];
            } else {
                const std::uint32_t v = value(a[0]);
                if (t <= 3) {
                    s = std::to_string(t <= 1 ? (v & 0xFF) : (v & 0xFFFF));
                } else {
                    s = str_long(static_cast<std::int32_t>(v));
                }
            }
            if (op == 0x12) {
                col_ = 1;
                row_ = 0x11;
            }
            f82fa_ = 0;
            typing_ = 1;                               // 0x312E5: PRINT types
            print(s, op == 0x12);
            typing_ = 0;                               // 0x313C9
            break;
        }
        case 0x13: {   // RETURN (+0x0B82)
            if (gosub_.empty()) {
                // 0x313D5: PC + 1; with no GOSUB, EXIT itself (0x3141E: a
                // near call to 0x30850, so PC + 1 again and EXIT's work;
                // session 51's audit).
                pc_ += 1;
                ecl_exit();
            } else {
                pc_ = gosub_.back();
                gosub_.pop_back();
            }
            break;
        }
        case 0x14: {   // AND_COMPARE (+0x0BD5)
            auto a = decode(4);
            flags_.fill(0);
            if (value(a[0]) == value(a[1]) && value(a[2]) == value(a[3])) flags_[0] = 1;
            else flags_[1] = 1;
            break;
        }
        case 0x15: {   // LIST_MENU dest, prompt, n, items (+0x103E, 0x3188E; session 57, no shipped script)
            // DecodeArgs numbers string operands apart (0x4B6DE): the
            // prompt is the first string; dest operand 1, n operand 3.
            highlight_ = 1;                            // DS:6B1D
            f82fa_ = 0;
            auto [first, items] = decode_var(3);
            std::string prompt;
            for (const Operand& o : first)
                if (o.is_string()) {
                    prompt = strings({o})[0];
                    break;
                }
            std::vector<ListNode> list;                // ListBuild ovl25 +0x1FF2; +0x29 := 0
            for (const std::string& s : strings(items)) list.push_back({s.substr(0, 40), false});
            col_ = 1;                                  // DS:5F3E, 5F3F
            row_ = 0x11;
            print(prompt, true);                       // Text.Print, fg 10 (0x31941)
            bool redraw = true;                        // [bp-0x10F] := 1
            int index = 0, picked = -1;                // [bp-0x10E] := 0
            // ovl15 +0x17D3 (0x4CDB3): ListMenu with an empty prompt and
            // CS:17CC's select line, rows 0x12..0x16.
            list_menu("", text_[Text::ListSelect], 1, 0x12, 0x26, 0x16, list, false, redraw, index, picked);
            set_var(first[0].type, static_cast<std::uint32_t>(static_cast<std::int32_t>(index)), dest(first[0]));
            fill_rect(1, 0x11, 0x26, 0x16, 0);         // ClearRect (0x31A0E)
            break;
        }
        case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: {   // IF_xx (+0x0C4F)
            pc_ += 1;
            if (!flags_[op - 0x16]) pc_ = static_cast<std::uint16_t>(pc_ + script_->skip_size(pc_));
            break;
        }
        case 0x1C: {   // CLEAR_MONSTERS (+0x135D)
            pc_ += 1;
            combatants_ = 0;
            f82fd_ = 0;
            icon_next_ = 8;
            loot_value_ = 0;
            loot_.clear();
            break;
        }
        case 0x1D: {   // ovl2 +0x13C3 (0x31C13): no shipped script (route s77_party_stat)
            auto a = decode(2);
            v0(0x4BB4) = 0;                            // 0x31C22, 0x31C2C
            v0(0x4BB5) = 0;
            // DS:8304, DS:8305 := the operands' values: nothing reads them.
            value(a[1]);
            value(a[0]);
            break;
        }
        case 0x1E: {   // PARTY_STAT, ovl2 +0x1477 (0x31CC7): no shipped script (route s77_party_stat)
            auto a = decode(6);
            // The selector: a variable's address (type 1, 0x31CE6), else a value; less 0x7C00.
            const std::uint16_t sel = static_cast<std::uint16_t>((a[0].type == 1 ? *a[0].word() : value(a[0])) - 0x7C00);
            if (sel != 0x19 && sel != 0x72 && sel != 0x9F) break;   // 0x31E1F: nothing
            // ovl2 +0x13FC (0x31C4C), the result writer: min, max, average, found.
            // Selector 0x72 writes the average from a local nothing set (-0x0F),
            // and an empty party divides by zero (0x31DD3): rejected (rule 4, D-0090).
            if (sel == 0x72) REJECT("D-0090", "PARTY_STAT selector 0x72 writes an unset local");
            if (party_.empty()) REJECT("D-0090", "PARTY_STAT with no party divides by zero");
            const std::size_t field = sel == 0x19 ? 0x15 : 0xE5;   // 0x31D81, 0x31E2F
            std::uint8_t lo = 0xFF, hi = 0;
            int sum = 0;
            for (const auto& m : party_) {
                const std::uint8_t x = m->b(field);
                lo = std::min(lo, x);
                hi = std::max(hi, x);
                sum += x;
            }
            set_var(a[2].type, lo, dest(a[2]));
            set_var(a[3].type, hi, dest(a[3]));
            set_var(a[4].type, static_cast<std::uint8_t>(sum / static_cast<int>(party_.size())), dest(a[4]));
            set_var(a[5].type, 0, dest(a[5]));
            break;
        }
        case 0x1F: {   // SPACE_COMBAT type, a2 (unused), a3, a4 (ovl3 +0x3600; game/space_combat.cpp)
            auto a = decode(4);
            space_combat(value(a[0]) & 0xFF, value(a[2]) & 0xFF, value(a[3]) & 0xFF);
            break;
        }
        case 0x20: {   // LOAD_ECL (+0x0CC7)
            auto a = decode(1);
            v0(0x4BF2) = block_id_;
            block_id_ = static_cast<std::uint8_t>(value(a[0]));
            load_block(block_id_);
            ecl_init();
            f82dc_ = 0;   // 0x3155E
            f82dd_ = 0;
            stop_ = 1;
            new_block_ = 1;
            break;
        }
        case 0x21: case 0x37: {   // LOAD_GEO, LOAD_WALLS (+0x0D5B)
            auto a = decode(3);
            const std::uint8_t a1 = value(a[0]) & 0xFF, a2 = value(a[1]) & 0xFF, a3 = value(a[2]) & 0xFF;
            area_flags_[0] = 1;   // DS:3581
            if (op == 0x21) {
                area_flags_[2] = 1;
                const std::uint8_t area = v0(0x4BE6);
                if (a1 != 0xFF && a1 != 0x7F && area != 0xA2 && area != 0xA8) {
                    v0(0x4BC5) = a1;
                    const std::string file = ecl_file_name("GEO");
                    auto g = dax_entry(file, a1);
                    if (!g || g->size() != 0x402)   // ovl28 +0x10B7
                        throw Fatal(text_[Text::GeoFatal]);
                    std::copy(g->begin() + 2, g->end(), geo_.begin());
                    v1(0x7EC9) = 0;
                } else if (a1 == 0x7F) {
                    v0(0x4BC5) = 0x7F;
                }
            } else {
                area_flags_[1] = 1;
                auto empty = [&](int slot) { walls_table_[2 * slot] = 0xFFFF; walls_table_[2 * slot + 1] = 0xFFFF; };
                if (a1 == 0x7F) {
                    load_walldef(0, 1);
                } else if (v0(0x4BE7) && v0(0x4BE8)) {
                    if (a1 == 0xFF) empty(0); else load_walldef(a1, 0);
                    if (a3 == 0xFF || a3 == 0x7F) empty(3); else load_walldef(a3, 3);
                } else {
                    const std::uint8_t w[3] = {a1, a2, a3};
                    for (int s = 1; s <= 3; ++s) {
                        if (w[s - 1] == 0xFF) empty(s);
                        else load_walldef(w[s - 1], s);
                    }
                }
            }
            // 0x31711..0x3174B: with DS:3582 and DS:3583 set and the
            // previous state 3, the redraw of a 3D area's first entry
            // (unless the state is 3 now) and DS:8301 cleared; otherwise
            // DS:8301 is left as it is (session 51's audit: the port had
            // always cleared it, so a loaded game's Redraw was skipped).
            if (area_flags_[1] && area_flags_[2] && prev_mode_ == 3) {
                if (mode_ != 3 && first_area_) {
                    layout_adventure();                // 015F:014F
                    draw_roster();                     // ovl25 +0x20F9
                    draw_status();                     // ovl23 +0x2820
                }
                first_area_ = 0;
            }
            break;
        }
        case 0x22: case 0x23: {   // SKILL_CHECK_QUIET / SKILL_CHECK skill, who, difficulty (+0x1664, 0x31EB4)
            // who = 0: CurChar alone; otherwise SkillCheck (ovl23 +0x2E94)
            // over the whole party list. The text cursor goes to the
            // window's corner (0x31F34); 0x23 then prints a message
            // (0x31F40..0x3203E, below). Then a2 := the best record's
            // 0-based position in the list (walked from the head,
            // 0x32043..0x32087) and a3 := the result 0..3.
            auto a = decode(3);
            const int skill = value(a[0]) & 0xFF, who = value(a[1]) & 0xFF, diff = value(a[2]) & 0xFF;
            check_skill_operand(skill, where, at);
            std::vector<Character*> recs;
            if (who == 0) {
                if (cur_) recs.push_back(cur_);
            } else {
                for (auto& m : party_) recs.push_back(m.get());
            }
            std::vector<int> totals;
            for (Character* r : recs) totals.push_back(skill_total(*r, skill));
            combat::Rng rng(seed_);
            const combat::SkillCheckResult res = combat::skill_check(totals, diff, rng);
            seed_ = rng.seed();
            data_note("skill", skill, res.result);
            col_ = left_;
            row_ = top_;
            Character* best = recs[static_cast<std::size_t>(res.best)];
            if (op == 0x23) {
                // Above 1: the best record's name + CS:1644 + CS:1646 +
                // the skill's name; else CS:1653 (who not 0) or the name,
                // + CS:1659 + the skill's name; Text.Print in the stored
                // window, green, cleared; four Game.Delays (0x3202A..
                // 0x3203E).
                std::string msg;
                if (res.result > 1)
                    msg = best->name() + text_[Text::ChkSpace] + text_[Text::ChkSucceeds] + skill_name(skill);
                else
                    msg = (who != 0 ? text_[Text::ChkParty] : best->name()) + text_[Text::ChkFails] + skill_name(skill);
                print_in(left_, top_, right_, bottom_, 0x0A, true, msg.substr(0, 255));
                for (int i = 0; i < 4; ++i) game_delay();
            }
            const int pos = index_of(best);
            set_var(a[1].type, static_cast<std::uint32_t>(pos < 0 ? party_.size() : pos), dest(a[1]));
            set_var(a[2].type, static_cast<std::uint32_t>(res.result), dest(a[2]));
            break;
        }
        case 0x24: combat_op(false); break;   // COMBAT (+0x198E; game/combat.cpp)
        case 0x25: case 0x26: {   // ON_GOTO, ON_GOSUB (+0x1AB0)
            auto [first, targets] = decode_var(2);
            const std::uint32_t i = value(first[0]) & 0xFF;
            if (i < targets.size()) {
                const std::uint16_t t = jump_target(targets[i]);
                if (op == 0x26) gosub_.push_back(pc_);
                pc_ = t;
            }
            break;
        }
        case 0x27: {   // TREASURE amount, n, types (+0x1B45)
            auto [first, types] = decode_var(2);
            loot_value_ = static_cast<std::int32_t>(static_cast<std::uint32_t>(loot_value_) + value(first[0]));
            for (const Operand& t : types) {
                // ITEM0 record of type t is DS:4EB9 + 16*t (the entry is
                // loaded at DS:4EB9 + 16); its first byte is the type, 0xFF
                // for a type that doesn't exist.
                const std::size_t ty = value(t) & 0xFF;
                // Types 1..254 past ITEM0's records read 0xFF (the start-up
                // loader fills the table first, 0x4D48C): skipped. 0 and
                // 255 read run-time memory (undefined): rejected.
                if (ty == 0 || ty == 255)
                    throw Error("D-0090: " + where + " at " + hex(at, 4) + ": TREASURE item type " + std::to_string(ty) +
                                " is outside ITEM0.DAX");
                const std::size_t r = 16 * (ty - 1);
                if (r + 16 > item0_.size() || item0_[r] == 0xFF) continue;
                Bytes node(kItemSize, 0);   // +0x00..+0x28 uninitialised in the original
                std::copy(item0_.begin() + r, item0_.begin() + r + 16, node.begin() + 0x2E);
                loot_.insert(loot_.begin(), node);   // prepend
            }
            break;
        }
        case 0x28: {   // ROB scope, percent, chance (+0x1C5E, 0x324AE); no shipped script uses it
            // Each target (CurChar, or every record) keeps (100 - percent)
            // / 100 of its credits in TP's 6-byte reals (ovl15 +0x1AE9:
            // Trunc(IntToReal(+0x2B) * factor)), then may lose each item
            // (ovl15 +0x1B1E): the chance drops by 90 for an item worth
            // over 255 (+0x36), by 50 for one over 24 (not below 0, and it
            // stays dropped for the next items); RollDice(1, 100) <= chance
            // takes it (unreadied first).
            auto a = decode(3);
            const int scope = value(a[0]) & 0xFF, percent = value(a[1]) & 0xFF, chance0 = value(a[2]) & 0xFF;
            const real48::Real factor = real48::div(real48::from_long(100 - percent), real48::Real{0x0087, 0, 0x4800});
            std::vector<Character*> targets;
            if (scope == 0) {
                if (cur_) targets.push_back(cur_);
            } else {
                for (const auto& p : party_) targets.push_back(p.get());
            }
            for (Character* p : targets) {
                p->setd(0x2B, static_cast<std::uint32_t>(real48::trunc(real48::mul(
                                  real48::from_long(static_cast<std::int32_t>(p->d(0x2B))), factor))));
                int chance = chance0;
                for (std::size_t k = 0; k < p->items.size();) {
                    const Bytes& it = p->items[k];
                    const int worth = it[0x36] | (it[0x37] << 8);
                    if (worth > 0xFF) chance = chance > 90 ? chance - 90 : 0;
                    else if (worth > 0x18) chance = chance > 50 ? chance - 50 : 0;
                    if (roll_dice(1, 100) <= chance) {
                        if (it[0x34] & 0x80) {
                            if (p != cur_)
                                throw Error("D-0090: " + where + " at " + hex(at, 4) + ": ROB unreadies an item of a record other "
                                                                          "than CurChar (Game.ReadyItem works on CurChar)");
                            bool flag = false;
                            ready_toggle(*p, static_cast<int>(k), flag);   // ovl18 +0x1F12
                        }
                        remove_item(*p, static_cast<int>(k));              // ovl23 +0x0863
                        continue;
                    }
                    ++k;
                }
            }
            break;
        }
        case 0x29: {   // WAIT (+0x1D42): PC+1, then the continue menu
            pc_ += 1;
            menu({continue_item()}, true, f82f7_);   // anim := DS:82F7 (0x325B3)
            clear_row24();   // 0653:1359
            break;
        }
        case 0x2A: {   // LOAD_INDEXED base, index, dest (+0x0F80)
            auto a = decode(3);
            const std::uint32_t addr = dest(a[0]) + value(a[1]);
            const std::uint32_t v = read_var(addr & 0xFFFF, size_of(a[2].type)) & 0xFFFF;
            set_var(a[2].type, v, dest(a[2]));
            break;
        }
        case 0x2B: case 0x31: {   // MENU, ACTION_MENU (+0x11DB)
            if (op == 0x31) print(text_[Text::WhatDoYouDo], false);
            highlight_ = 1;
            auto [first, items] = decode_var(2);
            const std::uint32_t d = dest(first[0]);
            std::vector<std::string> texts = strings(items);
            const int r = menu(texts, texts.size() == 1, f82f7_);   // anim := DS:82F7 (0x31ABB)
            set_var(first[0].type, static_cast<std::uint32_t>(r), d);
            clear_row24();   // ClearRect(0, 0x18, 0x27, 0x18)
            break;
        }
        case 0x2C: {   // YES_NO (+0x1D79)
            pc_ += 1;
            flags_.fill(0);
            if (yes_no() == 'Y') flags_[0] = 1;
            else flags_[1] = 1;
            break;
        }
        case 0x2D: {   // CALL (+0x2326)
            auto a = decode(1);
            const std::uint16_t sel = static_cast<std::uint16_t>((a[0].lo | a[0].hi.value_or(0) << 8) - 0x7FFF);
            if (sel == 0xADCC) refresh_map_byte();        // 0x2DCB (redraws when flagged)
            else if (sel == 0x401F) step_forward();       // 0xC01E
            else if (sel == 0x4019) {                     // 0xC018
                if (v0(0x4BE6) == 0) wall_ahead_ = static_cast<std::uint8_t>(wall_at(facing_, y_, x_));
            } else if (sel == 0xE804) {                   // 0x6803 (0x32C05..0x32C3A)
                // DrawPicture(3, 3, 1, DS:6A63: MenuScreen's frame), a dead
                // counter DS:6AB1, Game.Delay.
                draw_picture(anim_cur_);
                game_delay();
            } else if (sel == 0x3204) {                   // 0xB203 sound
                NOT_IMPLEMENTED("CALL 0xB203");
            }
            break;
        }
        case 0x2E: {   // DAMAGE flags, dice, sides, bonus, a5 (+0x1E97)
            const CurChar keep = cur_;
            auto a = decode(5);
            const std::uint8_t flags = value(a[0]) & 0xFF, dice = value(a[1]) & 0xFF;
            const std::uint8_t sides = value(a[2]) & 0xFF, a5 = value(a[4]) & 0xFF;
            const std::uint8_t bonus = value(a[3]) & 0xFF;   // a byte (BP-7, 0x3273E)
            int dmg = roll_dice(dice, sides) + bonus;
            const bool whole = flags & 0x40;
            int pick = 0;
            if (!whole) pick = roll_dice(1, v1(0x7F3E));   // pre-roll, 0x3278B
            // The list position `n` (1-based) from the head, as the
            // original walks it; past the end it follows a nil pointer
            // (D-0032).
            auto nth = [&](int n) -> Character& {
                Character* p = member(n < 1 ? 0 : n - 1);
                if (!p)
                    REJECT("D-0032", "DAMAGE picks list position " + std::to_string(n) + " past the end of the " +
                                         std::to_string(party_.size()) + "-record list (the original follows a nil pointer)");
                return *p;
            };
            if (!(flags & 0x80)) {
                // Attacks: `flags` times a random member and a hit roll
                // (0x328E6..0x32984).
                for (int i = 1; i <= flags; ++i) {
                    Character& p = nth(roll_dice(1, v1(0x7F3E)));
                    // Game.HitRoll ovl22 +0x0515: d20, <= 1 misses, 20 -> 100;
                    // hit if d + a5 > +0xD9 (the adjust for selector 0x10 is empty).
                    int d = roll_dice(1, 20);
                    if (d == 20) d = 100;
                    if (d > 1 && d + a5 > p.b(0xD9)) apply_damage(p, dmg);
                    dmg = roll_dice(dice, sides) + bonus;
                }
            } else {
                // Saving-throw modes (0x3279F..0x328E3): save bonus flags &
                // 0x1F, type a5 & 7; a successful save with flag bit 4
                // still takes the full damage.
                const int save_bonus = flags & 0x1F, type = a5 & 7;
                auto saved_damage = [&](Character& c, int t) {
                    if (!save(c, t, save_bonus) || (flags & 0x10)) apply_damage(c, dmg);
                };
                if (whole) {
                    // Every list record; bit 5: no save.
                    for (std::size_t i = 0; i < party_.size(); ++i) {
                        if (flags & 0x20) apply_damage(*party_[i], dmg);
                        else saved_damage(*party_[i], type);
                    }
                } else if (a5 & 0x80) {
                    // CurChar; a save only with a5 & 7 != 0, of type
                    // (a5 & 7) - 1 (the other modes use a5 & 7: quirk,
                    // reproduced; 0x32835..0x32843).
                    if (cur_) {
                        if (type) saved_damage(*cur_, type - 1);
                        else apply_damage(*cur_, dmg);
                    }
                } else {
                    // The pre-rolled member; always a save (bit 5 and a5 & 7
                    // = 0 don't skip it here; 0x3287B..0x328E3).
                    saved_damage(nth(pick), type);
                }
            }
            // Party-wipe check (0x32993).
            party_lost_ = 1;
            for (const auto& m : party_)
                if (m->b(0xD5)) party_lost_ = 0;
            if (party_lost_) {
                // 0x329C9: LayoutState0, the cursor at (2, 2), then Text.Print
                // in (1, 1)-(0x26, 0x16) with clear (which puts the cursor at
                // the window's top left again), Crt.Delay(3000).
                layout_state0();
                col_ = 2;
                row_ = 2;
                print_in(1, 1, 0x26, 0x16, 0x0A, true, text_[Text::PartyKilled]);
                pause_ms(3000);                        // 0x32A08
            }
            cur_ = keep;
            pause_continue();   // 0653:1291, always (0x32A19)
            break;
        }
        case 0x2F: case 0x30: {   // AND, OR (+0x0F04)
            auto a = decode(3);
            const std::uint32_t x = value(a[0]), y = value(a[1]);
            const std::uint8_t r = static_cast<std::uint8_t>(op == 0x2F ? (x & y) : (x | y));
            compare(0, r);
            set_var(a[2].type, r, dest(a[2]));
            break;
        }
        case 0x32: {   // PARTY_HAS_ITEM (+0x1DBF)
            auto a = decode(1);
            const std::uint8_t t = value(a[0]) & 0xFF;
            flags_.fill(0);
            flags_[1] = 1;
            for (const auto& m : party_)
                for (const Bytes& it : m->items)
                    if (it[0x2E] == t && !flags_[0]) {
                        data_note("asked", t);      // tools only: --data-trace
                        flags_[0] = 1;
                        flags_[1] = 0;
                    }
            break;
        }
        case 0x33: {   // NEWLINE (+0x2207)
            pc_ += 1;
            col_ = left_;
            ++row_;
            break;
        }
        case 0x34: {   // PASS_TIME n, unit (ovl2 +0x21D2, 0x32A22; ovl20 +0x0388)
            auto a = decode(2);
            const int unit = static_cast<int>(value(a[1]) & 0xFF);
            if (unit > 6) REJECT("D-0090", "PASS_TIME's unit above 6 writes outside the original's array");
            pass_time(static_cast<int>(value(a[0]) & 0xFF), unit);
            break;
        }
        case 0x35: {   // STORE_INDEXED a1, base, index (+0x0FEA)
            auto a = decode(3);
            const std::uint32_t addr = dest(a[1]) + value(a[2]);
            set_var(a[0].type, value(a[0]) & 0xFFFF, addr & 0xFFFF);
            break;
        }
        case 0x36: {   // ADD_NPC id, v (+0x22B9; ovl17 +0x14F9)
            auto a = decode(2);
            if (v1(0x7F3E) <= 7) {
                const int id = static_cast<int>(value(a[0]) & 0xFF);
                auto rec = load_monster(id);
                join_party(std::move(rec));
                // LoadNPC 0x4EC64: the icon, CPIC #id; in the demo CHARS
                // with +0xB8 0x0C / 0x00 / 0x1F for NPCs 0x6A / 0x6B / 0x6C.
                if (demo_) {
                    if (id == 0x6A) cur_->b(0xB8) = 0x0C;
                    else if (id == 0x6B) cur_->b(0xB8) = 0x00;
                    else if (id == 0x6C) cur_->b(0xB8) = 0x1F;
                    load_combat_icon("CHARS", cur_->b(0xB8), cur_->b(0xB9));
                } else {
                    cur_->b(0xB8) = static_cast<std::uint8_t>(id);
                    load_combat_icon("CPIC", id, cur_->b(0xB9));
                }
            }
            if (cur_) {
                cur_->b(0xE7) = static_cast<std::uint8_t>((value(a[1]) & 0xFF) / 2 | 0x80);
                recalc(*cur_);
                draw_roster();                         // 0x32B6D: Screen.Roster
            }
            break;
        }
        case 0x38: {   // TEAM_MENU v (+0x23F3, 0x32C43..0x32CA5)
            auto a = decode(1);
            const std::uint8_t keep = move_mode_;
            move_mode_ = 0;
            if (set_char_) {
                restore_cur();
                set_char_ = 0;
            }
            const std::uint32_t v = value(a[0]) & 0xFF;
            if (v == 0) {                              // 0x32C87: TeamMenu (BEGIN returns), Screen.Redraw
                const int loads = loads_;
                team_menu();
                // The original goes on with the script after a LOAD (N6,
                // its own fatal at the next picture the old block asks
                // for); the fix restarts the run with the loaded game.
                if (fixes_.script_load && loads_ != loads) throw RestartRun{};
                // A block the loaded file lacks restarts the run (load_block, D-0081).
                if (loads_ != loads) {
                    script_menu_load_ = true;
                    script_load_block_ = v0(0x4BF2);
                    script_load_mode_ = mode_;
                    script_load_prev_mode_ = prev_mode_;
                }
                redraw_screen();
            } else if (v == 9) {                       // 0x32C97: the rest menu, no redraw after it
                rest_menu();
            }
            move_mode_ = keep;                         // any other value: nothing (0x32C9C)
            break;
        }
        case 0x39: {   // PICK_CHARACTER prompt (+0x2248, 0x32A98)
            // The choice is CurChar itself; no variable is set.
            const std::uint8_t saved_move = move_mode_;
            move_mode_ = 0;
            no_char_ = 0;
            auto a = decode(1);
            if (!a[0].is_string()) REJECT("D-0018", "PICK_CHARACTER without a text operand (the prompt would be a stale string)");
            fill_rect(left_, top_, right_, bottom_, fill_);   // Text.FillRect; the cursor stays
            select_member(strings(a)[0]);
            move_mode_ = saved_move;
            break;
        }
        case 0x3A: {   // DELAY (+0x1E6B): Crt.Delay(var 0x4BFC * 100), ms
            pc_ += 1;
            game_delay();
            break;
        }
        case 0x3B: case 0x3C: REJECT("D-0003", "opcode 0x" + hex(op, 2) + " hangs the original");
        case 0x3D: {   // REDRAW (+0x221C, 0x32A6C)
            pc_ += 1;
            f3bed_ = 0;
            draw_roster();   // Screen.Roster(CurChar), Screen.StatusLine
            draw_status();
            first_area_ = 0;
            f82f6_ = 1;
            break;
        }
        case 0x3E: {   // REMOVE_CHAR (+0x245D)
            pc_ += 1;
            remove_char(false, true);
            saved_cur_ = cur_;
            draw_roster();                             // 0x32CD7: Screen.Roster(CurChar)
            break;
        }
        case 0x3F: {   // LOGBOOK text, n (+0x24B7)
            auto a = decode(2);
            const std::string text = strings(a).empty() ? std::string() : strings(a)[0];
            const int entry = static_cast<int>(value(a[1]) & 0xFFFF);
            print(text_[Text::LogRecord] + text + text_[Text::LogEntry] + std::to_string(entry) +
                      text_[Text::LogEnd],
                  false);
            // D-0083: LOGBOOK.TXT's entry as the game's text, then the
            // original's continue prompt.
            logbook_print(entry);
            menu({continue_item()}, true, f82f7_);   // anim := DS:82F7 (0x32DAF)
            clear_row24();
            break;
        }
        case 0x40: {   // TAKE_ITEM scope, type (+0x2575)
            auto a = decode(2);
            const std::uint8_t scope = value(a[0]) & 0xFF, v = value(a[1]) & 0xFF;
            const bool one = v & 0x80;
            const std::uint8_t t = v & 0x7F;
            const CurChar keep = cur_;
            std::size_t i = scope ? 0 : static_cast<std::size_t>(std::max(0, index_of(cur_)));
            bool done = false;
            while (i < party_.size() && !done) {
                Character& c = *party_[i];
                for (std::size_t k = 0; k < c.items.size() && !done;) {
                    Bytes& it = c.items[k];
                    if (it[0x2E] != t) { ++k; continue; }
                    data_note("asked", t);          // tools only: --data-trace
                    if (!(it[0x34] & 0x20) && it[0x38] > 1 && one) {
                        it[0x38] -= 1;
                        ++k;
                    } else {
                        c.items.erase(c.items.begin() + static_cast<std::ptrdiff_t>(k));   // Unready + RemoveItem
                    }
                    done = one;
                }
                done = scope == 0;
                recalc(c);
                ++i;
            }
            cur_ = keep;
            break;
        }
        case 0x41: {   // GIVE_XP scope, n (+0x2719)
            auto a = decode(2);
            const std::uint32_t n = value(a[1]) & 0xFFFF;
            const std::uint8_t scope = value(a[0]) & 0xFF;
            col_ = left_;
            row_ = top_;
            if (scope) {
                print(text_[Text::XpTeam], true);
                game_delay();   // Game.Delay (0x32FCD)
                for (auto& m : party_)
                    if (m->b(0xD5)) m->setd(0x2F, m->d(0x2F) + n);
            } else if (cur_) {
                print(text_[Text::XpCongrats] + cur_->name() + text_[Text::XpGains], true);
                game_delay();   // Game.Delay (0x33063)
                if (cur_->b(0xD5)) cur_->setd(0x2F, cur_->d(0x2F) + n);
            }
            break;
        }
        case 0x42: {   // CLEAR_EXIT (+0x2837)
            clear_window();
            refresh_map_byte();
            ecl_exit();                                // EXIT itself (0x330A9: call 0x30850)
            break;
        }
        case 0x43: {   // SOUND (+0x2860, 0x330B0): n <= 0x7F plays DS:0002 + n's effect; the
            // music cues (0x80 and up) are dropped by v1.00 (docs/notes/sound.md)
            auto a = decode(1);
            const std::uint32_t n = value(a[0]) & 0xFF;
            if (n <= 0x7F) sound(text_.ds8(0x0002 + n));
            break;
        }
        case 0x44: pc_ += 1; break;   // NOP, ovl2 +0x2898 (0x330E8): PC + 1
        case 0x45: {   // WALL_DISTANCE dest, direction 0..3 (+0x28A3, 0x330F3)
            auto a = decode(2);
            if (a[0].type == 0 || a[0].type == 0x80) REJECT("D-0004", "WALL_DISTANCE into an operand without an address");
            const int dir = static_cast<int>((value(a[1]) * 2) & 0xFF);
            // Another direction reaches WallAt's fall-through, which returns
            // an uninitialised local (0x5D586): undefined, rejected.
            if (v0(0x4BE6) != 0 && dir != 0 && dir != 2 && dir != 4 && dir != 6)
                REJECT("D-0018", "WALL_DISTANCE toward direction " + std::to_string(dir / 2) + " (WallAt would return garbage)");
            set_var(a[0].type, static_cast<std::uint32_t>(steps_ahead(x_, y_, dir)), dest(a[0]));
            break;
        }
        case 0x46: {   // LOOP_SET (+0x2903)
            auto a = decode(2);
            loop_pc_ = pc_;
            v0(0x4CF6) = static_cast<std::uint8_t>(value(a[0]));
            v0(0x4CF7) = static_cast<std::uint8_t>(value(a[1]));
            break;
        }
        case 0x47: {   // LOOP_NEXT (+0x293A)
            pc_ += 1;
            v0(0x4CF6) = static_cast<std::uint8_t>(v0(0x4CF6) + 1);
            if (v0(0x4CF6) <= v0(0x4CF7)) pc_ = loop_pc_;
            break;
        }
        case 0x48: {   // STASH_ITEMS mode (+0x2968)
            auto a = decode(1);
            const bool stash = (value(a[0]) & 0xFF) == 0;
            v0(0x4BB8) = stash ? 1 : 0;
            for (auto& m : party_) {
                auto& src = stash ? m->items : m->stash;
                auto& dst = stash ? m->stash : m->items;
                dst.insert(dst.begin(), src.begin(), src.end());   // src's nodes in front
                src.clear();
                recalc(*m);
            }
            break;
        }
        case 0x49: {   // SKILL_DAMAGE skill, scope, difficulty, dice, sides, bonus (+0x2AA0, 0x332F0)
            // CurChar (scope 0) or every record: SkillCheck (ovl23 +0x2E94)
            // and, below 2, the window cleared once and RollDice(dice,
            // sides) + bonus through ApplyDamage. After each record DS:357E
            // := (scope = 0): with scope 0 the script ends (Q8).
            auto a = decode(6);
            const int skill = value(a[0]) & 0xFF, scope = value(a[1]) & 0xFF, diff = value(a[2]) & 0xFF;
            check_skill_operand(skill, where, at);
            const int dice = value(a[3]) & 0xFF, sides = value(a[4]) & 0xFF, bonus = value(a[5]) & 0xFF;
            col_ = left_;                                  // DS:5F3E/5F3F := the window's corner
            row_ = top_;
            bool first = true;
            std::size_t i = 0;
            Character* p = scope ? (party_.empty() ? nullptr : party_.front().get()) : cur_;
            if (!scope && p) i = static_cast<std::size_t>(std::max(0, index_of(p)));
            while (p && !stop_) {
                combat::Rng rng(seed_);
                const int r = combat::skill_check({skill_total(*p, skill)}, diff, rng).result;
                seed_ = rng.seed();
                data_note("skill", skill, r);
                if (r < 2) {
                    if (first) {
                        first = false;
                        fill_rect(left_, top_, right_, bottom_, fill_);   // Text.FillRect
                    }
                    apply_damage(*p, (roll_dice(dice, sides) + bonus) & 0xFF);   // ovl15 +0x1D2C
                }
                ++i;
                p = i < party_.size() ? party_[i].get() : nullptr;
                stop_ = scope == 0;
            }
            break;
        }
        // ovl2 +0x2BC4 COMBAT_SOLO: DS:82EA := 1, then COMBAT (+0x198E)
        case 0x4A: combat_op(true); break;    // COMBAT_SOLO (DS:82EA := 1, then COMBAT)
        case 0x4B: {   // SHOP n (+0x2BD4, 0x33424; game/shop.cpp)
            auto a = decode(1);
            data_note("shop", static_cast<int>(value(a[0]) & 0xFF));   // tools only: --data-trace
            op_shop(static_cast<int>(value(a[0]) & 0xFF));
            break;
        }
        case 0x4C: {   // SET_MODE mode, picture (+0x2D37)
            auto a = decode(2);
            const std::uint8_t m = value(a[0]) & 0xFF;
            v0(0x4BA6) = m;
            f3bed_ = 0;
            map_mode_ = 0;
            switch (m) {
                case 0:
                    if (mode_ == 9) travel_free();   // ovl24 +0x0099 (0x59779)
                    mode_ = 4;
                    break;
                case 2: mode_ = 9; break;
                case 3: mode_ = 10; break;
                case 4: mode_ = 3; map_mode_ = 1; break;
                default: break;
            }
            if (m != 1 && m != 4) redraw_screen();   // ovl23 +0x26BE
            picture_ = static_cast<int>(value(a[1]) & 0xFF);
            show_picture(picture_);
            break;
        }
        default: REJECT("D-0003", "opcode 0x" + hex(op, 2) + " hangs the original");
    }
}

}  // namespace cotd::game
