// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/view3d.hpp"

#include <algorithm>

namespace cotd {

namespace {

// Constant tables of the data segment (equal in the EXE and in RAM).
constexpr int START[10] = {0x00, 0x02, 0x06, 0x0A, 0x16, 0x26, 0x36, 0x6E, 0x84, 0x9A};  // DS:0D96
constexpr int WIDTH[10] = {1, 1, 1, 3, 2, 2, 7, 2, 2, 1};                                // DS:0DA0
constexpr int HEIGHT[10] = {2, 4, 4, 4, 8, 8, 8, 11, 11, 2};                             // DS:0DAA
constexpr int COL[10] = {5, 4, 6, 4, 2, 7, 2, 0, 9, 5};                                  // DS:0DB4
constexpr int ROW[10] = {4, 3, 3, 3, 1, 1, 1, 0, 0, 4};                                  // DS:0DC8
constexpr int DX[9] = {0, 1, 1, 1, 0, -1, -1, -1, 0};                                    // DS:257C
constexpr int DY[9] = {-1, -1, 0, 1, 1, 1, 0, -1, 0};                                    // DS:2585
constexpr int kWallDef = 0x9C;
constexpr int kCells = 11;

}  // namespace

View3d::View3d(const std::uint8_t* plane0, const std::uint8_t* plane1, const Bytes& walls,
               std::vector<TileSet> tilesets, const Bytes& back, int block)
    : p0_(plane0), p1_(plane1), walls_(walls), sets_(std::move(tilesets)), px_(back), block_(block) {
    px_.resize(kSize * kSize);
}

int View3d::wall(int d, int y, int x) const {
    // The original's coordinates are signed bytes: a sum past 127 wraps
    // (runs s75_sweep_x/y: x 126..129 two squares ahead, D-0073).
    x = static_cast<std::int8_t>(x);
    y = static_cast<std::int8_t>(y);
    if (!(0 <= x && x < 16 && 0 <= y && y < 16)) {
        if (block_ == 0 || block_ == 0x50) return 0;
        // Not modulo: above 15 becomes 0, below 0 becomes 15 (0x5D4DB..).
        x = x < 0 ? 15 : x >= 16 ? 0 : x;
        y = y < 0 ? 15 : y >= 16 ? 0 : y;
    }
    const int i = y * 16 + x;
    switch (d) {
        case 0: return p0_[i] >> 4;
        case 2: return p0_[i] & 0xF;
        case 4: return p1_[i] >> 4;
        case 6: return p1_[i] & 0xF;
        default: return 0;
    }
}

Bytes View3d::tile_pixels(int n) const {
    for (const TileSet& s : sets_) {
        if (s.top < n) continue;
        if (!s.image || n == 0 || n < s.base) return {};
        const std::size_t f = static_cast<std::size_t>(n - s.base);
        return f < s.image->frames.size() ? s.image->frames[f] : Bytes{};
    }
    return {};
}

void View3d::tile(int n, int row, int col, bool masked) {
    // masked: DrawTile's last argument; Gfx.DrawMasked (0267:0BC9) skips
    // the colour key only when its bit 0 is set (0x1323F), else copies.
    for (const TileSet& s : sets_) {
        if (s.top < n) continue;
        // First set whose top covers n. A set that isn't loaded draws
        // nothing: DrawTile tests the image pointer (ovl33 0x60F66..0x60F70).
        if (!s.image || n == 0 || n < s.base) return;
        const std::size_t f = static_cast<std::size_t>(n - s.base);
        if (f >= s.image->frames.size()) return;
        const Bytes& px = s.image->frames[f];
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c) {
                const std::uint8_t v = px[r * 8 + c];
                // 0267:39A0 (0x16010): DrawMasked's VGA keyed path: as 0267:38FD, colour key (image +0x16) skipped (row loop 0267:3C46)
                // 0267:38FD (0x15F6D): DrawMasked's VGA opaque path, x, y in cells, frame, no clipping (row loop 0267:3BE7)
                if (!masked || v != s.key) px_[(row * 8 + r) * kSize + col * 8 + c] = v;
            }
        return;
    }
}

void View3d::piece(int kind, int w, int row, int col) {
    std::size_t b = static_cast<std::size_t>(w - 1) * kWallDef + START[kind];
    for (int a = row; a < row + HEIGHT[kind]; ++a)
        for (int c = col; c < col + WIDTH[kind]; ++c) {
            if (0 <= a && a < kCells && 0 <= c && c < kCells && b < walls_.size()) {
                const int t = walls_[b];
                if (t) tile(t, a, c);
            }
            ++b;
        }
}

