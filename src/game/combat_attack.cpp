// SPDX-License-Identifier: GPL-3.0-or-later
// Combat geometry (the line, the trace, the scan: ovl29; directions:
// ovl30), the Target command (ovl11 +0x3BF2 and its nested routines),
// the attack (ovl11 +0x2241, +0x0C32, +0x0125, +0x03B5) and its
// animations (ovl23 +0x0BE1, ovl11 +0x2B26, ovl30 +0x0C5C / +0x0F97).
// docs/notes/combat_screens.md; main-session readings of session 19.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr int kDx[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};   // DS:257C
constexpr int kDy[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};   // DS:2585

int s8(int v) { return static_cast<std::int8_t>(static_cast<std::uint8_t>(v)); }
int sign(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }   // ovl29 +0x0000

// The line of ovl29 +0x0196 (init) / +0x023F (step): Bresenham from
// (x1, y1) to (x2, y2); count += 2 per major step, + 1 per minor one;
// dir = DS:2168[3 (sy + 1) + (sx + 1)] of the step (8: none).
struct Line {
    int x2, y2, err = 0, adx, ady, cx, cy, sx, sy, count = 0, dir = 8;
    // ovl29 +0x0196 (0x5E366), LineInit.
    Line(int x1, int y1, int bx, int by)
        : x2(bx), y2(by), adx(std::abs(bx - x1)), ady(std::abs(by - y1)), cx(x1), cy(y1), sx(sign(bx - x1)),
          sy(sign(by - y1)) {}
    bool step(const GameText& t) {
        // ovl29 +0x023F LineStep (0x5E40F).
        int mx = 1, my = 1;
        bool ok = false;
        if (adx >= ady) {
            if (cx != x2) {
                cx += sx;
                mx = sx + 1;
                err += 2 * ady;
                count += 2;
                if (err >= adx) {
                    err -= 2 * adx;
                    cy += sy;
                    my = sy + 1;
                    count += 1;
                }
                ok = true;
            }
        } else if (cy != y2) {
            cy += sy;
            my = sy + 1;
            err += 2 * adx;
            count += 2;
            if (err >= ady) {
                err -= 2 * ady;
                cx += sx;
                mx = sx + 1;
                count += 1;
            }
            ok = true;
        }
        count &= 0xFF;   // a byte (+0x16)
        dir = t.ds8(0x2168 + static_cast<std::size_t>(3 * my + mx));
        return ok;
    }
};

}  // namespace

// --- positions and directions (ovl30) --------------------------------------------

int Game::pos_x(const Character& c) const {
    // PosX ovl30 +0x0D53: DS:60B3 + 4 IndexOf(rec) (entry 0 when absent).
    const int k = icon_index(&c);
    if (!k) throw Error(c.name() + " has no combat icon (ovl30 +0x0D53 would read entry 0)");
    return icons_[static_cast<std::size_t>(k - 1)].x;
}

int Game::pos_y(const Character& c) const {
    const int k = icon_index(&c);
    if (!k) throw Error(c.name() + " has no combat icon (ovl30 +0x0D7B would read entry 0)");
    return icons_[static_cast<std::size_t>(k - 1)].y;
}

int Game::icon_size(const Character& c) const {
    const int k = icon_index(&c);   // ovl30 +0x0DA3
    return k ? icons_[static_cast<std::size_t>(k - 1)].size : 0;
}

int Game::dir_to(int ax, int ay, int bx, int by) const {
    // Dir ovl30 +0x178D (0x604FD): the first d = 0..7 whose test passes;
    // the diagonals need adx * 0x6A / 256 <= ady <= adx * 0x26A / 256.
    const int adx = std::abs(bx - ax), ady = std::abs(by - ay);
    const int lo = adx * 0x6A / 256, hi = adx * 0x26A / 256;
    for (int d = 0; d < 8; ++d) {
        bool ok = false;
        switch (d) {
            case 0: ok = by <= ay && hi <= ady; break;
            case 1: ok = by < ay && bx > ax && hi >= ady && lo <= ady; break;
            case 2: ok = bx >= ax && lo >= ady; break;
            case 3: ok = by > ay && bx > ax && hi >= ady && lo <= ady; break;
            case 4: ok = by >= ay && hi <= ady; break;
            case 5: ok = by > ay && bx < ax && hi >= ady && lo <= ady; break;
            case 6: ok = bx <= ax && lo >= ady; break;
            case 7: ok = by < ay && bx < ax && hi >= ady && lo <= ady; break;
            default: break;
        }
        if (ok) return d;
    }
    throw Error("Dir (ovl30 +0x178D): no direction passes, the original loops forever");
}

int Game::dir_rec(const Character& a, const Character& b) const {
    return dir_to(pos_x(a), pos_y(a), pos_x(b), pos_y(b));   // ovl30 +0x199E
}

// --- the trace and the scan (ovl29) -----------------------------------------------

