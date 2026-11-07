// SPDX-License-Identifier: GPL-3.0-or-later
// The adventure loop in game state 4 (docs/notes/adventure.md):
// AdventureInput (ovl12 +0x0FE4), movement, Look, Save, and Ecl.RunLoop's
// main loop (ovl2 +0x3209, 0x33B85..).
#include <algorithm>
#include <cctype>

#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr int DX[8] = {0, 1, 1, 1, 0, -1, -1, -1};   // DS:257C
constexpr int DY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};   // DS:2585

}  // namespace

bool Game::passable(int x, int y, int f) const {
    // ovl28 +0x03B1 (0x5D381): 1 if there is no wall; otherwise the 2-bit
    // field of GEO plane 3 for that side, and only 1 lets the party pass.
    x = static_cast<std::int8_t>(x);   // signed bytes (D-0073)
    y = static_cast<std::int8_t>(y);
    if (!(0 <= x && x < 16 && 0 <= y && y < 16)) {
        if (block_id_ == 0 || block_id_ == 0x50) return false;
        x = x < 0 ? 15 : x >= 16 ? 0 : x;
        y = y < 0 ? 15 : y >= 16 ? 0 : y;
    }
    if (f & 1) return true;
    if (wall_at(f, y, x) == 0) return true;
    const int field = (geo_[0x300 + y * 16 + x] >> (f / 2 * 2)) & 3;   // N bits 1..0, E 3..2, S 5..4, W 7..6
    return field == 1;
}

void Game::edge_test() {
    // ovl12 +0x0000 (0x45260..0x45305): var 0x7ED5 := 1 if the step would
    // leave the map, and the square clamped: above 15 to 15, below 0 to 0
    // (signed words). On the map that leaves the square as it is; off it
    // (D-0073: a script's write) it brings it back to the edge (run
    // s75_offmap_ff: x 255 facing W gives 0,10).
    v1(0x7ED5) = 0;
    if (passable(x_, y_, facing_)) {
        const int nx = x_ + DX[facing_], ny = y_ + DY[facing_];
        if (nx > 15) { x_ = 15; v1(0x7ED5) = 1; }
        if (nx < 0) { x_ = 0; v1(0x7ED5) = 1; }
        if (ny > 15) { y_ = 15; v1(0x7ED5) = 1; }
        if (ny < 0) { y_ = 0; v1(0x7ED5) = 1; }
    }
}

void Game::step_party() {
    // Step ovl12 +0x00A9 (0x45309): sound, Delay(50), move, wrap, refresh
    // DS:6A32/6A33, then one minute (or ten when searching) passes.
    sound(text_.ds16(0x21F0));                         // 0x45310: Sound.Play(10)
    pause_ms(50);                                      // 0x45319: Crt.Delay(50) after the sound
    int x = x_ + DX[facing_], y = y_ + DY[facing_];
    x = x < 0 ? 15 : x > 15 ? 0 : x;
    y = y < 0 ? 15 : y > 15 ? 0 : y;
    x_ = static_cast<std::int8_t>(x);
    y_ = static_cast<std::int8_t>(y);
    wall_ahead_ = static_cast<std::uint8_t>(wall_at(facing_, y_, x_));
    map_byte_ = map_byte(x_, y_);
    if (v1(0x7ECA) & 1) pass_time(1, 2);
    else pass_time(1, 1);
}

void Game::select_char(int scan) {
    // ovl18 +0x299F (0x5293F). Nothing for a key from a click (DS:8434 and
    // DS:8435, 0x52945, main): the roster click has already moved CurChar.
    if (mouse_on_ && f8435_) return;
    if (party_.empty()) return;
    int i = cur_ ? index_of(cur_) : 0;
    const int n = static_cast<int>(party_.size());
    if (scan == 0x48) i = i == 0 ? n - 1 : i - 1;          // Up: previous; the head wraps to the last
    else if (scan == 0x50) i = i == n - 1 ? 0 : i + 1;     // Down: next; the last wraps to the head
    cur_ = party_[static_cast<std::size_t>(i)].get();
}

