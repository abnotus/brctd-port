// SPDX-License-Identifier: GPL-3.0-or-later
// The adventure screen in VGA mode 13h, drawn as the original draws it
// (docs/notes/screen.md: text cells, frame, roster, status line, menus,
// pictures, the view area, and the palette buffers).
#include <algorithm>
#include <cmath>
#include <cctype>
#include <unordered_map>

#include "core/view3d.hpp"
#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr int kRows = 25, kCols = 40;

// Frame tables (data segment): (x1, y1, x2, y2, start tile, end tile).
struct Line { int x1, y1, x2, y2, start, end; };
constexpr Line kOuter[] = {{0, 0, 39, 0, 0, 1}, {0, 0, 0, 23, 0, 3}, {39, 0, 39, 23, 1, 2},
                           {0, 23, 39, 23, 3, 2}};                                // DS:0DDC
constexpr Line kInner[] = {{16, 0, 16, 16, 6, 8}, {2, 2, 14, 2, 0, 1}, {2, 14, 14, 14, 3, 2},
                           {2, 2, 2, 14, 0, 3},   {14, 2, 14, 14, 1, 2}};         // DS:0E10

bool in(int t, std::initializer_list<int> set) { return std::find(set.begin(), set.end(), t) != set.end(); }

bool menu_upper(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'); }

}  // namespace

// --- set-up -------------------------------------------------------------------

void Game::init_screen() {
    // Font 8X8D1 #201 into DS:5F32 (0x16739), independent of the area.
    if (auto f = dax_entry("8X8D1.DAX", 201)) font_ = decode_font(*f, "8X8D1.DAX #201");
    else throw Fatal("8X8D1.DAX: no font entry 201");
    // ovl9 init (0x3FBF7, 0x3FC02): tile set 6 = BORDERS #0, set 4 =
    // 8X8D1 #202, both with colour key 13; their palettes into DS:908B
    // (#202 second, so it wins for 16..31), copied to DS:8D8B, then DAC
    // 16..31 (0x3FC13, 0x3FC25).
    const auto base = base_palette();
    for (int i = 0; i < 16; ++i) pal_908b_[i] = base[i];
    auto load_set = [&](int slot, const char* file, int id) {
        auto raw = dax_entry(file, id);
        if (!raw) throw Fatal(text_[Text::WallUnable] + std::to_string(id));
        tile_sets_[slot] = std::make_unique<Image>(decode_generic(*raw, std::string(file) + " #" + std::to_string(id)));
        load_palette(*tile_sets_[slot]);
    };
    load_set(6, "BORDERS.DAX", 0);
    load_set(4, "8X8D1.DAX", 202);
    for (int i = 16; i < 32; ++i) pal_8d8b_[i] = pal_908b_[i];
    set_dac(16, 16, pal_908b_);
    // 0x3FC2A..0x3FC6B: COMSPR #1..#25 into the combat images (slots
    // 0x0D..0x25), then the working palette's 0x40..0x5F into DS:8A8B.
    for (int k = 1; k <= 0x19; ++k) load_combat_icon("COMSPR", k, k + 0x0C);
    for (int i = 0x40; i < 0x60; ++i) pal_8a8b_[static_cast<std::size_t>(i)] = pal_908b_[static_cast<std::size_t>(i)];
    // 0x3FC70..0x3FC8E: CURSOR #1 (the map cursor, key 13) into DS:8413.
    auto cur = dax_entry("CURSOR.DAX", 1);
    if (!cur) throw Fatal(text_[Text::WallUnable] + "1");
    cursor_img_ = std::make_unique<Image>(decode_generic(*cur, "CURSOR.DAX #1"));
    load_palette(*cursor_img_);
}

void Game::load_palette(const Image& im) {
    // Image loads write only DS:908B + first*3 (0x12811..0x12832).
    for (int i = 0; i < im.count && i < static_cast<int>(im.palette.size()); ++i)
        pal_908b_[(im.first + i) & 0xFF] = im.palette[static_cast<std::size_t>(i)];
}

void Game::set_dac(int first, int count, const std::array<Rgb, 256>& from) {
    // Gfx.SetDacRange 0267:286C: (first, count, buffer).
    for (int i = first; i < first + count && i < 256; ++i) screen_.set_dac(i, from[static_cast<std::size_t>(i)]);
}

// --- text cells -------------------------------------------------------------------

void Game::draw_char(int col, int row, int fg, int bg, unsigned char ch, bool map) {
    // DrawChar 0653:026B: glyph = UpCase(ch) mod 64 (TP UpCase: a..z only),
    // drawn opaque at (8 col, 8 row): set bits fg, clear bits bg (0x17D39).
    if (col < 0 || col > 0x27 || row < 0 || row > 0x18) return;
    unsigned g = ch;
    if (map) g = static_cast<unsigned>((ch >= 'a' && ch <= 'z') ? ch - 0x20 : ch) & 0x3F;
    if (g >= font_.frames.size()) return;
    const Bytes& px = font_.frames[g];
    for (int r = 0; r < 8; ++r) {
        std::uint8_t* line = screen_.row(row * 8 + r) + col * 8;
        for (int c = 0; c < 8; ++c) line[c] = static_cast<std::uint8_t>(px[r * 8 + c] ? fg : bg);
    }
}

void Game::draw_string(int col, int row, int fg, int bg, const std::string& s) {
    // DrawString 0653:0424: no wrap; DrawChar clips past column 0x27.
    if (col > 0x27 || row > 0x27) return;
    for (unsigned char c : s) draw_char(col++, row, fg, bg, c);
}

void Game::fill_cells(int col, int row, int colour, int n) {
    // FillCells 0653:03D9 (0x16909): DrawChar with a count. Only the first
    // cell is clipped (0x16911, 0x167A1); the count loop (0x168D9) draws on
    // at VGA offset (row * 320 + col) * 8 (0x17D4E), so a cell past column
    // 0x27 lands one scanline lower at the left edge (column 8-bit).
    if (col < 0 || col > 0x27 || row < 0 || row > 0x18) return;
    for (int i = 0; i < n; ++i) {
        const int base = (row * 320 + ((col + i) & 0xFF)) * 8;
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c) {
                const int o = base + r * 320 + c;
                if (o < 320 * 200) screen_.row(o / 320)[o % 320] = static_cast<std::uint8_t>(colour);
            }
    }
}




void Game::fill_rect(int x1, int y1, int x2, int y2, int colour) {
    // Text cells x1..x2, y1..y2 filled with one colour.
    for (int y = y1 * 8; y <= y2 * 8 + 7 && y < 200; ++y)
        for (int x = x1 * 8; x <= x2 * 8 + 7 && x < 320; ++x) screen_.row(y)[x] = static_cast<std::uint8_t>(colour);
}

void Game::clear_cells(int x1, int y1, int x2, int y2) { fill_rect(x1, y1, x2, y2, 0); }   // 015F:029C

void Game::clear_row24() { fill_rect(0, 0x18, 0x27, 0x18, 0); }   // 0653:1359 / ClearRect row 0x18

// --- frame --------------------------------------------------------------------------

void Game::border_tile(int t, int col, int row) {
    // View.DrawTile with id DS:256C + t: tile set 6 (BORDERS #0), opaque.
    if (col < 0 || col >= kCols || row < 0 || row >= kRows || !tile_sets_[6]) return;
    const Image& im = *tile_sets_[6];
    if (t < 0 || static_cast<std::size_t>(t) >= im.frames.size()) return;
    screen_.blit(im.frames[static_cast<std::size_t>(t)], 8, 8, col * 8, row * 8);
}

void Game::border_line(int x1, int y1, int x2, int y2, int start, int end) {
    // DrawBorderLine ovl33 +0x047E (0x6120E).
    const bool horiz = y1 == y2;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    int x = x1, y = y1;
    int n = horiz ? x2 - x1 + 1 : y2 - y1 + 1;
    // ovl33 +0x030A (0x6109A): one cell of DrawBorderLine, nested
    auto step = [&](int kind) {
        int t = kind == 2 ? start : kind == 3 ? end : kind == 1 ? (horiz ? 4 : 5) : -1;
        if (t >= 0) {
            border_tile(t, x, y);
            // Junction tiles (sets CS:028A/02AA/02CA/02EA).
            if (in(t, {0, 3, 6, 8, 9})) border_tile(10, x + 2, y);
            if (in(t, {1, 2, 6, 7, 8})) border_tile(10, x - 2, y);
            if (in(t, {0, 1, 6, 7, 9})) border_tile(11, x, y + 2);
            if (in(t, {2, 3, 7, 8, 9})) border_tile(11, x, y - 2);
        }
        if (horiz) ++x; else ++y;
        --n;
    };
    step(2);
    step(1);
    step(0);
    while (n > 3) step(1);
    if (n == 3) step(0);
    if (n == 2) step(1);
    if (n == 1) step(3);
}

void Game::layout_outer() {
    // Screen.OuterFrame 015F:00C6: the lines at DS:0DDC.
    for (const Line& l : kOuter) border_line(l.x1, l.y1, l.x2, l.y2, l.start, l.end);
}

void Game::layout_sheet() {
    // Screen.LayoutSheet 015F:0241: ClearCells(1,1,0x26,0x16), the outer
    // frame and the lines at DS:0E7C.
    clear_cells(1, 1, 38, 22);
    layout_outer();
    border_line(0, 8, 39, 8, 9, 7);
    border_line(0, 13, 39, 13, 9, 7);
    border_line(11, 13, 11, 23, 6, 8);
}

void Game::layout_bigpic() {
    // 015F:016F: clear the inside, the outer frame, the line above the text
    // window (HLine 015F:00FF: tiles 9..7), and the text window rectangle.
    clear_cells(1, 1, 38, 22);
    for (const Line& l : kOuter) border_line(l.x1, l.y1, l.x2, l.y2, l.start, l.end);
    border_line(0, 16, 39, 16, 9, 7);
    left_ = 1;
    top_ = 0x11;
    right_ = 0x26;
    bottom_ = 0x16;
}