int Game::trace_cell(int x, int y) {
    // 0x5E607..0x5E61C: the byte at [DS:68A2] + 7 + 50 y + x, a word sum
    // (MUL's low word), so a cell off the field is read all the same: one
    // whose index stays inside the field's 1,250 bytes is that byte (a
    // column off a row's end reads the next row's: deterministic); a row
    // above the field wraps to the top of the block's segment (the block
    // is at offset 0: run s51_f9_trap), where the demo has HITEK.DAX #1's
    // decoded picture from byte 9721 at ES:FF60 (session 51's fuzzing F9:
    // four clock seeds' demos measured, the same 160 bytes): D-0075.
    const int idx = y * kFieldW + x;
    if (idx >= 0 && idx < kFieldW * kFieldH) return field_[static_cast<std::size_t>(idx)];
    // A cell just above the first (idx -7..-1) reads the field block's own
    // header, [DS:68A2] + 7 + idx (session 70: night 2026-10-30-2's cell
    // (-1,0) at ECL4 #65 0x9BE9, run s70_d0075_79385: the trap read 0x00 at
    // DI 0006; the dump in that fight, s70_d0075_79385b key 382, holds +6 =
    // 0, as in s70_d0075_53453b's). +6 is field_open_. Session 71: +2/+3
    // are the view's origin and +4/+5 the cursor (on, size), as the port
    // keeps them, in all five RAM dumps in fights (s70_d0075_79385b key
    // 382, s70_d0075_53453b 542, s70_d0075_flightA2 518 and 519,
    // s71_d0075_8607b 42: the cursor off, size 1 in each; on as 1 is
    // inference); +0/+1 (0D 0D, 22 82, 08 00) match nothing kept: 0.
    if (idx >= -7 && idx < 0) {
        const int k = 7 + idx;
        const std::string cell = "Trace (ovl29 0x5E61C) reads cell (" + std::to_string(x) + "," + std::to_string(y) +
                                 ") in the field block's header +" + std::to_string(k);
        int v = -1;
        switch (k) {
            case 2: v = field_ox_ & 0xFF; break;
            case 3: v = field_oy_ & 0xFF; break;
            case 4: v = cursor_on_ ? 1 : 0; break;
            case 5: v = cursor_size_ & 0xFF; break;
            case 6: v = field_open_ ? 1 : 0; break;
            default: break;
        }
        if (k == 6) {
            combat::diag("D-0075", cell + " (terrain doesn't block): " + std::to_string(v));
            return v;
        }
        if (v >= 0) {
            combat::diag("D-0075", cell + " (the view and cursor): " + std::to_string(v));
            return v;
        }
        combat::diag("D-0075", cell + ", not measured: 0");
        return 0;
    }
    const int addr = (idx + 7) & 0xFFFF;
    if (demo_ && addr >= 0xFF60) {
        if (!hitek_) hitek_ = dax_entry("HITEK.DAX", 1);
        if (hitek_ && hitek_->size() >= 9721 + 160) {
            combat::diag("D-0075", "Trace (ovl29 0x5E61C) reads cell (" + std::to_string(x) + "," + std::to_string(y) +
                                       ") above the battlefield: the demo's measured byte (HITEK.DAX #1 +" +
                                       std::to_string(9721 + addr - 0xFF60) + ")");
            return (*hitek_)[static_cast<std::size_t>(9721 + addr - 0xFF60)];
        }
    }
    // Above the field in a real fight (sessions 60-61, D-0075): the top of
    // the field block's segment holds the area's combat tile file #1 as
    // Dax.Load decoded it, freed after the tiles were copied: in 7 of 8
    // dumps its decoded entry starts at the fight's HeapPtr - 0x63, its
    // first 0x7E bytes overwritten since. Where that is from the field
    // depends on every allocation before the fight (the field's paragraph
    // offset is 0..15, HeapPtr - field 0xE04..0x12028 bytes, a free block
    // reused in one): the port has no heap, so the layouts are measured per
    // COMBAT site (KEYFEED's trap at 0x5E61C and a RAM dump in the fight)
    // and kept here; `from` is the lowest offset whose bytes matched the
    // entry (a read below it is not measured). A site whose segment top
    // lies past the entry's end reads memory never written: zeros (dax
    // null). Another site reads 0 with a diagnostic (not measured).
    struct Layout { int file, block, pc; const char* dax; int field_off, raw_at, from; const char* run; };
    static const Layout kLayouts[] = {
        {3, 50, 0x9EA9, "HITEK.DAX", 15, 0xC19D, 0xC21B, "s61_d0075_72630"},      // c2_28_pirates
        {4, 65, 0x9B71, "LOTEK.DAX", 8, 0x934E, 0x93CC, "s61_d0075_90941"},       // c2_36_mars
        {4, 65, 0x9BE9, "LOTEK.DAX", 7, 0xCE0E, 0xCE8C, "s61_d0075_75443"},       // c2_36_mars
        {4, 67, 0x8250, "HITEK.DAX", 8, 0xBDEC, 0xBE6A, "s60_d0075_mem"},         // c2_52_talon
        {4, 67, 0x82C2, "HITEK.DAX", 2, 0x9AEC, 0x9B6A, "s61_d0075_98596"},       // c2_68_juno
        {4, 67, 0x8B90, nullptr, 0, 0, 0x7CA7, "s61_d0075_74427"},                // c2_54_talon: zeros
        {5, 80, 0x927F, "VENUSCOM.DAX", 13, 0xE5C7, 0xE645, "s61_d0075_75805"},   // c2_57_venus
        {5, 80, 0x9772, "VENUSCOM.DAX", 8, 0xDB38, 0xDBB6, "s61_d0075_71674"},    // c2_57_venus
        {4, 67, 0x988C, "HITEK.DAX", 6, 0xD199, 0xD217, "s70_d0075_53453b"},      // c2_69_juno
        {4, 67, 0x8607, "HITEK.DAX", 10, 0xC2AC, 0xC32A, "s71_d0075_8607b"},      // c2_52_talon
        {5, 80, 0x8903, "VENUSCOM.DAX", 4, 0xB095, 0xB113, "s76_d0075_8903"},     // c2_74_farm01
        {4, 67, 0x86E3, "HITEK.DAX", 13, 0xC86C, 0xC8EA, "s76_d0075_86e3"},       // c2_52_talon
        {6, 96, 0x9E69, "HITEK.DAX", 15, 0x91F2, 0x9270, "s76_d0075_9e69"},       // c2_79_finale
        {4, 64, 0x9A26, "MARSCOM.DAX", 4, 0xC916, 0xC994, "s76_d0075_9a26"},      // c2_74_farm01
        {4, 64, 0x97DF, "MARSCOM.DAX", 2, 0xC8DF, 0xD788, "s77_d0075_97dfd"},     // c2_74_farm01
        {1, 16, 0x88BB, "HITEK.DAX", 1, 0xF4E1, 0xF55F, "s77_d0075_88bb"},        // c2_74_farm01
        {2, 35, 0x81F1, "HITEK.DAX", 14, 0xA79C, 0xA81A, "s78_d0075_81f1"},       // c2_29_psyche
        {4, 67, 0x836E, nullptr, 6, 0, 0x7CAD, "s79_d0075_836e"},                 // c2_70_juno: zeros
        {4, 67, 0x9767, nullptr, 10, 0, 0x7CB1, "s80_d0075_9767"},                // c2_69_juno 0x0731100C: zeros
        // Second layouts (session 79, M3), after the site's first row:
        {5, 80, 0x927F, "VENUSCOM.DAX", 11, 0xDC62, 0xDCE0, "s69_d0075_vF_mem2"}, // venus69_F, seed 0x0C00
        {5, 80, 0x8903, "VENUSCOM.DAX", 13, 0xC099, 0xC117, "s79_d0075_8903v"},   // c2_57_venus 0x1B0C112A
        // A third (session 80, goal 80 M6: tools/d0075_seed_search.py, 1 seed in 11,000 deciding):
        {5, 80, 0x927F, "VENUSCOM.DAX", 1, 0xE637, 0xE6B5, "s80_d0075_927f_2a24"}, // c2_57_venus 0x2A240E16
    };
    // A site can have a second measured layout (session 79, M3): the same
    // COMBAT instruction after other allocations (another route, or the
    // same route under another seed: s69_d0075_vF_mem2, s79_d0075_8903v).
    // The port can't tell them apart without the heap's history, so the
    // site's first row gives the value; where a later row of the site
    // gives another byte, the diagnostic says so and names both runs.
    struct Read { int v; std::string what; };
    auto read_of = [&](const Layout& L) -> std::optional<Read> {
        const int a = (idx + 7 + L.field_off) & 0xFFFF;
        if (a < L.from) return std::nullopt;
        if (!L.dax) return Read{0, "memory never written (0)"};
        const int at = a - L.raw_at;
        const auto raw = dax(L.dax).load(1);
        if (!raw || at < 0 || at >= static_cast<int>(raw->size())) return std::nullopt;
        return Read{(*raw)[static_cast<std::size_t>(at)], std::string(L.dax) + " #1 +" + std::to_string(at)};
    };
    if (!demo_ && idx < 0) {
        const Layout* first = nullptr;
        std::optional<Read> got;
        for (const Layout& L : kLayouts) {
            if (traced_file_ != L.file || block_id_ != L.block || op_at_ != L.pc) continue;
            if (!first) {
                first = &L;
                got = read_of(L);
                if (!got) break;
                continue;
            }
            const auto other = read_of(L);
            if (other && other->v != got->v) {
                combat::diag_keyed("D-0075 layouts", "D-0075", "Trace (ovl29 0x5E61C) reads cell (" + std::to_string(x) + "," +
                                           std::to_string(y) + ") above the battlefield at COMBAT ECL" +
                                           std::to_string(L.file) + " #" + std::to_string(L.block) + " 0x" +
                                           hex(static_cast<std::uint32_t>(L.pc), 4) + ", whose two measured layouts differ: " +
                                           std::to_string(got->v) + " in run " + first->run + ", " +
                                           std::to_string(other->v) + " in run " + L.run + "; the first is used");
                return got->v;
            }
        }
        if (got) {
            combat::diag("D-0075", "Trace (ovl29 0x5E61C) reads cell (" + std::to_string(x) + "," + std::to_string(y) +
                                       ") above the battlefield: " + got->what + ", the layout measured in run " +
                                       first->run);
            return got->v;
        }
    }
    // Below the field (session 52: real fights read rows 25 and 26, past
    // the block's end): the combat blocks AllocBlocks (ovl8 +0x17AF) takes
    // right after the field's block, one GetMem(0x12) per record in the
    // list's order, 18 bytes each with no rounding (TP 5.5's GetMem: every
    // HeapPtr in the dumps is odd), the field's block 7 + 1,250 bytes
    // (session 61, D-0075: in 7 of 8 RAM dumps each record's +0xF3 points
    // at field + 1257 + 18 k; session 60's 24-byte granules matched 3 of
    // run s60_d0075_c57's five reads by chance).
    if (idx >= kFieldW * kFieldH && below_site()) {
        // A field in a reused free block (session 71): the area nodes after
        // it, or a freed node's bytes as left (below_site, combat_area.cpp).
        const int at = idx - kFieldW * kFieldH;
        const std::string cell = "Trace (ovl29 0x5E61C) reads cell (" + std::to_string(x) + "," + std::to_string(y) +
                                 ") below the battlefield: ";
        for (std::size_t n = 0; n < areas_.size(); ++n)
            if (areas_[n].heap >= 0 && at >= areas_[n].heap && at < areas_[n].heap + 0x1A) {
                const int v = area_node_byte(n, at - areas_[n].heap);
                combat::diag("D-0075", cell + "an area's node +" + std::to_string(at - areas_[n].heap) + " = " +
                                           std::to_string(v) + ", the layout measured in run " + below_site()->run);
                return v;
            }
        if (below_on_ && at < static_cast<int>(below_img_.size()) && below_img_[static_cast<std::size_t>(at)] >= 0) {
            combat::diag("D-0075", cell + "a freed area node's byte, the layout measured in run " + below_site()->run);
            return below_img_[static_cast<std::size_t>(at)];
        }
        combat::diag("D-0075", cell + "past the area nodes at the measured site, not measured: 0");
        return 0;
    }
    if (idx >= kFieldW * kFieldH) {
        const int rel = 7 + idx - 1257;
        const std::size_t n = static_cast<std::size_t>(rel / 0x12), b = static_cast<std::size_t>(rel % 0x12);
        int v = 0;
        if (n < party_.size() && party_[n]->cb.size() == 0x12) v = party_[n]->cb[b];
        combat::diag("D-0075", "Trace (ovl29 0x5E61C) reads cell (" + std::to_string(x) + "," + std::to_string(y) +
                                   ") below the battlefield: combat block " + std::to_string(n + 1) + " +" +
                                   std::to_string(b) + " = " + std::to_string(v));
        return v;
    }
    // Above it outside the demo (row -1 in real fights) at a COMBAT site
    // the table lacks: not measured; 0, with a diagnostic naming the site
    // (session 60: the nights list each such site as one to measure).
    char site[48];
    std::snprintf(site, sizeof site, "ECL%d #%d 0x%04X", traced_file_, block_id_, op_at_);
    combat::diag("D-0075", "Trace (ovl29 0x5E61C) reads cell (" + std::to_string(x) + "," + std::to_string(y) +
                               ") off the battlefield at COMBAT " + site + ", a site not measured: 0");
    return 0;
}

