// SPDX-License-Identifier: GPL-3.0-or-later
// The 88x88 3D view (docs/formats/view3d.md), composed as ovl28 View.Draw.
#pragma once

#include <vector>

#include "core/common.hpp"
#include "core/image.hpp"

namespace cotd {

struct TileSet {
    int base = 0, top = 0;          // DS:2560[k], DS:256E[k]
    const Image* image = nullptr;   // DS:5F5A + 4k; nullptr = not loaded
    std::uint8_t key = 13;          // colour key at image +0x16
};

class View3d {
public:
    static constexpr int kSize = 88;

    // planes: GEO planes 0 and 1 (256 bytes each, y*16+x); walls: the wall
    // buffer [DS:6A22] (wall w at (w-1)*0x9C); tilesets: in table order;
    // back: the 88x88 background; block: DS:82F4 (off-map rule).
    View3d(const std::uint8_t* plane0, const std::uint8_t* plane1, const Bytes& walls,
           std::vector<TileSet> tilesets, const Bytes& back, int block);

    // View.Draw ovl28 +0x065E: facing 0/2/4/6 = N/E/S/W.
    Bytes draw(int x, int y, int facing);
    // The overhead map (AREA), View.AreaMap ovl28 +0x0000: what View.Draw
    // draws instead while DS:6A2E is set.
    Bytes area(int x, int y, int facing);
    Bytes tile_pixels(int n) const;   // tile n's 8 x 8 pixels (empty: none)

private:
    int wall(int dir, int y, int x) const;          // View.WallAt ovl28 +0x04E0
    void tile(int n, int row, int col, bool masked = true);   // View.DrawTile ovl33 +0x019B
    void piece(int kind, int w, int row, int col);  // View.DrawPiece ovl28 +0x0272

    const std::uint8_t* p0_;
    const std::uint8_t* p1_;
    const Bytes& walls_;
    std::vector<TileSet> sets_;
    Bytes px_;
    int block_;
};

}  // namespace cotd