void Game::layout_adventure() {
    layout_bigpic();   // 015F:014F
    for (const Line& l : kInner) border_line(l.x1, l.y1, l.x2, l.y2, l.start, l.end);
}

void Game::redraw_screen() {
    // ovl23 +0x26BE, by game state DS:3BE8. DS:6B70 := 1 first (0x585D4).
    f6b70_ = 1;
    // Then (0x585D9, main): roster
    // clicks allowed in the states with a roster (CS:269A = {0,1,4,6,10}).
    f8433_ = mode_ == 0 || mode_ == 1 || mode_ == 4 || mode_ == 6 || mode_ == 10;
    switch (mode_) {
        case 4:
            layout_adventure();
            draw_view_area();
            draw_roster();
            draw_status();
            first_area_ = 0;
            break;
        case 3:
            layout_bigpic();
            break;
        case 10:
            layout_adventure();
            draw_roster();
            break;
        case 1:
            // 0x58604 (main): the shop; its first Redraw keeps the screen the
            // script left (DS:82E8 = 0), then the picture's frame 0, the
            // roster, the status line.
            if (f82e8_) layout_adventure();
            draw_picture(0);
            draw_roster();
            draw_status();
            break;
        case 0:
            layout_state0();
            break;
        case 6:
            // 0x58687: after a fight, the victory picture (PIC 0x69 at
            // (3,3), its base frame) and the roster.
            layout_adventure();
            load_anim("PIC", 0x69);
            if (event_pic_)
                screen_.blit(event_pic_->frames[0], event_pic_->width,
                             event_pic_->height, 24, 24);
            draw_roster();
            break;
        case 8:
            // 0x586CC: LayoutSpace, MainView, the party's ship (space_combat.cpp).
            redraw_ship_screen();
            break;
        case 9:
            // 0x58702: the travel screen (ovl24 +0x01FE), the view area again,
            // the map again (the same pixels).
            travel_frame();
            draw_view_area();
            travel_map();
            break;
        default:
            // 0x58711: no case for the other states (2, 5, 7, 11 ...): Redraw
            // draws nothing (a fight's state 5, keys 5041).
            break;
    }
}

// --- roster and status line --------------------------------------------------------------

int Game::roster_colour(const Character& m) const {
    // Screen.RosterName ovl25 +0x22BE (0x5BDFE); never white.
    if (m.b(0xD5) == 0) return 12;
    if (m.b(0xD6) == 1 && mode_ == 5) return 14;
    if (m.b(0x29) < 8 && m.d(0x2F) > text_.ds32(0x31A2 + m.b(0x28) * 32 + (m.b(0x29) + 1) * 4))
        return 13;   // XP table DS:31A2 (docs/notes/levelup.md)
    return 11;
}

void Game::roster_line(const Character& m, int col, int r, int fg) {
    draw_string(col, r, fg, 0, m.name());
    // AC: shown 60 - raw (+0xD9), right-aligned at column 0x22 (0x5BED9).
    const int raw = m.b(0xD9);
    std::string ac = std::to_string(std::abs(raw - 60));
    if (raw > 60) ac = text_[Text::RosterMinus] + ac;
    const int accol = raw <= 50 ? 0x21 : raw <= 60 ? 0x22 : raw <= 69 ? 0x21 : 0x20;
    draw_string(accol, r, 15, 0, ac);
    // HP: right-aligned at column 0x26; yellow below maximum (0x5BF64).
    const int hp = m.w(0xE3);
    const int hpcol = hp <= 9 ? 0x26 : hp <= 99 ? 0x25 : 0x24;
    draw_string(hpcol, r, hp < m.w(0x45) ? 14 : 15, 0, std::to_string(hp));
}

void Game::draw_roster(const Character* white) {
    // ovl25 +0x20F9 (0x5BC39): the member its argument names in white
    // (0x5BCFC; CurChar, or SelectMember's choice).
    if (mode_ == 3 || mode_ == 9 || f3bed_) return;
    const int col = mode_ == 0 ? 1 : 0x11;
    set_roster_rect(col);                              // 0x5BC6B
    draw_string(col, 2, 15, 0, text_[Text::RosterName]);
    draw_string(0x21, 2, 15, 0, text_[Text::RosterAcHp]);
    int r = 4;
    for (const auto& m : party_) {
        clear_cells(col, r, 0x26, r);
        roster_line(*m, col, r, m.get() == white ? 15 : roster_colour(*m));
        ++r;
    }
    clear_cells(col, r, 0x26, r);
}

void Game::set_roster_rect(int col) {
    // The roster's click rectangle DS:6B3F..6B42: columns col..0x26, rows
    // 4..3 + members (both roster draws).
    roster_c0_ = static_cast<std::uint8_t>(col);
    roster_c1_ = 0x26;
    roster_r0_ = 4;
    roster_r1_ = static_cast<std::uint8_t>(3 + party_.size());
}

void Game::roster_row(const Character* c, bool white) {
    // Screen.RosterRow ovl23 +0x02DB (0x561EB): a row redrawn without a
    // clear, the name white (flag 1) or in its status colour; then the row
    // after the last member is cleared. Skipped in states 3 and 9 and
    // after a big picture. The row is DS:4EBD's, set to c only when nil
    // (0x56209) and cleared at the end (0x56374): after a roster click
    // (DS:4EBD := the old CurChar) the next RosterRow redraws the old
    // CurChar's row, whatever c is (route train_b).
    if (mode_ == 9 || mode_ == 3 || f3bed_) return;
    if (!prev_cur_) prev_cur_ = c;
    const int col = mode_ == 0 ? 1 : 0x11;
    set_roster_rect(col);                              // 0x56232
    int r = 4;
    for (const auto& m : party_) {
        if (m.get() == prev_cur_) roster_line(*m, col, r, white ? 15 : roster_colour(*m));
        ++r;
    }
    clear_cells(col, r, 0x26, r);
    prev_cur_ = nullptr;
}

void Game::draw_status() {
    // ovl23 +0x2820 (0x58730): "x,y DIR hh:mm" at (0x11, 15), green.
    if (mode_ == 3) return;
    const bool alt = v0(0x4CFD) != 0xFF;
    const int x = alt ? v0(0x4CFD) : static_cast<std::uint8_t>(x_);
    const int y = alt ? v0(0x4CFE) : static_cast<std::uint8_t>(y_);
    const int k = v0(0x4BE6) ? 2 : 1;
    const int d = (facing_ + k * v0(0x4CFF)) & 7;
    auto two = [&](int v) {
        std::string s = std::to_string(v);
        return s.size() < 2 ? text_[Text::StatusZero] + s : s;
    };
    std::string s = std::to_string(x) + text_[Text::StatusComma] + std::to_string(y) + text_[Text::StatusSpace] +
                    text_[static_cast<Text>(static_cast<int>(Text::Dir0) + d)] + text_[Text::StatusSpace] +
                    two(v0(0x4BC9)) + text_[Text::StatusColon] + two(10 * v0(0x4BC8) + v0(0x4BC7));
    if (debug_) s += text_[Text::StatusDebug];         // 0x58947: Ctrl+D's mark
    if (mode_ == 2) s += text_[Text::StatusCamping];
    else if (v1(0x7ECA) & 1) s += text_[Text::StatusSearch];
    if (s.size() > 40) s.resize(40);
    clear_cells(0x11, 15, 0x26, 15);
    draw_string(0x11, 15, 10, 0, s);
}

// --- the view area ----------------------------------------------------------------------

void Game::load_back(int id) {
    // ovl28 +0x0E1C: "BACK" + Str(DS:4898), id + 0x80 into DS:8486 and id
    // into DS:8482; palettes into DS:908B.
    const std::string file = ecl_file_name("BACK");
    back_far_.reset();
    back_near_.reset();
    if (auto r = dax_entry(file, id + 0x80)) {
        back_far_ = std::make_unique<Image>(decode_generic(*r, file + " #" + std::to_string(id + 0x80)));
        load_palette(*back_far_);
        flush_keys();                                  // Gfx.LoadImage's 0x12BED
    }
    if (auto r = dax_entry(file, id)) {
        back_near_ = std::make_unique<Image>(decode_generic(*r, file + " #" + std::to_string(id)));
        load_palette(*back_near_);
        flush_keys();                                  // Gfx.LoadImage's 0x12BED
    }
}

void Game::draw_view() {
    // View.Draw ovl28 +0x065E, composed at (16,16) in DS:3C7A and copied
    // to the screen at (24,24) (0267:183A).
    Bytes back(View3d::kSize * View3d::kSize, 0);
    std::vector<TileSet> sets;
    for (int k = 0; k < 7; ++k)
        sets.push_back({text_.ds16(0x2560 + 2 * k), text_.ds16(0x256E + 2 * k), tile_sets_[k].get(), 13});
    if (area_map_) {
        // 0x5D634: DS:6A2E set, the overhead map (ovl28 +0x0000) instead.
        View3d v(&geo_[0], &geo_[0x100], walls_, sets, back, block_id_);
        view_buf_ = v.area(x_, y_, facing_);
        screen_.blit(view_buf_, View3d::kSize, View3d::kSize, 24, 24);
        // A square off the map (D-0073): the party's tile falls outside
        // the window and the original draws it on the screen there, at
        // (24 + 8 col, 24 + 8 row) in the VGA's linear memory (run
        // s75_offmap_ff: x 255, on the window's left frame; past 64,000
        // it is off-screen memory: not drawn).
        const int ox = std::clamp(x_ - 5, 0, 5), oy = std::clamp(y_ - 5, 0, 5);
        const int col = x_ - ox, row = y_ - oy;
        if (col < 0 || col > 10 || row < 0 || row > 10) {
            const Bytes t = v.tile_pixels(0x110 + facing_ / 2);
            for (int r = 0; r < 8 && t.size() >= 64; ++r)
                for (int c = 0; c < 8; ++c) {
                    const long off = static_cast<long>(24 + 8 * row + r) * Framebuffer::kWidth + 24 + 8 * col + c;
                    if (off >= 0 && off < static_cast<long>(Framebuffer::kWidth) * Framebuffer::kHeight)
                        screen_.row(0)[off] = t[r * 8 + c];
                }
        }
        return;
    }
    const Image* bi = (map_byte_ & 0x80) ? back_near_.get() : back_far_.get();
    if (bi && !bi->frames.empty()) {
        back = bi->frames[0];   // opaque (0267:0D06)
    } else {
        // Background bands (ovl28 +0x017B, 0x5D14B): DS:6A2A, 6A2C, 6A2D.
        const std::uint8_t c6a2a = text_.ds8(0x0D86 + (map_byte_ > 0x7F ? v0(0x4BFE) : v0(0x4BFD)));
        // 0267:243A (0x14AAA): fill a rectangle of an image: x, w in cells, y, h in rows, the colour; dirty rows marked (VGA 0x14D0F)
        for (int y = 0; y < 44; ++y) std::fill_n(&back[y * 88], 88, c6a2a);
    }
    View3d v(&geo_[0], &geo_[0x100], walls_, sets, back, block_id_);
    view_buf_ = v.draw(x_, y_, facing_);
    screen_.blit(view_buf_, View3d::kSize, View3d::kSize, 24, 24);
}