void Game::select_member(const std::string& prompt, bool exit, bool alt, Character*& var, bool view) {
    // SelectMember ovl18 +0x2F14 (0x52EB4..0x53147): items "Select" [+
    // " View"] [+ " Exit"] (CS:2EE1/2EE8/2EEE); DS:6B16 := alt, DS:6B17 :=
    // CurChar, DS:6B49 := ' '. Until DS:6B49 is in {0, 13, 27, 'E', 'S'}
    // (CS:2EF4): the roster with *var in white, MenuScreen(prompt[:40] +
    // " ", items, remap on, animated in states 2 and 6) with DS:8433 := 1,
    // then cur := *var:
    //  - Down: cur's next, past the last the first (0x52FE3);
    //  - Up from a click: the clicked member (alt: DS:6B17, else CurChar),
    //    chosen at once ('S') when it is cur already (0x53018); Up from the
    //    keyboard: cur's previous, before the first the last (0x5306E);
    //  - 'V': VIEW of CurChar, CurChar put back, Screen.Redraw, cur :=
    //    CurChar (0x530DD);
    //  - with exit, 'E' or 0 (Esc): cur := nil (0x53111);
    // and *var := cur (0x5312D). PICK_CHARACTER passes &CurChar, no Exit, a
    // View; the trades pass their partner, Exit, alt and no View.
    std::string items = text_[Text::PickSelect];
    if (view) items += text_[Text::PickView];
    if (exit) items += text_[Text::PickExit];
    const std::string p = prompt.substr(0, 40) + " ";
    const std::uint8_t saved8433 = f8433_;             // 0x52ECE
    f6b16_ = alt ? 1 : 0;                              // 0x52ED7
    f6b17_ = cur_;
    int key = ' ';
    auto pos_of = [&](const Character* m) {
        for (std::size_t i = 0; i < party_.size(); ++i)
            if (party_[i].get() == m) return static_cast<int>(i);
        return -1;
    };
    // D-0055: PICK_CHARACTER with CurChar the record TeamMenu freed: the
    // roster whitens nobody, Down reads the freed record's +0xFF (its
    // successor at the removal), Up walks the party to nil and hangs in the
    // original (the key does nothing here, D-0056), and a choice of the
    // freed record itself keeps it (run s80_detach_pick_enter, _down, _up).
    const bool freed = &var == &cur_.p && cur_.detached;
    Character* const detached = cur_.p;
    if (freed) var = &freed_cur_;
    while (!(key == 0 || key == kEnter || key == kEsc || key == 'E' || key == 'S')) {
        draw_roster(var);
        f8433_ = 1;                                    // 0x52F95
        const MenuKey mk = menu_screen(p, items, true, mode_ == 2 || mode_ == 6);
        f8433_ = saved8433;
        key = mk.key;
        Character* cur = var;
        if (mk.ext) {
            const int n = static_cast<int>(party_.size());
            const int i = pos_of(cur);
            if (i < 0 && mk.key == 0x50 && (cur == &startup_partner_ || cur == &stray_partner_) && n > 0) {
                // Down from the start-up partner: cur := 3A25:3525's +0xFF,
                // which lies in the overlay buffer and holds whatever code
                // is loaded there (5 values in 51 memory dumps): measured
                // in the shop's Trade, a pointer that is no member, then
                // nil at its +0xFF, so the next Down gives the first
                // member (run s60_c29_dump: capture 364 nobody white, 365
                // the first member white; D-0056).
                combat::diag("D-0056", "SelectMember (ovl18 +0x2F14, 0x52FEA): Down from the start-up partner reads "
                                       "the overlay buffer; the port uses the measured pointers (no member, then nil)");
                cur = cur == &startup_partner_ ? &stray_partner_ : party_.front().get();
            } else if (i < 0 && mk.key == 0x50 && cur == &freed_cur_ && n > 0) {
                combat::diag("D-0055", "SelectMember (ovl18 +0x2F14, 0x52FEA): Down from the CurChar TeamMenu freed "
                                       "reads its +0xFF; the port uses its successor at the removal (measured)");
                cur = detached_next_ && pos_of(detached_next_) >= 0 ? detached_next_ : party_.front().get();
            } else if (i < 0 && mk.key == 0x50 && cur == &removed_partner_ && n > 0 &&
                       (!removed_next_ || pos_of(removed_next_) >= 0)) {
                // Down from a record REMOVE freed: its +0xFF, the successor
                // it had (nil: the party's first); measured, run
                // s69_d0056_down (ACE removed: DOC white, then DUNE; D-0056).
                combat::diag("D-0056", "SelectMember (ovl18 +0x2F14, 0x52FEA): Down from a removed record reads its "
                                       "+0xFF; the port uses its successor at the removal (measured)");
                cur = removed_next_ ? removed_next_ : party_.front().get();
            } else if (i < 0 && mk.key == 0x50 && cur == &removed_partner_ && n > 0 && removed_next_ &&
                       freed_next_.count(removed_next_)) {
                // Down onto a successor REMOVE freed since: that record, no
                // member (nobody white), and the next Down reads its +0xFF,
                // the successor it had (run s80_d0056_succ_gone: ACE and DOC
                // removed, nothing white, then DUNE; D-0056).
                combat::diag("D-0056", "SelectMember (ovl18 +0x2F14, 0x52FEA): Down onto a successor REMOVE freed; "
                                       "the next Down reads its +0xFF (measured)");
                removed_next_ = freed_next_[removed_next_];
            } else if (i < 0 && (mk.key == 0x50 || (mk.key == 0x48 && !f8435_))) {
                // A partner that isn't a member (Trade's DS:387C from
                // start-up or a removed record): Down reads its +0xFF, Up
                // walks the party to nil +0xFF; neither is a member, and
                // what they read is the memory layout's (D-0056).
                combat::diag("D-0056", "SelectMember (ovl18 +0x2F14, 0x52FE3 / 0x5306E): Up from a partner that is "
                                       "no member, or Down from a freed record whose successor is gone: the key does "
                                       "nothing");
            } else if (mk.key == 0x50) {
                cur = party_[static_cast<std::size_t>((i + 1) % n)].get();
            } else if (mk.key == 0x48 && f8435_) {
                Character* clicked = alt ? f6b17_ : cur_;
                if (cur == clicked) key = 'S';
                cur = clicked;
            } else if (mk.key == 0x48) {
                cur = party_[static_cast<std::size_t>((i + n - 1) % n)].get();
            }
        } else if (mk.key == 'V') {
            const CurChar keep = cur_;
            view_character();
            cur_ = keep;
            redraw_screen();
            cur = cur_;
            key = 'V';
        } else if (exit && (key == 'E' || key == 0)) {
            cur = nullptr;
        }
        var = cur;
    }
    if (freed) {
        if (var == &freed_cur_) {
            cur_.p = detached;                         // still the freed record: detached
            cur_.detached = true;
        } else {
            cur_.detached = false;                     // a member chosen
        }
    }
    f6b16_ = 0;                                        // 0x5313F
}

