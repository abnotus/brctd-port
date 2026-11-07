// SPDX-License-Identifier: GPL-3.0-or-later
#include "ecl/script.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace cotd::ecl {

namespace {

// Handlers' DecodeArgs counts (ecl.md opcode table). -1: no handler, or a
// handler that never advances PC (0x3B, 0x3C, >= 0x4D): the original hangs.
constexpr int kArgs[0x4D] = {
    0, 1, 1, 2, 3, 3, 3, 3, 2, 2, 1, 3, 4, 0, 1, 2,   // 00-0F
    2, 1, 1, 0, 4, 3, 0, 0, 0, 0, 0, 0, 0, 2, 6, 4,   // 10-1F (15: var(3))
    1, 3, 3, 3, 0, 2, 2, 2, 3, 0, 3, 2, 0, 1, 5, 3,   // 20-2F (25-27, 2B: var(2))
    3, 2, 1, 0, 2, 3, 2, 3, 1, 1, 0, -1, -1, 0, 0, 2, // 30-3F (31: var(2))
    2, 2, 0, 1, 0, 2, 2, 0, 1, 6, 0, 1, 2};           // 40-4C

bool is_if(std::uint8_t op) { return op >= 0x16 && op <= 0x1B; }
bool is_jump(std::uint8_t op) { return op == 0x01 || op == 0x02; }
bool is_on(std::uint8_t op) { return op == 0x25 || op == 0x26; }
// No fall-through after EXIT, GOTO, RETURN, LOAD_ECL (stops the block,
// ovl2 0x31554) and CLEAR_EXIT.
bool ends(std::uint8_t op) {
    return op == 0x00 || op == 0x01 || op == 0x13 || op == 0x20 || op == 0x42;
}

// Static value of an operand when it is a constant (GetArgValue).
std::optional<std::uint32_t> static_value(const Operand& a) {
    if (a.type == 0) return a.lo;
    if (a.type == 2 || a.type == 0x81) return a.word();
    if (a.type == 4) {
        std::int32_t low = static_cast<std::int16_t>(a.lo | *a.hi << 8);   // CWD (Q3)
        return static_cast<std::uint32_t>((a.b3 << 24) + (a.b2 << 16) + low);
    }
    return std::nullopt;
}

struct Insn {
    std::uint8_t op = 0;
    std::vector<Operand> args;
    std::optional<std::uint32_t> size;   // nullopt: unknown statically
    std::vector<EclError> problems;
};

}  // namespace

EclError::EclError(const std::string& code_, const std::string& where, std::uint32_t addr_,
                   const std::string& msg)
    : Error(where + " at " + hex(addr_, 4) + " (entry byte 0x" + hex(addr_ - kBase + 2, 4) +
            "): " + msg + " [" + code_ + "]"),
      code(code_),
      addr(addr_) {}

int arg_count(std::uint8_t op) { return op < 0x4D ? kArgs[op] : -1; }

int var_arg_k(std::uint8_t op) {
    switch (op) {
        case 0x15: return 3;
        case 0x25: case 0x26: case 0x27: case 0x2B: case 0x31: return 2;
        default: return 0;
    }
}

int skip_args(std::uint8_t op) {
    switch (op) {
        case 0x01: case 0x02: case 0x0A: case 0x0E: case 0x11: case 0x12: case 0x20:
        case 0x2D: case 0x32: case 0x34: case 0x36: case 0x38: case 0x39: case 0x3C:
        case 0x43: case 0x48: case 0x4B:
            return 1;
        case 0x03: case 0x08: case 0x09: case 0x0F: case 0x10: case 0x1F: case 0x41:
        case 0x40: case 0x1D: case 0x3F: case 0x45: case 0x46: case 0x4C:
            return 2;
        case 0x04: case 0x05: case 0x06: case 0x07: case 0x0B: case 0x0C: case 0x21:
        case 0x28: case 0x2A: case 0x2F: case 0x30: case 0x35: case 0x37: case 0x3B:
        case 0x22: case 0x23:
            return 3;
        case 0x14: return 4;
        case 0x2E: return 5;
        case 0x1E: case 0x49: return 6;
        default: return -1;
    }
}

std::string unpack_text(const std::uint8_t* p, std::size_t n) {
    // ovl15 +0x1198 (0x4C778): three bytes make four six-bit codes.
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        const std::size_t g = std::min<std::size_t>(3, n - i);
        const std::uint8_t b0 = p[i], b1 = g > 1 ? p[i + 1] : 0, b2 = g > 2 ? p[i + 2] : 0;
        const std::uint8_t codes[4] = {static_cast<std::uint8_t>(b0 >> 2),
                                       static_cast<std::uint8_t>((b0 & 3) << 4 | b1 >> 4),
                                       static_cast<std::uint8_t>((b1 & 0xF) << 2 | b2 >> 6),
                                       static_cast<std::uint8_t>(b2 & 0x3F)};
        for (std::size_t k = 0; k < g + 1; ++k)   // n bytes give n+1 codes
            // ovl15 +0x073C TextChar (0x4BD1C): a code below 0x20 + 0x40, else itself.
            if (codes[k]) out += static_cast<char>(codes[k] < 0x20 ? codes[k] + 0x40 : codes[k]);
    }
    return out;
}

Script::Script(const Bytes& raw, std::string where) : where_(std::move(where)) {
    if (raw.size() < 2) throw Error(where_ + ": block shorter than its 2-byte prefix");
    mem_.assign(raw.begin() + 2, raw.end());
    limit_ = kBase + static_cast<std::uint32_t>(std::max<std::size_t>(kArea, mem_.size()));
    if (mem_.size() < kArea) mem_.resize(kArea, 0);
}

