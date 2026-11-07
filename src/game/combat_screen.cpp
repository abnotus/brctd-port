// SPDX-License-Identifier: GPL-3.0-or-later
// The combat screen: the battlefield view (ovl30), the combatant icons
// (ovl31), the side panel and the combat messages (ovl23), the layout
// (015F:01F1). docs/notes/combat_screens.md; main-session readings of
// session 19 unless noted.
#include <algorithm>

#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr int kDx[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};   // DS:257C
constexpr int kDy[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};   // DS:2585

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }

struct Line { int x1, y1, x2, y2, start, end; };
// DS:0E5C: the combat frame (015F:0000's list, 0xFE ends it).
constexpr Line kCombatLines[] = {{0, 0, 39, 0, 0, 1}, {0, 0, 0, 22, 0, 3}, {39, 0, 39, 22, 1, 2},
                                 {0, 22, 39, 22, 3, 2}, {22, 0, 22, 22, 6, 8}};

}  // namespace

// --- images ------------------------------------------------------------------------

void Game::load_combat_icon(const std::string& name, int id, int slot) {
    // LoadCombatIcon ovl31 +0x0166 (0x608C6): only COMSPR, ICON, CHARS and
    // CPIC (CPIC + the area digit DS:4898); id into DS:5F86 + 8 slot and
    // id + 0x80 into +4, colour key 8; then CHARS copies the working palette's
    // 0x60..0x8F into DS:8A8B, CPIC 0x90..0xFF.
    if (name != "COMSPR" && name != "ICON" && name != "CHARS" && name != "CPIC") return;
    if (slot < 0 || slot >= static_cast<int>(combat_icons_.size())) throw Error("combat icon slot " + std::to_string(slot));
    const std::string file = name + (name == "CPIC" ? std::to_string(ecl_file_) : "") + ".DAX";
    CombatIcon& ic = combat_icons_[static_cast<std::size_t>(slot)];
    for (int frame = 0; frame < 2; ++frame) {
        const int eid = (id + (frame ? 0x80 : 0)) & 0xFF;
        auto raw = dax_entry(file, eid);
        if (!raw) {
            // Gfx.LoadImage (0x12B63..0x12BAA): the old image freed, then
            // no entry (DaxLoad: size 0) and nothing loaded: the slot is
            // empty (NPC 62's CPIC1 icon, route lowmem_chain).
            (frame ? ic.action : ic.ready) = IconFrame{};
            continue;
        }
        Image im = decode_generic(*raw, file + " #" + std::to_string(eid));
        if (im.frames.empty()) throw Error(file + " #" + std::to_string(eid) + ": no frame");
        load_palette(im);
        flush_keys();                                  // Gfx.LoadImage's 0x12BED
        (frame ? ic.action : ic.ready) = IconFrame{im.width, im.height, im.frames[0]};
    }
    ic.loaded = true;
    if (name == "CHARS")
        for (int i = 0x60; i < 0x90; ++i) pal_8a8b_[static_cast<std::size_t>(i)] = pal_908b_[static_cast<std::size_t>(i)];
    if (name == "CPIC")
        for (int i = 0x90; i < 0x100; ++i) pal_8a8b_[static_cast<std::size_t>(i)] = pal_908b_[static_cast<std::size_t>(i)];
    flush_keys();                                      // 0x60B1E
}

