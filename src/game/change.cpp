// SPDX-License-Identifier: GPL-3.0-or-later
// CHANGE, the adventure's and the travel screen's 'C': ovl12 +0x0B3B
// (0x45D9B..0x45EBC) with Order (+0x0820), Messages (+0x0349), Level
// (+0x0236) and PURGE (ovl13 +0x186D, modify.cpp). docs/notes/adventure.md
// "CHANGE".
#include <algorithm>

#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr std::size_t kChangeItems = 0x0915;   // DS: Order and Purge (state 4 only)
constexpr std::size_t kChangeItems2 = 0x0967;  // DS: Messages, Anim, Level, Exit
constexpr std::size_t kLevelItems = 0x093E;    // DS: the five levels and Exit
constexpr std::size_t kOrderItems = 0x09BE;    // DS: String[40] x 2: Select mode's items, Place mode's
constexpr std::size_t kSpeedNames = 0x0990;    // DS: String[7] x 3
constexpr std::size_t kSpeedSuffixes = 0x09A8; // DS: String[3] x 3
constexpr std::size_t kSpeedIndex = 0x09B4;    // DS: 10 bytes, name << 4 | suffix

}  // namespace

std::string Game::ds_pascal(std::size_t off) const {
    // A Pascal string read as the original's StrConcat reads it: its own
    // length byte, whatever the declared size (Messages with a speed of 10
    // or more reads past its tables into other constants).
    const std::size_t n = text_.ds8(off);
    std::string s;
    for (std::size_t i = 1; i <= n; ++i) s += static_cast<char>(text_.ds8(off + i));
    return s;
}

void Game::change_menu() {
    // ChangeMenu ovl12 +0x0B3B: until 0 or 'E' (CS:0B00; its second byte,
    // 0, is the empty prompt CS:0B01). Nothing is redrawn on the way out.
    int key = ' ';
    while (key != 0 && key != 'E') {
        std::string items2 = text_.ds_str(kChangeItems2, 40);
        const bool port = input_ && input_->options_menu();
        if (port) {
            // The port's Options in place of DS:0967's first two items
            // (D-0065): Messages (its speed is the screen's first line) and
            // Anim (toggles DS:3BEE, which nothing but the save reads; the
            // screen keeps that toggle). Its S the hotkey (O is Order's),
            // the item marked as the port's own (screen.cpp kPortItem,
            // drawn as its 'o'). M, Messages' key, opens the screen too and
            // stays out of the key log (session 35, the owner's).
            const std::size_t a = items2.find(' ');
            const std::size_t b = a == std::string::npos ? a : items2.find(' ', a + 1);
            if (b != std::string::npos) items2 = "\x01ptionS" + items2.substr(b);
            options_key_ = 'M';
            input_->options_key('M');
        }
        const std::string items = (mode_ == 9 ? std::string() : text_.ds_str(kChangeItems, 40)) + items2;
        const MenuKey mk = menu_screen("", items, false, true);
        if (port) {
            options_key_ = 0;
            input_->options_key(0);
        }
        key = mk.key;
        if (mk.ext) {                                    // 0x45E13
            roster_row(cur_, false);
            select_char(key);
            roster_row(cur_, true);
        } else if (key == 'O') {
            change_order();
        } else if (key == 'P') {
            // PURGE of the last member leaves the adventure with no party
            // (CurChar nil) and DS:3BF6 clear (only PURGE's No path sets it,
            // 0x482AD): the menu comes again and the adventure goes on
            // (run s26_purge_all, route purge_all).
            purge_character();
        } else if (key == 'M') {
            change_messages();
        } else if (key == 'A') {
            ds3bee_ = ds3bee_ ? 0 : 1;                   // 0x45E6B: no reader but the save
            show_message(text_[ds3bee_ ? Text::ChangeAnimOn : Text::ChangeAnimOff]);
        } else if (key == 'L') {
            change_level();
        } else if (key == 'S' && input_ && input_->options_menu()) {
            options_screen();
        }
    }
}