Bytes View3d::area(int x, int y, int f) {
    // View.AreaMap ovl28 +0x0000 (0x5CFD0): an 11 x 11 window of the 16 x 16
    // map, its corner clamped to 0..5 so it never leaves the map; per
    // square the tile 0x100 + 1 (N) + 2 (E) + 4 (S) + 8 (W) for each side
    // with a wall (WallAt > 0), then the party's tile 0x110 + facing / 2.
    // DrawTile(col + 2, row + 2, id, 1, 0): unmasked, into the buffer.
    const int ox = std::clamp(x - 5, 0, 5), oy = std::clamp(y - 5, 0, 5);
    for (int j = 0; j <= 10; ++j)
        for (int i = 0; i <= 10; ++i) {
            const int mx = ox + i, my = oy + j;
            int t = 0x100;
            if (wall(0, my, mx) > 0) t += 1;
            if (wall(2, my, mx) > 0) t += 2;
            if (wall(4, my, mx) > 0) t += 4;
            if (wall(6, my, mx) > 0) t += 8;
            tile(t, j, i, false);
        }
    // A square off the map (D-0073) puts the party's tile outside the
    // window; the original draws it on the screen there (Game::draw_view
    // does, from tile_pixels), not in this buffer.
    if (0 <= y - oy && y - oy <= 10 && 0 <= x - ox && x - ox <= 10) tile(0x110 + f / 2, y - oy, x - ox, false);
    return px_;
}

Bytes View3d::draw(int x, int y, int f) {
    const int L = (f + 6) % 8, B = (f + 4) % 8, R = (f + 2) % 8;
    const int x0 = x + DX[f] * 2, y0 = y + DY[f] * 2;
    // Band 2: facing walls two squares ahead, then the edge pieces.
    {
        struct Pass { int side, other, step; };
        for (const Pass& ps : {Pass{L, R, -2}, Pass{R, L, 2}}) {
            int fx = x0, fy = y0, off = 0, prev = 0;
            const int adj = ps.step < 0 ? 1 : -1;
            for (int k = 0; k < 4; ++k) {
                const int w = wall(f, fy, fx);
                const bool inside = 0 <= fx && fx < 16 && 0 <= fy && fy < 16;
                if (!inside && wall(ps.other, fy, fx) == 0) prev = 0;
                if (w == 0) {
                    if (prev && wall(ps.side, fy - DY[ps.side], fx - DX[ps.side]))
                        piece(9, prev, ROW[9], COL[9] + off + adj);
                    prev = 0;
                } else {
                    if (prev) piece(9, prev, ROW[9], COL[9] + off + adj);
                    piece(0, w, ROW[0], COL[0] + off);
                    prev = w;
                }
                off += ps.step;
                fx += DX[ps.side];
                fy += DY[ps.side];
            }
        }
        struct Side { int side, kind, step, adj; };
        for (const Side& sd : {Side{L, 1, -2, -1}, Side{R, 2, 2, 1}}) {
            int fx = x0, fy = y0, off = 0;
            for (int i = 0; i < 3; ++i) {
                const int w = wall(sd.side, fy, fx);
                if (w) piece(sd.kind, w, ROW[sd.kind], COL[sd.kind] + off + (i == 0 ? 0 : sd.adj));
                off += sd.step;
                fx += DX[sd.side];
                fy += DY[sd.side];
            }
        }
    }
    // Band 1: one square ahead.
    const int x1 = x0 + DX[B], y1 = y0 + DY[B];
    {
        struct Pass { int side, kside, off, step; };
        for (const Pass& ps : {Pass{L, 4, -6, 3}, Pass{R, 5, 6, -3}}) {
            int fx = x1 + 2 * DX[ps.side], fy = y1 + 2 * DY[ps.side], off = ps.off;
            const int mv = ps.side == L ? R : L;
            for (int k = 0; k < 3; ++k) {
                if (const int w = wall(f, fy, fx)) piece(3, w, ROW[3], COL[3] + off);
                if (const int w2 = wall(ps.side, fy, fx)) piece(ps.kside, w2, ROW[ps.kside], COL[ps.kside] + off);
                off += ps.step;
                fx += DX[mv];
                fy += DY[mv];
            }
        }
    }
    // Band 0: the party's own row.
    const int x2 = x1 + DX[B], y2 = y1 + DY[B];
    {
        struct Pass { int side, kside, off, step; };
        for (const Pass& ps : {Pass{L, 7, -7, 7}, Pass{R, 8, 7, -7}}) {
            int fx = x2 + DX[ps.side], fy = y2 + DY[ps.side], off = ps.off;
            const int mv = ps.side == L ? R : L;
            for (int k = 0; k < 2; ++k) {
                if (const int w = wall(f, fy, fx)) piece(6, w, ROW[6], COL[6] + off);
                if (const int w2 = wall(ps.side, fy, fx)) piece(ps.kside, w2, ROW[ps.kside], COL[ps.kside] + off);
                off += ps.step;
                fx += DX[mv];
                fy += DY[mv];
            }
        }
    }
    return px_;
}

}  // namespace cotd