void Game::draw_logbook_map(const LogbookMap& m) {
    // D-0083 (session 64): a map page of the Log Book, full screen, from
    // the game's own data, drawn as the book draws its plans (the owner's:
    // as close to the original as we can): paper floor, navy walls and
    // marks, dark navy round the deck. One square on the page is one GEO square,
    // one step of the party; a faint dot at each floor corner counts them.
    // The floor is what the party could walk to from the start square (the
    // game's own passability, ovl28 +0x03B1); a wall it passes is a door,
    // drawn as the book's small rectangle; a wall type the block names
    // gets its mark instead (SYMBOL: ~ squiggle, # airlock box, or a
    // letter). The book's words from the file in the game's font. The map
    // as large as the screen allows, the title on the top row, the key
    // under it; the caller waits and puts the screen back.
    auto g = dax_entry(m.geo, m.entry);
    if (!g || g->size() != 0x402) {
        combat::diag("D-0083", m.geo + " #" + std::to_string(m.entry) + ": no such GEO entry; the map is left out");
        return;
    }
    // The page's own colours in DAC slots no screen of the Log Book uses;
    // the caller's saved screen (its DAC too) puts the game's back.
    constexpr std::uint8_t kFloor = 0xF8, kInk = 0xF9, kHull = 0xFA, kDots = 0xFB;
    // The book's colours, softened (the owner's: white glares, where the
    // game itself rarely shows it): paper floor, navy ink, a dark navy
    // hull (the owner: darker than the book's pale blue). 6-bit DAC
    // values; the other schemes tried: STATUS session 64.
    screen_.set_dac(kFloor, Rgb{50, 48, 42});
    screen_.set_dac(kInk, Rgb{6, 10, 36});
    screen_.set_dac(kHull, Rgb{14, 18, 30});
    screen_.set_dac(kDots, Rgb{40, 40, 37});
    const std::uint8_t* p0 = g->data() + 2;           // plane 0: N (high nibble), E (low)
    const std::uint8_t* p1 = g->data() + 2 + 0x100;   // plane 1: S, W
    const std::uint8_t* p3 = g->data() + 2 + 0x300;   // plane 3: each side's passability, 2 bits
    const int w = m.x1 - m.x0 + 1, h = m.y1 - m.y0 + 1;
    // The key wrapped between its items (after ", " or "; "), so a letter
    // stays on the line with what it means.
    std::vector<std::string> key;
    for (const std::string& k : m.key) {
        std::vector<std::string> items;
        for (std::size_t at = 0; at < k.size();) {
            std::size_t cut = k.size();
            for (const char* sep : {", ", "; "}) {
                const std::size_t c = k.find(sep, at);
                if (c != std::string::npos) cut = std::min(cut, c + 1);
            }
            items.push_back(k.substr(at, cut - at));
            at = cut;
            while (at < k.size() && k[at] == ' ') ++at;
        }
        std::string cur;
        for (const std::string& it : items) {
            if (!cur.empty() && cur.size() + 1 + it.size() > 40) {
                key.push_back(cur);
                cur.clear();
            }
            cur += (cur.empty() ? "" : " ") + it;
            while (cur.size() > 40) {
                std::size_t sp = cur.rfind(' ', 40);
                if (sp == std::string::npos || sp == 0) sp = 40;
                key.push_back(cur.substr(0, sp));
                cur = cur.substr(sp + (cur[sp] == ' ' ? 1 : 0));
            }
        }
        if (!cur.empty()) key.push_back(cur);
    }
    const int avail = 200 - 8 * (1 + static_cast<int>(key.size()) + 2);   // the title, the key, a blank row, the prompt
    // A square's side as large as the screen allows, with half a square
    // of hull round the deck for the marks on its outer walls (airlocks,
    // the weapons' barrels).
    int S = std::max(8, std::min(320 / (w + 1), avail / (h + 1)));
    const int gz = std::max(1, (S * 3 / 4 - 2) / 8);   // the labels' glyph scale, clear of the door marks
    int t = std::max(1, S / 8 - 1);            // the key's icons draw at their own S and t
    const int M = S / 2, x0 = (320 - w * S) / 2, y0 = 8 + M + (avail - h * S - 2 * M) / 2;
    clear_cells(0, 0, 0x27, 0x18);
    for (std::size_t i = 0; i < m.title.size() && i < 40; ++i)
        draw_char(static_cast<int>((40 - std::min<std::size_t>(m.title.size(), 40)) / 2 + i), 0, 15, 0,
                  static_cast<unsigned char>(m.title[i]));
    // Everything of the map stays inside its rectangle and hull margin (a barrel past the
    // hull doesn't reach the key's rows).
    int cx0 = x0 - M, cy0 = y0 - M, cx1 = x0 + w * S + M, cy1 = y0 + h * S + M;   // the clip; a key icon's cell later
    auto put = [&](int px, int py, std::uint8_t c) {
        if (px >= cx0 && px < cx1 && py >= cy0 && py < cy1) screen_.row(py)[px] = c;
    };
    auto rect = [&](int rx, int ry, int rw, int rh, std::uint8_t c) {
        for (int py = ry; py < ry + rh; ++py)
            for (int px = rx; px < rx + rw; ++px) put(px, py, c);
    };
    auto type = [&](int x, int y, int side) {   // side 0 N, 1 E, 2 S, 3 W
        if (x < 0 || x > 15 || y < 0 || y > 15) return 0;
        const int i = y * 16 + x;
        return side == 0 ? p0[i] >> 4 : side == 1 ? p0[i] & 0xF : side == 2 ? p1[i] >> 4 : p1[i] & 0xF;
    };
    auto passes = [&](int x, int y, int side) {
        if (x < 0 || x > 15 || y < 0 || y > 15) return false;
        return type(x, y, side) == 0 || ((p3[y * 16 + x] >> (side * 2)) & 3) == 1;
    };
    // The floor: flooded from the start square, past the parts the book
    // leaves blank (HIDE).
    std::vector<char> floor(256, 0);
    {
        const int sx = m.fx >= 0 ? m.fx : (m.x0 + m.x1) / 2, sy = m.fy >= 0 ? m.fy : (m.y0 + m.y1) / 2;
        std::vector<std::pair<int, int>> todo{{sx, sy}};
        static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};
        while (!todo.empty()) {
            auto [x, y] = todo.back();
            todo.pop_back();
            if (x < m.x0 || x > m.x1 || y < m.y0 || y > m.y1 || floor[static_cast<std::size_t>(y * 16 + x)]) continue;
            bool hid = false;
            for (const auto& hd : m.hidden) hid = hid || (x >= hd.x0 && x <= hd.x1 && y >= hd.y0 && y <= hd.y1);
            if (hid) continue;
            floor[static_cast<std::size_t>(y * 16 + x)] = 1;
            for (int d = 0; d < 4; ++d)
                if (passes(x, y, d)) todo.push_back({x + DX[d], y + DY[d]});
        }
    }
    auto inside = [&](int x, int y) {
        return x >= m.x0 && x <= m.x1 && y >= m.y0 && y <= m.y1 && floor[static_cast<std::size_t>(y * 16 + x)];
    };
    auto sx_of = [&](double x) { return static_cast<int>(std::lround(x0 + (x - m.x0) * S)); };
    auto sy_of = [&](double y) { return static_cast<int>(std::lround(y0 + (y - m.y0) * S)); };
    // The hull, the floor, the step dots.
    rect(x0 - M, y0 - M, w * S + 2 * M, h * S + 2 * M, kHull);
    for (int y = m.y0; y <= m.y1; ++y)
        for (int x = m.x0; x <= m.x1; ++x)
            if (inside(x, y)) rect(sx_of(x), sy_of(y), S, S, kFloor);
    for (int y = m.y0; y <= m.y1 + 1; ++y)
        for (int x = m.x0; x <= m.x1 + 1; ++x)
            if (inside(x, y) && inside(x - 1, y) && inside(x, y - 1) && inside(x - 1, y - 1)) put(sx_of(x), sy_of(y), kDots);
    auto mark_of = [&](int ty) -> std::string {
        for (const auto& sym : m.symbols)
            if (sym.type == ty) return sym.text;
        return "";
    };
    // A glyph of the game's font, scaled, ink on floor.
    auto glyph = [&](unsigned char ch, int gx, int gy, int sc) {
        const unsigned g8 = static_cast<unsigned>(std::toupper(ch)) & 0x3F;
        if (g8 >= font_.frames.size()) return;
        const Bytes& px = font_.frames[g8];
        for (int r = 0; r < 8 * sc; ++r)
            for (int c = 0; c < 8 * sc; ++c) put(gx + c, gy + r, px[(r / sc) * 8 + c / sc] ? kInk : kFloor);
    };
    // Each edge beside the floor once: its wall, then its mark.
    struct Edge { int x, y; bool horizontal; int ty; bool door; };
    std::vector<Edge> edges;
    for (int y = m.y0; y <= m.y1 + 1; ++y)
        for (int x = m.x0; x <= m.x1; ++x) {   // the N edge of (x, y)
            const int a1 = y <= m.y1 ? type(x, y, 0) : 0, a2 = y - 1 >= m.y0 ? type(x, y - 1, 2) : 0;
            if ((a1 || a2) && (inside(x, y) || inside(x, y - 1)))
                edges.push_back({x, y, true, a1 ? a1 : a2, (a1 && passes(x, y, 0)) || (a2 && passes(x, y - 1, 2))});
        }
    for (int y = m.y0; y <= m.y1; ++y)
        for (int x = m.x0; x <= m.x1 + 1; ++x) {   // the W edge of (x, y)
            const int a1 = x <= m.x1 ? type(x, y, 3) : 0, a2 = x - 1 >= m.x0 ? type(x - 1, y, 1) : 0;
            if ((a1 || a2) && (inside(x, y) || inside(x - 1, y)))
                edges.push_back({x, y, false, a1 ? a1 : a2, (a1 && passes(x, y, 3)) || (a2 && passes(x - 1, y, 1))});
        }
    for (const Edge& e : edges) {
        const int ex = sx_of(e.x), ey = sy_of(e.y);
        if (e.horizontal) rect(ex, ey - t / 2, S + t / 2 + 1, t, kInk);
        else rect(ex - t / 2, ey, t, S + t / 2 + 1, kInk);
    }
    // A wall's mark centred at (cx, cy), the wall along x when horizontal:
    // the map's edges and the key's icons alike.
    auto edge_mark = [&](int cx, int cy, bool horizontal, const std::string& mk, bool door) {
        // A box across the wall, long L along it, deep D across it.
        auto across = [&](int L, int D, bool bar) {
            const int bx = horizontal ? cx - L / 2 : cx - D / 2, by = horizontal ? cy - D / 2 : cy - L / 2;
            const int bw = horizontal ? L : D, bh = horizontal ? D : L;
            rect(bx, by, bw, bh, kInk);
            rect(bx + t, by + t, bw - 2 * t, bh - 2 * t, kFloor);
            if (bar) {   // the airlock's inner bar
                if (horizontal) rect(cx - L / 4, cy - t / 2, L / 2, t, kInk);
                else rect(cx - t / 2, cy - L / 4, t, L / 2, kInk);
            }
        };
        if (mk == "#") {
            across(S * 3 / 5, S * 2 / 5, true);
        } else if (mk == "~") {   // the book's: a gap in the wall, two wavy lines along it
            const int L = S / 2, dd = std::max(2, S / 8), amp = std::max(1, S / 16);
            if (horizontal) rect(cx - L / 2, cy - t / 2 - 1, L, t + 2, kFloor);
            else rect(cx - t / 2 - 1, cy - L / 2, t + 2, L, kFloor);
            for (int k = -1; k <= 1; k += 2)
                for (int u = -L / 2; u <= L / 2; ++u) {
                    const int v = k * dd + static_cast<int>(std::lround(amp * std::sin(u * 2 * 3.14159265 / (L / 2.0))));
                    const int sz = std::max(1, t * 2 / 3);
                    if (horizontal) rect(cx + u, cy + v - sz / 2, 1, sz, kInk);
                    else rect(cx + v - sz / 2, cy + u, sz, 1, kInk);
                }
        } else if (!mk.empty()) {
            glyph(static_cast<unsigned char>(mk[0]), cx - 4, cy - 4, 1);
        } else if (door) {
            across(S * 2 / 5, S / 4, false);   // the book's door
        }
    };
    for (const Edge& e : edges) {
        const int ex = sx_of(e.x), ey = sy_of(e.y);
        edge_mark(e.horizontal ? ex + S / 2 : ex, e.horizontal ? ey : ey + S / 2, e.horizontal, mark_of(e.ty), e.door);
    }
    // The marks the file places: dotted controls, arrows, weapon mounts,
    // the connecting tube, the rooms' letters.
    auto dot = [&](int px, int py) { rect(px - t / 2, py - t / 2, t, t, kInk); };
    auto seg = [&](double ax, double ay, double bx, double by) {
        const int n = static_cast<int>(std::max(std::abs(bx - ax), std::abs(by - ay))) + 1;
        for (int k = 0; k <= n; ++k)
            dot(static_cast<int>(std::lround(ax + (bx - ax) * k / n)), static_cast<int>(std::lround(ay + (by - ay) * k / n)));
    };
    for (const auto& ar : m.arrows) {
        const double ax = x0 + (ar.x1 - m.x0) * S, ay = y0 + (ar.y1 - m.y0) * S;
        const double bx = x0 + (ar.x2 - m.x0) * S, by = y0 + (ar.y2 - m.y0) * S;
        if (ar.dots) {
            const double len = std::hypot(bx - ax, by - ay);
            const int n = std::max(1, static_cast<int>(len / (S / 4.0)));
            for (int k = 0; k <= n; ++k) dot(static_cast<int>(std::lround(ax + (bx - ax) * k / n)), static_cast<int>(std::lround(ay + (by - ay) * k / n)));
            continue;
        }
        seg(ax, ay, bx, by);
        const double len = std::hypot(bx - ax, by - ay);
        if (len < 1) continue;
        const double ux = (bx - ax) / len, uy = (by - ay) / len, hl = S * 0.35;
        seg(bx, by, bx - hl * (ux + uy), by - hl * (uy - ux));
        seg(bx, by, bx - hl * (ux - uy), by - hl * (uy + ux));
    }
    // A weapon: a dark disc, two barrels with heads, outwards.
    auto gun_at = [&](double cx, double cy, char dir) {
        const double r = S * 0.34;
        for (int py = static_cast<int>(cy - r); py <= static_cast<int>(cy + r); ++py)
            for (int px = static_cast<int>(cx - r); px <= static_cast<int>(cx + r); ++px)
                if ((px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r) put(px, py, kInk);
        const int ux = dir == 'E' ? 1 : dir == 'W' ? -1 : 0, uy = dir == 'S' ? 1 : dir == 'N' ? -1 : 0;
        for (const double off : {-S * 0.1, S * 0.1}) {
            const double bx = cx - uy * off + ux * r, by = cy + ux * off + uy * r;
            const double ex = cx - uy * off + ux * S * 0.75, ey = cy + ux * off + uy * S * 0.75;
            seg(bx, by, ex, ey);
            const double hl = S * 0.08;
            seg(ex, ey, ex - hl * (ux + uy), ey - hl * (uy - ux));
            seg(ex, ey, ex - hl * (ux - uy), ey - hl * (uy + ux));
        }
    };
    // The connecting tube: a dotted circle with T (the font's, or three
    // strokes where a glyph doesn't fit).
    auto tube_at = [&](double cx, double cy) {
        const double r = S * 0.36;
        for (int k = 0; k < 360; k += 3)
            dot(static_cast<int>(std::lround(cx + r * std::cos(k * 3.14159265 / 180))),
                static_cast<int>(std::lround(cy + r * std::sin(k * 3.14159265 / 180))));
        if (S >= 16) {
            glyph('T', static_cast<int>(cx) - 4, static_cast<int>(cy) - 4, 1);
        } else {
            const int x = static_cast<int>(cx), y = static_cast<int>(cy);
            for (int i = -1; i <= 1; ++i) put(x + i, y - 1, kInk);
            put(x, y, kInk);
            put(x, y + 1, kInk);
        }
    };
    for (const auto& gun : m.guns) gun_at(x0 + (gun.x - m.x0 + 0.5) * S, y0 + (gun.y - m.y0 + 0.5) * S, gun.dir);
    for (const auto& [tx, ty] : m.tubes) tube_at(x0 + (tx - m.x0 + 0.5) * S, y0 + (ty - m.y0 + 0.5) * S);
    for (const auto& l : m.labels)
        for (std::size_t i = 0; i < l.text.size(); ++i)
            glyph(static_cast<unsigned char>(l.text[i]), sx_of(l.x + static_cast<int>(i)) + (S - 8 * gz) / 2,
                  sy_of(l.y) + (S - 8 * gz) / 2, gz);
    // The key, in the game's green; "@X" pictures the map's mark X in two
    // cells, drawn by the map's own code on a patch of floor, as the book's
    // key pictures its marks: D a door, A an airlock, F a force-field door
    // (each across a wall), T the connecting tube, W a weapon, . the
    // controls.
    auto icon = [&](char x, int col, int row) {
        const int X = col * 8, Y = row * 8;
        cx0 = X; cy0 = Y; cx1 = X + 16; cy1 = Y + 8;
        S = 16; t = 1;
        rect(X, Y, 16, 8, kFloor);
        switch (x) {
            case 'D': case 'A': case 'F':
                S = 20;   // as large as the cell's 8 rows allow
                rect(X, Y + 4, 16, 1, kInk);
                edge_mark(X + 8, Y + 4, true, x == 'A' ? "#" : x == 'F' ? "~" : "", true);
                break;
            case 'T': S = 10; tube_at(X + 8, Y + 4); break;
            case 'W': S = 8; gun_at(X + 5, Y + 4, 'E'); break;
            case '.': for (int i = 1; i < 16; i += 3) put(X + i, Y + 4, kInk); break;
            default: break;
        }
    };
    int row = 0x17 - static_cast<int>(key.size());   // the key ends a row above the prompt
    for (const std::string& k : key) {
        if (row > 0x17) break;
        int col = 0;
        for (std::size_t i = 0; i < k.size() && col < 40; ++i, ++col) {
            if (k[i] == '@' && i + 1 < k.size()) {
                icon(k[++i], col, row);
                ++col;
                continue;
            }
            draw_char(col, row, 10, 0, static_cast<unsigned char>(k[i]));
        }
        ++row;
    }
}