int Game::adventure_input() {
    // AdventureInput ovl12 +0x0FE4 (0x46244), state 4.
    v1(0x7EC9) = 0;
    if (mode_ == 3) return map_input();
    if (mode_ != 4 && mode_ != 9)
        // 0x46716..0x4673E: in any other state it reads no key and returns
        // its local [BP-3] unwritten (D-0077): undefined, reached only by a
        // damaged script or save (BEGIN in state 0 is D-0077's own case).
        throw Error("AdventureInput (ovl12 +0x0FE4) in game state " + std::to_string(mode_) +
                    " reads no key and returns an unwritten local (D-0077)");
    // The square the party stands on, by GEO file and entry (tools/
    // coverage_count.py, session 55); not on Mars's and Venus's plains.
    if (mode_ == 4 && v0(0x4BE6) != 0xA2 && v0(0x4BE6) != 0xA8 && v0(0x4BC5) != 0x7F)
        data_note("square", ecl_file_ * 256 + v0(0x4BC5), (y_ & 15) * 16 + (x_ & 15));
    bool done = mode_ == 9;
    int ret = mode_ == 9 ? travel_input() : 0;
    // The loop tests DS:3BF6 at its bottom (0x464E8): with the party lost
    // the menu still takes one key.
    if (!done) do {
        if (!move_mode_) {
            f8433_ = 1;                                // 0x46270
            const MenuKey mk = menu_screen("", text_[Text::AdventureMenu], true);
            f8433_ = 0;
            if (mk.ext) {
                // 0x4636E: an extended key redraws CurChar's roster row
                // (ovl23 +0x02DB, flag 0: DS:4EBD's row, the old CurChar's
                // after a roster click), lets ovl18 +0x299F move CurChar,
                // redraws the new CurChar's row in white (flag 1) and the
                // status line.
                roster_row(cur_, false);
                select_char(mk.key);
                roster_row(cur_, true);
                draw_status();
                continue;
            }
            switch (mk.key) {
                case 'M':
                    clear_row24();
                    move_mode_ = 1;
                    break;
                case 'A':
                    // 0x462BD: with var 0x4BFB set, the row-24 message
                    // CS:0FB4 in yellow and the status line; otherwise
                    // DS:6A2E toggled and View.Draw (no DrawViewArea).
                    if (v0(0x4BFB) != 0) {
                        row_message(text_[Text::NotHere], 0x0E, 0);
                        draw_status();
                    } else {
                        area_map_ = area_map_ ? 0 : 1;
                        draw_view();
                    }
                    break;
                case 'S':
                    save_menu();
                    break;
                case 'L':
                    // Look (0x4633A): the status line before the time passes.
                    v1(0x7ECA) = static_cast<std::uint8_t>(v1(0x7ECA) | 2);
                    draw_status();
                    pass_time(1, 2);
                    pc_ = entries_[1];
                    done = true;
                    ret = 'L';
                    break;
                case 'V':
                    // 0x46311: ViewCharacter (ovl18 +0x0F75) on CurChar, with
                    // a local exit flag only GEAR sets; then Screen.Redraw.
                    view_character();
                    redraw_screen();
                    break;
                case 'C':
                    change_menu();                     // 0x46335: CHANGE, nothing redrawn after
                    break;
                default:
                    break;
            }
        } else {
            const MenuKey mk = menu_screen("", text_[Text::MoveMenu], true);
            if (!mk.ext) {
                if (mk.key == 'E') move_mode_ = 0;
                continue;
            }
            switch (mk.key) {
                case 0x48:   // forward
                    edge_test();
                    done = true;
                    draw_status();
                    ret = 0;
                    break;
                case 0x50:   // turn around (no sound)
                case 0x4B:   // turn left
                case 0x4D:   // turn right
                    facing_ = static_cast<std::uint8_t>((facing_ + (mk.key == 0x50 ? 4 : mk.key == 0x4B ? 6 : 2)) % 8);
                    if (mk.key != 0x50) sound(text_.ds16(0x21F0));   // 0x46460 / 0x464AF
                    wall_ahead_ = static_cast<std::uint8_t>(wall_at(facing_, y_, x_));
                    draw_view();
                    draw_status();
                    break;
                default:
                    break;
            }
        }
    } while (!done && !party_lost_);
    // On the way out the text window is cleared once after each print
    // (0x46716: DS:82FA).
    if (!f82fa_) {
        fill_rect(left_, top_, right_, bottom_, fill_);
        f82fa_ = 1;
    }
    return ret;
}