std::uint8_t Script::byte(std::uint32_t addr) const {
    if (addr < kBase || addr >= limit_) throw EclError("D-0007", where_, addr, "read past the script area");
    return mem_[addr - kBase];
}

std::vector<Operand> Script::decode_args(std::uint32_t& pc, int n) const {
    std::vector<Operand> args;
    for (int k = 0; k < n; ++k) {
        Operand a;
        a.type = byte(pc + 1);
        a.lo = byte(pc + 2);
        pc += 2;
        if (a.type == 1 || a.type == 2 || a.type == 3 || a.type == 5 || a.type == 0x81) {
            pc += 1;
            a.hi = byte(pc);
        } else if (a.type == 4) {
            a.hi = byte(pc + 1);
            a.b2 = byte(pc + 2);
            a.b3 = byte(pc + 3);
            pc += 3;
        } else if (a.type == 0x80) {
            std::vector<std::uint8_t> packed;
            for (int i = 0; i < a.lo; ++i) packed.push_back(byte(pc + 1 + i));
            pc += a.lo;
            a.text = unpack_text(packed.data(), packed.size());
        }
        args.push_back(std::move(a));
    }
    return args;
}

std::array<std::uint16_t, 5> Script::entries() const {
    std::array<std::uint16_t, 5> out{};
    std::uint32_t pc = kBase;
    for (auto& e : out) {
        auto a = decode_args(pc, 1);
        e = a[0].word().value_or(0);
        pc += 1;
    }
    return out;
}

std::uint32_t Script::skip_size(std::uint32_t addr) const {
    const int n = skip_args(byte(addr));
    if (n < 0) return 1;
    std::uint32_t pc = addr;
    decode_args(pc, n);
    return pc + 1 - addr;
}

std::vector<EclError> Script::validate() const {
    auto decode = [this](std::uint32_t addr) {
        Insn ins;
        try {
            ins.op = byte(addr);
            auto err = [&](const char* code, const std::string& msg) {
                ins.problems.emplace_back(code, where_, addr, msg);
            };
            const int n = arg_count(ins.op);
            if (n < 0) {
                err("D-0003", "opcode 0x" + hex(ins.op, 2) + " hangs the original");
                return ins;
            }
            std::uint32_t pc = addr;
            ins.args = decode_args(pc, n);
            if (const int k = var_arg_k(ins.op)) {
                (void)k;
                auto m = static_value(ins.args.back());
                if (!m) return ins;   // operand count from a variable: run time only
                // DecodeArgs takes the count as a byte (0x4B5EA); past
                // kMaxOperands it writes outside its arrays (D-0078).
                if ((*m & 0xFF) > kMaxOperands) {
                    err("D-0078", "operand count " + std::to_string(*m & 0xFF) + " past the original's " +
                                      std::to_string(kMaxOperands) + "-operand arrays");
                    return ins;
                }
                auto rest = decode_args(pc, static_cast<int>(*m & 0xFF));
                ins.args.insert(ins.args.end(), rest.begin(), rest.end());
            }
            ins.size = pc + 1 - addr;
            for (const auto& a : ins.args)
                if (!(a.type <= 5 || a.type == 0x80 || a.type == 0x81))
                    err("D-0006", "operand type 0x" + hex(a.type, 2) + " has no value");
            if (is_jump(ins.op) || is_on(ins.op)) {
                const std::size_t from = is_jump(ins.op) ? 0 : 2;
                const std::size_t to = is_jump(ins.op) ? 1 : ins.args.size();
                for (std::size_t i = from; i < to; ++i)
                    if (!ins.args[i].word()) {
                        err("D-0004", "jump through an operand without a high byte");
                        break;
                    }
            }
            if (ins.op == 0x03) {
                const int strs = ins.args[0].is_string() + ins.args[1].is_string();
                if (strs == 1) err("D-0005", "COMPARE of a string with a non-string");
            }
        } catch (const EclError& e) {
            ins.size.reset();
            ins.problems.push_back(e);
        }
        return ins;
    };

    std::map<std::uint32_t, Insn> insns;
    std::vector<std::uint32_t> todo;
    for (auto e : entries()) todo.push_back(e);
    while (!todo.empty()) {
        const std::uint32_t a = todo.back();
        todo.pop_back();
        if (insns.count(a)) continue;
        Insn& ins = insns[a] = decode(a);
        if (!ins.size) continue;
        const std::uint32_t nxt = a + *ins.size;
        if (is_jump(ins.op)) {
            if (auto w = ins.args[0].word()) todo.push_back(*w);
        } else if (is_on(ins.op)) {
            for (std::size_t i = 2; i < ins.args.size(); ++i)
                if (auto w = ins.args[i].word()) todo.push_back(*w);
        }
        if (is_if(ins.op)) {
            try {
                todo.push_back(nxt + skip_size(nxt));
            } catch (const EclError& e) {
                ins.problems.push_back(e);
            }
        }
        const bool q8 = ins.op == 0x49 && static_value(ins.args[1]) == 0u;
        if (!ends(ins.op) && !q8) todo.push_back(nxt);
    }
    std::vector<EclError> out;
    for (auto& [addr, ins] : insns)
        for (auto& e : ins.problems) out.push_back(e);
    return out;
}

}  // namespace cotd::ecl