void Game::draw_view_area() {
    // ovl26 +0x00A6 (0x5C0E6).
    if (party_lost_) return;
    const std::uint8_t area = v0(0x4BE6);
    if (area == 0xA2 || area == 0xA8) {                // 0x5C0FF: the planet map, any state
        if (f6b70_) draw_big_pic();                    // no cursor
        f6b70_ = 0;
        return;
    }
    if (area == 0x70) {                                // 0x5C116: the travel hub, any state
        draw_picture(0);
        f6b70_ = 0;
        return;
    }
    if (mode_ != 4) return;
    map_byte_ = map_byte(x_, y_);
    if (f959b_) {
        set_dac(0x20, 0x94, pal_8d8b_);
        f959b_ = 0;
    }
    if (v0(0x4BFB) != 0 || v0(0x4CFD) != 0xFF) area_map_ = 0;
    draw_view();
    f6b70_ = 0;
}

void Game::show_picture(int p) {
    // PICTURE helper ovl2 0x31125.
    if (p == 0xFF) {
        // 0x311A8: redraw the view if a picture covered it (DS:82F7/82F9),
        // unless the state just left 4; then DS:82DC := 0, DS:82DD := 0
        // (0x311DD) either way.
        if (!(prev_mode_ == 4 && mode_ != 4) && (f82f7_ || f82f9_)) {
            f959b_ = 1;
            f6b70_ = 1;
            draw_view_area();
            f82f7_ = 0;
            f82f9_ = 0;
        }
        f82dc_ = 0;
        f82dd_ = 0;
        return;
    }
    f959b_ = 1;   // 0x31131: any picture; DS:82F7 := 1 (the view is covered)
    f82f7_ = 1;
    f82dd_ = 1;   // 0x31136
    if (p < 0x70) {
        load_anim("PIC", p);
        // DrawPicture ovl27 +0x0023 (3, 3, 1, frame 0): frame 0 (not the
        // base frame DS:6A60 when that is another: PIC1 #65, route base_to_ship).
        draw_picture(0);
    } else {
        load_big_pic(p);                               // 0x3114A
        draw_big_pic();
        f959b_ = 1;                                    // 0x31154
        if (mode_ == 3) draw_map_cursor();             // 0x31160: not DS:3BED in state 3
        else f3bed_ = 1;
        f6b70_ = 0;
    }
}