void Game::change_level() {
    // Level ovl12 +0x0236 (0x45496): the current level highlighted (DS:6B1D,
    // DS:6B48 := 0xFF), one MenuScreen, var 0x4BB6 := the choice.
    int l = v0(0x4BB6);
    highlight_ = static_cast<std::uint8_t>(l + 1);
    menu_sum_ = 0xFF;
    const MenuKey mk = menu_screen("", text_.ds_str(kLevelItems, 40), false, true);
    if (mk.ext) {
        roster_row(cur_, false);
        select_char(mk.key);
        roster_row(cur_, true);
    } else {
        switch (mk.key) {
            case 'N': l = 0; break;
            case 'C': l = 1; break;
            case 'T': l = 2; break;
            case 'V': l = 3; break;
            case 'A': l = 4; break;
            default: break;
        }
    }
    v0(0x4BB6) = static_cast<std::uint8_t>(l);
}

std::string Game::speed_name(int v) const {
    // The Messages speed's name: DS:0990's name and DS:09A8's suffix by
    // the index DS:09B4[v]. Speed 0 is Fastest (DS:09B4[0] = 0x22); its
    // waits are 0 (Crt.Delay(0) a character, Game.Delay 0). D-0062's
    // "Instant" was withdrawn in session 35.
    const std::uint8_t ix = text_.ds8(kSpeedIndex + static_cast<std::size_t>(v));
    return ds_pascal(kSpeedNames + 8 * static_cast<std::size_t>(ix >> 4)) +
           ds_pascal(kSpeedSuffixes + 4 * static_cast<std::size_t>(ix & 3));
}

void Game::change_messages() {
    // Messages ovl12 +0x0349 (0x455A9): the speed var 0x4BFC (Game.Delay's
    // factor) in the text window, Faster (when above 0) / Slower (below 9)
    // until 0 or 'E' (CS:0329); Down is faster, Up slower.
    line_home();                                         // 0653:112F
    int key;
    do {
        lp_col_ = left_;                                 // DS:4A1C/4A1D := DS:5F40/41
        lp_row_ = top_;
        print_line(0, "");
        const int v = v0(0x4BFC);
        print_line(10, text_[Text::SpeedIs] + str_byte(v) + text_[Text::SpeedOpen] + speed_name(v) +
                           text_[Text::SpeedClose]);
        std::string items;
        if (v > 0) items += text_[Text::SpeedFaster];
        if (v < 9) items += text_[Text::SpeedSlower];
        items += text_[Text::SpeedExit];
        const MenuKey mk = menu_screen(text_[Text::SpeedPrompt], items, false, true);
        key = mk.key;
        if (mk.ext) {
            if (key == 0x50 && v0(0x4BFC) > 0) --v0(0x4BFC);
            else if (key == 0x48 && v0(0x4BFC) < 9) ++v0(0x4BFC);
        } else if (key == 'F') {
            --v0(0x4BFC);
        } else if (key == 'S') {
            ++v0(0x4BFC);
        }
        if (mk.ext) key = -1;                            // a scan code is never 0 / 'E' here
    } while (key != 0 && key != 'E');
    line_home();
}

void Game::move_up() {
    // MoveUp ovl12 +0x053B (0x4579B): CurChar swaps with its predecessor;
    // at the head it goes to the tail.
    const int i = index_of(cur_);
    if (i < 0 || party_.size() < 2) return;
    if (i == 0) std::rotate(party_.begin(), party_.begin() + 1, party_.end());
    else std::swap(party_[static_cast<std::size_t>(i)], party_[static_cast<std::size_t>(i - 1)]);
}

void Game::move_down() {
    // MoveDown ovl12 +0x067C (0x458DC): CurChar swaps with its successor;
    // at the tail it goes to the head.
    const int i = index_of(cur_);
    if (i < 0 || party_.size() < 2) return;
    if (i + 1 == static_cast<int>(party_.size())) std::rotate(party_.begin(), party_.end() - 1, party_.end());
    else std::swap(party_[static_cast<std::size_t>(i)], party_[static_cast<std::size_t>(i + 1)]);
}