void Game::load_field_tiles(const std::string& name, int first, int count) {
    // LoadTiles ovl31 +0x0022 (0x60782): the DAX entry 1's frames, count of
    // them, into the tile image DS:5F82 from frame `first` (0x3B frames).
    // System.Move (0x60832) copies count frames whatever the image holds:
    // a frame past the file's reads the memory past the image (D-0063).
    // There the reference machine holds the decoded entry the image was
    // built over (its bytes from the image's size, 0x1B + frames x size,
    // to its end), then heap bytes, zeros in 20 of 21 recorded dumps
    // (session 44: the Mars and Venus wild fights' slots, LOTEK's slot
    // 0x34 in the Mars village; analysis/s44/tilemodel.py).
    auto raw = dax_entry(name, 1);
    if (!raw) throw Error(name + ": no entry 1 (the battlefield tiles)");
    Image im = decode_generic(*raw, name + " #1");
    load_palette(im);
    field_tiles_.assign(0x3B, Bytes());
    field_past_.assign(0x3B, false);
    const std::size_t fs = static_cast<std::size_t>(im.width) * static_cast<std::size_t>(im.height);
    const std::size_t img = 0x1B + im.frames.size() * fs;
    for (int i = 0; i < count && first + i < 0x3B; ++i) {
        const auto slot = static_cast<std::size_t>(first + i);
        if (i < static_cast<int>(im.frames.size())) {
            field_tiles_[slot] = im.frames[static_cast<std::size_t>(i)];
            continue;
        }
        Bytes px(fs, 0);
        for (std::size_t j = 0; j < fs; ++j) {
            const std::size_t p = 0x1B + static_cast<std::size_t>(i) * fs + j;
            if (p >= img && p < raw->size()) px[j] = (*raw)[p];
        }
        field_tiles_[slot] = std::move(px);
        field_past_[slot] = true;
        field_past_file_ = name;
    }
    flush_keys();                                      // Gfx.LoadImage's 0x12BED; 0x60842
}

// --- the backbuffer DS:3C7A (168 wide in combat, screen origin (8,8)) --------------

void Game::field_blit(const Bytes& px, int w, int h, int x, int y, bool masked, int key, bool mirror) {
    if (px.size() < static_cast<std::size_t>(std::max(w, 0) * std::max(h, 0))) return;   // nil or cut short: nothing
    note_drawn(px.data(), px.size());                 // tools only (core/image.hpp)
    for (int r = 0; r < h; ++r) {
        const int Y = y + r;
        if (Y < 0 || Y >= kViewPx) continue;
        for (int c = 0; c < w; ++c) {
            const int X = x + c;
            if (X < 0 || X >= kViewPx) continue;
            const std::uint8_t v = px[static_cast<std::size_t>(r * w + (mirror ? w - 1 - c : c))];
            if (masked && v == key) continue;
            field_buf_[static_cast<std::size_t>(Y * kViewPx + X)] = v;
        }
    }
}

void Game::field_present() {
    // Present 0267:183A: the backbuffer to the screen at (8,8).
    screen_.blit(field_buf_, kViewPx, kViewPx, 8, 8);
}

void Game::draw_combat_icon(int rx, int ry, int mode, int frame, int slot) {
    // DrawCombatIcon ovl31 +0x03C9 (0x60B29): DS:5F86 + 8 slot + 4 frame
    // (nil: nothing) at (24 rx, 24 ry) of the backbuffer, colour key 8;
    // mode > 3 draws it mirrored (0267:0F0B: a horizontal mirror).
    if (slot < 0 || slot >= static_cast<int>(combat_icons_.size())) return;
    const CombatIcon& ic = combat_icons_[static_cast<std::size_t>(slot)];
    if (!ic.loaded) return;
    const IconFrame& f = frame ? ic.action : ic.ready;
    field_blit(f.px, f.w, f.h, 24 * rx, 24 * ry, true, 8, mode > 3);
}

// --- the map view (ovl30) -------------------------------------------------------------

int Game::field_tile_at(int x, int y) const {
    if (x < 0 || x >= kFieldW || y < 0 || y >= kFieldH) return 0;
    return field_[static_cast<std::size_t>(y * kFieldW + x)];
}

