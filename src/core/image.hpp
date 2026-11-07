// SPDX-License-Identifier: GPL-3.0-or-later
// Graphics DAX entries (docs/formats/images.md), VGA output only.
#pragma once

#include <array>
#include <optional>
#include <functional>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;   // 6-bit DAC values
    bool operator==(const Rgb&) const = default;
};

enum class ImageKind { Generic, Anim, Font };

struct Image {
    ImageKind kind = ImageKind::Generic;
    int width = 0, height = 0;
    int x = 0, y = 0;                     // header position
    int first = 0, count = 0;             // palette range the image brings
    std::vector<Rgb> palette;             // count entries, index first + i
    std::vector<Bytes> frames;            // width*height palette indices each
    int base = 0;                         // anim: base frame (DS:6A60)
    int loop = 0;                         // anim: the frame the cycle restarts at (DS:6A61)
    std::vector<int> delays;              // anim: per-frame delay byte
};

// Generic image: Gfx.ParseImageHeader 0267:00EC + Gfx.ConvertPixels 0267:0588.
Image decode_generic(const Bytes& raw, const std::string& where);
// Animated picture (PIC*, SPRIT*): Game.LoadAnimPicture ovl27 +0x02D0.
Image decode_anim(const Bytes& raw, const std::string& where);
// Font 8X8D1 #201: 8 bytes per glyph, bit 7 leftmost; pixels 0/1.
Image decode_font(const Bytes& raw, const std::string& where);
// By file name, as daximg.decode: font entry, PIC/SPRIT anims, else generic.

// Gfx.UnpackFrame 0129:0103: c < 0x80 copies c+1 bytes, else repeats the
// next byte 257-c times, until the packed bytes are consumed.
Bytes unpack_frame(const std::uint8_t* p, std::size_t n, const std::string& where);

// A SPRIT frame as the loader prepares it: colours 0 and 0x0D swapped;
// drawn with key 0x0D.
Bytes sprite_view(const Bytes& frame);

inline void register_frames(const Image&, const std::string&) {}
inline void note_drawn(const std::uint8_t*, std::size_t) {}

}  // namespace cotd
