// SPDX-License-Identifier: GPL-3.0-or-later
// The AdLib versions of the game's PC speaker effects (D-0069;
// docs/notes/sound.md "Sound lab"): the designs (an instrument, notes on
// the effect unit's clock), the drafts made from the speaker version at
// run time, the designs file, and the voice that plays a design on an
// OPL2, for the game's sound (app/audio.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/common.hpp"
#include "core/speaker.hpp"

namespace cotd::lab {

// The effects the lab lists: Sound.Play's ids 2..20 (StartSpeaker 1..19).
constexpr int kFirstId = 2, kLastId = 20;

// One OPL2 voice's instrument: the modulator's and the carrier's registers
// 0x20 (AM, vibrato, EG type, KSR, multiple), 0x40 (KSL, total level), 0x60
// (attack, decay), 0x80 (sustain level, release), 0xE0 (wave), then the
// channel's 0xC0 (feedback, connection).
struct Instrument {
    std::array<int, 11> regs{};   // 20m 20c 40m 40c 60m 60c 80m 80c E0m E0c C0
    bool operator==(const Instrument&) const = default;
};
// The default instrument: the speaker's square wave.
Instrument default_instrument();

// A note: a MIDI note number (60 = middle C, 69 = 440 Hz) or -1 for a rest,
// held for `ticks` ticks of the effect unit's clock (236.7 Hz), so a design
// and its speaker version have the same timing.
struct Note {
    int pitch = -1;
    int ticks = 1;
    bool operator==(const Note&) const = default;
};

enum class Verdict { NotYet, Approved };

struct Design {
    int id = 0;
    Instrument ins = default_instrument();
    int shift = 0;            // semitones added to every note
    int level = 0;            // dB over the speaker version's loudness (loudness_gain)
    int step = 1;             // the draft: the speaker's pitch read every `step` ticks
    bool edited = false;      // the notes were edited: kept in the file
    std::vector<Note> notes;  // the draft from the speaker version unless edited
    Verdict verdict = Verdict::NotYet;
    bool operator==(const Design&) const = default;
};

// The speaker's state at each tick of effect `id` (Sound.Play's id), from
// its first tick to the one where no channel plays any more.
std::vector<SpeakerEngine::State> effect_states(const Bytes& image, int id);
// The first draft: every `step` ticks the speaker's pitch (from the PIT
// divisor; off: a rest) as a note, clamped to the OPL2's range, equal
// neighbours merged; the divisor-0 tick each effect starts with joins the
// note after it.
std::vector<Note> draft(const std::vector<SpeakerEngine::State>& states, int step);
// The gain that gives the AdLib version the speaker version's loudness
// (the RMS over each one's whole effect), times the design's level.
double loudness_gain(const Bytes& image, const Design& d);
// A note's frequency, with the design's shift.
double note_hz(int pitch);
// The OPL2's highest note (block 7, F-number 1023 at 49,716 Hz).
constexpr int kTopPitch = 114;

// The designs file (docs/notes/sound.md "Sound lab"): only what differs
// from a fresh design is written; a missing file reads as none.
std::map<int, Design> parse_designs(const std::string& text, const std::string& where);
// The designs the game plays (D-0069): data/sfx_adlib.toml as built into
// the engine (cmake/sfx_designs.cmake: the owner's verdicts and edits
// only, nothing derived from the game), each listed effect's notes made
// at run time from the player's START.EXE unless the owner edited them.
// Only an approved design replaces its speaker effect.
std::map<int, Design> built_in_designs(const Bytes& image);
// The same from another designs text (tests: `cotd sound-render --designs`).
std::map<int, Design> game_designs(const Bytes& image, const std::string& text, const std::string& where);
// A design as kept: the stored fields over a fresh one with the draft.

// The AdLib version on an OPL2 (Nuked-OPL3 at 49,716 Hz): the design's
// notes on channel 0, one at each unit tick, a note after a sounding one
// changing the pitch without striking again.
class AdlibVoice {
public:
    // `gain` scales the chip's output (loudness_gain).
    AdlibVoice(const Design& d, double gain);
    ~AdlibVoice();
    AdlibVoice(const AdlibVoice&) = delete;
    AdlibVoice& operator=(const AdlibVoice&) = delete;
    // The next stereo frame.
    void next(std::int16_t out[2]);
    // Every note played and the release heard (0.1 s after the last).
    bool done() const;
    // Every note played (the release may still sound).
    bool ended() const { return ended_; }

private:
    void tick();
    struct Chip;
    Chip* chip_;
    std::vector<Note> notes_;
    int shift_;
    double gain_;
    std::size_t at_ = 0;
    int left_ = 0;
    bool sounding_ = false, ended_ = false;
    double ticks_ = 0;
    int tail_ = 0;
};

}  // namespace cotd::lab