int Game::field_underlying(int x, int y, int v) const {
    // The tile under an area effect (1, 2, 4: the list DS:6B4F, none yet)
    // or a body (5: DS:6862 + 7n, the last match), else v (+0x14F2 and
    // DrawTile 0x5EEE9).
    if (v == 1 || v == 2 || v == 4) {                  // set CS:14D2
        for (const AreaFx& a : areas_)
            for (int i = 1; i <= 9; ++i) {
                int cx, cy;
                area_cell(a, i, cx, cy);
                if (a.on[static_cast<std::size_t>(i)] && cx == x && cy == y) v = a.under[static_cast<std::size_t>(i)];
            }
    } else if (v == 5) {
        for (const Body& b : bodies_)
            if (b.x == x && b.y == y) v = b.old;
    }
    return v;
}

void Game::field_draw_tile(int rx, int ry, int mx, int my) {
    // DrawTile ovl30 +0x00F1 (0x5EE61): terrain over 0x9A or 0 counts as
    // 0x15; its frame DS:258D[4 t] from the tile image, opaque. Tiles 1..5
    // draw the tile underneath, then their own frame as the combat image
    // slot 0x21 + frame div 2, image (frame and 1), masked.
    auto norm = [](int t) { return (t > 0x9A || t == 0) ? 0x15 : t; };
    auto frame_of = [&](int t) { return static_cast<int>(text_.ds8(0x258D + 4 * static_cast<std::size_t>(t))); };
    auto tile_frame = [&](int f) -> const Bytes& {
        if (f < 0 || f >= static_cast<int>(field_tiles_.size()) || field_tiles_[static_cast<std::size_t>(f)].empty())
            throw Error("battlefield tile frame " + std::to_string(f) + " is not loaded (LoadTiles ovl31 +0x0022)");
        if (field_past_[static_cast<std::size_t>(f)])
            combat::diag("D-0063", "LoadTiles (ovl31 +0x0022, 0x60832): battlefield tile frame " + std::to_string(f)
                         + " lies past " + field_past_file_ + " #1's frames; the reference machine's bytes are drawn "
                         "(the decoded entry past the image, then zeros)");
        return field_tiles_[static_cast<std::size_t>(f)];
    };
    const int t = norm(field_tile_at(mx, my));
    const int tl = frame_of(t);
    if (t > 5) {
        field_blit(tile_frame(tl), 24, 24, 24 * rx, 24 * ry, false, 0, false);
        return;
    }
    const int u = norm(field_underlying(mx, my, field_underlying(mx, my, field_tile_at(mx, my))));
    field_blit(tile_frame(frame_of(u)), 24, 24, 24 * rx, 24 * ry, false, 0, false);
    const int slot = 0x21 + tl / 2;
    if (slot < static_cast<int>(combat_icons_.size()) && combat_icons_[static_cast<std::size_t>(slot)].loaded) {
        const CombatIcon& ic = combat_icons_[static_cast<std::size_t>(slot)];
        const IconFrame& f = tl & 1 ? ic.action : ic.ready;
        field_blit(f.px, f.w, f.h, 24 * rx, 24 * ry, true, 8, false);
    }
}

bool Game::footprint(int size, int i, int& dx, int& dy) const {
    // ovl30 +0x0000 (0x5ED70): DS:27EE + 8 size + 2 i; dx < 0 ends.
    if (size == 0) return false;
    dx = s8(text_.ds8(0x27EE + static_cast<std::size_t>(8 * size + 2 * i)));
    dy = s8(text_.ds8(0x27EF + static_cast<std::size_t>(8 * size + 2 * i)));
    return dx >= 0;
}

int Game::icon_index(const Character* c) const {
    // IndexOf ovl30 +0x0DCB: k (1..) of the record's icon, 0 for none.
    for (std::size_t k = 0; k < icons_.size(); ++k)
        if (icons_[k].rec == c) return static_cast<int>(k + 1);
    return 0;
}

