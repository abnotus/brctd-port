// SPDX-License-Identifier: GPL-3.0-or-later
// Text window, menus and line input (resident segment 0653, ovl15, ovl25).
// The text itself isn't drawn headless; the cursor, the waits and the keys
// they consume are modelled exactly, and what was printed is kept as a
// transcript.
#include <algorithm>
#include <cctype>
#include <cstring>

#include "game/combat_rules.hpp"
#include "game/game.hpp"

namespace cotd::game {

namespace {

// DS:0EA0: the punctuation that ends a word: ! , - . : ; ?
bool punct(char c) {
    return c == '!' || c == ',' || c == '-' || c == '.' || c == ':' || c == ';' || c == '?';
}

}  // namespace

int Game::next_key(const char* what) {
    // Input.GetKey 08AD:01CE (main): DS:8435 := 0; a key, or with a driver
    // a button (INT 33h fn 3): left 0x0D, right 0x1B, DS:8435 := 1 and the
    // pointer in DS:8437/8439 (docs/notes/mouse.md). A key typed ahead
    // waits in the input's buffer (game/input.hpp) until read or flushed
    // (flush_keys).
    if (!input_) throw Parked{what};
    for (;;) {
        steps_unread_ = 0;
        const int k = input_->read_key(screen_, what);
        if (is_options_key(k)) {   // the port's options screen; then read on (D-0065)
            options_screen();
            continue;
        }
        note_transcript(std::string("[key ") + key_name(k) + " at " + what + "]");
        if (const int g = get_key(k); g >= 0) return g;
    }
}

int Game::next_key_for(const char* what, int ms) {
    // next_key, giving up after ms (>= 0): -1 then, and nothing is read.
    if (!input_) throw Parked{what};
    for (;;) {
        steps_unread_ = 0;
        const int k = input_->read_key_for(screen_, what, ms);
        if (k < 0) return k;
        if (is_options_key(k)) {
            options_screen();
            continue;
        }
        note_transcript(std::string("[key ") + key_name(k) + " at " + what + "]");
        if (const int g = get_key(k); g >= 0) return g;
    }
}

int Game::read_key(const char* what) {
    // Crt.ReadKey (InputString 0653:07E5): the keyboard only; a click is
    // never seen (the original would go on waiting: dropped here).
    if (!input_) throw Parked{what};
    for (;;) {
        steps_unread_ = 0;
        const int k = input_->read_key(screen_, what);
        if (is_options_key(k)) {
            options_screen();
            continue;
        }
        note_transcript(std::string("[key ") + key_name(k) + " at " + what + "]");
        if (!is_click(k)) return k;
    }
}

int Game::get_key(int k) {
    // GetKey's result for what the input gave: the key, the button's key,
    // or -1 for a click without a driver (not seen: read on).
    f8435_ = 0;
    if (k == 0x13 || k == 0x15) ctrl_key(k);           // ^S, ^U (0x18D05)
    if (k == 0x04) toggle_debug();                     // ^D (0x18D7E)
    if (!is_click(k)) return k;
    if (!mouse_on_) return -1;
    // 08AD:0123 (0x18BF3): GetKey's mouse wait: buttons 1 Enter, 2 Esc, DS:8435 := 1, a key leaves it
    f8435_ = 1;
    mouse_x_ = click_x(k);
    mouse_y_ = click_y(k);
    return click_buttons(k) == 1 ? kEnter : kEsc;
}

void Game::clear_window() {
    // ClearRect 0653:110F: the window rectangle filled with colour 0.
    fill_rect(left_, top_, right_, bottom_, 0);
    note_transcript("[clear]");
}

void Game::set_window(int l, int t, int r, int b, int fill) {
    // SetWindow 0653:115F (0x1768F): DS:5F40..44, then the line printer
    // cleared.
    left_ = l;
    top_ = t;
    right_ = r;
    bottom_ = b;
    fill_ = fill;
    line_home();
}

void Game::line_home() {
    // 0653:112F (0x1765F): the line printer at the window's top left, the
    // window filled with DS:5F44, nothing printed yet.
    lp_col_ = left_;
    lp_row_ = top_;
    fill_rect(left_, top_, right_, bottom_, fill_);
    lp_used_ = false;
}

void Game::print_line(int fg, const std::string& text) {
    // PrintLine 0653:118A (0x176BA): a line wider than the window splits at
    // its last space before the right edge (both halves printed in turn);
    // below the window's bottom it waits (0x177C1: FlushKeys,
    // PressContinue, row 24 cleared) and starts again at the top. Drawn
    // with DrawString on DS:5F44.
    const std::string s = text.substr(0, 255);
    const int width = right_ - left_ + 1;
    if (static_cast<int>(s.size()) > width) {
        int i = right_ - 1;   // 1-based character index
        while (i >= 1 && s[static_cast<std::size_t>(i - 1)] != ' ') --i;
        if (i < 1)
            throw Error("D-0090: PrintLine (0653:118A, 0x176FA): a line with no space to split at reads before the string");
        print_line(fg, s.substr(0, static_cast<std::size_t>(std::max(i - 1, 0))));
        print_line(fg, s.substr(static_cast<std::size_t>(i)));
        return;
    }
    if (lp_row_ > top_ && !lp_used_) line_home();
    else if (lp_row_ > bottom_) {
        if (lp_used_) pause_continue();
        line_home();
    }
    draw_string(lp_col_, lp_row_, fg, fill_, s);
    note_transcript(s);
    message_chars(s.size());   // a ship fight's message line
    lp_used_ = true;
    ++lp_row_;
}

void Game::press_continue(int fg, int bg) {
    // 0653:0C02 (fg, bg): row 0x18 blanked, the prompt DS:0EDE drawn at
    // column 0 in the caller's colours (0653:1291 passes black on white,
    // ApplyDamage white on black), then GetKey until Enter or Esc (set
    // CS:0BE2).
    press_continue(fg, bg, text_[Text::PressContinue]);
}

void Game::press_continue(int fg, int bg, const std::string& text) {
    // 0653:0C02 with the caller's text (RemoveItem's own message).
    fill_cells(0, 0x18, 0, 40);
    draw_string(0, 0x18, fg, bg, text);
    for (;;) {
        const int k = next_key("press button or enter to continue");
        if (k == kEnter || k == kEsc) return;
    }
}

void Game::pause_continue() {
    // 0653:1291: flush the keys (0x177C4), 0653:0C02, clear row 0x18.
    flush_keys();
    press_continue();
    clear_row24();
}

void Game::flush_keys() {
    // Input.FlushKeys 08AD:03E4: KeyPressed / ReadKey until no key is
    // waiting. Headless runs have a key in the buffer only after a burst
    // (game/input.hpp); the window's buffer holds what was typed ahead.
    if (input_) input_->flush();
}

void Game::print(const std::string& s, bool clear, int fg) {
    // Text.Print 0653:056C over the window DS:5F40..43 (docs/formats/ecl.md
    // "Text output"); characters drawn by 0653:04B3 in fg on 0.
    const int l = left_, t = top_, r = right_, b = bottom_;
    if (l > 0x27 || t > 0x18 || r > 0x27 || b > 0x27) return;
    if (col_ < l || col_ > r || row_ < t || row_ > b) {
        col_ = l;
        row_ = t;
    }
    if (clear) {
        clear_window();
        col_ = l;
        row_ = t;
    }
    if (s.empty()) return;
    note_transcript(s);
    message_chars(s.size());   // in a fight: for the message wait
    const int len = static_cast<int>(s.size());
    // The text options (session 27): Instant draws the text without the
    // typewriter's waits (D-0057); while it types, the window may drop the
    // keys that arrive (D-0059).
    const bool types = typing_ && !(input_ && input_->text_speed() == TextSpeed::Instant);
    if (types && input_) input_->typing(true);
    auto at = [&](int i) { return s[static_cast<std::size_t>(i - 1)]; };   // 1-based
    int pos = 1;
    while (pos <= len) {
        int i = pos;
        while (i < len && punct(at(i))) ++i;
        while (i < len && !punct(at(i)) && at(i) != ' ') ++i;
        if (at(i) != ' ')
            while (i + 1 < len && punct(at(i + 1))) ++i;
        // D-0089: the Log Book's text (D-0083, the port's own) breaks a run
        // wider than the whole window at the window's edge. A script's text
        // keeps the original's way (below), which shipped text never meets.
        if (printing_logbook_ && col_ == l && i - pos > r - l) i = pos + (r - l);
        // 0653:04B3 (0x169E3): the run's characters from pos to i, each
        // moving the column on; DrawChar drops a column past 0x27 (0x167A1).
        auto put = [&] {
            for (; pos <= i; ++pos) {
                draw_char(col_++, row_, fg, 0, static_cast<unsigned char>(at(pos)));
                // 0x16A2F: the typewriter, Crt.Delay(var 0x4BFC * 3) after
                // each character while DS:3BF8 is set.
                if (types) pause_ms((v0(0x4BFC) * 3) & 0xFFFF);
            }
        };
        if (col_ + (i - pos) <= r) {
            put();
            continue;
        }
        // Wrap: next line, skip spaces, page full -> wait and clear.
        col_ = l;
        ++row_;
        // 0653:0530 (0x16A60): after a wrap, skip spaces: pos < len and s[pos] = ' '
        while (pos < len && at(pos) == ' ') ++pos;
        if (row_ > b && pos < len) {
            col_ = l;
            row_ = t;
            if (types && input_) input_->typing(false);   // the page's wait reads keys
            pause_continue();
            flush_keys();                              // 0x16CC4
            clear_window();
            if (types && input_) input_->typing(true);
            // 0x16CDD: the run is printed on the new page whether it fits or
            // not, so a run wider than the window moves down to the page's
            // end, waits, and prints at the top from the left column, the
            // columns past 0x27 dropped (run s80_longrun: 39 letters in the
            // 38-column window, the last on the frame). Session 74 had the
            // loop page forever (D-0089's first reading).
            put();
        }
    }
    if (types && input_) input_->typing(false);
    if (col_ > r) {
        col_ = l;
        ++row_;
    }
}

void Game::logbook_print(int n) {
    // D-0083: LOGBOOK.TXT's entry n after the original's LOGBOOK line, as
    // the game's own text: Text.Print's word wrap in the text window, each
    // paragraph on a line of its own, and Print's page-full pause (the
    // continue wait, the keys flushed, the window cleared) where a
    // paragraph starts below the window. A map paragraph is a page drawn
    // from the game's data (draw_logbook_map).
    const LogbookEntry* e = logbook_text_ ? logbook_text_->entry(n) : nullptr;
    if (!e || !logbook_named(n)) return;
    logbook_shown_.push_back(n);
    const int l = left_, t = top_, b = bottom_;
    auto end_line = [&]() {
        if (col_ != l) {
            col_ = l;
            ++row_;
        }
        if (row_ > b) {
            pause_continue();
            flush_keys();                               // 0x16CC4, as Print's page
            clear_window();
            col_ = l;
            row_ = t;
        }
    };
    // The pages' keys are the port's own (the original has no such pages):
    // they stay out of the key log (a log replays with the Log Book off,
    // as injected keys run, and so as the original).
    if (input_) input_->quiet_keys(true);
    bool done_maps = false;
    end_line();   // the entry starts on a line of its own
    auto page_break = [&]() {
        pause_continue();
        flush_keys();
        clear_window();
        col_ = l;
        row_ = t;
    };
    for (const std::string& p : e->paragraphs) {
        if (p[0] == LogbookText::kMap) {
            // The entry's maps, one page each, browsed with the game's own
            // menu on the bottom row (Next Prev Exit, the arrows, PgUp and
            // PgDn); Exit (or Esc, or Next on the last) goes back to the
            // text and the game's screen.
            if (done_maps) continue;
            done_maps = true;
            if (row_ != t || col_ != l) page_break();
            const Framebuffer saved = screen_;
            const int n_maps = static_cast<int>(e->maps.size());
            for (int i = 0; i >= 0 && i < n_maps;) {
                draw_logbook_map(e->maps[static_cast<std::size_t>(i)]);
                const MenuKey mk = menu_screen("", n_maps > 1 ? "~Next ~Prev ~Exit" : "~Exit", false);
                const bool next = mk.key == 'N' || mk.key == kEnter || (mk.ext && (mk.key == 0x4D || mk.key == 0x50 || mk.key == 0x51));
                const bool prev = mk.key == 'P' || (mk.ext && (mk.key == 0x4B || mk.key == 0x48 || mk.key == 0x49));
                if (mk.key == 'E' || mk.key == 0 || n_maps == 1) break;
                if (next) ++i;
                else if (prev && i > 0) --i;
            }
            flush_keys();
            screen_ = saved;   // the game's screen back
            continue;
        }
        end_line();
        printing_logbook_ = true;
        print(p, false);
        printing_logbook_ = false;
    }
    if (input_) input_->quiet_keys(false);
}

int Game::menu(const std::vector<std::string>& items, bool single, bool anim) {
    // Menu helper ovl15 +0x16DD: the items joined as "~it1 ~it2 ...",
    // shown through the display transform (ovl15 +0x15A6: '~' marks the
    // hotkey, which is upper-cased; other [0-9A-Z] get +0x20, so digits
    // become P..Y: Q9), then MenuScreen until a non-extended key in
    // [0-9A-Z] (single-item menus also accept 0 and Enter).
    std::string joined;
    for (std::size_t k = 0; k < items.size(); ++k) joined += (k ? " ~" : "~") + items[k];
    std::string shown;
    std::vector<char> hot;
    for (std::size_t i = 0; i < joined.size(); ++i) {
        char c = joined[i];
        if (c == '~' && i + 1 < joined.size()) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(joined[++i])));
            hot.push_back(c);
            shown += c;
        } else {
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) c = static_cast<char>(c + 0x20);
            shown += c;
        }
    }
    note_transcript("[menu]");
    int key = 0;
    for (;;) {
        const MenuKey mk = menu_screen("", shown, true, anim);
        if (mk.ext) continue;
        key = mk.key;
        const bool ends = (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
                          (single && (key == 0 || key == kEnter));
        if (ends) break;
    }
    if (key == kEnter) return 0;
    for (int i = 0; i < static_cast<int>(hot.size()); ++i)
        if (hot[static_cast<std::size_t>(i)] == key) return i;
    return 0xFF;
}

