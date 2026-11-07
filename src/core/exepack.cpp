// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/exepack.hpp"

#include <algorithm>
#include <cstring>

namespace cotd {

Bytes exepack_image(const Bytes& data) {
    if (data.size() < 0x1C || !((data[0] == 'M' && data[1] == 'Z') || (data[0] == 'Z' && data[1] == 'M')))
        throw Error("START.EXE: not an MZ file");
    const std::size_t lastpage = le16(&data[2]), pages = le16(&data[4]);
    const std::size_t hdr_len = le16(&data[8]) * 16u;
    const std::size_t end = pages * 512 - ((512 - lastpage) % 512);
    const std::size_t cs = le16(&data[0x16]);
    if (end > data.size() || hdr_len > end) throw Error("START.EXE: truncated");
    Bytes image(data.begin() + hdr_len, data.begin() + end);
    const std::size_t stub = cs * 16;
    std::size_t dest_len, skip_len = 1;
    if (stub + 18 <= image.size() && image[stub + 14] == 'R' && image[stub + 15] == 'B') {
        dest_len = le16(&image[stub + 12]);
    } else if (stub + 18 <= image.size() && image[stub + 16] == 'R' && image[stub + 17] == 'B') {
        dest_len = le16(&image[stub + 12]);
        skip_len = le16(&image[stub + 14]);
    } else {
        throw Error("START.EXE: no EXEPACK 'RB' signature");
    }
    // Decompress backwards from the end, as the stub does.
    const std::size_t out_len = (dest_len - skip_len + 1) * 16;
    Bytes buf(std::max(out_len, stub), 0);
    std::copy(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(std::min(stub, image.size())), buf.begin());
    auto at = [&](std::ptrdiff_t i) -> std::uint8_t& {
        if (i < 0 || static_cast<std::size_t>(i) >= buf.size()) throw Error("START.EXE: corrupt EXEPACK data");
        return buf[static_cast<std::size_t>(i)];
    };
    std::ptrdiff_t src = static_cast<std::ptrdiff_t>((cs - skip_len) * 16 + 15);
    for (int n = 0; n < 16 && at(src) == 0xFF; ++n) --src;   // 0xFF padding
    std::ptrdiff_t dst = static_cast<std::ptrdiff_t>((dest_len - skip_len) * 16 + 15);
    for (;;) {
        const std::uint8_t op = at(src);
        const std::size_t length = static_cast<std::size_t>(at(src - 1) << 8 | at(src - 2));
        src -= 3;
        if ((op & 0xFE) == 0xB0) {
            const std::uint8_t v = at(src--);
            for (std::size_t i = 0; i < length; ++i) at(dst--) = v;
        } else if ((op & 0xFE) == 0xB2) {
            for (std::size_t i = 0; i < length; ++i) at(dst--) = at(src--);
        } else {
            throw Error("START.EXE: bad EXEPACK command");
        }
        if (op & 1) break;
    }
    buf.resize(out_len);
    return buf;
}

}  // namespace cotd