int Game::occupant(int x, int y) const {
    // The occupancy grid DS:6267 (rebuilt by ovl30 +0x059E from the icon
    // table): the last icon whose footprint covers (x, y).
    if (x < 0 || x > 0x31 || y < 0 || y > 0x18) return 0;
    int occ = 0;
    for (std::size_t k = 0; k < icons_.size(); ++k) {
        const Icon& ic = icons_[k];
        for (int i = 0; i < 4; ++i) {
            int dx, dy;
            if (footprint(ic.size, i, dx, dy) && ic.x + dx == x && ic.y + dy == y) occ = static_cast<int>(k + 1);
        }
    }
    return occ;
}

bool Game::in_view(int rx, int ry) const { return rx >= 0 && rx <= 6 && ry >= 0 && ry <= 6; }   // +0x0883

bool Game::icon_visible(const Character* c, bool all) const {
    // IsVisible ovl30 +0x08B4 (0x5F624): any (all = 0) or every square of
    // its footprint in view.
    const int k = icon_index(c);
    if (!k) return false;
    const Icon& ic = icons_[static_cast<std::size_t>(k - 1)];
    if (ic.size == 0) return false;
    bool out = true;
    for (int i = 0; i < 4; ++i) {
        int dx, dy;
        if (!footprint(ic.size, i, dx, dy)) continue;
        if (!in_view(ic.x - field_ox_ + dx, ic.y - field_oy_ + dy)) {
            out = false;
            if (all) return false;
        } else {
            out = true;
            if (!all) return true;
        }
    }
    return out;
}

void Game::draw_icon_of(int k) {
    // The occupant's ready icon at its relative position (facing cb+7).
    const Icon& ic = icons_[static_cast<std::size_t>(k - 1)];
    const Character* c = ic.rec;
    draw_combat_icon(ic.x - field_ox_, ic.y - field_oy_, c->cb.size() == 0x12 ? c->cb[7] : 0, 0, c->b(0xB9));
}

bool Game::field_scroll(int x, int y, int margin) {
    // Scroll ovl30 +0x096E (0x5F6DE).
    int cx = s8(field_ox_ + 3), cy = s8(field_oy_ + 3);
    const int m = margin == 0xFF ? 0 : margin;
    if (margin != 0xFF && x >= s8(cx - m) && x <= s8(cx + m) && y >= s8(cy - m) && y <= s8(cy + m)) return false;
    if (x < s8(cx - m)) {
        while (x < cx && cx > 3) --cx;
    } else if (x > s8(cx + m)) {
        while (x > cx && cx < 0x2E) ++cx;
    }
    if (y < s8(cy - m)) {
        while (y < cy && cy > 3) --cy;
    } else if (y > s8(cy + m)) {
        while (y > cy && cy < 0x15) ++cy;
    }
    field_ox_ = s8(cx - 3);
    field_oy_ = s8(cy - 3);
    for (int ry = 0; ry <= 6; ++ry)
        for (int rx = 0; rx <= 6; ++rx) field_draw_tile(rx, ry, s8(field_ox_ + rx), s8(field_oy_ + ry));
    return true;
}

void Game::field_erase_icon(int rx, int ry, int k) {
    // EraseIcon ovl30 +0x072B (0x5F49B): k = 0: the square's occupant; its
    // footprint's squares in view get their tiles back, or the one square.
    int mx = s8(field_ox_ + rx), my = s8(field_oy_ + ry);
    if (k == 0) k = occupant(mx, my);
    if (k > 0) {
        const Icon& ic = icons_[static_cast<std::size_t>(k - 1)];
        rx = s8(ic.x - field_ox_);
        ry = s8(ic.y - field_oy_);
        mx = s8(field_ox_ + rx);
        my = s8(field_oy_ + ry);
        for (int i = 0; i < 4; ++i) {
            int dx, dy;
            if (footprint(ic.size, i, dx, dy) && in_view(rx + dx, ry + dy)) field_draw_tile(rx + dx, ry + dy, mx + dx, my + dy);
        }
        return;
    }
    if (in_view(rx, ry)) field_draw_tile(rx, ry, mx, my);
}

