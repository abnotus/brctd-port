// SPDX-License-Identifier: GPL-3.0-or-later
// The 320x200 mode-13h screen: palette indices plus the 256-entry DAC.
#pragma once

#include <array>
#include <optional>
#include <string>

#include "core/common.hpp"
#include "core/image.hpp"

namespace cotd {

class Framebuffer {
public:
    static constexpr int kWidth = 320, kHeight = 200;

    Framebuffer();

    std::uint8_t* row(int y) { return &px_[static_cast<std::size_t>(y) * kWidth]; }
    const Bytes& pixels() const { return px_; }
    const std::array<Rgb, 256>& dac() const { return dac_; }

    void clear(std::uint8_t index = 0);
    // Draw a w x h frame at (x, y); pixels equal to key are skipped.
    void blit(const Bytes& frame, int w, int h, int x, int y,
              std::optional<std::uint8_t> key = std::nullopt);
    // Gfx.SetDacRange 0267:286C: DAC[first..first+count) := colours.
    void set_dac(int first, const std::vector<Rgb>& colours);
    void set_dac(int index, Rgb c) { dac_[index & 0xFF] = c; }
    // An image's own palette into the DAC (its first..first+count range).
    void load_palette(const Image& im);


private:
    Bytes px_;
    std::array<Rgb, 256> dac_{};
};

// DAC after mode 13h and Game.Init (ovl9 +0x0101): entries
// 0..14 from the 8-bit table DS:078E as the DAC keeps them (low 6 bits);
// entry 15 is never written by the game and keeps the BIOS mode-13h
// default (63, 63, 63) (docs/formats/images.md "Base palette").
std::array<Rgb, 16> base_palette();

}  // namespace cotd
