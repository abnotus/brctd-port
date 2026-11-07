// SPDX-License-Identifier: GPL-3.0-or-later
// Turbo Pascal 5.5's 6-byte Real, bit for bit as the game's runtime library
// (System unit, segment 0BE4) computes it.
//
// Ported from the disassembly of the player's unpacked START.EXE (System
// code at linear 0xBE40; flat address = 0x10000 + 0xBE40 + offset, e.g.
// 0BE4:0CA5 is flat 0x1CAE5). The System unit there is byte-identical to
// TURBO.TPL's (tools/tp55/verify.py), and every routine here is checked
// against the same RTL running in DOSBox-X (tools/tp55/pas/R1.PAS,
// tests/test_real48.py).
//
// Register convention of the RTL: a real travels in DX:BX:AX (AL = the
// exponent byte, AH = the lowest mantissa byte, DH bit 7 = the sign), a
// second operand in DI:SI:CX. Real keeps exactly those three words.
#pragma once

#include <cstdint>
#include <string>

#include "core/common.hpp"

namespace cotd::real48 {

struct Real {
    std::uint16_t lo = 0;    // AX: exponent byte (bias 129, 0 = zero), mantissa byte 0
    std::uint16_t mid = 0;   // BX: mantissa bytes 1-2
    std::uint16_t hi = 0;    // DX: mantissa bytes 3-4; bit 15 = sign

    std::uint8_t exponent() const { return static_cast<std::uint8_t>(lo); }
    bool negative() const { return (hi & 0x8000) != 0; }
    friend bool operator==(const Real&, const Real&) = default;

    // The 6 bytes as stored in memory (little-endian AX, BX, DX).
};

// A runtime error the RTL raises (System.RunError 0BE4:00D1): 200 division
// by zero, 205 floating point overflow, 207 invalid floating point
// operation. The port stops where the original would halt.
struct RuntimeError : Error {
    int code;
    RuntimeError(int c, const std::string& where);
};

// System.IntToReal 0BE4:0CB9 (flat 0x1CAF9; body 0BE4:0BF8, 0x1CA38-0x1CA76).
Real from_long(std::int32_t v);
// System.RealAdd 0BE4:0C93 (0x1CAD3; body 0BE4:098F, 0x1C7CF-0x1C891). 205 on overflow.
Real add(Real a, Real b);
// System.RealSub 0BE4:0C99 (0x1CAD9; body 0BE4:098B negates b, then the add).
Real sub(Real a, Real b);
// System.RealMul 0BE4:0CA5 (0x1CAE5; body 0BE4:0A52, 0x1C892-0x1C996). 205 on overflow.
Real mul(Real a, Real b);
// System.RealDiv 0BE4:0CAB (0x1CAEB; body 0BE4:0B57, 0x1C997-0x1CA0D, shared
// tail 0x1C975). 200 for a zero divisor, 205 on overflow.
Real div(Real a, Real b);
// System.Trunc 0BE4:0CBD / System.Round 0BE4:0CC5 (0x1CAFD / 0x1CB05; body
// 0BE4:0C37, 0x1CA77-0x1CAD2). 207 when the result leaves the longint range.
std::int32_t trunc(Real x);
std::int32_t round(Real x);
// System.Sin 0BE4:0DDC (0x1CC1C-0x1CC7C, polynomial 0x1CF28-0x1CF8F, its
// coefficients at 0BE4:0E3D). Never raises an error.
Real sin(Real x);
// System.Cos 0BE4:0DC9 (0x1CC09-0x1CC1B): -pi/2 added, negated, then Sin's body.
Real cos(Real x);
// System.RandomReal 0BE4:1166 (0x1CFA6-0x1CFC2) after its NextRand: the new
// RandSeed's 32 bits as a mantissa, value seed / 2^32.
Real from_seed(std::uint32_t seed);


}  // namespace cotd::real48