bool Game::trace(int x1, int y1, int& x2, int& y2, int& d) {
    // Trace ovl29 +0x03E3 (0x5E5B3): cell by cell from (x1, y1) to (x2,
    // y2), both inclusive. DS:38D6 tile 1 met (and the stop when DS:38D9),
    // DS:38D7 tile 4, DS:38D8 terrain class 2 (DS:258B + 4 t) after the
    // first two cells. Blocked by terrain class 1 (unless [DS:68A2]+6),
    // the stop, or more than 2 d + 1 counts: (x2, y2) := the cell. d :=
    // the count. DS:38D9 := 0 at both exits.
    auto terr1 = [&](int t) { return static_cast<int>(text_.ds8(0x258B + 4 * static_cast<std::size_t>(t))); };
    trace_flags_[0] = trace_flags_[1] = trace_flags_[2] = 0;
    int skip = 2;
    const int limit = 2 * d + 1;
    Line l(x1, y1, x2, y2);
    for (;;) {
        const int t = trace_cell(l.cx, l.cy);
        bool stop = false;
        if (t == 1) {
            trace_flags_[0] = 1;
            if (trace_flags_[3]) stop = true;
        } else if (t == 4) {
            trace_flags_[1] = 1;
        } else if (skip > 0) {
            --skip;
        } else if (terr1(t) == 2) {
            trace_flags_[2] = 1;
        }
        if ((!field_open_ && terr1(t) == 1 && (x1 != x2 || y1 != y2)) || stop || l.count > limit) {
            x2 = l.cx;
            y2 = l.cy;
            d = l.count;
            trace_flags_[3] = 0;
            return false;
        }
        if (!l.step(text_)) {
            d = l.count;
            trace_flags_[3] = 0;
            return true;
        }
    }
}

bool Game::cone(int ax, int ay, int tx, int ty, int d) const {
    // Cone ovl29 +0x0541 (0x5E711), signed bytes: t inside the octant d
    // seen from a's neighbour P in that direction (0xFF, 8: any).
    if (ax < 0 || ax > 0x31 || ay < 0 || ay > 0x18 || tx < 0 || tx > 0x31 || ty < 0 || ty > 0x18) return false;
    if (d == 0xFF) d = 8;
    const int P = s8(ax + kDx[d]), Q = s8(ay + kDy[d]), T = tx, U = ty;
    if ((ax == T && ay == U) || (P == T && Q == U)) return true;
    switch (d) {
        case 0: return (T >= P && U <= P - T + Q) || (T <= P && U <= T - P + Q);
        case 1: return (T >= P && U <= P - T + Q) || (T >= P + Q - U && U <= Q);
        case 2: return (T >= P + Q - U && U <= Q) || (T >= P + U - Q && U >= Q);
        case 3: return (T >= P + U - Q && U >= Q) || (T >= P && U >= T - P + Q);
        case 4: return (T >= P && U >= T - P + Q) || (T <= P && U >= P - T + Q);
        case 5: return (T <= P && U >= P - T + Q) || (T <= P + Q - U && U >= Q);
        case 6: return (T <= P + Q - U && U >= Q) || (T <= P + U - Q && U <= Q);
        case 7: return (T <= P + U - Q && U <= Q) || (T <= P && U <= T - P + Q);
        case 8: return true;
        default: throw Error("Cone (ovl29 +0x0541): direction " + std::to_string(d) + " returns an unset local");
    }
}

void Game::scan(int x, int y, int limit, int dir, int size) {
    // Scan ovl29 +0x08CF (0x5EA9F): every icon with a size, reached from
    // any square of the footprint at (x, y) by a cone and an unblocked
    // trace: (k, the least trace count (first pair on a tie), its octant
    // or dir), then the exchange sort ovl29 +0x002E (0x5E1FE).
    std::array<int, 4> ax{}, ay{};
    for (int c = 0; c < 4; ++c) {
        int dx, dy;
        if (footprint(size, c, dx, dy)) {
            ax[static_cast<std::size_t>(c)] = s8(x + dx);
            ay[static_cast<std::size_t>(c)] = s8(y + dy);
        } else {
            ax[static_cast<std::size_t>(c)] = -1;   // 0xFF
        }
    }
    scan_n_ = 0;
    for (std::size_t k = 1; k <= icons_.size(); ++k) {   // entry count + 1 is always empty (Place clears 1..0xFF)
        const Icon& ic = icons_[k - 1];
        if (ic.size == 0) continue;
        std::array<int, 4> tx{}, ty{};
        for (int c = 0; c < 4; ++c) {
            int dx, dy;
            if (footprint(ic.size, c, dx, dy)) {
                tx[static_cast<std::size_t>(c)] = s8(ic.x + dx);
                ty[static_cast<std::size_t>(c)] = s8(ic.y + dy);
            } else {
                tx[static_cast<std::size_t>(c)] = -1;
            }
        }
        bool found = false;
        int best = 0xFF, ba = 0, bt = 0;
        for (int t = 0; t < 4; ++t) {
            if (tx[static_cast<std::size_t>(t)] < 0) continue;
            for (int a = 0; a < 4; ++a) {
                if (ax[static_cast<std::size_t>(a)] < 0) continue;
                const int px = ax[static_cast<std::size_t>(a)], py = ay[static_cast<std::size_t>(a)];
                if (!cone(px, py, tx[static_cast<std::size_t>(t)], ty[static_cast<std::size_t>(t)], dir)) continue;
                int qx = tx[static_cast<std::size_t>(t)], qy = ty[static_cast<std::size_t>(t)], d = limit;
                if (!trace(px, py, qx, qy, d)) continue;
                found = true;
                if (d < best) {
                    best = d;
                    ba = a;
                    bt = t;
                }
            }
        }
        if (!found) continue;
        int D = 0;
        if (dir < 8) D = dir;
        else
            while (!cone(ax[static_cast<std::size_t>(ba)], ay[static_cast<std::size_t>(ba)],
                         tx[static_cast<std::size_t>(bt)], ty[static_cast<std::size_t>(bt)], D))
                ++D;
        if (scan_n_ >= 0x48) throw Error("Scan (ovl29 +0x08CF): more than 0x48 entries");
        scan_[static_cast<std::size_t>(++scan_n_)] = {static_cast<int>(k), best & 0xFF, D};
    }
    for (int i = 1; i <= scan_n_ - 1; ++i)
        for (int j = i + 1; j <= scan_n_; ++j) {
            ScanEntry& a = scan_[static_cast<std::size_t>(i)];
            ScanEntry& b = scan_[static_cast<std::size_t>(j)];
            if (b.cnt < a.cnt || (b.cnt == a.cnt && b.dir < a.dir && b.dir % 2 <= a.dir % 2)) std::swap(a, b);
        }
}

int Game::collect_enemies(Character& c, int range) {
    // CollectEnemies ovl23 +0x1ABE (0x579CE): the scan with the range as
    // its limit, kept to the other side (Side ovl23 +0x1A46: +0xD6 = 0 ->
    // 1, else 0); their icon numbers also go to DS:6B7F + i (the AI's).
    scan(pos_x(c), pos_y(c), range, 0xFF, icon_size(c));
    const int other = c.b(0xD6) == 0 ? 1 : 0;
    int m = 0;
    for (int i = 1; i <= scan_n_; ++i) {
        const Character* r = icons_[static_cast<std::size_t>(scan_[static_cast<std::size_t>(i)].k - 1)].rec;
        if (r && r->b(0xD6) == other) scan_[static_cast<std::size_t>(++m)] = scan_[static_cast<std::size_t>(i)];
    }
    scan_n_ = m;
    return m;
}