void Game::load_big_pic(int p) {
    // LoadBigPic ovl27 +0x0CB6 (0x5CEE6): the event picture always freed;
    // the same picture still in DS:6B10 only sets the DAC from DS:848B.
    free_picture();
    if (f6b14_ == p && big_pic_) {
        set_dac(0x20, 0xE0, pal_848b_);
        return;
    }
    fill_rect(0x15, 0x18, 0x27, 0x18, 0);              // Text.LoadingLine 0653:12D2
    draw_string(0, 0x18, 10, 0, text_[Text::Loading]);
    const std::string file = ecl_file_name("BIGPIC");
    auto raw = dax_entry(file, p);
    if (!raw) {
        // Gfx.LoadImage with no such entry (Dax.Load's size 0, 0x12BAA):
        // the old image freed, nothing loaded, no flush; the rest goes on.
        // LoadSavedGame on Mars asks for 0x47, which no BIGPIC holds
        // (route c_mars_load).
        big_pic_.reset();
    } else {
        big_pic_ = std::make_unique<Image>(decode_generic(*raw, file + " #" + std::to_string(p)));
        load_palette(*big_pic_);
        flush_keys();                                  // Gfx.LoadImage's 0x12BED
    }
    f6b14_ = static_cast<std::uint8_t>(p);
    std::copy(pal_908b_.begin() + 0x20, pal_908b_.end(), pal_848b_.begin() + 0x20);   // 0x5CF75
    set_dac(0x20, 0xE0, pal_908b_);
    clear_row24();                                     // 0653:1359 (0x5CF8C)
}

void Game::draw_big_pic() {
    // DrawBigPic ovl27 +0x0D67 (0x5CF97): LayoutBigPic, then DS:6B10's
    // frame 0 opaque at (8, 8). With DS:6B10 nil: the layout only
    // (inference: Gfx.DrawImage's nil test not read).
    layout_bigpic();
    if (big_pic_ && !big_pic_->frames.empty())
        screen_.blit(big_pic_->frames[0], big_pic_->width, big_pic_->height, 8, 8);
}

void Game::draw_map_cursor() {
    // ovl16 +0x0000 (0x4D5F0): with DS:6B15 set, the screen cell at (var
    // 0x4BC3 + 1, var 0x4BC4 + 1) saved into DS:5F7A, CURSOR keyed (13)
    // over a copy of it, the copy drawn; then Present's dirty rows reset
    // (0x4D699).
    if (!map_mode_) return;
    const int x = (v0(0x4BC3) + 1) * 8, y = (v0(0x4BC4) + 1) * 8;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            cursor_under_[static_cast<std::size_t>(r * 8 + c)] =
                (y + r < 200 && x + c < 320) ? screen_.row(y + r)[x + c] : 0;
    if (cursor_img_ && !cursor_img_->frames.empty())
        screen_.blit(cursor_img_->frames[0], cursor_img_->width, cursor_img_->height, x, y, std::uint8_t{13});
    bb_dirty_.fill(false);
}

void Game::restore_map_cursor() {
    // ovl16 +0x00E5 (0x4D6D5): DS:5F7A back at the vars' *current* cell.
    if (!map_mode_) return;
    screen_.blit(cursor_under_, 8, 8, (v0(0x4BC3) + 1) * 8, (v0(0x4BC4) + 1) * 8);
}

void Game::free_picture() {
    // ovl27 +0x0B04 (0x5CD34): the picture's (or sprite's) frames freed,
    // then the last load forgotten (0x5CE30: DS:6AFC := '', DS:6B0E :=
    // 0xFF), so the next load of the same picture loads and draws it.
    event_pic_.reset();
    sprite_frames_.clear();
    anim_name_.clear();
    anim_id_ = 0xFF;
}

void Game::draw_picture(int k) {
    // DrawPicture ovl27 +0x0023 (3, 3, 1, k): nothing when frame k isn't
    // loaded (0x5C273), the band first in state 9 (0x5C29B), then the
    // frame opaque at cell (3, 3), straight to the screen.
    if (!event_pic_ || k < 0 || k >= static_cast<int>(event_pic_->frames.size())) return;
    if (mode_ == 9) draw_band();
    screen_.blit(event_pic_->frames[static_cast<std::size_t>(k)], event_pic_->width, event_pic_->height, 24, 24);
}

void Game::load_anim(const std::string& name, int id) {
    // LoadAnimPicture ovl27 +0x02D0 (docs/notes/encounter.md).
    if (id == 0xFF) return;
    if (name == anim_name_ && id == anim_id_) {
        // The same as the last load: only the DAC is set.
        if (anim_sprite_) set_dac(0xB0, 0x50, pal_878b_);
        else if (mode_ != 9) set_dac(0x20, 0xE0, pal_878b_);
        return;
    }
    // 0x5C574: DS:6B10 freed unless the state is 9 (the travel map).
    if (mode_ != 9) big_pic_.reset();
    // 0653:12D2: FillRect(0x15, 0x18, 0x27, 0x18, 0), "Loading...Please
    // Wait" at (0, 0x18) in green.
    fill_rect(0x15, 0x18, 0x27, 0x18, 0);
    draw_string(0, 0x18, 10, 0, text_[Text::Loading]);
    // 0x5CD34: the previous frames are freed (FillChar also nils DS:6A8C[]).
    event_pic_.reset();
    sprite_frames_.clear();
    anim_name_ = name;
    anim_sprite_ = !name.empty() && (name[0] == 'S' || name[0] == 's');
    anim_id_ = id;
    // The file: name + the area digit; PIC has fixed files for some ids
    // (CS:02B7..02BB).
    char digit = static_cast<char>('0' + ecl_file_);
    if (name == "PIC") {
        if (id == 'i' || id == '@') digit = '7';
        else if (id == 'H' || id == '\\' || id == 'J') digit = '8';
        else if (id == '>') digit = '9';
    }
    const std::string file = name + digit + ".DAX";
    auto raw = dax_entry(file, id);
    // 0x5C5A3: FillChar(DS:6A45, 0x1F, 0): the frames and the current frame, DS:6A63.
    anim_cur_ = anim_loop_ = anim_last_ = 0;
    anim_delays_.clear();
    if (!raw) {
        // 0x5C69D: the message and a key, then the game plays on with no picture
        // (route s77_pic_missing; a LOAD in a script's team menu reaches it, D-0081).
        press_continue(0x0E, 0, text_[Text::PicNotFound]);
        return;
    }
    auto im = std::make_unique<Image>(decode_anim(*raw, file + " #" + std::to_string(id)));
    // DS:6A61 / 6A62 / 6A45..: they stay after the picture is freed (only
    // the frame pointers are cleared, 0x5CDE5).
    anim_loop_ = im->loop;
    anim_last_ = static_cast<int>(im->frames.size()) - 1;
    anim_delays_ = im->delays;
    load_palette(*im);
    if (anim_sprite_) {
        for (const Bytes& f : im->frames) sprite_frames_.push_back(sprite_view(f));
        // CopyPaletteRange(0xB0, 0x50, DS:878B), SetDacRange(0xB0, 0x50).
        // Gfx.CopyPaletteRange 0267:2829 (0x14E99): VGA: DS:908B[first..] to dest[first..]; inlined at each call
        std::copy(pal_908b_.begin() + 0xB0, pal_908b_.end(), pal_878b_.begin() + 0xB0);
        set_dac(0xB0, 0x50, pal_878b_);
    } else if (mode_ == 9) {
        // 0x5CCDF: in state 9 only 0x40..0xFF (the map keeps 0x20..0x3F).
        std::copy(pal_908b_.begin() + 0x40, pal_908b_.end(), pal_878b_.begin() + 0x40);
        set_dac(0x40, 0xC0, pal_878b_);
        event_pic_ = std::move(im);
    } else {
        pal_878b_ = pal_908b_;
        set_dac(0x20, 0xE0, pal_908b_);
        event_pic_ = std::move(im);
    }

}