void Game::field_redraw_square(int x, int y) {
    // RedrawSquare ovl30 +0x04D3 (0x5F243): erase, then the occupant if visible.
    field_erase_icon(s8(x - field_ox_), s8(y - field_oy_), 0);
    const int k = occupant(x, y);
    if (k > 0 && icon_visible(icons_[static_cast<std::size_t>(k - 1)].rec, false)) draw_icon_of(k);
}

void Game::field_draw_square(int x, int y) {
    // DrawSquare ovl30 +0x0356 (0x5F0C6): the cursor's footprint (size
    // [DS:68A2]+5) squares in view: the tile, the box (COMSPR #20, slot
    // 0x20) when [DS:68A2]+4, then the occupant if visible.
    const int rx = s8(x - field_ox_), ry = s8(y - field_oy_);
    for (int i = 0; i < 4; ++i) {
        int dx, dy;
        if (!footprint(cursor_size_, i, dx, dy) || !in_view(rx + dx, ry + dy)) continue;
        field_draw_tile(rx + dx, ry + dy, x + dx, y + dy);
        if (cursor_on_) draw_combat_icon(rx + dx, ry + dy, 0, 0, 0x20);
    }
    const int k = occupant(x, y);
    if (k > 0 && icon_visible(icons_[static_cast<std::size_t>(k - 1)].rec, false)) draw_icon_of(k);
}

void Game::field_view(int x, int y, int margin, int dir) {
    // View ovl30 +0x0AFC (0x5F86C).
    int nx = s8(x + kDx[dir]), ny = s8(y + kDy[dir]);
    if (field_scroll(nx, ny, margin)) {
        for (std::size_t k = 0; k < icons_.size(); ++k) {
            const Icon& ic = icons_[k];
            if (ic.rec && ic.rec->b(0xD5) != 0 && ic.size > 0 && icon_visible(ic.rec, false))
                draw_icon_of(static_cast<int>(k + 1));
        }
    }
    field_redraw_square(x, y);
    if (!in_view(s8(nx - field_ox_), s8(ny - field_oy_))) {
        nx = std::clamp(nx, 0, 0x31);
        ny = std::clamp(ny, 0, 0x18);
    }
    field_draw_square(nx, ny);
    field_present();
}

void Game::focus_on(Character& c, int margin, bool cursor) {
    // Highlight ovl30 +0x1466 (0x601D6): the box on (and the cursor the
    // record's size), View on its square if DS:6BDA, then the cursor flag
    // off and its size 1.
    cursor_on_ = cursor;
    const int k = icon_index(&c);
    cursor_size_ = k ? icons_[static_cast<std::size_t>(k - 1)].size : 0;
    if (f6bda_ && k) field_view(icons_[static_cast<std::size_t>(k - 1)].x, icons_[static_cast<std::size_t>(k - 1)].y, margin, 8);
    cursor_on_ = false;
    cursor_size_ = 1;
}

// --- the frame, the redraw, the panel and messages (ovl23) -------------------------

void Game::combat_layout() {
    // 015F:01F1 (0x117E1): the inside cleared, the frame lines DS:0E5C, row
    // 0x17 cleared, the text window (0x17, 1, 0x17, 0x15).
    clear_cells(1, 1, 0x26, 0x15);
    for (const Line& l : kCombatLines) border_line(l.x1, l.y1, l.x2, l.y2, l.start, l.end);
    clear_cells(0, 0x17, 0x27, 0x17);
    left_ = 0x17;
    top_ = 1;
    right_ = 0x17;
    bottom_ = 0x15;
}

void Game::combat_redraw() {
    // ovl23 +0x2AF9 (0x58A09): ovl30 +0x00E3 (empty), the layout, View(the
    // centre, 0xFF, 8).
    combat_layout();
    field_view(s8(field_ox_ + 3), s8(field_oy_ + 3), 0xFF, 8);
}

