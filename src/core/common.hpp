// SPDX-License-Identifier: GPL-3.0-or-later
// Shared basics: byte buffers, little-endian reads, the engine's error type.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cotd {

using Bytes = std::vector<std::uint8_t>;

// Every load or run-time check the port makes (DIVERGENCE.md, quirk
// policy) raises this; the message names the file, entry and offset.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A fatal condition the original also stops on (e.g. a missing DAX entry);
// the message is the original's own text (core/gametext.hpp).
struct Fatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}
inline std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
inline void put16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::uint8_t* p, std::uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

std::string hex(std::uint32_t v, int digits);

}  // namespace cotd
