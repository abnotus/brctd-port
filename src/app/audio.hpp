// SPDX-License-Identifier: GPL-3.0-or-later
// The window's sound: the music driver's port (core/music.hpp) on an OPL
// chip (Nuked-OPL3, docs/THIRD_PARTY.md) and the effect unit's port
// (core/speaker.hpp) on a PC speaker or the Tandy's chip, mixed and played
// through SDL3 (D-0067, D-0068, D-0093).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd {

class Audio {
public:
    // `device` false: no audio device is opened; render() gives the
    // samples instead (cotd sound-render, the sound lab's test).
    explicit Audio(bool device = true);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // False when the host has no audio device: the music is then not heard,
    // but the driver's clock still runs (WindowInput::clock_cs).
    bool ok() const;
    // The driver's fn 0 and fn 1 on `song`, then its ticks from the audio
    // thread; stop() is fn 3.
    void start(const Bytes& song);
    void stop();
    // 0 (muted) .. 10 (the chip's full level).
    void set_volume(int volume);
    // The effect unit from START.EXE's load image (once), and its command
    // n (Game::sound: Sound.Play's ids, 0 silence, 0xFF removed).
    void effects(const Bytes& image);
    void sound(int n);
    // Where the effects play: the PC speaker (D-0068, tests), the Tandy
    // 1000's chip (D-0093, the default in play) or their AdLib versions
    // (D-0069: each approved design, the sound lab's, built in, on its own
    // OPL2; an effect without one on the speaker).
    enum class Fx { Speaker, Tandy, Adlib };
    void effects_device(Fx fx);
    // Tests: the designs from this text instead of the built-in file.
    // The next `frames` stereo frames (16-bit, 49,716 Hz) as the device
    // would get them.
    // A rejection on the audio thread stops its source (the music, or the
    // effects) and is kept; this says each once (combat::diag, D-0082: the
    // console, and the key log in play) on the caller's thread. render()
    // calls it; the window, at each event pump.
    void report_faults();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cotd