int Game::steps_ahead(int x, int y, int dir, std::uint32_t site) {
    // StepsAhead ovl15 +0x057D (0x4BB5D): free squares toward dir, at most
    // 5, testing the wall on the dir side of each square; x/y aren't
    // wrapped (WallAt clamps them). With var 0x4BE6 = 0: 2, and var
    // 0x7EC1 := 2.
    if (v0(0x4BE6) == 0) {
        v1(0x7EC1) = 2;
        return 2;
    }
    // D-0071: WallAt (0x5D4B0..0x5D5AA) sets its result only for 0/2/4/6
    // (and off the map in blocks 0 and 0x50); for a diagonal facing it
    // returns its stale [BP-1], the same byte at every turn of the loop, so
    // the count is 0 (a wall) or 5. The byte is what was left at that depth
    // before: the handler's BP-31 in SPRITE_START, BP-39 in COMBAT (every
    // handler has the same BP, Dispatch 0x3361C). A diagonal facing arises
    // only on the wild maps (state 3: ECL4 #64 Mars, ECL5 #80 Venus; the
    // blocks they lead to set the facing first), so every site is here,
    // by file << 24 | block << 16 | the instruction's address; true: a wall.
    // How each is known (docs/notes/encounter.md "D-0071's sites"):
    //  - a byte-variable operand of the SPRITE_START itself: ReadVar's
    //    [BP-5] (0x4C3D6) is the slot, the variable's region (98B9: 1,
    //    9A05: 0);
    //  - after SAVE: SetVar's last push at BP-12 (Move's destination,
    //    region 0: (var - 0x4B00) >> 8 for byte[0x4C06]: 1; region 1: 0);
    //  - after ADD (0x30997): the high byte of its call's return CS;
    //  - COMBAT after ADD_MONSTERS: +0xF0 of the monster record it copied
    //    to BP-0x117, 0 in every MON0CHA record;
    //  - after WAIT, SET_MODE, PRINT_CLEAR (deep calls): measured, var
    //    0x7EC1 at the fight's first screen, runs s45_d0071_*, s46_d0071_*.
    // A site not here (none in the shipped scripts) reads "no wall" with a
    // diagnostic naming it.
    if (dir & 1) {
        const bool off = !(0 <= x && x < 16 && 0 <= y && y < 16);
        if (off && (block_id_ == 0 || block_id_ == 0x50)) return 5;   // WallAt's 0; x/y don't move
        static const std::pair<std::uint32_t, bool> kSites[] = {
            // ECL4 #64, Mars. SPRITE_START:
            {0x044097CDu, false},   // after SAVE to 0x7EC6 (region 1); measured s45_d0071_mem
            {0x04409856u, true},    // after PRINT_CLEAR; measured s45_d0071_herd
            {0x044098B9u, true},    // its operand byte[0x7F7B] (region 1); s46_d0071_m98B9
            {0x04409A05u, false},   // its last operand byte[0x4C0A] (region 0); s46_d0071_m9A05
            // COMBAT after ADD_MONSTERS (8A17 follows TREASURE: no fight, no clamp):
            {0x044097DFu, false}, {0x04409868u, false}, {0x044098CCu, false}, {0x04409A26u, false},
            // ECL5 #80, Venus. SPRITE_START:
            {0x055084FEu, true},    // after ADD; s46_d0071_v84FE
            {0x0550857Bu, true},    // after SAVE to 0x4C06 (region 0, 0x106); s46_d0071_v857B
            {0x0550865Du, true},    // the same; s46_d0071_v865D
            {0x055088F2u, true},    // the same; s46_d0071_v88F2
            {0x05508FCFu, false},   // after WAIT; measured s46_d0071_v8FCF
            {0x0550926Eu, false},   // after WAIT; measured s46_d0071_v926E
            {0x05509753u, true},    // after SET_MODE 4; measured s46_d0071_v9753
            {0x05509BA7u, false},   // after PRINT_CLEAR; measured s46_d0071_v9BA7
            // COMBAT after ADD_MONSTERS:
            {0x05508522u, false}, {0x05508593u, false}, {0x05508903u, false}, {0x05508FE0u, false},
            {0x0550927Fu, false}, {0x05509772u, false}, {0x05509BC3u, false},
            // COMBAT after IF_EQ or the sub's RETURN: moot, only after 865D,
            // which leaves var 0x7EC1 at 0 for the clamp to keep
            // (tests/test_d0071.py test_8679_only_after_865D).
            {0x05508679u, false},
        };
        const auto* m = std::find_if(std::begin(kSites), std::end(kSites),
                                     [&](const auto& e) { return e.first == site; });
        if (m != std::end(kSites)) return m->second ? 0 : 5;
        char where[48];
        std::snprintf(where, sizeof where, "ECL%u #%u 0x%04X", site >> 24, (site >> 16) & 0xFF, site & 0xFFFF);
        combat::diag("D-0071", std::string("GAME.OVR ovl15 +0x057D (0x4BB5D), StepsAhead: facing ") +
                                   std::to_string(dir) + " at " + where +
                                   ": View.WallAt's result is a stale stack byte, unmeasured here: no wall");
        return 5;
    }
    int n = 0;
    while (n < 5 && wall_at(dir, y, x) == 0) {
        ++n;
        if (dir == 0) --y;
        else if (dir == 2) ++x;
        else if (dir == 4) ++y;
        else if (dir == 6) --x;
    }
    return n;
}

void Game::show_sprite(int frame) {
    // Show3DSprite ovl27 +0x0C2D (0x5CE5D): frame 1..3 (distance + 1) is
    // DS:6A8C[frame - 1], masked (key 0x0D) into the backbuffer's view
    // square; the flush copies the whole square to the screen at (24,24).
    if (frame < 1 || frame > 3) throw Fatal(text_[Text::SpriteRange]);
    const std::size_t k = static_cast<std::size_t>(frame - 1);
    if (k >= sprite_frames_.size()) return;
    const Bytes& f = sprite_frames_[k];
    note_drawn(f.data(), f.size());                   // tools only (core/image.hpp)
    for (std::size_t i = 0; i < view_buf_.size() && i < f.size(); ++i)
        if (f[i] != 0x0D) view_buf_[i] = f[i];
    screen_.blit(view_buf_, 88, 88, 24, 24);
}

void Game::sprite_step(int sprite, int pic, int dist) {
    // SpriteStep ovl15 +0x0611 (0x4BBF1).
    note_transcript("[sprite " + std::to_string(sprite) + " at distance " + std::to_string(dist) + "]");
    if (pic == 0xFF) f82dd_ = 0;
    if (!f82dd_) {
        if (!f82dc_) {
            if (area_map_) {
                area_map_ = 0;
                f6b70_ = 1;
                draw_view_area();
            }
            if (v0(0x4BE6) != 0) {
                load_anim("SPRIT", sprite);
                f82dc_ = 1;
                f82f9_ = 1;
            }
        } else {
            f6b70_ = 1;
            draw_view_area();
        }
        if (mode_ == 4) show_sprite(dist + 1);
    }
    // DS:82FF is only ever written 0 (ovl8 0x3FB71, 0x3FEF1), so its test
    // with DS:3BEF is always true (inference).
    if (!f82dd_ && dist == 0 && mode_ == 4 && pic != 0xFF) {
        f82f7_ = 1;
        load_anim("PIC", pic);
        f82dd_ = 1;
        if (event_pic_)
            screen_.blit(event_pic_->frames[0], event_pic_->width,
                         event_pic_->height, 24, 24);
        f959b_ = 1;
    }
}

// --- menus --------------------------------------------------------------------------

namespace {
int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }

// MenuScreen's Parse (ovl25 +0x011A, 0x59C5A): every [0-9A-Z] character
// starts an item, which ends two characters before the next one starts
// (the last runs to the end); '@' splits the rows, row1 := the number of
// items begun before it.
struct MenuItems {
    std::vector<int> start, end;
    int row1 = 0;
};
//
// The port's own items only (never the original's strings): kPortItem
// starts an item without being a hotkey and is drawn as a lower-case 'o';
// the item's next capital is its hotkey and starts nothing (CHANGE's
// "Options" with S, O being Order's: D-0065).
constexpr char kPortItem = '\x01';
MenuItems parse_menu(const std::string& items) {
    // ovl25 +0x011A (0x59C5A), the comment above.
    MenuItems m;
    bool port_item = false;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const char c = items[static_cast<std::size_t>(i)];
        if (c == '@') m.row1 = static_cast<int>(m.start.size());
        if (c == ' ') port_item = false;
        const bool cap = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
        if (c == kPortItem || (cap && !port_item)) {
            if (!m.start.empty()) m.end.push_back(i - 2);
            m.start.push_back(i);
            port_item = c == kPortItem;
        }
    }
    if (!m.start.empty()) m.end.push_back(static_cast<int>(items.size()) - 1);
    return m;
}
// An item's key, the character it starts with (MenuEnterKey 0x59F65 and
// the clicks); a port item's key is its capital.
char item_key(const std::string& items, int start) {
    char c = items[static_cast<std::size_t>(start)];
    if (c == kPortItem)
        for (std::size_t i = static_cast<std::size_t>(start) + 1; i < items.size() && items[i] != ' '; ++i)
            if (items[i] >= 'A' && items[i] <= 'Z') return items[i];
    return c;
}
}  // namespace