int Game::distance(Character& a, Character& b) {
    // Distance ovl23 +0x1BDA (0x57AEA): the scan from a with the terrain
    // open; b's trace count div 2 (the last entry's when b isn't reached).
    // [DS:68A2]+6 is left 0, and the entries 1..0x48 are restored but not
    // their count DS:68A6 (quirk, reproduced).
    const auto saved = scan_;
    field_open_ = true;
    scan(pos_x(a), pos_y(a), 0xFF, 0xFF, icon_size(a));
    field_open_ = false;
    int i = 1;
    while (icons_[static_cast<std::size_t>(scan_[static_cast<std::size_t>(i)].k - 1)].rec != &b && i < scan_n_) ++i;
    const int d = scan_[static_cast<std::size_t>(i)].cnt / 2;
    for (std::size_t k = 1; k < scan_.size(); ++k) scan_[k] = saved[k];
    return d;
}

// --- weapons -----------------------------------------------------------------------

Bytes* Game::weapon_of(Character& c) {
    if (c.weapon < 0 || c.weapon >= static_cast<int>(c.items.size())) return nullptr;
    return &c.items[static_cast<std::size_t>(c.weapon)];
}

int Game::item_byte(int type, int k) const {
    const std::size_t i = 9 * static_cast<std::size_t>(type) + static_cast<std::size_t>(k);
    if (i >= items_table_.size()) throw Error("ITEMS: type " + std::to_string(type) + " is past the table");
    return items_table_[i];
}


int Game::weapon_range(Character& c) {
    // WeaponRange ovl18 +0x2B88 (0x52B28).
    Bytes* w = weapon_of(c);
    if (!w) return 1;
    int r = item_byte((*w)[0x2E], 7);
    if (r == 0 || r == 0xFF) r = 1;
    if (item_byte((*w)[0x2E], 2) == 8 && find_readied(c, 6)) r = 0x0C;
    return r;
}

Bytes* Game::find_readied(Character& c, int cls) {
    // ovl18 +0x1E8C (0x51E2C): the first readied item of the class (ITEMS byte 0).
    for (Bytes& it : c.items)
        if ((it[0x34] & 0x80) && item_byte(it[0x2E], 0) == cls) return &it;
    return nullptr;
}

bool Game::weapon_heavy(Character& c) {   // ovl23 +0x2B2F (0x58A3F): kind >= 7
    const Bytes* w = weapon_of(c);
    return w && item_byte((*w)[0x2E], 2) >= 7;
}

bool Game::weapon_area(Character& c) {    // ovl23 +0x2B72 (0x58A82)
    return weapon_heavy(c) && (*weapon_of(c))[0x38] > 0;
}

bool Game::weapon_ranged(Character& c) {  // ovl23 +0x2BA7 (0x58AB7): range byte > 1
    const Bytes* w = weapon_of(c);
    return w && item_byte((*w)[0x2E], 7) > 1;
}

bool Game::weapon_loaded(Character& c) {  // ovl23 +0x2BEA (0x58AFA): ammunition, kind > 0
    const Bytes* w = weapon_of(c);
    if (!w) throw Error("ovl23 +0x2BEA without a readied weapon reads 0000:0038");
    return (*w)[0x38] > 0 && item_byte((*w)[0x2E], 2) > 0;
}

bool Game::can_fire(Character& c) {
    // CanFire ovl6 +0x04F3 (0x3A2F3): not sprinting, and a kind-1 weapon
    // has ammunition.
    if (c.b(0xD4) == 0x0B) return false;
    const Bytes* w = weapon_of(c);
    if (w && item_byte((*w)[0x2E], 2) == 1 && (*w)[0x38] == 0) return false;
    return true;
}

// --- Target: ovl11 +0x3BF2 (0x444F2..0x44914) ------------------------------------

void Game::target_cmd(Character& actor, int range, int a12, bool fire, bool& done) {
    marker_from(0x20, 0, 0, false);                    // 0267:2E33: the cursor box
    done = false;
    int rng = range;
    if (rng == 0xFF) rng = weapon_range(actor);
    if (rng == 0) rng = 1;
    const int ax = pos_x(actor), ay = pos_y(actor);
    field_redraw_square(ax, ay);
    // BuildTargets ovl11 +0x3A71 (0x44371): the actor and the other side
    // (Keep ovl11 +0x0BDA) from a scan; the actor's last target current.
    std::vector<ScanEntry> list(1);
    std::size_t cur = 1;
    scan(ax, ay, 0xFF, 0xFF, icon_size(actor));
    for (int i = 1; i <= scan_n_; ++i) {
        const ScanEntry& e = scan_[static_cast<std::size_t>(i)];
        Character* r = icons_[static_cast<std::size_t>(e.k - 1)].rec;
        const bool keep = r && (r == &actor || r->b(0xD6) != actor.b(0xD6));
        if (!keep) continue;
        list.push_back(e);
        if (r == actor.cb_target) cur = list.size() - 1;
    }
    const std::size_t count = list.size() - 1;
    auto rec_of = [&](std::size_t i) { return icons_[static_cast<std::size_t>(list[i].k - 1)].rec; };
    // NextTarget ovl11 +0x382A (0x4412A).
    auto next_target = [&](int delta) -> Character* {
        Character* old = rec_of(cur);
        if (!old) old = &actor;
        if (count < 1) throw Error("NumTargets < 1 (ovl11 +0x382A): the original stops with a fatal error");
        const Icon& p = icons_[static_cast<std::size_t>(list[cur].k - 1)];
        const int px = p.x, py = p.y;
        cur = static_cast<std::uint8_t>(cur + delta);
        if (cur == 0) cur = count;
        if (cur > count) cur = 1;
        Character* nw = rec_of(cur);
        const Icon& n = icons_[static_cast<std::size_t>(list[cur].k - 1)];
        if (!nw || n.x < 0 || n.y < 0) return old;
        anim_icon(px, py, n.x, n.y, 1, 0);
        return nw;
    };
    Character* target = next_target(0);
    if (target != &actor) {   // 0x444C2
        menu_sum_ = 0xFF;
        highlight_ = 4;
    }
    int key = 0x20;
    int tx = pos_x(*target), ty = pos_y(*target);
    bool flag = true;
    // 'A' (0x4477C), also after Manual: AttackTarget ovl11 +0x3186
    // (0x43A86) unless the area weapon's AreaAttack (+0x3270).
    auto attack_now = [&] {
        done = true;
        if (a12 == 1) {
            if (weapon_heavy(actor)) {
                // 0x447CD: an area weapon (kind >= 7) is thrown at the
                // square (-0xE8/-0xE9: the target's, or Aim's cursor):
                // AreaAttack ovl11 +0x3270, no AttackTarget.
                area_attack(actor, tx, ty, done);
            } else {
                if (!target) throw Error("AttackTarget without a target (ovl11 +0x3186)");
                done = true;
                if (!can_attack(actor, *target)) done = false;
                if (done) {
                    cursor_on_ = false;
                    field_view(s8(field_ox_ + 3), s8(field_oy_ + 3), 3, 8);
                    note_attacker(actor, *target);
                    attack_command(actor, target, false, done);
                }
            }
        }
        if (!done) {
            cursor_on_ = false;
            field_view(tx, ty, 0, 8);
        }
    };
    while (key != 0 && key != 'A' && key != 'E') {     // CS:3BD2
        // TargetMenu ovl11 +0x2FF4 (0x438F4).
        std::string items = text_[Text::CbTargetItems];
        {
            int d = 0xFF, qx = tx, qy = ty;
            const bool saved = field_open_;
            field_open_ = true;
            trace(pos_x(actor), pos_y(actor), qx, qy, d);   // the result is unused
            field_open_ = saved;
            d /= 2;
            show_range(d, weapon_range(actor));
            if (rng >= d && flag && fire) {
                if (weapon_area(actor)) items += text_[Text::CbTargetAttack];
                else if (target == &actor) {
                } else if (!weapon_ranged(actor) || weapon_loaded(actor)) items += text_[Text::CbTargetAttack];
            }
        }
        items += text_[Text::CbTargetExit];
        if (target) {
            focus_on(*target, 3, true);
            f6bd9_ = 1;
            combat_panel(*target);
        }
        const MenuKey mk = menu_screen(text_[Text::CbAim], items, true);   // 0x43A72
        key = mk.key;
        if (!mk.ext) {
            switch (key) {
                case 'N': flag = true; target = next_target(1); break;
                case 'P': flag = true; target = next_target(0xFF); break;
                case 'M': {                                    // 0x44640: Manual (Aim)
                    bool chosen = false;
                    Character* tp = nullptr;
                    int rx = tx, ry = ty;
                    aim(actor, rng, a12, false, fire, flag, chosen, tp, rx, ry, tx, ty);
                    if (flag && chosen) {
                        target = tp;
                        tx = rx;
                        ty = ry;
                        key = 'A';
                        highlight_ = 4;
                        attack_now();
                    } else {
                        highlight_ = 3;
                        tx = pos_x(actor);
                        ty = pos_y(actor);
                        target = &actor;
                    }
                    break;
                }
                case 'A': attack_now(); break;                 // 0x4477C
                case 'C':
                    cursor_on_ = true;
                    cursor_size_ = 1;
                    field_view(tx, ty, 0, 8);
                    cursor_on_ = false;
                    break;
                case 'E':
                    cursor_on_ = false;
                    field_view(tx, ty, 0, 8);
                    break;
                case 0:                                        // Esc
                    cursor_on_ = false;
                    field_redraw_square(tx, ty);
                    field_view(ax, ay, 0, 8);
                    break;
                default: break;
            }
        }
        if ((key == 'N' || key == 'P' || key == 'A') && target) {   // 0x448C6 (pos(nil) is dead)
            tx = pos_x(*target);
            ty = pos_y(*target);
        }
    }
    clear_cells(0, 0x17, 0x27, 0x17);
}

