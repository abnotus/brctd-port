// SPDX-License-Identifier: GPL-3.0-or-later
// The PC speaker as the window plays it (D-0068): the effect unit
// (core/speaker.hpp) ticked at its 236.7 Hz against the output's sample
// clock, the speaker a square wave sampled as its mean over each sample.
// The window's sound (app/audio.cpp).
#pragma once

#include <string>

#include "core/sn76489.hpp"
#include "core/speaker.hpp"

namespace cotd {

// The output's sample rate: the OPL chip's own (OPL3_Generate).
constexpr int kAudioRate = 49716;

class SpeakerVoice {
public:
    // The speaker's level against the chip's (both scaled by the volume).
    static constexpr double kLevel = 5000.0;

    SpeakerVoice(const Bytes& image, std::string where) : unit_(image, std::move(where)) {}

    SfxUnit& unit() { return unit_; }
    const SfxUnit& unit() const { return unit_; }
    // The next sample: the unit's ticks owed are run first; the speaker
    // keeps its last state between ticks and is off while the unit is
    // removed (TimerRemove's port 61h).
    double next();

private:
    SfxUnit unit_;
    double ticks_ = 0;             // the unit's ticks owed, in fractions
    SpeakerEngine::State state_;   // what the speaker does now (PIT channel 2, port 61h)
    double phase_ = 0;             // the square wave's phase, in periods
};

// The effects on the Tandy 1000's SN76489 (session 78, D-0093): the same
// unit run on its Tandy tables (TandyUpdate's port-C0h bytes each tick, as
// BUCK.CFG's "T" has the original play them), into core/sn76489.hpp.
class TandyVoice {
public:
    // A voice at full volume against the speaker's square (SpeakerVoice::kLevel).
    static constexpr double kGain = SpeakerVoice::kLevel / 8191.0;
    // The DC blocker's corner: the chip's output is one-sided (0 or minus the
    // level), so an effect's start and end would step; the port takes the
    // steady part out, as a coupling capacitor would (D-0093; inference about
    // the Tandy's own output stage).
    static constexpr double kCornerHz = 10.0;

    TandyVoice(const Bytes& image, std::string where)
        : unit_(image, std::move(where), SpeakerEngine::Device::Tandy) {}

    SfxUnit& unit() { return unit_; }
    // The next sample: the unit's ticks owed are run first (each tick's
    // bytes to the chip); the chip silenced while the unit is removed.
    double next();

private:
    SfxUnit unit_;
    Sn76489 chip_;
    double ticks_ = 0;
    bool silenced_ = true;          // the four volumes off, written once while removed
    double x1_ = 0, y1_ = 0;        // the DC blocker's last input and output
};

}  // namespace cotd
