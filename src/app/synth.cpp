// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/synth.hpp"

namespace cotd {

namespace {
// The integral of a square wave (+1 for the first half period, -1 for the
// second) from the period's start to phase u in [0, 1): the speaker is
// sampled as the wave's mean over each sample, so a tone above the
// sample rate's half averages out instead of aliasing.
double square_integral(double u) { return u < 0.5 ? u : 1.0 - u; }
}  // namespace

double SpeakerVoice::next() {
    // The unit's INT 8 at 1193182 / 0x13B1 while installed.
    ticks_ += SpeakerEngine::kTickHz / kAudioRate;
    while (ticks_ >= 1.0) {
        ticks_ -= 1.0;
        state_ = unit_.installed() ? unit_.tick() : SpeakerEngine::State{};
    }
    if (!unit_.installed()) state_ = {};   // TimerRemove's port 61h: off at once
    if (state_.bits != 3) return 0;
    // Mode 3 at 1193182 / divisor Hz (0: 65536).
    const double hz = 1193182.0 / (state_.divisor ? state_.divisor : 65536);
    const double step = hz / kAudioRate;
    const double p0 = phase_, p1 = p0 + step;
    const double v = kLevel * (square_integral(p1 - static_cast<long>(p1)) - square_integral(p0)) / step;
    phase_ = p1 - static_cast<long>(p1);
    return v;
}

double TandyVoice::next() {
    // The unit's INT 8 at 1193182 / 0x13B1 while installed.
    ticks_ += SpeakerEngine::kTickHz / kAudioRate;
    while (ticks_ >= 1.0) {
        ticks_ -= 1.0;
        if (unit_.installed()) {
            for (const std::uint8_t b : unit_.tandy_tick().bytes()) chip_.write(b);
            silenced_ = false;
        } else if (!silenced_) {
            static constexpr std::uint8_t kOff[] = {0x9F, 0xBF, 0xDF, 0xFF};   // TandyUpdate's silence
            for (const std::uint8_t b : kOff) chip_.write(b);
            silenced_ = true;
        }
    }
    const double x = chip_.sample(kAudioRate) * kGain;
    const double r = 1.0 - 6.283185307179586 * kCornerHz / kAudioRate;
    const double y = x - x1_ + r * y1_;
    x1_ = x;
    y1_ = y;
    return y;
}

}  // namespace cotd