int Game::yes_no(const std::string& prompt, int highlight) {
    // YesNo ovl25 +0x1F6D (0x5BAAD): DS:6B1D := highlight; DS:6B48 := 0xFF
    // (so the highlight isn't reset); MenuScreen(prompt, "Yes No" CS:1F46,
    // no remapping) until DS:6B49 is in {'N', 'Y'} (CS:1F4D). The prompt,
    // text and hotkey colours the caller set are kept for every retry
    // (0x5BADE). An extended key counts by its scan code: Shift-F6 (0x59)
    // is Yes (docs/notes/keys.md).
    highlight_ = static_cast<std::uint8_t>(highlight);
    const int p = menu_prompt_fg_, t = menu_text_fg_, h = menu_hot_fg_;
    for (;;) {
        menu_hot_fg_ = h;
        menu_text_fg_ = t;
        menu_prompt_fg_ = p;
        menu_sum_ = 0xFF;
        const MenuKey mk = menu_screen(prompt, text_[Text::YesNoItems], false);
        if (mk.key == 'Y' || mk.key == 'N') return mk.key;
    }
}

std::string Game::input_string(const std::string& prompt, int fg, int bg, int max, int entry, const char* refused) {
    // InputString 0653:07E5 (0x16D15) with its drawing on row 0x18: the row
    // cleared (FillCells 0x16909), the prompt (at most 40) in fg/bg, then
    // per key a white cursor cell after the text; typed characters are
    // drawn white where the cursor was; Backspace clears two cells. Enter
    // or Esc ends it and clears the row.
    const std::string p = prompt.substr(0, 40);
    fill_cells(0, 0x18, 0, 0x28);
    draw_string(0, 0x18, fg, bg, p);
    InputCursor d{static_cast<int>(p.size())};
    return input_string_keys(max, &d, entry, refused);
}