int Game::map_input() {
    // AdventureInput ovl12 +0x0FE4 in state 3 (0x465A5..0x4670D): the
    // planet map. Out of move mode the MapMenu items (remap 0, anim 1): M enters
    // move mode, S saves, anything else asks again. In move mode the MoveMenu item
    // (remap 1): an arrow or keypad key steps (DS:6A31 by the scan code;
    // other extended keys end the input with the facing kept); E by a
    // click on the map (pointer y <= 0x80) steps toward it (Combat.Dir,
    // ovl30 +0x178D; the key returned is DS:3046[dir]), any other E
    // leaves move mode.
    int key = 0;
    bool done = false;
    do {
        if (!move_mode_) {
            const MenuKey mk = menu_screen("", text_[Text::MapMenu], false, true);
            key = mk.key;
            if (!mk.ext) {
                if (key == 'M') {
                    clear_row24();                     // 0653:1359
                    move_mode_ = 1;
                } else if (key == 'S') {
                    save_menu();                       // 0x461DF
                }
            }
        } else {
            const MenuKey mk = menu_screen("", text_[Text::MoveMenu], true);
            key = mk.key;
            if (!mk.ext) {
                if (key == 'E') {
                    if (f8435_ && static_cast<std::int16_t>(mouse_y_) <= 0x80) {
                        facing_ = static_cast<std::uint8_t>(dir_to(v0(0x4BC3), v0(0x4BC4), mouse_x_ / 8 - 1, mouse_y_ / 8 - 1));
                        key = text_.ds8(0x3046 + facing_);
                        done = true;
                    } else {
                        move_mode_ = 0;
                    }
                }
            } else {
                done = true;
                switch (key) {
                    case 0x48: facing_ = 0; break;
                    case 0x49: facing_ = 1; break;
                    case 0x4D: facing_ = 2; break;
                    case 0x51: facing_ = 3; break;
                    case 0x50: facing_ = 4; break;
                    case 0x4F: facing_ = 5; break;
                    case 0x4B: facing_ = 6; break;
                    case 0x47: facing_ = 7; break;
                    default: break;
                }
                draw_status();                         // ovl23 +0x2820: nothing in state 3
            }
        }
    } while (!done);
    if (!f82fa_) {                                     // 0x46716
        fill_rect(left_, top_, right_, bottom_, fill_);
        f82fa_ = 1;
    }
    return key;
}

