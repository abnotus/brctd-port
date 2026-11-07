// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/dax.hpp"

namespace cotd {

Bytes rle_decode(const std::uint8_t* src, std::size_t n, std::size_t raw_size,
                 const std::string& where) {
    Bytes out;
    out.reserve(raw_size);
    std::size_t i = 0;
    while (out.size() < raw_size) {
        if (i >= n) throw Error(where + ": RLE stream ended early [D-0002]");
        const std::uint8_t c = src[i++];
        std::size_t run;
        if (c <= 0x7E) {
            run = c + 1u;
            if (i + run > n) throw Error(where + ": RLE stream ended early [D-0002]");
        } else if (c >= 0x81) {
            run = 256u - c;
            if (i >= n) throw Error(where + ": RLE stream ended early [D-0002]");
        } else {
            continue;  // 0x7F, 0x80: no-op
        }
        if (out.size() + run > raw_size)
            throw Error(where + ": RLE run at stream byte " + std::to_string(i - 1) +
                        " overruns raw size " + std::to_string(raw_size) + " [D-0002]");
        if (c <= 0x7E) {
            out.insert(out.end(), src + i, src + i + run);
            i += run;
        } else {
            out.insert(out.end(), run, src[i]);
            i += 1;
        }
    }
    return out;
}

// 0800:07ED Dax.Open (0x187ED): the directory read, a word's length then the entries.
DaxFile::DaxFile(Bytes data, std::string where) : data_(std::move(data)), where_(std::move(where)) {
    if (data_.size() < 2) throw Error(where_ + ": too short for a DAX directory");
    const std::size_t dsize = le16(data_.data());
    if (dsize % 9) throw Error(where_ + ": directory size is not a multiple of 9");
    if (2 + dsize > data_.size()) throw Error(where_ + ": directory past end of file");
    for (std::size_t k = 0; k < dsize / 9; ++k) {
        const std::uint8_t* p = data_.data() + 2 + 9 * k;
        entries_.push_back({p[0], le32(p + 1), le16(p + 5), le16(p + 7)});
    }
}

const DaxEntry* DaxFile::find(std::uint8_t id) const {
    for (const auto& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

Bytes DaxFile::load(const DaxEntry& e) const {
    const std::size_t base = 2 + entries_.size() * 9;
    const std::size_t start = base + e.offset;
    const std::string where = where_ + " #" + std::to_string(e.id);
    if (start > data_.size()) throw Error(where + ": entry offset past end of file");
    // The game reads until raw_size is produced and never looks at
    // stored_size; the rest of the file bounds the stream.
    return rle_decode(data_.data() + start, data_.size() - start, e.raw_size, where);
}

std::optional<Bytes> DaxFile::load(std::uint8_t id) const {
    const DaxEntry* e = find(id);
    if (!e) return std::nullopt;
    return load(*e);
}

}  // namespace cotd
