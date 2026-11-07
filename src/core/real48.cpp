// SPDX-License-Identifier: GPL-3.0-or-later
// TP 5.5 6-byte Real (core/real48.hpp). Each routine follows the RTL's own
// instruction sequence in START.EXE's System unit (addresses: seg:off in
// 0BE4, and flat = 0x10000 + 0xBE40 + off, as tools/asmview.py prints).
// Register names in the comments are the RTL's; where a 48-bit shift or add
// stands for a chain of SHR/RCR or ADD/ADC it is the same operation on the
// same bits.
#include "core/real48.hpp"

#include <cmath>
#include <utility>

namespace cotd::real48 {

namespace {

constexpr std::uint64_t kMask48 = 0xFFFFFFFFFFFFull;

std::uint8_t lo8(std::uint16_t r) { return static_cast<std::uint8_t>(r); }
std::uint8_t hi8(std::uint16_t r) { return static_cast<std::uint8_t>(r >> 8); }
void set_lo(std::uint16_t& r, std::uint8_t v) { r = static_cast<std::uint16_t>((r & 0xFF00) | v); }
void set_hi(std::uint16_t& r, std::uint8_t v) { r = static_cast<std::uint16_t>((r & 0x00FF) | (v << 8)); }

std::uint16_t adc16(std::uint16_t a, std::uint16_t b, bool& c) {
    const std::uint32_t s = std::uint32_t{a} + b + (c ? 1u : 0u);
    c = (s >> 16) != 0;
    return static_cast<std::uint16_t>(s);
}
std::uint16_t sbb16(std::uint16_t a, std::uint16_t b, bool& c) {
    const std::uint32_t s = std::uint32_t{a} - b - (c ? 1u : 0u);
    c = ((s >> 16) & 1) != 0;
    return static_cast<std::uint16_t>(s);
}

std::uint64_t pack(std::uint16_t dx, std::uint16_t bx, std::uint16_t ax) {
    return std::uint64_t{dx} << 32 | std::uint64_t{bx} << 16 | ax;
}
void unpack(std::uint64_t m, std::uint16_t& dx, std::uint16_t& bx, std::uint16_t& ax) {
    dx = static_cast<std::uint16_t>(m >> 32);
    bx = static_cast<std::uint16_t>(m >> 16);
    ax = static_cast<std::uint16_t>(m);
}

// DX:BX:AX at the routine's return, and the carry flag it returns with
// (callers inside the RTL ignore the carry; the public wrappers raise).
struct Res {
    Real r;
    bool cf = false;
};

constexpr Real kZero{};

// FP48 0BE4:098F (flat 0x1C7CF-0x1C891): DX:BX:AX + DI:SI:CX.
Res add_core(Real a, Real b) {
    std::uint16_t ax = a.lo, bx = a.mid, dx = a.hi;
    std::uint16_t cx = b.lo, si = b.mid, di = b.hi;
    if (lo8(cx) == 0) return {{ax, bx, dx}};                    // 0x1C7D1
    if (lo8(ax) == 0) return {{cx, si, di}};                    // 0x1C7D5 -> 0x1C84F
    if (lo8(ax) > lo8(cx)) {                                    // 0x1C7D7: smaller exponent in DX:BX:AX
        std::swap(ax, cx);
        std::swap(bx, si);
        std::swap(dx, di);
    }
    const std::uint8_t diff = static_cast<std::uint8_t>(lo8(cx) - lo8(ax));
    if (diff >= 0x28) return {{cx, si, di}};                    // 0x1C7E4: too small to matter
    const std::uint8_t exp = lo8(cx);                           // 0x1C7E8 XCHG AL,CL
    set_lo(cx, diff);
    // BP: AH = the smaller operand's sign, AL = the larger exponent.
    const std::uint16_t bp = static_cast<std::uint16_t>((hi8(dx) & 0x80) << 8 | exp);
    const bool differ = ((bp ^ di) & 0x8000) != 0;              // 0x1C7F3 (flags kept by PUSHF)
    set_lo(ax, 0);                                              // the guard byte
    dx |= 0x8000;
    di |= 0x8000;
    std::uint64_t m = pack(dx, bx, ax) >> diff;                 // 0x1C800-0x1C822, bits below AL lost
    set_lo(cx, 0);                                              // the shift loops leave CL = 0
    const std::uint64_t o = pack(di, si, cx);
    cx = bp;                                                    // CL = exponent, CH = sign
    if (!differ) {
        m += o;                                                 // 0x1C827
        if (m >> 48) {                                          // 0x1C830: carry, RCR in
            m = (m & kMask48) >> 1 | 1ull << 47;
            set_lo(cx, static_cast<std::uint8_t>(lo8(cx) + 1));
            if (lo8(cx) == 0) {                                 // 0x1C85C: overflow
                unpack(m, dx, bx, ax);
                return {{ax, bx, dx}, true};
            }
        }
    } else {
        const bool borrow = m < o;                              // 0x1C85E
        m = (m - o) & kMask48;
        if (borrow) {                                           // 0x1C869: negate, flip sign
            m = (0 - m) & kMask48;
            cx ^= 0x8000;
        }
        if (m == 0) return {kZero};                             // 0x1C87F (exponent byte 0)
        while (!(m & 1ull << 47)) {                             // 0x1C881-0x1C88D
            m = (m << 1) & kMask48;
            set_lo(cx, static_cast<std::uint8_t>(lo8(cx) - 1));
            if (lo8(cx) == 0) return {kZero};                   // underflow: 0x1C990
        }
    }
    unpack(m, dx, bx, ax);
    bool c = false;                                             // 0x1C83C: round on the guard byte
    ax = adc16(ax, 0x80, c);
    bx = adc16(bx, 0, c);
    dx = adc16(dx, 0, c);
    if (c) {                                                    // 0x1C856: RCR DX only
        dx = static_cast<std::uint16_t>(dx >> 1 | 0x8000);
        set_lo(cx, static_cast<std::uint8_t>(lo8(cx) + 1));
        if (lo8(cx) == 0) return {{ax, bx, dx}, true};
    }
    set_lo(ax, lo8(cx));                                        // 0x1C847
    set_hi(dx, static_cast<std::uint8_t>((hi8(dx) & 0x7F) | hi8(cx)));
    return {{ax, bx, dx}};
}

// FP48 0BE4:098B (flat 0x1C7CB): XOR DI,8000h, then the add.
Res sub_core(Real a, Real b) {
    b.hi ^= 0x8000;
    return add_core(a, b);
}

// The shared tail of mul and div (flat 0x1C975-0x1C996): round, then the
// exponent checks. CX holds the unbiased exponent with bit 15 = not sign.
Res round_tail(std::uint16_t ax, std::uint16_t bx, std::uint16_t dx, std::uint16_t cx) {
    bool c = false;
    ax = adc16(ax, 0x80, c);
    bx = adc16(bx, 0, c);
    dx = adc16(dx, 0, c);
    if (c) {                                                    // 0x1C980
        dx = static_cast<std::uint16_t>(dx >> 1 | 0x8000);
        ++cx;
    }
    if (cx & 0x4000) return {kZero};                            // 0x1C983: underflow -> 0
    ++cx;                                                       // 0x1C988
    set_lo(ax, lo8(cx));
    dx ^= static_cast<std::uint16_t>(hi8(cx) << 8);             // XOR DH,CH: the sign
    return {{ax, bx, dx}, (hi8(cx) & 1) != 0};                  // SHR CH,1: overflow
}

// FP48 0BE4:0A52 (flat 0x1C892-0x1C996): DX:BX:AX * DI:SI:CX.
Res mul_core(Real a, Real b) {
    std::uint16_t ax = a.lo, bx = a.mid, dx = a.hi;
    std::uint16_t cx = b.lo, si = b.mid, di = b.hi;
    if (lo8(ax) == 0 || lo8(cx) == 0) return {kZero};          // 0x1C892-0x1C898
    std::uint16_t bp = dx;
    dx = (dx ^ di) & 0x8000;
    const std::uint8_t ea = lo8(ax);                            // XCHG DL,AL
    set_lo(ax, 0);
    dx = static_cast<std::uint16_t>(dx + ea + lo8(cx));         // ADD DL,CL / ADC DH,AL
    set_lo(cx, 0);                                              // MOV CL,AL
    bp |= 0x8000;
    di |= 0x8000;
    const std::uint16_t expsign = dx;                           // PUSH DX
    bool shortcut = hi8(ax) == 0 && bx == 0;                    // 0x1C8B4
    if (!shortcut && hi8(cx) == 0 && si == 0) {                 // 0x1C8BC: the other one is short
        std::swap(ax, cx);
        std::swap(bx, si);
        std::swap(bp, di);
        shortcut = true;
    }
    if (shortcut) {                                             // 0x1C8C9: BP times DI:SI:CH
        ax = static_cast<std::uint16_t>((bp >> 8) * hi8(cx));   // MOV AL,AH / MUL CH
        bx = ax;
        std::uint32_t p = std::uint32_t{bp} * si;
        bool c = false;
        bx = adc16(bx, static_cast<std::uint16_t>(p), c);
        dx = static_cast<std::uint16_t>((p >> 16) + (c ? 1 : 0));
        cx = dx;
        p = std::uint32_t{bp} * di;
        c = false;
        ax = adc16(static_cast<std::uint16_t>(p), cx, c);
        dx = static_cast<std::uint16_t>((p >> 16) + (c ? 1 : 0));
    } else {                                                    // 0x1C8E8: the full product
        const std::uint16_t w0 = ax, w1 = bx, w2 = bp, w3 = cx, w4 = si, w5 = di;
        std::uint16_t s, d, b;
        bool c;
        auto acc3 = [&](std::uint16_t x, std::uint16_t y, std::uint16_t& r0, std::uint16_t& r1,
                        std::uint16_t& r2) {
            const std::uint32_t p = std::uint32_t{x} * y;
            c = false;
            r0 = adc16(r0, static_cast<std::uint16_t>(p), c);
            r1 = adc16(r1, static_cast<std::uint16_t>(p >> 16), c);
            r2 = adc16(r2, 0, c);
        };
        s = static_cast<std::uint16_t>(hi8(w0) * hi8(w3));     // 0x1C8F2 MUL byte
        d = 0;
        b = 0;
        acc3(w0, w4, s, d, b);
        acc3(w1, w3, s, d, b);
        s = 0;                                                  // 0x1C916
        acc3(w0, w5, d, b, s);
        acc3(w1, w4, d, b, s);
        acc3(w2, w3, d, b, s);
        d = 0;                                                  // 0x1C93C
        acc3(w1, w5, b, s, d);
        acc3(w2, w4, b, s, d);
        const std::uint32_t p = std::uint32_t{w2} * w5;         // 0x1C956
        c = false;
        ax = adc16(static_cast<std::uint16_t>(p), s, c);
        dx = adc16(static_cast<std::uint16_t>(p >> 16), d, c);
        bx = b;
    }
    std::swap(ax, bx);                                          // 0x1C963
    cx = expsign;
    if (!(dx & 0x8000)) {                                       // 0x1C966: one normalising shift
        unpack(pack(dx, bx, ax) << 1 & kMask48, dx, bx, ax);
        --cx;
    }
    cx = static_cast<std::uint16_t>(cx - 0x8081);               // 0x1C971
    return round_tail(ax, bx, dx, cx);
}

// FP48 0BE4:0B57 (flat 0x1C997-0x1CA0D): DX:BX:AX / DI:SI:CX (CL != 0,
// checked by the caller). Restoring division on 40-bit mantissas, quotient
// bits stored inverted: 16 + 16 + 10 of them.
Res div_core(Real a, Real b) {
    std::uint16_t ax = a.lo, bx = a.mid, dx = a.hi;
    std::uint16_t cx = b.lo, si = b.mid, di = b.hi;
    if (lo8(ax) == 0) return {kZero};                           // 0x1C997
    std::uint16_t bp = dx;
    dx ^= di;
    di |= 0x8000;
    bp |= 0x8000;
    dx &= 0x8000;
    const std::uint8_t ea = lo8(ax);                            // XCHG AL,DL
    set_lo(ax, 0);
    dx = static_cast<std::uint16_t>(dx + ea - lo8(cx));         // SUB DL,CL / SBB DH,AL
    const std::uint16_t expsign = dx;
    std::uint8_t ah = hi8(ax);
    const std::uint8_t ch = hi8(cx);
    std::uint16_t words[2] = {0, 0};
    int nwords = 0;
    int count = 2;                                              // AL
    std::uint16_t q = 1;                                        // DX with its sentinel bit
    bool cf;
    auto subtract = [&]() {                                     // SUB AH,CH / SBB BX,SI / SBB BP,DI
        bool c = ah < ch;
        ah = static_cast<std::uint8_t>(ah - ch);
        bx = sbb16(bx, si, c);
        bp = sbb16(bp, di, c);
    };
    // 0x1C9B8: compare, subtract when the remainder is not below the divisor.
    auto compare = [&]() {
        if (bp != di) cf = bp < di;
        else if (bx != si) cf = bx < si;
        else cf = ah < ch;
        if (!cf) subtract();
    };
    compare();
    for (;;) {
        const bool out = (q & 0x8000) != 0;                     // 0x1C9CA RCL DX,1
        q = static_cast<std::uint16_t>(q << 1 | (cf ? 1 : 0));
        if (out) {                                              // 0x1C9DF
            --count;
            if (count < 0) break;
            words[nwords++] = q;
            q = count != 0 ? 1 : 0x40;
        }
        // 0x1C9CE: SHL AH / RCL BX / RCL BP
        const bool top = (bp & 0x8000) != 0;
        bp = static_cast<std::uint16_t>(bp << 1 | bx >> 15);
        bx = static_cast<std::uint16_t>(bx << 1 | ah >> 7);
        ah = static_cast<std::uint8_t>(ah << 1);
        if (top) {
            subtract();
            cf = false;                                         // CLC
        } else {
            compare();
        }
    }
    ax = static_cast<std::uint16_t>(q << 6);                    // 0x1C9ED
    bx = words[1];
    dx = words[0];
    cx = expsign;
    ax = static_cast<std::uint16_t>(~ax);
    bx = static_cast<std::uint16_t>(~bx);
    dx = static_cast<std::uint16_t>(~dx);
    if (!(dx & 0x8000)) {                                       // 0x1C9FE: RCL with CF = 0
        unpack(pack(dx, bx, ax) << 1 & kMask48, dx, bx, ax);
        --cx;
    }
    cx = static_cast<std::uint16_t>(cx + 0x8080);               // 0x1CA07
    return round_tail(ax, bx, dx, cx);
}

// FP48 0BE4:0BCE (flat 0x1CA0E-0x1CA37): compare DX:BX:AX with DI:SI:CX;
// returns CF (a below b) and ZF (equal) as the RTL sets them.
struct Flags {
    bool cf, zf;
};
Flags magnitude(Real a, Real b) {                               // 0x1CA25
    if (a.exponent() != b.exponent()) return {a.exponent() < b.exponent(), false};
    if (a.exponent() == 0) return {false, true};
    if (a.hi != b.hi) return {a.hi < b.hi, false};
    if (a.mid != b.mid) return {a.mid < b.mid, false};
    return {hi8(a.lo) < hi8(b.lo), hi8(a.lo) == hi8(b.lo)};
}
Flags compare(Real a, Real b) {
    if ((a.hi ^ b.hi) & 0x8000) return {a.negative(), false};   // 0x1CA14: RCL DX,1
    if (a.negative()) {                                         // 0x1CA1E
        const Flags f = magnitude(a, b);
        return f.zf ? f : Flags{!f.cf, false};                  // CMC
    }
    return magnitude(a, b);
}

// F48F 0BE4:0D07 (flat 0x1CB47-0x1CB97): Int(x), the fraction bits masked off.
Real int_part(Real x) {
    if (x.exponent() >= 0xA8) return x;                         // already integral
    if (x.exponent() <= 0x80) return kZero;                     // 0x1CB95: |x| < 1
    const int n = x.exponent() - 0x80;                          // integer bits, hidden one included
    const std::uint64_t mask = ((1ull << n) - 1) << (40 - n);   // DX:BX:AH, ones from the top
    Real r;
    r.hi = static_cast<std::uint16_t>(x.hi & (mask >> 24));
    r.mid = static_cast<std::uint16_t>(x.mid & (mask >> 8));
    r.lo = static_cast<std::uint16_t>((x.lo & ((mask & 0xFF) << 8)) | x.exponent());
    return r;
}

// F48F 0BE4:0D58 (flat 0x1CB98-0x1CBAB): Frac(x) = x - Int(x), carry ignored.
Real frac(Real x) {
    return sub_core(x, int_part(x)).r;
}

// The odd polynomial of F48F (flat 0x1CF28-0x1CF8F): x * (Horner over x^2 of
// the n coefficients, then + 1). Carries ignored throughout.
Real poly_odd(Real x, const Real* coef, int n) {
    const Real y = mul_core(x, x).r;                            // 0x1CF33
    Real acc = mul_core(coef[0], y).r;                          // 0x1CF41-0x1CF78
    for (int i = 1; i < n; ++i) {
        acc = add_core(acc, coef[i]).r;                         // 0x1CF6C
        acc = mul_core(acc, y).r;                               // 0x1CF78
    }
    acc = add_core(acc, Real{0x0081, 0, 0}).r;                  // 0x1CF89: + 1.0
    return mul_core(acc, x).r;                                  // 0x1CF3E JMP to the multiply
}

// Sin's coefficients, 0BE4:0E3D (flat 0x1CC7D-0x1CCA6), seven 6-byte reals.
constexpr Real kSinCoef[7] = {
    {0x9D58, 0x9F39, 0xD73F}, {0x4360, 0x309D, 0x3092}, {0xAA67, 0x283F, 0xD732},
    {0xB66E, 0x1D2A, 0x38EF}, {0x0D74, 0x00D0, 0xD00D}, {0x887A, 0x8888, 0x0888},
    {0xAB7E, 0xAAAA, 0xAAAA},
};
constexpr Real kTwoPi{0x2183, 0xDAA2, 0x490F};                  // 0x1CC20
constexpr Real kPi{0x2182, 0xDAA2, 0x490F};                     // DEC CL
constexpr Real kHalfPi{0x2181, 0xDAA2, 0x490F};                 // DEC CL
constexpr Real kMinusHalfPi{0x2181, 0xDAA2, 0xC90F};            // Cos, 0x1CC09

// Sin's body, F48F 0BE4:0DDC (flat 0x1CC1C-0x1CC7C).
Real sin_body(Real x) {
    if (x.exponent() < 0x6C) return x;                          // tiny: Sin x = x
    Real abs = x;
    abs.hi &= 0x7FFF;
    if (!compare(abs, kTwoPi).cf) {                             // 0x1CC2D: reduce mod 2 pi
        const Real q = div_core(x, kTwoPi).r;
        x = mul_core(frac(q), kTwoPi).r;
    }
    if (x.negative()) x = add_core(x, kTwoPi).r;                // 0x1CC43
    const bool below_pi = compare(x, kPi).cf;                   // 0x1CC4D (PUSHF)
    if (!below_pi) x = sub_core(x, kPi).r;
    if (!compare(x, kHalfPi).cf) {                              // 0x1CC58: pi - x
        x.hi |= 0x8000;
        x = add_core(x, kPi).r;
    }
    if (x.exponent() >= 0x6C) x = poly_odd(x, kSinCoef, 7);     // 0x1CC65
    if (!below_pi && x.exponent() != 0) x.hi ^= 0x8000;         // 0x1CC72
    return x;
}

[[noreturn]] void raise(int code, const char* where) {
    throw RuntimeError(code, where);
}

const char* error_name(int code) {
    switch (code) {
    case 200: return "division by zero";
    case 205: return "floating point overflow";
    case 207: return "invalid floating point operation";
    default: return "runtime error";
    }
}

// F48H/FP48 0BE4:0C37 (flat 0x1CA77-0x1CAD2): CH = 0 truncates, 1 rounds
// (the last bit shifted out added; ties away from zero).
std::int32_t to_long(Real x, bool rounding, const char* where) {
    std::uint16_t ax = x.mid, bx = x.lo, dx = x.hi;             // XCHG AX,BX
    if (lo8(bx) > 0x9F) raise(207, where);                      // 0x1CA7C
    std::uint8_t cl = static_cast<std::uint8_t>(0x9F - lo8(bx));
    if (cl > 0x1F) return 0;                                    // 0x1CACE: |x| < 0.5
    ++cl;
    set_hi(bx, hi8(dx));                                        // the sign
    dx |= 0x8000;
    if (cl >= 0x11) {
        ax = dx;
        dx = 0;
        cl = static_cast<std::uint8_t>(cl - 16);
    }
    if (cl >= 9) {
        ax = static_cast<std::uint16_t>(ax >> 8 | dx << 8);
        dx = static_cast<std::uint16_t>(dx >> 8);
        cl = static_cast<std::uint8_t>(cl - 8);
    }
    bool cf = false;
    do {                                                        // 0x1CAA6
        cf = (ax & 1) != 0;
        ax = static_cast<std::uint16_t>(ax >> 1 | dx << 15);
        dx = static_cast<std::uint16_t>(dx >> 1);
    } while (--cl);
    if (cf && rounding) {                                       // 0x1CAAE
        bool c = false;
        ax = adc16(ax, 1, c);
        dx = adc16(dx, 0, c);
        if (dx & 0x8000) raise(207, where);
    }
    std::uint32_t v = std::uint32_t{dx} << 16 | ax;
    if (hi8(bx) & 0x80) v = 0 - v;                              // 0x1CABC
    return static_cast<std::int32_t>(v);
}

}  // namespace



RuntimeError::RuntimeError(int c, const std::string& where)
    : Error("TP runtime error " + std::to_string(c) + " (" + error_name(c) + ") in " + where), code(c) {}

Real from_long(std::int32_t v) {
    std::uint16_t ax = static_cast<std::uint16_t>(v);
    std::uint16_t dx = static_cast<std::uint16_t>(static_cast<std::uint32_t>(v) >> 16);
    if ((ax | dx) == 0) return kZero;                           // 0x1CA3C
    const std::uint8_t ch = hi8(dx);                            // the sign
    if (dx & 0x8000) {                                          // 0x1CA44: negate DX:AX
        bool c = false;
        ax = adc16(static_cast<std::uint16_t>(~ax), 1, c);
        dx = adc16(static_cast<std::uint16_t>(~dx), 0, c);
    }
    std::uint16_t bx = ax;
    ax = 0x00A0;                                                // 2^31 scale
    if (dx == 0) {                                              // 0x1CA53
        std::swap(dx, bx);
        ax = 0x0090;
        if (hi8(dx) == 0) {
            dx = static_cast<std::uint16_t>(dx << 8);           // XCHG DH,DL
            ax = 0x0088;
        }
    }
    while (!(dx & 0x8000)) {                                    // 0x1CA63-0x1CA6D
        set_lo(ax, static_cast<std::uint8_t>(lo8(ax) - 1));
        dx = static_cast<std::uint16_t>(dx << 1 | bx >> 15);
        bx = static_cast<std::uint16_t>(bx << 1);
    }
    if (!(ch & 0x80)) dx &= 0x7FFF;                             // 0x1CA6F
    return {ax, bx, dx};
}

Real add(Real a, Real b) {
    const Res r = add_core(a, b);
    if (r.cf) raise(205, "System.RealAdd 0BE4:0C93");
    return r.r;
}

Real sub(Real a, Real b) {
    const Res r = sub_core(a, b);
    if (r.cf) raise(205, "System.RealSub 0BE4:0C99");
    return r.r;
}

Real mul(Real a, Real b) {
    const Res r = mul_core(a, b);
    if (r.cf) raise(205, "System.RealMul 0BE4:0CA5");
    return r.r;
}

Real div(Real a, Real b) {
    if (b.exponent() == 0) raise(200, "System.RealDiv 0BE4:0CAB");
    const Res r = div_core(a, b);
    if (r.cf) raise(205, "System.RealDiv 0BE4:0CAB");
    return r.r;
}

std::int32_t trunc(Real x) {
    return to_long(x, false, "System.Trunc 0BE4:0CBD");
}

std::int32_t round(Real x) {
    return to_long(x, true, "System.Round 0BE4:0CC5");
}

Real sin(Real x) {
    return sin_body(x);
}

Real cos(Real x) {
    x = add_core(x, kMinusHalfPi).r;                            // carry ignored
    if (x.exponent() != 0) x.hi ^= 0x8000;                      // 0x1CC15
    return sin_body(x);
}

Real from_seed(std::uint32_t seed) {
    std::uint16_t bx = static_cast<std::uint16_t>(seed);
    std::uint16_t dx = static_cast<std::uint16_t>(seed >> 16);
    std::uint16_t ax = 0x0080;
    int n = 32;
    for (; n > 0 && !(dx & 0x8000); --n) {                      // 0x1CFB0-0x1CFBB
        dx = static_cast<std::uint16_t>(dx << 1 | bx >> 15);
        bx = static_cast<std::uint16_t>(bx << 1);
        set_lo(ax, static_cast<std::uint8_t>(lo8(ax) - 1));
    }
    if (n == 0) set_lo(ax, 0);                                  // 0x1CFBD: seed 0
    dx &= 0x7FFF;
    return {ax, bx, dx};
}

}  // namespace cotd::real48