bool Game::aim(Character& actor, int range, int mode, bool allow_empty, bool fire, bool& ok, bool& chosen,
              Character*& tptr, int& rx, int& ry, int tx, int ty) {
    // Aim ovl11 +0x32F0 (0x43BF0..0x44115), attack mode (1): a free cursor
    // (DS:6BD0/6BD1, the box) from (tx, ty), one square per key; each square
    // shows its Range (or a cleared row when the line is blocked), the
    // panel of what stands there (a body too) and "Attack" when it may be
    // attacked, else "Exit". Returns the cursor's validity in ok.
    // Mode 2 (the jetpack's flight, session 32): "Flying" and Land
    // instead of the range, the panel and Attack (0x43CFA..0x43D58).
    if (mode != 1 && mode != 2) throw Error("aiming in mode " + std::to_string(mode));
    const int wr = weapon_range(actor);
    const int x0 = tx, y0 = ty;
    int& cx = aim_x_;                                  // DS:6BD0 / 6BD1: a map click moves it
    int& cy = aim_y_;
    cx = tx;
    cy = ty;
    f6bd2_ = 1;                                        // 0x43BF6 .. 0x4410D
    tptr = nullptr;
    rx = ry = 0;
    cursor_on_ = true;
    cursor_size_ = 1;
    int key = 0x20, dir = 8;
    chosen = false;
    while (key != 0 && key != 'A' && key != 'E' && key != 'L') {   // CS:32A4
        field_view(s8(cx), s8(cy), 2, dir);
        cx = s8(cx + kDx[dir]);
        cy = s8(cy + kDy[dir]);
        cx = std::clamp(cx, 0, 0x31);
        cy = std::clamp(cy, 0, 0x18);
        const int occ = occupant(cx, cy), terr = field_tile_at(cx, cy);   // MapAt
        flush_keys();                                  // 0x43CDD
        ok = false;
        if (mode == 2) {
            // No range limit, line or cost: Land where the square is open
            // terrain and nobody stands (keypad 5, scan 0x4C = 'L', lands
            // anywhere: a quirk kept).
            draw_string(0, 0x17, 0x0A, 0, text_[Text::CbFlying]);
            std::string items;
            if (text_.ds8(0x258A + 4 * static_cast<std::size_t>(terr)) != 0xFF && occ == 0) {
                items = text_[Text::CbLand];
                ok = true;
            }
            key = menu_screen(text_[Text::CbUseCursor], items, true).key;   // 0x43F76
        }
        int d = 0xFF, qx = cx, qy = cy;
        Character* t = nullptr;
        if (mode == 1) {
            if (trace(pos_x(actor), pos_y(actor), qx, qy, d)) {
                ok = true;
                show_range(d / 2, wr);
            } else {
                clear_cells(0, 0x17, 0x27, 0x17);
            }
            d /= 2;
            if (ok) {
                if (occ > 0) t = icons_[static_cast<std::size_t>(occ - 1)].rec;
                else if (terr == 5)
                    for (const Body& b : bodies_)
                        if (b.x == cx && b.y == cy) t = b.rec;
            }
            if (t) {
                f6bd9_ = 1;
                combat_panel(*t);
            } else {
                clear_cells(0x17, 1, 0x26, 0x15);
            }
            const bool keep = t && (t == &actor || t->b(0xD6) != actor.b(0xD6));   // ovl11 +0x0BDA
            if (!(range >= d && text_.ds8(0x258A + 4 * static_cast<std::size_t>(terr)) != 0xFF && fire)) ok = false;
            else if (weapon_area(actor)) {
            } else if (!((t || allow_empty) && keep && (mode != 1 || (t != &actor && !(occ == 0 && terr == 5))) &&
                         (!weapon_ranged(actor) || weapon_loaded(actor))))
                ok = false;
            key = menu_screen(text_[Text::CbUseCursor], text_[ok ? Text::CbAimAttack : Text::CbAimExit], true).key;   // 0x43F76
        }
        switch (key) {
            case 'L':
                actor.b(0xD4) = 0x0E;
                field_redraw_square(cx, cy);
                break;
            case 'A':
                chosen = true;
                rx = cx;
                ry = cy;
                tptr = t;
                cursor_on_ = false;
                field_redraw_square(cx, cy);
                break;
            case 0x48: dir = 0; break;
            case 0x49: dir = 1; break;
            case 0x4D: dir = 2; break;
            case 0x51: dir = 3; break;
            case 0x50: dir = 4; break;
            case 0x4F: dir = 5; break;
            case 0x4B: dir = 6; break;
            case 0x47: dir = 7; break;
            case ' ':
            case 0x35:
                field_view(s8(cx), s8(cy), 0, 8);
                dir = 8;
                break;
            case 'E':
            case 0:
                field_redraw_square(cx, cy);
                tptr = nullptr;
                rx = x0;
                ry = y0;
                field_view(x0, y0, 0, 8);
                cursor_on_ = false;
                field_redraw_square(x0, y0);
                break;
            default: dir = 8; break;
        }
    }
    f6bd2_ = 0;
    return ok;
}

void Game::show_range(int d, int wr) {
    // ShowRange ovl11 +0x2EEB (0x437EB), Band ovl11 +0x253B (0x42E3B).
    d &= 0xFF;
    std::string band;
    if (d <= wr) {
        const int b = wr <= 1 ? 0 : d > ((3 * wr) >> 2) ? 5 : d > (wr >> 1) ? 2 : 0;
        band = text_[b == 0 ? Text::CbShort : b == 2 ? Text::CbMedium : Text::CbLong];
    }
    const std::string s = (text_[Text::CbRange] + std::to_string(d) + text_[Text::CbRangeSp] + band).substr(0, 25);
    draw_string(0, 0x17, 0x0A, 0, s);
    clear_cells(static_cast<int>(s.size()), 0x17, 0x27, 0x17);
}

bool Game::can_attack(Character& a, Character& t) {
    // ovl11 +0x2DEE (0x436EE): an ally of a player-controlled attacker
    // asks (CS:2DE0, YesNo); Yes turns every Okay hired monster (+0xE7 >
    // 0x7F, not 0xB3) against the party.
    const int opp = t.b(0xD6) == 0 ? 1 : 0;
    if (opp == a.b(0xD6) || a.b(0xD7) != 0) return true;
    menu_prompt_fg_ = 14;
    if (yes_no(text_[Text::CbAttackAlly], 2) != 'Y') return false;
    v1(0x7F33) = 1;
    for (const auto& p : party_)
        if (p->b(0xD4) == 1 && p->b(0xE7) != 0xB3 && p->b(0xE7) > 0x7F) {
            p->b(0xD6) = 1;
            p->cb_target = nullptr;
            p->b(0xD7) = 1;
        }
    count_sides();
    return true;
}

void Game::note_attacker(Character& a, Character& t) {
    // ovl11 +0x0F45 (0x41845): the target counts its attackers (cb+0x0D)
    // and how far round they come from its facing (cb+0x0E, mod 8).
    t.cb[0x0D] = static_cast<std::uint8_t>(t.cb[0x0D] + 1);
    int v = (dir_rec(t, a) - t.cb[7] + 8) % 8;
    if (v > 4) v = 8 - v;
    t.cb[0x0E] = static_cast<std::uint8_t>((t.cb[0x0E] + v) % 8);
}

// --- the attack --------------------------------------------------------------------

