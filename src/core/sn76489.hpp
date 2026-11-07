// SPDX-License-Identifier: GPL-3.0-or-later
// The Tandy 1000's sound chip, an SN76489 family PSG as the NCR 8496 the
// Tandy fitted (session 78; docs/notes/sound.md "The Tandy"): three square
// tone voices and a noise voice, written a byte at a time at port C0h.
// Written for the Tandy's effects (D-0093) from the chip's documented
// behaviour, with the NCR 8496's specifics as DOSBox-X's emulation has them
// (MAME's sn76496.cpp, BSD-3-Clause, read as the reference; no code taken):
// a 16-bit noise register with taps at bits 1 and 5, writes with bit 7
// clear ignored for the volume and noise registers, the noise register
// reset only when the noise mode changes, and the output inverted.
#pragma once

#include <array>
#include <cstdint>

namespace cotd {

class Sn76489 {
public:
    static constexpr double kClock = 3579545.0;   // the Tandy's 14.31818 MHz / 4
    // The counters step at kClock / 16 (a tone of period N is kClock / 32N;
    // DOSBox-X: the loop at clock / 2, a step every 8 passes).
    static constexpr int kDivider = 16;

    Sn76489();
    void write(std::uint8_t data);
    // The next sample at `rate` Hz: the mean of the chip's output over the
    // sample's span (a level of 8191 a voice at full volume, 2 dB a step;
    // inverted, as the NCR 8496's is). `point`: the value at the sample's
    // end instead, as DOSBox-X's emulation takes it (for the check).
    int sample(double rate, bool point = false);

private:
    void step();
    std::array<int, 8> reg_{};
    int last_ = 0;
    std::array<int, 4> period_{}, count_{}, out_{}, vol_{};
    std::array<int, 16> table_{};
    unsigned rng_ = 0x8000;
    double pos_ = 0;   // chip steps owed to the next sample
};

}  // namespace cotd