void Game::change_order() {
    // Order ovl12 +0x0820 (0x45A80): Select mode's or Place mode's items (remap)
    // until 0 or 'E' (CS:0785). Select / Enter / Place toggle the mode
    // (the name and CS:07B3 by NameMessage; ClearText); in Place mode
    // Up/Down move CurChar through the party (MoveUp / MoveDown), else they
    // choose CurChar. DS:6B4A (1 while it runs) has no reader.
    bool sel = false;
    int key = ' ';
    while (key != 0 && key != 'E') {
        Character* const old = cur_;
        const MenuKey mk = menu_screen(text_[Text::OrderPrompt], text_.ds_str(kOrderItems + 0x29 * (sel ? 1 : 0), 40), true, true);
        key = mk.ext ? -1 : mk.key;
        if (cur_ != old) {
            // 0x45ADE: CurChar changed under the menu: a roster click. DS:8433 is 0
            // inside CHANGE from the adventure; TEAM_MENU 9 keeps it at 1 from
            // SET_MODE's redraw, and no shipped script uses that (route s77_u_order_click).
            sel = !sel;
            if (sel) {
                name_message(*cur_, text_[Text::OrderSelected], 10, false);
            } else if (old && cur_) {
                const int oi = index_of(old);
                if (oi < 0) throw Error("Order (ovl12 0x45BDD): the member to move is not in the party (the original's own fatal)");
                auto moved = std::move(party_[static_cast<std::size_t>(oi)]);
                party_.erase(party_.begin() + oi);
                const int ci = index_of(cur_);
                if (ci < 0) throw Error("Order (ovl12 0x45C90): the place to move to is not in the party (the original's own fatal)");
                party_.insert(party_.begin() + ci, std::move(moved));
                draw_roster();
            } else {
                draw_roster();
            }
        } else if (mk.ext) {
            if (!sel) {
                roster_row(cur_, false);
                select_char(mk.key);
                roster_row(cur_, true);
            } else {
                if (mk.key == 0x48) move_up();
                else if (mk.key == 0x50) move_down();
                draw_roster();
            }
        } else if (key == kEnter || key == 'P' || key == 'S') {   // CS:0800
            sel = !sel;
            if (sel && cur_) {
                name_message(*cur_, text_[Text::OrderSelected], 10, false);
            } else if (sel) {
                // No party (PURGE of the last member): the original prints the
                // name of a record at 0000:0000, the interrupt table (D-0053).
                combat::diag("D-0053", "Order (ovl12 +0x0820, 0x45A80): NameMessage with no party reads a name at "
                                       "0000:0000 (the interrupt table): nothing is printed");
            } else {
                clear_text();
            }
        }
    }
    clear_text();                                        // 0x45D52
}

void Game::restore_cur() {
    // DS:4A30 is kept by ovl2 alone (EnterBlock, RunLoop, REMOVE_CHAR);
    // TeamMenu's REMOVE and PURGE (from TEAM_MENU 0) free a record without
    // touching it, so it can name a freed record, which the original then
    // makes CurChar (the heap block, its first bytes TP's free-list link):
    // the roster whitens nobody (run s55_blind_s01). The engine keeps
    // CurChar for its other readers, detached (D-0055).
    if (!saved_cur_ || index_of(saved_cur_) >= 0) {
        cur_ = saved_cur_;
        return;
    }
    combat::diag("D-0055", "DS:4A30 names a record TeamMenu removed (REMOVE or PURGE from TEAM_MENU 0): CurChar "
                           "is kept, the roster whitens nobody");
    cur_.detached = true;
}

void Game::rest_menu() {
    // TEAM_MENU 9, ovl12 +0x15D8 (0x46838..0x468FE, main session): until
    // 'E' typed as a key (Esc does not leave): MenuScreen('', RestItems
    // CS:15B2, no remap, animated). R: ShowMessage(RestRests CS:15C8),
    // Rest(1), Screen.Roster; S: the adventure's save
    // (0x461DF); C: CHANGE (0x45D9B); an extended key: RosterRow,
    // SelectChar, RosterRow, the status line. No shipped script uses it.
    for (;;) {
        const MenuKey mk = menu_screen("", text_[Text::RestItems], false, true);
        if (!mk.ext) {
            if (mk.key == 'R') {
                show_message(text_[Text::RestRests]);
                rest(1);
                draw_roster();
            } else if (mk.key == 'S') {
                save_menu();
            } else if (mk.key == 'C') {
                change_menu();
            }
        } else {
            roster_row(cur_, false);
            select_char(mk.key);
            roster_row(cur_, true);
            draw_status();
        }
        if (mk.key == 'E' && !mk.ext) return;
    }
}

}  // namespace cotd::game