void Game::map_step() {
    // MapStep 0x453CC: the cell under the cursor back, var 0x4BF0/0x4BF1
    // := the old square, one square by DS:6A31 (DS:257C / DS:2585),
    // clamped to 0..0x25 and 0..0x0E (signed bytes; no passability test),
    // the cursor at the new square, PassTime(0x0C, 3).
    restore_map_cursor();
    v0(0x4BF0) = v0(0x4BC3);
    v0(0x4BF1) = v0(0x4BC4);
    int x = static_cast<std::int8_t>(static_cast<std::uint8_t>(v0(0x4BC3) + DX[facing_]));
    int y = static_cast<std::int8_t>(static_cast<std::uint8_t>(v0(0x4BC4) + DY[facing_]));
    x = std::clamp(x, 0, 0x25);
    y = std::clamp(y, 0, 0x0E);
    v0(0x4BC3) = static_cast<std::uint8_t>(x);
    v0(0x4BC4) = static_cast<std::uint8_t>(y);
    draw_map_cursor();
    pass_time(0x0C, 3);                                // ovl20 +0x0388
}

void Game::save_menu() {
    // 0x461DF: SaveGame (ovl17 +0x220C) with its slot menu, then "Quit to
    // DOS?" (Yes/No with No highlighted).
    int key = 0;
    for (;;) {
        // Until DS:6B49 is in {0, 'A'..'J'} (CS:2159). An extended key
        // counts by its scan code, so Home, Up and PgUp (0x47..0x49) pick
        // slots G, H and I (0x4F96C; deterministic quirk, reproduced).
        const MenuKey mk = menu_screen(text_[Text::SaveWhich], text_[Text::SaveSlots], mode_ == 2);
        key = mk.key;
        if (key == 0 || (key >= 'A' && key <= 'J')) break;
    }
    if (key) {
        save_game(static_cast<char>(key));
        clear_row24();
    }
    menu_prompt_fg_ = 14;   // DS:6B45 := 0x0E (0x461EA)
    if (yes_no(text_[Text::QuitToDos], 2) == 'Y') throw QuitToDos{};   // No first
}