void Game::draw_menu_line(const std::string& prompt, const std::string& items) {
    // MenuScreen's drawing (DrawItems 0x59D83): one row (0x18), or two
    // (0x17 and 0x18) when the items hold '@'. The prompt at column 0 of
    // the first row, the items right after it; at '@' the rest of the row
    // is cleared and the next row starts at the prompt's width (its first
    // cells untouched). The highlighted item is drawn fg 0 on DS:6B47,
    // other [0-9A-Z] in DS:6B47, the rest in DS:6B46.
    const bool two = items.find('@') != std::string::npos;
    int row = two ? 0x17 : 0x18;
    if (!prompt.empty()) draw_string(0, row, menu_prompt_fg_, 0, prompt);
    const MenuItems m = parse_menu(items);
    const int n = static_cast<int>(m.start.size());
    if (highlight_ == 0 || highlight_ > n) highlight_ = 1;
    int hs = -1, he = -1;
    if (n) {
        hs = m.start[static_cast<std::size_t>(highlight_ - 1)];
        he = m.end[static_cast<std::size_t>(highlight_ - 1)];
    }
    const int plen = static_cast<int>(prompt.size());
    int c = 1;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const char ch = items[static_cast<std::size_t>(i)];
        const int col = c + plen - 1;
        if (ch == '@') {
            if (col <= 0x27) fill_rect(col, row, 0x27, row, 0);
            ++row;
            c = 1;
            continue;
        }
        const unsigned char g = ch == kPortItem ? 'o' : static_cast<unsigned char>(ch);
        if (i >= hs && i <= he && menu_hot_fg_) draw_char(col, row, 0, menu_hot_fg_, g);
        else if (menu_upper(ch)) draw_char(col, row, menu_hot_fg_, 0, g);
        else draw_char(col, row, menu_text_fg_, 0, g);
        ++c;
    }
    const int end = c + plen - 1;
    if (end <= 0x27) fill_rect(end, row, 0x27, row, 0);   // 0x59F55
}

long Game::anim_elapsed() const {
    // GetTime − SS:[BP-0x204] as a signed long (0x5ABAC..0x5ABB5); past
    // midnight it is negative and no frame changes until a key.
    if (!input_ || anim_t0_ < 0) return 0;
    const long now = input_->clock_cs();
    return now < 0 ? 0 : now - anim_t0_;
}

int Game::menu_anim_wait() const {
    // The window polls for a key until the current frame's time is up;
    // headless input answers at once (docs/notes/replay.md "Time").
    if (!input_ || anim_t0_ < 0 || anim_last_ <= 0) return -1;
    const int delay = anim_cur_ < static_cast<int>(anim_delays_.size()) ? anim_delays_[static_cast<std::size_t>(anim_cur_)] : 0;
    const long elapsed = anim_elapsed();
    if (elapsed < 0) return -1;
    return static_cast<int>(std::max(0L, delay * 10L - elapsed) * 10);
}

int Game::menu_timeout_wait() const {
    // The window polls for a key until the timeout has run out too.
    if (menu_timeout_cs_ <= 0 || !input_ || anim_t0_ < 0) return -1;
    return static_cast<int>(std::max(0L, menu_timeout_cs_ - anim_elapsed()) * 10);
}

void Game::menu_anim_step(bool& anim, bool entry) {
    // MenuScreen with its animation flag (0x5AB67..0x5AC0F): on entry the
    // current frame DS:6A63 is drawn when it is past 0 (and with fewer
    // than two frames past the base the flag goes); then, while it polls
    // for a key, a frame whose time (DS:6A45 + k, tenths) has run out on
    // its clock (GetTime, centiseconds) gives way to the next, back to
    // DS:6A61 past the last, and the clock restarts (0x5AC02). A sprite
    // (DS:6A44) never animates. The window passes real time; the headless
    // input answers the poll at once, as KEYFEED does after the first key,
    // so no time passes there: only a frame with time 0 gives way.
    auto draw = [&](int k) { draw_picture(k); };   // DrawPicture ovl27 +0x0023 (3, 3, 1, k)
    if (entry) {
        if (anim_sprite_) anim = false;                // DS:6A44
        if (anim && anim_cur_ > 0) {
            draw(anim_cur_);
            if (anim_last_ < 2) anim = false;
        }
    }
    if (!anim || anim_last_ <= 0) return;
    const int delay = anim_cur_ < static_cast<int>(anim_delays_.size()) ? anim_delays_[static_cast<std::size_t>(anim_cur_)] : 0;
    if (delay * 10L > anim_elapsed()) return;          // 0x5ABD0..0x5ABD8
    if (++anim_cur_ > anim_last_) anim_cur_ = anim_loop_;
    draw(anim_cur_);
    if (input_ && anim_t0_ >= 0) anim_t0_ = input_->clock_cs();   // 0x5AC02
}