void Game::attack_command(Character& a, Character* t, bool flag, bool& done) {
    // AttackCommand ovl11 +0x2241 (0x42B41..0x42E38).
    f6bda_ = 1;
    f6bd9_ = 1;
    round_limit_ = static_cast<std::uint8_t>(round_ + 15);
    if (t) {
        const int d = dir_rec(*t, a);
        if (t->cb[0x0D] < 2 && !flag) t->cb[7] = static_cast<std::uint8_t>(d);
        if (icon_visible(t, false)) {
            face(*t, t->cb[7], 0, true);
            face(*t, t->cb[7], 0, false);
        }
        a.cb[7] = static_cast<std::uint8_t>(dir_rec(a, *t));
    }
    combat_panel(a);
    face(a, a.cb[7], 1, false);                        // the action icon
    a.cb_target = t;
    pause_ms(100);                                     // Crt.Delay(100)
    const int wi = a.weapon;
    Bytes* w = weapon_of(a);
    f6993_ = 0;
    if (w && (*w)[0x3D] != 0 && (*w)[0x3C] >= 5 && (*w)[0x3C] <= 12)   // set CS:2200
        weapon_special(a, (*w)[0x3C]);
    if (f6993_) {
        if (icon_visible(&a, false)) {
            face(a, a.cb[7], 1, true);
            face(a, a.cb[7], 0, false);
        }
        return;
    }
    if (weapon_heavy(a))
        throw Error("AttackCommand with a heavy weapon (ovl11 0x42CC7): the original stops with its fatal error "
                    "(ovl11 CS:2220)");
    // A nil target comes only from AreaAttack (0x43B70), whose two callers
    // (0x446D5, 0x447CD) let only a heavy weapon through; every shipped
    // heavy weapon is special (returned above) and any other stops at the
    // fatal error just above. So this is unreachable (DIVERGENCE.md
    // D-0048, rule 4: rejected).
    if (!t) throw Error("D-0048: AttackCommand (ovl11 +0x2241) without a target and with a weapon that is neither "
                        "heavy nor special: unreachable in the original (its callers gate on WeaponHeavy)");
    if (w) data_note("use", (*w)[0x2E], a.b(0xD6));    // tools only: --data-trace
    if (weapon_range(a) > 1) projectile(pos_x(a), pos_y(a), pos_x(*t), pos_y(*t), *w);
    done = true;
    if (a.b(0xDB) > 0 || a.b(0xDC) > 0) {
        const CurChar saved = cur_;
        cur_ = &a;
        attack(a, *t, flag, done);
        if (wi >= 0 && wi < static_cast<int>(a.items.size())) {
            Bytes& it = a.items[static_cast<std::size_t>(wi)];
            const int type = it[0x2E];
            if (item_byte(type, 2) > 0) {
                it[0x38] = static_cast<std::uint8_t>(it[0x38] - slot_attacks_[1]);
                if (it[0x38] == 0 && item_byte(type, 8) != 0) remove_item(a, wi);
            }
        }
        recalc(a);
        cur_ = saved;
    }
    if (icon_visible(&a, false)) {
        face(a, a.cb[7], 1, true);
        face(a, a.cb[7], 0, false);
    }
}

void Game::attack(Character& a, Character& t, bool flag, bool& done) {
    // Attack ovl11 +0x0C32 (0x41532..0x41842): every attack left, slot 2
    // (a second natural attack) first, until the target is down (+0xD5 = 0).
    bool from_behind = flag;
    done = false;
    slot_hits_ = {0, 0, 0};
    slot_attacks_ = {0, 0, 0};
    bool any_hit = false, down = false;
    dmg_ = 0;
    a.cb[6] = 1;
    recalc(t);
    effect_hook(t, 0x0B, "Attack (0x41586)");
    int back = 0;
    behind_ = 0;
    if (behind(a, t)) {
        from_behind = true;
        back = 1;
        behind_ = backstab(a, t) ? 2 : 1;
    }
    int ac = t.b(static_cast<std::size_t>(0xD9 + back));
    if (from_behind && a.b(0x28) == 5) ac += 2;        // a Rogue
    ac &= 0xFF;
    ac_modifiers(a, t, ac);
    const int mod = combat::attack_modifier(a.rec, v1(0x7F70), v1(0x7F71), v0(0x4BB6));
    for (int slot = a.cb[2]; slot >= 1; --slot) {
        while (a.b(static_cast<std::size_t>(0xDA + slot)) > 0 && !down) {
            a.b(static_cast<std::size_t>(0xDA + slot)) = static_cast<std::uint8_t>(a.b(static_cast<std::size_t>(0xDA + slot)) - 1);
            a.cb[2] = static_cast<std::uint8_t>(slot);
            ++slot_attacks_[static_cast<std::size_t>(slot)];
            combat::Rng rng(seed_);
            const combat::AttackRoll roll = combat::attack_roll(a.rec, ac, mod, rng, has_effect(a, 0x0D), has_effect(t, 1));
            seed_ = rng.seed();
            if (roll.hook10) effect_hook(a, 0x0A, "AttackRoll (ovl22 +0x0583)");
            if (roll.hook10) debug_show(text_[Text::DebugThac0], a.rec[0xD8], ac, mod);   // 0x5568B: the modifier, [bp-2]
            if (roll.hit) {
                const Bytes* w = weapon_of(a);
                if (w && (*w)[0x2E] == 0x0F) {
                    // 0x416BB: the stun weapon (type 15): a save (type 0)
                    // against effect 0x0E for d4 + 4; no damage, no hit
                    // counted (so the miss message follows), and back to
                    // the loop's top past the down checks (0x417B0).
                    const bool saved = save(t, 0, 0);
                    const int dur = roll_dice(1, 4) + 4;
                    apply_effect(t, 0x0E, dur, 0, 0, true, saved, text_[Text::CbStunMsg]);
                    continue;
                }
                ++slot_hits_[static_cast<std::size_t>(slot)];
                any_hit = true;
                sound(text_.ds16(0x21EA));                  // 0x4171C: a hit (7)
                note_readied(t, 1, "worn_hit");             // tools only: --data-trace
                damage_roll(a, t, slot);
                if (t.b(0xD5) != 0) attack_result(a, t, dmg_, dmg_, true);
                if (t.b(0xD5) != 0 && dmg_ > 0) effect_hook(a, slot + 1, "Attack (0x4177D)");
            }
            if (t.b(0xD5) == 0) down = true;
            if (a.b(0xD5) == 0) a.b(static_cast<std::size_t>(0xDA + slot)) = 0;
        }
        if (slot == 1) break;
    }
    if (!any_hit) {
        sound(text_.ds16(0x21EE));                      // 0x417C6: a miss (9)
        attack_result(a, t, 0, 0, false);
    }
    done = !(a.b(0xDB) > 0 || a.b(0xDC) > 0);
    if (a.b(0xD5) == 0) done = true;
    if (done) done = end_turn(a);
}

bool Game::behind(Character& a, Character& t) {
    // ovl11 +0x41BC (0x44ABC): the target has two attackers or more this
    // round, is small (+0x34 & 0x7F <= 1), and faces within one step of
    // the line from the attacker (cb+7 - Dir in {-1, 0, 1, 7}: CS:419C
    // and 0xFF, bytes).
    const int v = (t.cb[7] - dir_rec(a, t)) & 0xFF;
    return t.cb[0x0D] > 1 && (t.b(0x34) & 0x7F) <= 1 && (v == 0xFF || v == 0 || v == 1 || v == 7);
}

bool Game::backstab(Character& a, Character& t) {
    // ovl11 +0x410C (0x44A0C): close range, right behind, and a SkillCheck
    // of skill 0x3E or else 0x4E (difficulty 1 for a Rogue, else 2).
    if (weapon_range(a) >= 2) return false;
    if (dir_rec(a, t) != t.cb[7]) return false;
    const int diff = a.b(0x28) == 5 ? 1 : 2;
    for (int skill : {0x3E, 0x4E}) {
        combat::Rng rng(seed_);
        const int r = combat::skill_check({skill_total(a, skill)}, diff, rng).result;
        seed_ = rng.seed();
        data_note("skill", skill, r);
        if (r >= 2) return true;
    }
    return false;
}

void Game::ac_modifiers(Character& a, Character& t, int& ac) {
    // ovl11 +0x2592 (0x42E92): a guard's ranged shot +5 and nothing else;
    // else cover on the trace (DS:38D8) +2 and the range band (0, +2
    // past half, +5 past three quarters).
    const int dist = distance(a, t);
    if (weapon_ranged(a) && a.cb[4] != 0) {
        ac = (ac + 5) & 0xFF;
        return;
    }
    int tx = pos_x(t), ty = pos_y(t), d = 0xFF;
    trace(pos_x(a), pos_y(a), tx, ty, d);
    if (trace_flags_[2]) ac = (ac + 2) & 0xFF;
    const int wr = weapon_range(a), dd = dist & 0xFF;
    const int band = wr <= 1 ? 0 : dd > ((3 * wr) >> 2) ? 5 : dd > (wr >> 1) ? 2 : 0;
    ac = (ac + band) & 0xFF;
}

