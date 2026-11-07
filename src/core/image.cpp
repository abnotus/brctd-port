// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/image.hpp"

#include <cstdlib>
#include <unordered_map>

#include "core/files.hpp"

namespace cotd {

namespace {

constexpr std::size_t kAnimFrame = 0x1E40;   // 88 x 88, fixed in the loader

std::vector<Rgb> read_palette(const Bytes& raw, std::size_t p, int count,
                              const std::string& where) {
    if (p + 3u * count > raw.size()) throw Error(where + ": palette past end of entry");
    std::vector<Rgb> pal;
    for (int i = 0; i < count; ++i) {
        Rgb c{raw[p + 3 * i], raw[p + 3 * i + 1], raw[p + 3 * i + 2]};
        if (c.r > 0x3F || c.g > 0x3F || c.b > 0x3F)
            throw Error(where + ": palette value above 6 bits");
        pal.push_back(c);
    }
    return pal;
}

}  // namespace

Image decode_generic(const Bytes& raw, const std::string& where) {
    if (raw.size() < 10) throw Error(where + ": image header cut short");
    Image im;
    im.height = raw[0];
    im.width = raw[1] * 8;
    im.x = le16(&raw[2]);
    im.y = le16(&raw[4]);
    const int n = le16(&raw[6]);
    im.first = raw[8];
    im.count = raw[9] + 1;
    std::size_t p = 10;
    im.palette = read_palette(raw, p, im.count, where);
    p += 3u * im.count + im.count / 2;                     // EGA nibble map
    p += (im.first == 0x20 && im.count == 0x70) ? 32 : 4;  // CGA 2-bit map
    const std::size_t size = static_cast<std::size_t>(im.width) * im.height;
    if (size == 0 || p + n * size != raw.size())
        throw Error(where + ": image size mismatch");
    for (int k = 0; k < n; ++k)
        im.frames.emplace_back(raw.begin() + p + k * size, raw.begin() + p + (k + 1) * size);
    register_frames(im, where);
    return im;
}

Bytes unpack_frame(const std::uint8_t* src, std::size_t n, const std::string& where) {
    Bytes out;
    std::size_t i = 0;
    while (i < n) {
        const std::uint8_t c = src[i];
        if (c < 0x80) {
            if (i + 2 + c > n) throw Error(where + ": packed literal runs past the frame");
            out.insert(out.end(), src + i + 1, src + i + 2 + c);
            i += c + 2u;
        } else {
            if (i + 1 >= n) throw Error(where + ": packed run cut short");
            out.insert(out.end(), 0x101u - c, src[i + 1]);
            i += 2;
        }
    }
    return out;
}

Image decode_anim(const Bytes& raw, const std::string& where) {
    if (raw.size() < 11) throw Error(where + ": picture header cut short");
    Image im;
    im.kind = ImageKind::Anim;
    im.height = le16(&raw[0]);
    im.width = le16(&raw[2]) * 8;
    im.x = le16(&raw[4]);
    im.y = le16(&raw[6]);
    const int nframes = std::min<int>(raw[8], 8) + 1;
    im.first = raw[9];
    im.count = raw[10];                                    // count, not count-1
    std::size_t p = 11;
    im.palette = read_palette(raw, p, im.count, where);
    p += 3u * im.count + im.count / 2 + 4;
    if (p + 2 + 3u * nframes > raw.size()) throw Error(where + ": frame table cut short");
    im.base = raw[p];                                      // DS:6A60
    im.loop = raw[p + 1];                                  // DS:6A61
    p += 2;
    std::vector<std::size_t> sizes;
    for (int k = 0; k < nframes; ++k) {
        sizes.push_back(le16(&raw[p]));
        im.delays.push_back(raw[p + 2]);
        p += 3;
    }
    if (static_cast<std::size_t>(im.width) * im.height != kAnimFrame)
        throw Error(where + ": picture frame is not 88x88");
    std::vector<Bytes> plain;
    for (std::size_t sz : sizes) {
        if (p + sz > raw.size()) throw Error(where + ": picture size mismatch");
        plain.push_back(unpack_frame(raw.data() + p, sz, where));
        if (plain.back().size() != kAnimFrame)
            throw Error(where + ": frame doesn't unpack to 0x1E40 bytes");
        p += sz;
    }
    if (p != raw.size() || im.base >= nframes) throw Error(where + ": picture size mismatch");
    // 0129:0072 (0x11302): frame k into DS:6A88: the base frame moved, another through 0129:0000; a nil frame does nothing (the port decodes at load)
    const Bytes& b = plain[im.base];
    for (int k = 0; k < nframes; ++k) {
        Bytes f = plain[k];
        if (k != im.base)
            // 0129:0000 (0x11290): a non-base frame: unpacked (0129:0103) and XORed with the base frame (0129:00DB)
            for (std::size_t i = 0; i < f.size(); ++i) f[i] ^= b[i];   // 0129:00DB
        im.frames.push_back(std::move(f));
    }
    register_frames(im, where);
    return im;
}

Image decode_font(const Bytes& raw, const std::string& where) {
    if (raw.size() % 8) throw Error(where + ": font size not a multiple of 8");
    Image im;
    im.kind = ImageKind::Font;
    im.width = im.height = 8;
    for (std::size_t g = 0; g < raw.size() / 8; ++g) {
        Bytes px;
        for (int r = 0; r < 8; ++r)
            for (int b = 0; b < 8; ++b) px.push_back((raw[8 * g + r] >> (7 - b)) & 1);
        im.frames.push_back(std::move(px));
    }
    return im;
}


Bytes sprite_view(const Bytes& frame) {
    Bytes out(frame);
    for (auto& v : out) v = v == 0 ? 0x0D : v == 0x0D ? 0 : v;
    return out;
}


}  // namespace cotd
