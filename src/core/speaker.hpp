// SPDX-License-Identifier: GPL-3.0-or-later
// The PC speaker's sound effects: the original's resident effect unit
// (segment 0906 of START.EXE's load image), ported (docs/notes/sound.md
// "The effects"). Its routines run on a copy of the unit's own segment,
// read from the player's START.EXE: the effect table (0906:047B), the
// modulation tables (05CB, 06EB) and the effect streams (08EB..0DB3), with
// the channel records the routines keep there (0149 + 0x30 i).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd {

class SpeakerEngine {
public:
    // The unit's INT 8 rate: PIT channel 0 at 0x13B1 (TimerInstall 0906:1298).
    static constexpr double kTickHz = 1193182.0 / 0x13B1;
    // The playable effects: StartSpeaker(e) for e = 1..19 (Sound.Play ids
    // 2..20); e = 0 is silence.
    static constexpr int kEffects = 20;

    // What one tick sends to the speaker (SpeakerUpdate 0906:1393): port
    // 61h's bits 0..1 (0 off, 3 the square wave on; the shipped effects use
    // only these) and, when they are set, the divisor written to port 42h
    // (0 is 65536).
    struct State {
        int bits = 0;
        std::uint16_t divisor = 0;
        bool operator==(const State& o) const { return bits == o.bits && divisor == o.divisor; }
    };

    // The Tandy 1000's SN76489 (session 78; docs/notes/sound.md "The
    // Tandy"): the same unit's Tandy tables (records 0906:02E3, notes 0463,
    // effects 0523), run by the same ChannelStep and EventFetch.
    enum class Device { Speaker, Tandy };

    // What one tick leaves for TandyUpdate (0906:1408, 0x1A468) to write:
    // `silent` when it wrote the four volumes off (no effect, or the unit
    // disabled); else each channel record's frequency word (+08) and level
    // word (+0A), from which it writes, channel c (0..3; 3 the noise):
    // 0x80 | c << 5 | ((f >> 2) & 0xFF) >> 4, then (c < 3) (f >> 2) >> 8;
    // and 0x90 | c << 5 | (0xFFFF - level) >> 12.
    struct TandyState {
        bool silent = true;
        std::uint16_t freq[4] = {0, 0, 0, 0};
        std::uint16_t level[4] = {0, 0, 0, 0};
        // The port-C0h bytes TandyUpdate writes for this state, in order.
        std::vector<std::uint8_t> bytes() const;
    };

    // `image`: START.EXE's unpacked load image (core/exepack.hpp); `where`
    // names it in errors. Every effect 1..19 is played to its end once
    // here: an effect that uses what no shipped effect does (note groups,
    // channel switches, calls: the unreachable effect 20's), reads outside
    // the segment or doesn't end within 5,000 ticks is rejected, naming
    // the effect and the stream offset.
    SpeakerEngine(const Bytes& image, std::string where, Device device = Device::Speaker);

    // StartSpeaker(e) (0906:165E): the channels' records cleared and
    // pointed at effect e's streams; the effect sounds from the next tick.
    void start(int e);
    // One tick (SpeakerUpdate).
    State tick();
    // One tick on the Tandy (TandyUpdate; a Device::Tandy engine).
    TandyState tandy_tick();
    // Tandy channel c's record word at +off (the trace's view: the trap
    // reads the records whether TandyUpdate wrote them or not).
    // Some channel still plays.
    bool active() const;

private:
    std::uint16_t rw(std::uint32_t o) const;
    void ww(std::uint32_t o, std::uint32_t v);
    std::uint8_t rb(std::uint32_t o) const;
    void step(std::uint16_t si);
    void fetch(std::uint16_t si);

    std::vector<std::uint8_t> seg_;
    std::string where_;
    int effect_ = 0;
    Device device_ = Device::Speaker;
    static constexpr std::uint32_t kRecordBytes = 0x30;
    std::uint32_t records_ = 0x0149, notes_ = 0x02C9, table_ = 0x047B;
};

// The unit's commands as the game gives them (SfxCommand 0906:0000 with
// SfxInit and TimerInstall / TimerRemove): an effect cuts the one playing;
// the engine is installed by the first Sound.Play and removed by every
// Dax.Load (SfxCommand 0xFF), which silences it.
class SfxUnit {
public:
    SfxUnit(const Bytes& image, std::string where, SpeakerEngine::Device device = SpeakerEngine::Device::Speaker)
        : engine_(image, std::move(where), device) {}

    // Sound.Play(id) (07DA:0000): SfxInit when the engine isn't installed
    // (TimerInstall, StartSpeaker(0)), then SfxCommand(id).
    void play(int id);
    // SfxCommand(n): 0 silence, 0xFF remove, 2..20 effect n - 1; others
    // (1: SetSpeakerEnable(1), which changes nothing) nothing.
    void command(int n);
    bool installed() const { return installed_; }
    // An effect still plays (the sound lab's end of an effect).
    bool active() const { return installed_ && engine_.active(); }
    // One tick of the installed engine.
    SpeakerEngine::State tick() { return engine_.tick(); }
    // One tick of the installed engine on the Tandy (a Device::Tandy unit).
    SpeakerEngine::TandyState tandy_tick() { return engine_.tandy_tick(); }

private:
    SpeakerEngine engine_;
    bool installed_ = false;
};

}  // namespace cotd
