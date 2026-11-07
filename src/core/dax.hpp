// SPDX-License-Identifier: GPL-3.0-or-later
// DAX container (docs/formats/dax.md): directory of entries, each RLE-coded.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd {

struct DaxEntry {
    std::uint8_t id;
    std::uint32_t offset;
    std::uint16_t raw_size;
    std::uint16_t stored_size;
};

class DaxFile {
public:
    // Parses the directory; `where` names the file in error messages.
    DaxFile(Bytes data, std::string where);

    const std::vector<DaxEntry>& entries() const { return entries_; }
    // First entry with this id (0800:08FD searches linearly; first wins).
    const DaxEntry* find(std::uint8_t id) const;
    // Decoded bytes of an entry; Error on a bad stream (D-0002).
    Bytes load(const DaxEntry& e) const;
    std::optional<Bytes> load(std::uint8_t id) const;
    const std::string& where() const { return where_; }

private:
    Bytes data_;
    std::string where_;
    std::vector<DaxEntry> entries_;
};

// RLE as 0800:0731 (linear 0x8731), until raw_size bytes are produced:
// c <= 0x7E copies c+1 bytes; c >= 0x81 repeats the next byte 256-c times;
// 0x7F and 0x80 consume only the control byte. A run past raw_size or a
// stream that ends early is rejected (DIVERGENCE.md D-0002).
Bytes rle_decode(const std::uint8_t* src, std::size_t n, std::size_t raw_size,
                 const std::string& where);

}  // namespace cotd
