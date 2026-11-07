// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/framebuffer.hpp"

#include <algorithm>

namespace cotd {

std::array<Rgb, 16> base_palette() {
    // DS:078E (START.unpacked.exe file offset 0xEE8E): the 16 EGA colours
    // as 8-bit components 0/85/170/255. INT 10h AX=1012h stores them in the
    // 6-bit DAC, which keeps the low 6 bits: 0/21/42/63.
    static constexpr std::uint8_t table[16][3] = {
        {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
        {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
        {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
        {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255}};
    std::array<Rgb, 16> out{};
    for (int i = 0; i < 15; ++i)   // count 15 (ovl9 +0x01BC): entries 0..14
        out[i] = {static_cast<std::uint8_t>(table[i][0] & 0x3F),
                  static_cast<std::uint8_t>(table[i][1] & 0x3F),
                  static_cast<std::uint8_t>(table[i][2] & 0x3F)};
    out[15] = {63, 63, 63};        // BIOS mode-13h default, not the table
    return out;
}

Framebuffer::Framebuffer() : px_(static_cast<std::size_t>(kWidth) * kHeight, 0) {
    const auto base = base_palette();
    std::copy(base.begin(), base.end(), dac_.begin());
}

void Framebuffer::clear(std::uint8_t index) { std::fill(px_.begin(), px_.end(), index); }

void Framebuffer::blit(const Bytes& frame, int w, int h, int x, int y,
                       std::optional<std::uint8_t> key) {
    note_drawn(frame.data(), frame.size());           // tools only (core/image.hpp)
    for (int r = 0; r < h; ++r) {
        const int sy = y + r;
        if (sy < 0 || sy >= kHeight) continue;
        for (int c = 0; c < w; ++c) {
            const int sx = x + c;
            if (sx < 0 || sx >= kWidth) continue;
            const std::uint8_t v = frame[static_cast<std::size_t>(r) * w + c];
            if (key && v == *key) continue;
            px_[static_cast<std::size_t>(sy) * kWidth + sx] = v;
        }
    }
}

void Framebuffer::set_dac(int first, const std::vector<Rgb>& colours) {
    for (std::size_t i = 0; i < colours.size(); ++i) dac_[(first + i) & 0xFF] = colours[i];
}

void Framebuffer::load_palette(const Image& im) { set_dac(im.first, im.palette); }


}  // namespace cotd