void Game::print_in(int l, int t, int r, int b, int fg, bool clear, const std::string& s) {
    // Text.Print with an explicit window (the stored window DS:5F40.. is
    // the caller's argument only; the cursor DS:5F3E/5F3F moves).
    const int sl = left_, st = top_, sr = right_, sb = bottom_;
    left_ = l;
    top_ = t;
    right_ = r;
    bottom_ = b;
    print(s, clear, fg);
    left_ = sl;
    top_ = st;
    right_ = sr;
    bottom_ = sb;
}

void Game::name_message(Character& c, const std::string& msg, int row, bool flag) {
    // NameMessage ovl23 +0x0A78 (0x56988), combat state: the name at
    // (0x17, row), the message from row + 1 (window to 0x26, 0x15, green,
    // cleared first); flag: Game.Delay, then ClearText. State 8 (0x569F8):
    // PrintLine(11, name + ' ' + msg + '.') and never the flag. Otherwise
    // (0x56A3C): row is ignored, r := 0x11 (DS:6B6E is only ever 0), the
    // rows r..0x16 cleared, the name at (1, r + 1), the message from r + 2
    // (window (1, r + 2)-(0x26, 0x16), green, cleared).
    if (mode_ == 5) {
        clear_cells(0x17, row, 0x26, 0x15);
        draw_char_name(c, 0x17, row);
        message_chars(c.name().size());
        print_in(0x17, row + 1, 0x26, 0x15, 0x0A, true, msg);
    } else if (mode_ == 8) {
        print_line(11, c.name() + text_[Text::NmSpace8] + msg + text_[Text::NmDot8]);
        flag = false;
    } else {
        const int r = 0x11;
        clear_cells(1, r, 0x26, 0x16);
        draw_char_name(c, 1, r + 1);
        print_in(1, r + 2, 0x26, 0x16, 0x0A, true, msg);
    }
    if (flag) {
        game_delay();   // Game.Delay 0653:0E3B
        clear_text();
    }
}

void Game::clear_text() {
    // ClearText ovl23 +0x0BA7: the combat panel's text rows in state 5;
    // otherwise the text window filled with its colour (0x56AD4).
    if (mode_ == 5) clear_cells(0x17, 0x0A, 0x26, 0x15);
    else fill_rect(left_, top_, right_, bottom_, fill_);
    msg_chars_ = 0;   // nothing left to read
}

void Game::combat_panel(Character& c) {
    // Panel ovl23 +0x04AC (0x563BC): only when DS:6BD9 (then cleared).
    if (!f6bd9_) return;
    f6bd9_ = 0;
    clear_cells(0x17, 1, 0x26, 0x15);
    name_message(c, text_[Text::CbSpace], 1, false);
    draw_string(0x17, 3, 0x0A, 0, text_[Text::CbPanelHp]);
    draw_hp(c, 0x21, 3, false, false);
    draw_string(0x17, 5, 0x0A, 0, text_[Text::CbAc]);
    draw_ac(c, 0x1A, 5);
    row_ = 5;   // DS:5F3F := 5
    if (c.weapon >= 0 && c.weapon < static_cast<int>(c.items.size()))
        print_in(0x17, 7, 0x26, 9, 0x0A, true, item_name(c.items[static_cast<std::size_t>(c.weapon)]));
    const int row = row_ + 1;
    std::string status;
    auto has = [&](int t) {
        for (const Bytes& e : c.effects)
            if (!e.empty() && e[0] == t) return true;
        return false;
    };
    if (c.b(0xD5) == 0) status = text_.ds_str(0x12B0 + 13 * static_cast<std::size_t>(c.b(0xD4)), 12);   // DS:12B0
    else if (has(1)) status = text_[Text::CbHelpless];
    else if (has(0x0D)) status = text_[Text::CbBlinded];
    else if (has(0x0E)) status = text_[Text::CbStunned];
    else if (has(2)) status = text_[Text::CbStatusIntim];
    if (!status.empty()) draw_string(0x17, row + 1, 15, 0, status);
    msg_chars_ = 0;   // the panel stays up: no message to wait for
}

}  // namespace cotd::game