Game::MenuKey Game::menu_screen(const std::string& prompt, const std::string& items, bool remap, bool anim) {
    // MenuScreen ovl25 +0x0F15 (0x5AA55), keys main-session checked (session
    // 19, 0x5AC45..0x5AF31); docs/notes/combat_screens.md "MenuScreen".
    static constexpr int kRemap[10] = {0, 0x4F, 0x50, 0x51, 0x4B, 0x20, 0x4D, 0x47, 0x48, 0x49};   // DS:3046
    // 0x59BE3: a different item string (byte sum vs DS:6B48, unless 0xFF)
    // starts with its first item highlighted.
    std::uint8_t sum = 0;
    for (unsigned char c : items) sum = static_cast<std::uint8_t>(sum + c);
    if (sum != menu_sum_ && menu_sum_ != 0xFF) highlight_ = 1;
    menu_sum_ = sum;
    const MenuItems m = parse_menu(items);
    const int n = static_cast<int>(m.start.size());
    const bool two = items.find('@') != std::string::npos;
    if (highlight_ == 0 || highlight_ > n) highlight_ = 1;   // 0x5AAE2
    // Prev/Next wrap around (0x5A7EE, 0x5A823); HighlightRow (0x5A858)
    // jumps between the rows by the first row's count.
    auto prev = [&] { highlight_ = static_cast<std::uint8_t>(highlight_ == 1 ? n : highlight_ - 1); };
    auto next = [&] { highlight_ = static_cast<std::uint8_t>(highlight_ == n ? 1 : highlight_ + 1); };
    auto row_jump = [&] {
        int h = highlight_;
        if (h <= m.row1) h = std::min(h + m.row1, n);
        else {
            h -= m.row1;
            if (h > m.row1) h = m.row1;
        }
        highlight_ = static_cast<std::uint8_t>(h);
    };
    MenuKey out;
    bool redraw = true, first_pass = true;
    anim_t0_ = input_ ? input_->clock_cs() : -1;   // 0x5AB09
    const int plen = static_cast<int>(prompt.size());
    const int first_row = two ? 0x17 : 0x18;          // [bp-0x24A]
    int& row = row_click_;
    int& col = col_click_;
    // MenuEnterKey 0x59F65: the highlighted item's hotkey (0x0D if none).
    auto enter_key = [&] {
        const char h = n ? item_key(items, m.start[static_cast<std::size_t>(highlight_ - 1)]) : 0;
        return h ? static_cast<int>(static_cast<unsigned char>(h)) : kEnter;
    };
    // A left click (0x5AD16..0x5AE11, main): the first test that holds
    // wins. done/ext/key as the original's [bp-0x212], DS:6B1E, DS:6B49.
    auto click = [&](bool& done) -> MenuKey {
        // SkillRowGate 0x5A8CC (always DS:6B44 := 0xFF) / SkillRowPick 0x5A8FB.
        f6b44_ = 0xFF;
        if (mode_ == 0x0B && row <= 0x16) {
            if (col >= 1 && col <= 0x26 && row >= 6) {
                f6b44_ = static_cast<std::uint8_t>(row - 5);
                f8435_ = 0;
                pause_ms(0x41);                        // Crt.Delay 0B00:029E
            }
            done = true;
            return {kEnter, false};
        }
        // IconGrid 0x5A634 (IconSelect).
        if (f6b74_ && row <= 0x16) {
            if (row >= 4 && row < 0x16 && col >= 0x13 && col < 0x25) {
                done = true;
                return {0x21, false};
            }
            f8435_ = 0;                                // after WaitRelease
            return {0, false};
        }
        // RosterHit 0x5A003 / RosterClick 0x5A07C.
        const bool roster_state = mode_ == 0 || mode_ == 1 || mode_ == 4 || mode_ == 6 || mode_ == 10;   // CS:04A3
        if (f8433_ && roster_state && !(mode_ == 4 && move_mode_) && col >= roster_c0_ && col <= roster_c1_ &&
            row >= roster_r0_ && row <= roster_r1_) {
            Character* const entry = cur_;
            int i = 0;
            const int steps = static_cast<std::uint8_t>(row - roster_r0_);
            for (int s = 0; s < steps && i < static_cast<int>(party_.size()); ++s) ++i;
            if (i >= static_cast<int>(party_.size())) return {kEnter, false};   // nil: nothing (key unchanged)
            Character* const node = party_[static_cast<std::size_t>(i)].get();
            MenuKey r{0x48, true};
            done = true;
            if (f6b16_) f6b17_ = node;
            else if (node == cur_) r = {enter_key(), false};
            else {
                prev_cur_ = cur_;                      // DS:4EBD
                cur_ = node;
            }
            draw_roster();                             // 0x5A1B7
            if (mode_ == 0 && f6b4b_) {
                r = {kEnter, false};
                done = false;
                if (node == entry) {
                    f6b4c_ = 1;
                    r = {enter_key(), false};
                    done = true;
                    f8435_ = 0;
                }
            }
            return r;
        }
        // OverheadHit 0x5A94A (state 3) / TravelHit 0x5A99C (state 9).
        if ((mode_ == 3 && move_mode_ && col >= 1 && col <= 0x26 && row >= 1 && row <= 0x0F) ||
            (mode_ == 9 && move_mode_ && col >= 0x11 && col <= 0x26 && row >= 1 && row <= 0x16)) {
            done = true;
            return {enter_key(), false};
        }
        // Combat's Move / Aim: MapClick 0x5A265.
        // ovl25 +0x06CD MapHit (0x5A20D): a fight's Aim or Move, above row 0x16.
        if ((f6bd7_ || f6bd2_) && mode_ == 5 && row <= 0x15) return map_click(done, enter_key());
        // ViewHit 0x5A42F / 0x5A481: the 3D view's arrows, DS:302B.
        if (move_mode_ && mode_ == 4 && col >= 3 && col <= 0x0D && row >= 3 && row <= 0x0D) {
            done = true;
            return {text_.ds8(0x302B + static_cast<std::size_t>(row * 11 + col)), true};
        }
        // MenuRowHit 0x5A4BA / ItemHit 0x5A4E8: the item under the click
        // (1-based positions on its row; the second row of a two-row menu
        // continues past the '@'). In state 8 it waits for the button's
        // release (0x5A620), which a click as a press has already had.
        if (row >= first_row) {
            int p = col + 1;
            MenuKey r{kEnter, false};
            if (p <= plen) return r;
            p -= plen;
            if (two) {
                const int at = static_cast<int>(items.find('@')) + 1;   // Pos('@', items)
                if (row > first_row) p += at;
                else if (at <= p) return r;
            }
            for (int i = 0; i < n; ++i)
                if (m.start[static_cast<std::size_t>(i)] + 1 <= p && p <= m.end[static_cast<std::size_t>(i)] + 1) {
                    r = {static_cast<unsigned char>(item_key(items, m.start[static_cast<std::size_t>(i)])), false};
                    highlight_ = static_cast<std::uint8_t>(i + 1);
                    draw_menu_line(prompt, items);     // 0x59D83
                    done = true;
                }
            return r;
        }
        // DS:6B77 (MODIFY CHARACTER, 0x5ADA3): ModifyPick.
        if (f6b77_) return modify_pick(done, kEnter);
        // ListHit 0x5A734: any listed line at or above the click's row.
        if (f6b4b_) {
            MenuKey r{kEnter, false};
            if (col >= list_c0_ && col <= list_c1_)
                for (int k = 1; k <= 0x19 && !done; ++k)
                    if (list_rows_[static_cast<std::size_t>(k)] <= row && list_rows_[static_cast<std::size_t>(k)] != 0xFF) {
                        done = true;
                        r = {n ? static_cast<unsigned char>(item_key(items, m.start[static_cast<std::size_t>(highlight_ - 1)])) : 0, false};
                    }
            return r;
        }
        // 0x5ADBF: combat's menus: the map rows give Target, or Manual
        // when the prompt is "Aim:" (CS:0EB0).
        if (mode_ == 5 && row <= 0x16) {
            if (two) {
                done = true;
                return {'T', false};
            }
            if (prompt == text_[Text::MsAim]) {
                done = true;
                return {'M', false};
            }
            return {kEnter, false};
        }
        // 0x5ADFE: elsewhere a one-item menu takes the click as a raw Enter.
        if (n > 1) return {0, false};
        done = true;
        return {kEnter, false};
    };
    for (;;) {
        if (redraw) {
            draw_menu_line(prompt, items);
            note_transcript("[menu " + prompt + "|" + items + "|" + std::to_string(highlight_) + "]");
        }
        menu_anim_step(anim, first_pass);
        first_pass = false;
        redraw = true;
        // 0x5AC0F: the timeout (DS:6B1F > 0, signed): once GetTime less
        // the stamp (the entry's, or the picture's last frame's: 0x5AB09,
        // 0x5AC02) reaches it, DS:6B49 := DS:6B23 and the menu ends (the
        // highlight unchanged), unless the poll that follows (0x5AC45)
        // finds a key, which is then taken as usual.
        const bool timed_out = menu_timeout_cs_ > 0 && input_ && anim_t0_ >= 0 && input_->clock_cs() >= 0 &&
                               anim_elapsed() >= menu_timeout_cs_;
        int wait = anim ? menu_anim_wait() : -1;
        if (const int t = menu_timeout_wait(); t >= 0) wait = wait < 0 ? t : std::min(wait, t);
        // 0x5AC45: no key yet → back to the animation (0x5AF31 → 0x5AB9F).
        const int k = timed_out ? next_key_for("menu", 0) : wait >= 0 ? next_key_for("menu", wait) : next_key("menu");
        if (k < 0) {
            if (timed_out) {
                out = {menu_timeout_key_, false};
                input_->timed_out();
                break;
            }
            redraw = false;
            continue;
        }
        if (f8435_) {                                  // 0x5AC59
            row = mouse_y_ / 8;
            col = mouse_x_ / 8;
        }
        if (f8435_ && (k == kEnter || k == kEsc)) {
            // A button. Right (0x5ACD2): the skill rows' gate, else Esc.
            bool done = false;
            MenuKey r;
            if (k == kEsc) {
                f6b44_ = 0xFF;
                if (mode_ == 0x0B && row <= 0x16) {
                    done = true;
                    r = {kEsc, false};
                    if (col >= 1 && col <= 0x26 && row >= 6) {
                        f6b44_ = static_cast<std::uint8_t>(row - 5);
                        f8435_ = 0;
                        pause_ms(0x41);
                    }
                } else if (f6b77_) {
                    r = modify_pick(done, kEsc);       // 0x5ACF4: MODIFY's fields
                } else {
                    done = true;
                    r = {0, false};
                }
            } else {
                r = click(done);
            }
            if (done) { out = r; break; }
            redraw = false;                            // 0x5AF31 → 0x5AB9F: no redraw
            continue;
        }
        if (k >= 0x100) {
            // 0x5AC81: an extended key ends the menu (done := 1), but with
            // more than one item Left/Right (and Up/Down with two rows) move
            // the highlight, and Prev/Next/HighlightRow clear the caller's
            // done flag (0x5A817, 0x5A84C, 0x5A8C0): the menu goes on.
            const int scan = k - 0x100;
            if (n > 1) {
                if (scan == 0x4B) { prev(); continue; }
                if (scan == 0x4D) { next(); continue; }
                if ((scan == 0x48 || scan == 0x50) && two) { row_jump(); continue; }
            }
            out = {scan, true};
            break;
        }
        if (k == kEsc) { out = {0, false}; break; }
        if (k == kEnter) {   // 0x5AD16
            const char h = n ? item_key(items, m.start[static_cast<std::size_t>(highlight_ - 1)]) : 0;
            out = {h ? static_cast<int>(h) : static_cast<int>(kEnter), false};
            break;
        }
        const int K = std::toupper(k);
        bool done = false;
        const bool in_set = K == ' ' || (K >= '0' && K <= '9') || (K >= 'A' && K <= 'Z');   // CS:0EB5
        if (in_set) {
            if (K == ' ') done = true;
            else
                for (int i = 0; i < static_cast<int>(items.size()); ++i)   // 0x5AE5B: the last match wins
                    if (items[static_cast<std::size_t>(i)] == K) {
                        done = true;
                        for (int s = 0; s < n; ++s)
                            if (m.start[static_cast<std::size_t>(s)] == i ||
                                (items[static_cast<std::size_t>(m.start[static_cast<std::size_t>(s)])] == kPortItem &&
                                 m.start[static_cast<std::size_t>(s)] < i && i <= m.end[static_cast<std::size_t>(s)]))
                                highlight_ = static_cast<std::uint8_t>(s + 1);
                        draw_menu_line(prompt, items);
                    }
        }
        bool moved = false;
        if (!done && n > 1) {   // 0x5AEAC: these keep the menu going (done stays 0)
            if (K == '4') { prev(); moved = true; }
            else if (K == '6') { next(); moved = true; }
            else if ((K == '2' || K == '8' || K == 'H' || K == 'P') && two) { row_jump(); moved = true; }
        }
        if (remap && ((K >= '1' && K <= '9') || K == '\\')) {   // 0x5AEF2: CS:0EF5
            out = {K == '\\' ? 0x37 : kRemap[K - '0'], true};
            break;
        }
        if (done) { out = {K, false}; break; }
        redraw = moved;   // any other key is swallowed: no redraw (0x5AF31)
    }
    // The defaults are restored at the exit (0x5AF3B), and the keyboard
    // flushed (0x5AF4A): keys typed ahead (a held key's repeats) never
    // reach what follows.
    menu_prompt_fg_ = 13;
    menu_hot_fg_ = 15;
    menu_text_fg_ = 10;
    f6b1e_ = out.ext ? 1 : 0;                          // DS:6B1E (ViewCharacter's flag from the booty menu)
    flush_keys();
    return out;
}

Game::MenuKey Game::map_click(bool& done, int enter_key) {
    // MapClick ovl25 +0x0725 (0x5A265, main): the clicked square S (3x3
    // cells from (1,1), truncating division) against R, Aim's cursor or
    // CurChar's icon, both relative to the view's origin. S = R: Enter,
    // kept only for Attack / Land (CS:0705). In Aim, S inside the 7x7
    // view: the cursor jumps there (the old square redrawn), ext 0x21
    // inside the border, 0x35 (recentre) on it. Otherwise the keypad key
    // toward S (DS:0D7E by Combat.Dir).
    done = true;
    int rx, ry;
    if (f6bd2_) {
        rx = s8(aim_x_) - s8(field_ox_);
        ry = s8(aim_y_) - s8(field_oy_);
    } else {
        rx = s8(pos_x(*cur_)) - s8(field_ox_);
        ry = s8(pos_y(*cur_)) - s8(field_oy_);
    }
    const int sx = (col_click_ - 1) / 3, sy = (row_click_ - 1) / 3;
    if (sx == rx && sy == ry) {
        if (enter_key == 'A' || enter_key == 'L') return {enter_key, false};
        done = false;
        return {0, false};
    }
    if (f6bd2_ && sx >= 0 && sx <= 6 && sy >= 0 && sy <= 6) {
        field_redraw_square(aim_x_, aim_y_);
        aim_x_ = s8(s8(field_ox_) + sx);
        aim_y_ = s8(s8(field_oy_) + sy);
        return {sx >= 1 && sx <= 5 && sy >= 1 && sy <= 5 ? 0x21 : 0x35, true};
    }
    return {text_.ds8(0x0D7E + static_cast<std::size_t>(dir_to(rx, ry, sx, sy) & 0xFF)), true};
}

}  // namespace cotd::game