void Game::damage_roll(Character& a, Character& t, int slot) {
    // DamageRoll ovl11 +0x0125 (0x40A25): the dice (combat::damage_roll),
    // the hooks, then a weapon's kind against the line from the target
    // back to the attacker (kind 2: tile 1 or the target's +0x48 bits 0x10
    // (Percent 50) / 0x20 (Percent 75) cancel it; kind 3: tile 4; kind 4:
    // +0x48 bit 0x40).
    combat::Rng rng(seed_);
    dmg_ = static_cast<std::uint8_t>(combat::damage_roll(a.rec, slot, behind_ == 2, rng));
    seed_ = rng.seed();
    effect_hook(a, 4, "DamageRoll (0x40ABE)");
    effect_hook(t, 5, "DamageRoll (0x40ACB)");
    if (Bytes* w = weapon_of(a)) {
        int ax = pos_x(a), ay = pos_y(a), d = 0xFF;
        trace(pos_x(t), pos_y(t), ax, ay, d);
        const int terr = field_tile_at(ax, ay);   // ovl30 +0x06BC: the map at the trace's end
        if (terr == 1) trace_flags_[0] = 1;
        else if (terr == 4) trace_flags_[1] = 1;
        const int kind = item_byte((*w)[0x2E], 2);
        auto percent = [&](int n) { return n > static_cast<int>(random(100)); };   // ovl23 +0x3070
        if (kind == 2) {
            if (!trace_flags_[0] && (t.b(0x48) & 0x10)) note_readied(t, 7, "worn_use");   // tools only
            if (trace_flags_[0]) dmg_ = 0;
            else if ((t.b(0x48) & 0x10) && percent(50)) dmg_ = 0;
            else if ((t.b(0x48) & 0x20) && percent(75)) dmg_ = 0;
        } else if (kind == 3) {
            if (trace_flags_[1]) dmg_ = 0;
        } else if (kind == 4) {
            if (t.b(0x48) & 0x40) dmg_ = 0;
        }
    }
    f698d_ = 0;   // DS:698D := 0 on every path (0x40BF0)
}

void Game::attack_result(Character& a, Character& t, int apply, int show, bool hit) {
    // AttackResult ovl11 +0x03B5 (0x40CB5..0x40FA4).
    name_message(a, text_[behind_ == 2 ? Text::CbBackstabs : behind_ == 3 ? Text::CbSlays : Text::CbAttacks], 10, false);
    draw_char_name(t, 0x17, 12);
    message_chars(t.name().size());
    int row = 13;
    const std::string prefix = behind_ == 1 ? text_[Text::CbBehind] : std::string();
    std::string s;
    if (hit) {
        if (behind_ == 3) s = text_[Text::CbCruel];
        else
            s = (prefix + text_[Text::CbHitting] + str_byte(show & 0xFF)).substr(0, 80) +
                text_[(show & 0xFF) == 1 ? Text::CbPoint : Text::CbPoints];
        if (behind_ != 3) s = s.substr(0, 80) + text_[Text::CbOfDamage];
        take_damage(t, apply & 0xFF);
    } else {
        s = prefix + text_[Text::CbMisses];
    }
    s = s.substr(0, 80);
    if (t.b(0xD4) != 2) print_in(0x17, row, 0x26, row + 3, 0x0A, true, s);
    row = (row_ + 1) & 0xFF;                           // DS:5F3F + 1
    game_delay();             // Game.Delay
    if (t.b(0xD5) == 0) {
        name_message(t, text_[Text::CbGoesDown], row, false);
        row += 2;
        if (t.cb[0x0F] == 0) {
            if (t.b(0xD4) == 4) {
                draw_string(0x17, row, 0x0A, 0, text_[Text::CbDying]);
                message_chars(text_[Text::CbDying].size());
            }
            if (t.b(0xD4) == 2 || t.b(0xD4) == 3) name_message(t, text_[Text::CbKilled], row, false);
        }
        clear_on_down(t);
        effect_hook(t, 0x0D, "AttackResult (0x40F7A)");
        if (t.b(0xD5) == 0) death_anim(t);
        else game_delay();
    }
    clear_text();
}

void Game::clear_on_down(Character& c) {
    // ovl22 +0x089D (0x558ED): the effects DS:0C9F[1..4] = 2, 1, 0x0E, 0x1D go.
    for (int i = 1; i <= 4; ++i) effect_remove(c, text_.ds8(0x0C9F + static_cast<std::size_t>(i)), "ClearOnDown");
}

// --- drawing: facing, the marker, the missile, a death ---------------------------

void Game::face(Character& c, int dir, int frame, bool no_draw) {
    // Face ovl30 +0x0C5C (0x5F9CC).
    const int k = icon_index(&c);
    if (!icon_visible(&c, true) && f6bda_) field_view(pos_x(c), pos_y(c), 3, 8);
    if ((dir / 4 != c.cb[7] / 4 || frame != 0 || no_draw) && f6bda_) field_erase_icon(0, 0, k);
    c.cb[7] = static_cast<std::uint8_t>(dir);
    if (!no_draw && icon_visible(&c, false) && f6bda_) {
        const Icon& ic = icons_[static_cast<std::size_t>(k - 1)];
        draw_combat_icon(s8(ic.x - field_ox_), s8(ic.y - field_oy_), dir, frame, c.b(0xB9));
        field_present();
    }
}

void Game::marker_from(int slot, int sub, int frame, bool mirror) {
    // 0267:2E33 (0x154A3): the combat image DS:5F86 + 8 slot + 4 sub into
    // frame `frame` of the marker DS:6BCB, mirrored when asked (0267:0F0B),
    // with its colour key.
    Bytes px;
    int w = 24, h = 24;
    if (slot >= 0 && slot < static_cast<int>(combat_icons_.size()) && combat_icons_[static_cast<std::size_t>(slot)].loaded) {
        const CombatIcon& ic = combat_icons_[static_cast<std::size_t>(slot)];
        const IconFrame& f = sub ? ic.action : ic.ready;
        px = f.px;
        w = f.w;
        h = f.h;
        if (mirror && px.size() >= static_cast<std::size_t>(w * h))
            for (int r = 0; r < h; ++r)
                std::reverse(px.begin() + r * w, px.begin() + (r + 1) * w);
    }
    marker_.w = w;
    marker_.h = h;
    marker_.frames[static_cast<std::size_t>(frame & 3)] = px;
}