void Game::run_loop() {
    // Ecl.RunLoop's main loop (0x33B85..0x33CE6), state 4.
    {
        for (;;) {
            int key = adventure_input();
            saved_cur_ = cur_;
            if (!new_block_) v0(0x4BF2) = block_id_;
            while ((v1(0x7ECA) > 1 || std::toupper(key) == 'E') && !party_lost_) {
                const std::uint8_t search = v1(0x7ECA) & 1;
                v1(0x7ECA) = 1;
                f6b70_ = 1;
                draw_view_area();
                run_ecl(entries_[1]);
                if (new_block_) enter_block();
                v1(0x7ECA) = search;
                if (party_lost_) break;
                key = adventure_input();
                saved_cur_ = cur_;
            }
            if (party_lost_) break;
            run_ecl(entries_[0]);
            if (new_block_) {
                // 0x33C42: EnterBlock, then 0x33CDF: the loop's end, which
                // leaves it when the party was lost (session 51's audit).
                enter_block();
                if (party_lost_) break;
                continue;
            }
            if (party_lost_) break;
            v0(0x4BF0) = static_cast<std::uint8_t>(x_);
            v0(0x4BF1) = static_cast<std::uint8_t>(y_);
            // MoveAttempt ovl12 +0x14E8: state 4 steps and frees the picture
            // (0x4679E); state 9 is TravelMove(0) (0x467EA).
            if (mode_ == 9) {
                if (v1(0x7EC9) < 0xFF) travel_move();
                else v1(0x7EC9) = 0;
            } else if (mode_ == 3) {
                // 0x467C3: MapStep, the status line (nothing in state 3);
                // no FreePicture (state 4 only).
                if (v1(0x7EC9) < 0xFF) map_step();
                else v1(0x7EC9) = 0;
            } else if (mode_ == 4) {
                if (v1(0x7EC9) < 0xFF) {
                    f6b70_ = 1;
                    if (passable(x_, y_, facing_)) step_party();
                    draw_status();
                } else {
                    v1(0x7EC9) = 0;
                }
                free_picture();                        // ovl27 +0x0B04 (0x4679E)
            }
            // Any other state: no move at all (0x467EA..0x4680D; session
            // 51's audit: the port had stepped).
            const std::uint8_t area = v0(0x4BE6);
            if (area != 0xA2 && area != 0xA8) draw_view_area();
            // 0x33C8D: in state 4, the party off the square the loop kept
            // (vars 0x4BF0/0x4BF1, before the move): Sound.Play(DS:21F0).
            if (mode_ == 4 && (x_ != static_cast<std::int8_t>(v0(0x4BF0)) || y_ != static_cast<std::int8_t>(v0(0x4BF1))))
                sound(text_.ds16(0x21F0));
            f82f7_ = 0;
            run_ecl(entries_[1]);
            if (new_block_) enter_block();
            if (party_lost_) break;
        }
    }
    // 0x33CE9 (ovl2 +0x3499): the party lost: DS:3BF6 := 0, every record
    // left removed (the next one read before each removal).
    party_lost_ = 0;
    while (!party_.empty()) {
        cur_ = party_.front().get();
        remove_char(false, true);                      // ovl13 +0x1C47
    }
}

}  // namespace cotd::game