std::string Game::input_string_keys(int max, InputCursor* d, int entry, const char* refused) {
    // The key rules (0x16D61..0x16F3F): Crt ReadKey; extended keys become
    // digits (Home 7, Up 8, PgUp 9, Left 4, scan 0x20 5, Right 6, End 1,
    // Down 2, PgDn 3, Ins 0; others nothing); space only after the first
    // character; 0x21..0x7A appended below max; BS deletes; Enter or Esc
    // ends. Result: trailing spaces trimmed, upper case, at most 40.
    std::string s;
    if (input_) input_->text_entry(entry);   // the pad's picker (D-0085)
    for (;;) {
        if (d) fill_cells(d->pos + 1, 0x18, 0x0F, 1);   // the cursor
        int k = read_key("input");
        switch (k) {
            case kHome: k = '7'; break;
            case kUp: k = '8'; break;
            case kPgUp: k = '9'; break;
            case kLeft: k = '4'; break;
            case 0x100 + 0x20: k = '5'; break;
            case kRight: k = '6'; break;
            case kEnd: k = '1'; break;
            case kDown: k = '2'; break;
            case kPgDn: k = '3'; break;
            case kIns: k = '0'; break;
            default: if (k >= 0x100) k = 0; break;
        }
        if (k == ' ') {
            if (!s.empty() && static_cast<int>(s.size()) < max) {
                s += ' ';
                if (d) draw_string(++d->pos, 0x18, 15, 0, " ");
            }
        } else if (k >= 0x21 && k <= 0x7A) {
            // D-0092: a character a file name can't hold isn't taken (no
            // echo, as a key the prompt ignores).
            if (refused && std::strchr(refused, k)) continue;
            if (static_cast<int>(s.size()) < max) {
                s += static_cast<char>(k);
                if (d) draw_string(++d->pos, 0x18, 15, 0, std::string(1, static_cast<char>(k)));
            }
        } else if (k == kBackspace) {
            if (!s.empty()) {
                s.pop_back();
                if (d) fill_cells(d->pos--, 0x18, 0, 2);
            }
        }
        if (k == kEnter || k == kEsc) break;
    }
    if (input_) input_->text_entry(kEntryNone);
    if (d) fill_cells(0, 0x18, 0, 0x28);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (s.size() > 40) s.resize(40);
    return s;
}

}  // namespace cotd::game