void Game::anim_icon(int x1, int y1, int x2, int y2, int frames, int delay) {
    // AnimIcon ovl23 +0x0BE1 (0x56AF1..0x571DE): the marker walks the line
    // in thirds of a square. The view is forced onto a centre first; a
    // walk that leaves the view recentres near the target and comes back.
    auto draw_at = [&](int px, int py, int f, bool keep) {
        // 0267:0D06 with flags 5 (masked, the background saved), then the
        // background put back unless `keep`.
        const Bytes& img = marker_.frames[static_cast<std::size_t>(f & 3)];
        const int X = px * 8, Y = py * 8;
        Bytes under;
        for (int r = 0; r < marker_.h; ++r)
            for (int c = 0; c < marker_.w; ++c) {
                const int xx = X + c, yy = Y + r;
                under.push_back(xx >= 0 && xx < kViewPx && yy >= 0 && yy < kViewPx
                                    ? field_buf_[static_cast<std::size_t>(yy * kViewPx + xx)]
                                    : 0);
            }
        if (!img.empty()) field_blit(img, marker_.w, marker_.h, X, Y, true, 8, false);
        if (keep) return;
        field_present();
        pause_ms(delay);
        field_blit(under, marker_.w, marker_.h, X, Y, false, 0, false);
    };
    std::array<int, 0x94> dirs;
    dirs.fill(8);
    int n = 0;
    {
        Line l(3 * x1, 3 * y1, 3 * x2, 3 * y2);
        bool end = false;
        while (!end) {
            end = !l.step(text_);
            if (n >= static_cast<int>(dirs.size())) throw Error("AnimIcon (ovl23 +0x0BE1): the path is past 0x94 steps");
            dirs[static_cast<std::size_t>(n++)] = l.dir;
        }
    }
    const int m = (n - 2) & 0xFF;
    if (m < 2 || n < 2) return;
    const int dx = x2 - x1, dy = y2 - y1;
    int mode, cx, cy;
    if (in_view(x1 - field_ox_, y1 - field_oy_) && in_view(x2 - field_ox_, y2 - field_oy_)) {
        mode = 1;
        cx = field_ox_ + 3;
        cy = field_oy_ + 3;
    } else if (std::abs(dx) <= 6 && std::abs(dy) <= 6) {
        mode = 1;
        cx = x1 + dx / 2;
        cy = y1 + dy / 2;
    } else {
        mode = 0;
        cx = field_ox_ + 3;
        cy = field_oy_ + 3;
    }
    field_view(s8(cx), s8(cy), 0xFF, 8);
    int f = 0, i = 0, ex = 0, ey = 0;
    for (int guard = 0;; ++guard) {
        if (guard > 0x100) {
            // D-0080: the recentring (0x56F27: y2 - 0x18 for a target near
            // the bottom row) can leave the walk short of the target every
            // time: the original loops here forever (run s54_animicon). The
            // marker is put at the target instead and the game goes on.
            combat::diag("D-0080", "AnimIcon (ovl23 +0x0BE1, 0x56ECE): the marker never reaches (" +
                                       std::to_string(x2) + "," + std::to_string(y2) +
                                       "): the original loops forever; drawn at the target");
            if (!in_view(x2 - field_ox_, y2 - field_oy_)) field_view(x2, y2, 3, 8);
            draw_at(3 * (x2 - field_ox_), 3 * (y2 - field_oy_), f, delay == 0);
            break;
        }
        int px = 3 * (x1 - field_ox_) + ex, py = 3 * (y1 - field_oy_) + ey;
        bool stop = false;
        do {
            const int ddx = kDx[dirs[static_cast<std::size_t>(i)]], ddy = kDy[dirs[static_cast<std::size_t>(i)]];
            px += ddx;
            py += ddy;
            ++i;
            stop = px < 0 || px > 0x12 || py < 0 || py > 0x12;
            if (!stop && (delay > 0 || px % 3 == 0 || py % 3 == 0)) {
                draw_at(px, py, f, false);
                if (++f >= frames) f = 0;
            }
            if (!stop && i < m) {
                ex += ddx;
                ey += ddy;
                if (std::abs(ex) == 3) { x1 += sign(ex); ex = 0; }
                if (std::abs(ey) == 3) { y1 += sign(ey); ey = 0; }
            }
        } while (i < m && !stop);
        if (i >= m) {                                   // 0x570F9: at the target
            if (!in_view(x2 - field_ox_, y2 - field_oy_)) field_view(x2, y2, 3, 8);
            draw_at(3 * (x2 - field_ox_), 3 * (y2 - field_oy_), f, delay == 0);
            break;
        }
        // 0x56ECE: the marker left the view: recentre near the target and
        // walk back from it to the view's edge.
        int ax2 = 0, ay2 = 0;
        ex = ey = 0;
        x1 = x2;
        y1 = y2;
        if (x2 + 3 > 0x31) ax2 = x2 - 0x31;
        else if (x2 < 3) ax2 = 3 - x2;
        if (y2 + 3 > 0x18) ay2 = y2 - 0x18;
        else if (y2 < 3) ay2 = 3 - y2;
        field_view(s8(x2 + ax2), s8(y2 + ay2), 0xFF, 8);
        px = 3 * (x2 - field_ox_);
        py = 3 * (y2 - field_oy_);
        i = m;
        stop = false;
        while (!stop) {
            const int ddx = -kDx[dirs[static_cast<std::size_t>(i)]], ddy = -kDy[dirs[static_cast<std::size_t>(i)]];
            px += ddx;
            py += ddy;
            if (px > 0x12) x1 = field_ox_ + 6;
            else if (px < 0) x1 = field_ox_;
            if (py > 0x12) y1 = field_oy_ + 6;
            else if (py < 0) y1 = field_oy_;
            stop = px < 0 || px > 0x12 || py < 0 || py > 0x12;
            if (!stop) {
                ex += ddx;
                ey += ddy;
                if (std::abs(ex) == 3) { x1 += sign(ex); ex = 0; }
                if (std::abs(ey) == 3) { y1 += sign(ey); ey = 0; }
                --i;
            }
        }
        if (mode != 0) break;
    }
    field_present();
}

void Game::projectile(int x1, int y1, int x2, int y2, const Bytes& w) {
    // Projectile ovl11 +0x2B26 (0x43426): the missile's images by the
    // weapon's type (+0x2E) and the direction (nested 0x4337F), then
    // AnimIcon with a 10 ms delay; first its sound by the type
    // (0x4342C..0x434B8; melee and the grenades none).
    switch (w[0x2E]) {
        case 0x05: case 0x07: sound(text_.ds16(0x21F4)); break;            // crossbow, Bolt Gun (12)
        case 0x06: sound(text_.ds16(0x21FC)); break;                       // Needle Gun (17)
        case 0x09: case 0x0D: sound(text_.ds16(0x21F8)); break;            // lasers (14)
        case 0x0A: case 0x0B: sound(text_.ds16(0x21FA)); break;            // rockets (16)
        case 0x0C: sound(text_.ds16(0x21FE)); break;                       // Microwave Gun (18)
        case 0x0E: sound(text_.ds16(0x21F6)); break;                       // Heat Gun (13)
        case 0x0F: sound(text_.ds16(0x21E8)); break;                       // Sonic Stunner (6)
        case 0x10: case 0x11: case 0x12: sound(text_.ds16(0x21E2)); break; // launchers, Plasma Thrower (3)
        default: break;
    }
    const int d = dir_to(s8(x1), s8(y1), s8(x2), s8(y2));
    int frames = 1;
    auto by_dir = [&](int base) {
        if (d & 1) {
            if (d == 3 || d == 5) marker_from(base + 1, 1, 0, d == 5);
            else marker_from(base + 1, 0, 0, d == 7);
        } else {
            marker_from(base + d % 4, d / 4, 0, false);
        }
    };
    switch (w[0x2E]) {
        case 0x11: case 0x0E: by_dir(0x0D); break;
        case 0x09: case 0x0D: by_dir(0x10); break;
        case 0x05: by_dir(0x13); break;
        case 0x07: case 0x06: by_dir(0x16); break;
        case 0x12: case 0x0A: case 0x0B: by_dir(0x19); break;
        case 0x0C: case 0x0F:
            marker_from(0x1E, 0, 0, false);
            marker_from(0x1E, 1, 1, false);
            frames = 2;
            break;
        default:
            marker_from(0x25, 0, 0, false);
            marker_from(0x25, 1, 1, false);
            frames = 2;
            break;
    }
    anim_icon(x1, y1, x2, y2, frames, 10);
}

void Game::death_anim(Character& c) {
    // ovl30 +0x0F97 (0x5FD07): the skull (slot 0x1F) flashes over the
    // icon nine times, a party-side record leaves a body, the icon goes.
    if (mode_ != 5) {
        sound(text_.ds16(0x21E6));                      // 0x5FD18: a death (5)
        game_delay();
        return;
    }
    // 0x5FD25: the guard compares the body entry at an uninitialised
    // local's index with the record eight times and returns when they
    // match: undefined (D-0035). Reference machine: the animation ran
    // (s18x_f2 k0055 shows the body), so the guard didn't match.
    combat::diag("D-0035", "ovl30 +0x0F97 (0x5FD2F): the death animation's body guard reads an uninitialised "
                           "local; the reference machine's outcome (no match) is used");
    const int k = icon_index(&c);
    const int x = pos_x(c), y = pos_y(c);
    if (!icon_visible(&c, true)) field_view(x, y, 3, 8);
    field_erase_icon(0, 0, k);
    sound(text_.ds16(0x21E6));                          // 0x5FDB5: a death (5)
    const Icon ic = icons_[static_cast<std::size_t>(k - 1)];
    for (int f = 0; f <= 8; ++f) {
        for (int i = 0; i <= 3; ++i) {
            int dx, dy;
            if (!footprint(ic.size, i, dx, dy)) continue;
            const int rx = s8(ic.x - field_ox_) + dx, ry = s8(ic.y - field_oy_) + dy;
            if (in_view(rx, ry)) draw_combat_icon(rx, ry, 0, f % 2, 0x1F);
        }
        field_present();
        pause_ms(10);
    }
    if (c.cb[0x0F] == 0) {
        const int t = field_tile_at(x, y);
        bodies_.push_back({&c, x, y, field_underlying(x, y, t)});   // ovl30 +0x14F2
        if (t != 1 && t != 2 && t != 4) field_[static_cast<std::size_t>(y * kFieldW + x)] = 5;   // set CS:0F77
    }
    game_delay();
    field_erase_icon(0, 0, k);
    icons_[static_cast<std::size_t>(k - 1)].size = 0;
    field_view(s8(field_ox_ + 3), s8(field_oy_ + 3), 3, 8);
    c.cb[1] = 0;
    c.cb[3] = 0;
    c.cb[4] = 0;
}

}  // namespace cotd::game
