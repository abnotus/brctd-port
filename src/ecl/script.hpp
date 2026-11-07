// SPDX-License-Identifier: GPL-3.0-or-later
// ECL script blocks (docs/formats/ecl.md): decoding and the load-time half
// of the quirk policy (DIVERGENCE.md D-0003 .. D-0007).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd::ecl {

constexpr std::uint32_t kBase = 0x8000;
constexpr std::uint32_t kArea = 0x2401;   // LoadEcl: FillChar([3BD4], 0x2401, 0)
// DecodeArgs' operand arrays (type DS:6D99, hi 6DD9, lo 6E19, b3 6E59, b2
// 6E99, index k = 1..n): 0x40 bytes apart, so k past 63 leaves them (D-0078).
constexpr int kMaxOperands = 63;

// One decoded operand (DecodeArgs, ovl15 +0x0000).
struct Operand {
    std::uint8_t type = 0;
    std::uint8_t lo = 0;
    std::optional<std::uint8_t> hi;   // set only for 3-byte operands and type 4
    std::uint8_t b2 = 0, b3 = 0;      // type 4
    std::string text;                 // type 0x80
    bool is_string() const { return type >= 0x80; }
    // ovl15 +0x0717 MakeWord (0x4BCF7): lo + (hi << 8).
    std::optional<std::uint16_t> word() const {
        if (!hi) return std::nullopt;
        return static_cast<std::uint16_t>(lo | *hi << 8);
    }
};

// A construct the port rejects; message names block and offset.
struct EclError : Error {
    EclError(const std::string& code, const std::string& where, std::uint32_t addr,
             const std::string& msg);
    std::string code;
    std::uint32_t addr;
};

// Operand count per opcode; -1 = no handler (hangs the original);
// var(k) opcodes are marked in kVarArgs.
int arg_count(std::uint8_t op);
int var_arg_k(std::uint8_t op);        // k for var(k) opcodes, else 0
int skip_args(std::uint8_t op);        // ovl15 +0x1BEB table; -1 = 1 byte

std::string unpack_text(const std::uint8_t* p, std::size_t n);   // ovl15 +0x1198

class Script {
public:
    Script(const Bytes& raw, std::string where);

    // Byte at a script address; bytes past the block read as the zeros
    // LoadEcl left; past the 0x2401-byte area: rejected (D-0007).
    std::uint8_t byte(std::uint32_t addr) const;
    // Mutable script memory (region 3 variables live here).
    Bytes& mem() { return mem_; }
    const Bytes& mem() const { return mem_; }
    const std::string& where() const { return where_; }

    // DecodeArgs from pc (on the opcode or the byte before the first
    // operand); returns the operands and the pc of the last byte read.
    std::vector<Operand> decode_args(std::uint32_t& pc, int n) const;
    // EclInit: the five entry records' raw hi:lo.
    std::array<std::uint16_t, 5> entries() const;
    // Bytes the skip routine steps over at addr (Q1 reproduced).
    std::uint32_t skip_size(std::uint32_t addr) const;
    // Static checks from the five entries (Script.validate in ecl.py):
    // every rejected construct in reachable code.
    std::vector<EclError> validate() const;

private:
    Bytes mem_;     // 0x2401 bytes: the block, then zeros
    std::uint32_t limit_;
    std::string where_;
};

}  // namespace cotd::ecl
