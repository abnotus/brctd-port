// SPDX-License-Identifier: GPL-3.0-or-later
// The music driver (MUSIC.DAX #1, a separate x86 image the original loads at
// run time): its AdLib backend and score sequencer, ported (docs/notes/sound.md).
// Addresses are the driver's own IPs, "drv:XXXX" (image offset + 0x100).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd {

class MusicDriver {
public:
    // An OPL2 register write: register (0x00..0xFF), value.
    using Sink = std::function<void(int reg, int value)>;

    // One INT 1Ch tick: PIT channel 0 at divisor 0x4000 (drv:02E6).
    static constexpr double kTickHz = 1193182.0 / 0x4000;

    // `file` is a MUSIC.DAX song entry (#2..#9: GLBR parts, then SLBR);
    // `where` names it in errors. Rejects a file the driver would misread.
    MusicDriver(Bytes file, std::string where, Sink sink);

    // fn 0 (drv:0352: init, the timer installed, the counter at 0) and fn 1
    // (drv:0396: play song 0): the writes up to the first tick.
    void start();
    // One INT 1Ch tick (drv:2984): the counter + 1; on an even count the
    // sequencer runs (drv:0982).
    void tick();
    // fn 3 (drv:03CB): StopAll's Silence, then the backend's shutdown.
    void stop();
    std::uint32_t counter() const { return counter_; }

private:
    struct Track {
        std::uint32_t time = 0;     // +0: 0 = ended
        std::size_t pos = 0;        // +4: the stream pointer
        std::uint16_t rem = 0;      // +0xA
        std::uint8_t velocity = 0;  // +0xC
        std::uint8_t state = 3;     // +0xD: 0 gap, 1 sounding, 2 tied, 3 off
        std::uint8_t note = 0;
    };

    std::uint8_t at(std::size_t p) const;
    void write(int reg, int value) { sink_(reg, value & 0xFF); }
    void silence();
    void program(int k, int instrument);
    void note(int k, int n, int velocity);
    void seq_tick(std::uint32_t now);
    static std::uint16_t duration(std::uint8_t d);

    Bytes file_;
    std::string where_;
    Sink sink_;
    std::array<std::array<std::uint8_t, 12>, 8> instruments_{};
    std::vector<std::size_t> streams_;
    std::uint16_t tempo_ = 0, div_ = 0;
    std::uint8_t velocity0_ = 0;
    std::vector<Track> tracks_;
    std::array<int, 6> channel_{};      // drvDS:00BD: track -> OPL channel
    std::array<int, 6> instrument_{};   // drvDS:00C9: track -> instrument
    std::uint16_t used_ = 0;            // drvDS:03BC: the allocated melodic channels
    std::uint32_t counter_ = 0;         // drv:292E
    bool playing_ = false;
};

}  // namespace cotd
