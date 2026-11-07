// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/music.hpp"

#include <algorithm>
#include <utility>

namespace cotd {

namespace {

// drvDS:00DC: the 18 operator offsets; drvDS:0109: channel -> 1-based index
// (modulator DC[idx - 1], carrier DC[idx + 2]).
constexpr std::array<int, 18> kOps{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0A,
                                   0x0B, 0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
constexpr std::array<int, 11> kChannelIndex{1, 2, 3, 7, 8, 9, 13, 17, 15, 18, 14};
// drvDS:0114: F-numbers of the twelve semitones.
constexpr std::array<int, 12> kFnum{0x157, 0x16D, 0x181, 0x198, 0x1B1, 0x1CB,
                                    0x1E6, 0x203, 0x222, 0x243, 0x266, 0x28A};
constexpr std::size_t kParts = 0x1BC;   // the score follows the parts (drv:0352: file + 0x1BC)

std::uint16_t be16(const Bytes& b, std::size_t p) {
    return static_cast<std::uint16_t>(b[p] << 8 | b[p + 1]);
}

}  // namespace

MusicDriver::MusicDriver(Bytes file, std::string where, Sink sink)
    : file_(std::move(file)), where_(std::move(where)), sink_(std::move(sink)) {
    auto need = [&](std::size_t end, const char* what) {
        if (end > file_.size())
            throw Error(where_ + ": " + what + " past the end of the entry (" + std::to_string(file_.size()) +
                        " bytes)");
    };
    // The parts file (drv:0124 checks 'GLBR'; drv:20FA loads it): a count of
    // N parts at +0xA, their sizes at +0xC; the AdLib ones are parts 0..7.
    need(0x0C, "the parts header");
    if (file_[0] != 'G' || file_[1] != 'L' || file_[2] != 'B' || file_[3] != 'R')
        throw Error(where_ + ": not a GLBR parts file (drv:0124)");
    const std::size_t n = be16(file_, 0x0A);
    need(0x0C + 2 * n, "the part sizes");
    std::size_t p = 0x0C + 2 * n;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t size = be16(file_, 0x0C + 2 * i);
        if (i < 8) {
            if (size != 12) throw Error(where_ + ": AdLib part " + std::to_string(i) + " is not 12 bytes (drv:126F)");
            need(p + 12, "an AdLib part");
            std::copy_n(file_.begin() + static_cast<std::ptrdiff_t>(p), 12, instruments_[i].begin());
        }
        p += size;
    }
    if (p > kParts) throw Error(where_ + ": the parts run past 0x1BC, where the driver reads the score (drv:0352)");
    // The score (drv:06ED): +8 the song count C, +0xA (C + 1) offsets from
    // the end of that table; song 0: tempo, velocity, K voices, five stream
    // offsets from the song + 0xE (word 0 the speaker's voice, 1..K-1 the
    // tracks: drv:0731).
    need(kParts + 0x0A, "the score header");
    const std::size_t songs = file_[kParts + 8];
    if (songs < 1) throw Error(where_ + ": a score with no song (drv:0396 plays song 0)");
    const std::size_t table = kParts + 0x0A, base = table + 2 * (songs + 1);
    need(base, "the song table");
    const std::size_t song = base + be16(file_, table);
    need(song + 0x0E, "song 0's header");
    tempo_ = be16(file_, song);
    velocity0_ = file_[song + 2];
    const std::size_t voices = file_[song + 3];
    if (voices < 2 || voices > 5) throw Error(where_ + ": song 0 has " + std::to_string(voices) + " voices (1 + 1..4 tracks)");
    // drv:0608: T div (0x1110 div 0x34) = T div 84.
    div_ = static_cast<std::uint16_t>(tempo_ / (0x1110 / 0x34));
    if (div_ == 0) throw Error(where_ + ": song 0's tempo " + std::to_string(tempo_) + " gives a zero divisor (drv:0A8E divides by it)");
    for (std::size_t d = 1; d < voices; ++d) {
        const std::size_t s = song + 0x0E + be16(file_, song + 4 + 2 * d);
        // Every stream must end with FF before the entry does: the
        // sequencer reads two bytes an event and looks one event ahead.
        std::size_t q = s;
        for (;;) {
            need(q + 1, "a track's stream (no FF)");
            if (file_[q] == 0xFF) break;
            need(q + 4, "a track's event");
            q += 2;
        }
        streams_.push_back(s);
    }
}

std::uint8_t MusicDriver::at(std::size_t p) const { return file_.at(p); }

std::uint16_t MusicDriver::duration(std::uint8_t d) {
    // drv:0621 (16-bit unsigned).
    std::uint16_t x = static_cast<std::uint16_t>(0x6900 >> (d & 7));
    if (d & 8) x = static_cast<std::uint16_t>((x >> 1) * 3);
    if (d & 0x30) {
        const int m = (d & 0x30) >> 3;
        x = static_cast<std::uint16_t>((x / (m + 1)) * m);
    }
    return x;
}

void MusicDriver::silence() {
    // drv:121B (via drv:22DB; port 0x61 &= 0xFC is the speaker's).
    for (int ch = 0; ch < 9; ++ch) write(0xB0 + ch, 0x0E);
    write(0xBD, 0x20);
}

void MusicDriver::start() {
    // fn 0: Init (drv:1250): Silence, rhythm mode, CSM off / note select,
    // every operator's waveform 0, waveform select off. One Silence before
    // it, not two (session 72: the original's every OPL write, logged by a
    // patched DOSBox-X; the raw capture drops repeated values).
    silence();
    write(0xBD, 0x20);
    write(0x08, 0x40);
    for (int op : kOps) write(0xE0 + op, 0);
    write(0x01, 0x00);
    used_ = 0;
    for (int k = 0; k < 6; ++k) channel_[static_cast<std::size_t>(k)] = k;
    counter_ = 0;
    // fn 1: StopAll's Silence (drv:0573), then SongStart (drv:0731) at the
    // first non-zero count, 1: the tracks' time starts odd, the sequencer
    // runs on even counts.
    silence();
    tracks_.clear();
    for (std::size_t s : streams_) {
        Track t;
        t.time = 1;
        t.pos = s;
        t.velocity = velocity0_;
        tracks_.push_back(t);
    }
    playing_ = true;
}

void MusicDriver::stop() {
    // fn 3 (drv:03CB): the timer removed, StopAll's Silence (drv:0573),
    // the backend's shutdown (drv:126B: Silence's AdLib part).
    if (!playing_) return;
    playing_ = false;
    tracks_.clear();
    silence();
    silence();
}

void MusicDriver::tick() {
    if (!playing_) return;
    ++counter_;
    if (counter_ & 1) return;            // drv:2984: odd counts return at once
    seq_tick(counter_);
}

void MusicDriver::program(int k, int instrument) {
    // ProgramChange drv:151B: the track's old channel freed, the lowest free
    // melodic channel 0..6 taken (an instrument's byte +1, a fixed channel,
    // is 0 in every shipped song), then Program: each operator's BD, 08, TL,
    // [the modulator: C0], AR/DR, SL/RR, the multiplier.
    const auto& I = instruments_.at(static_cast<std::size_t>(instrument));
    if (I[1] != 0)
        throw Error(where_ + ": instrument " + std::to_string(instrument) + " names a fixed channel (drv:151B: not ported)");
    const auto ku = static_cast<std::size_t>(k);
    used_ = static_cast<std::uint16_t>(used_ & ~(1u << channel_[ku]));
    int ch = 0;
    while (ch < 7 && (used_ >> ch & 1)) ++ch;
    if (ch == 7) throw Error(where_ + ": no free melodic channel (drv:151B: not ported)");
    used_ = static_cast<std::uint16_t>(used_ | 1u << ch);
    const int idx = kChannelIndex[static_cast<std::size_t>(ch)];
    for (int o = 0; o < 2; ++o) {
        const int op = kOps[static_cast<std::size_t>(o == 0 ? idx - 1 : idx + 2)];
        const auto ou = static_cast<std::size_t>(o);
        write(0xBD, 0x20);
        write(0x08, 0x40);
        write(0x40 + op, I[4 + ou]);
        if (o == 0) write(0xC0 + ch, I[0xA]);
        write(0x60 + op, I[6 + ou]);
        write(0x80 + op, I[8 + ou]);
        write(0x20 + op, I[2 + ou]);
    }
    channel_[ku] = ch;
    instrument_[ku] = instrument;
}

void MusicDriver::note(int k, int n, int velocity) {
    // Note drv:12AB (melodic channels): the key released (A0 := 0x4A, B0 :=
    // 0x0E); for a note-on the carrier's TL scaled by the velocity, then the
    // F-number and the block with the key on.
    const auto ku = static_cast<std::size_t>(k);
    const int ch = channel_[ku];
    write(0xA0 + ch, 0x4A);
    write(0xB0 + ch, 0x0E);
    if (velocity < 0) return;
    const int t = instruments_[static_cast<std::size_t>(instrument_[ku])][5];
    const int tl = (0x3F - (((0x3F - (t & 0x3F)) * velocity) >> 7)) | (t & 0xC0);
    write(0x40 + kOps[static_cast<std::size_t>(kChannelIndex[static_cast<std::size_t>(ch)] + 2)], tl);
    const int f = kFnum[static_cast<std::size_t>(n % 12)];
    const int block = std::clamp(n / 12 - 1, 0, 7);
    write(0xA0 + ch, f & 0xFF);
    write(0xB0 + ch, 0x20 | block << 2 | f >> 8);
}

void MusicDriver::seq_tick(std::uint32_t now) {
    // SeqTick drv:0982: each track whose time has come takes one event.
    bool ended = false;
    for (std::size_t k = 0; k < tracks_.size(); ++k) {
        Track& t = tracks_[k];
        const int ki = static_cast<int>(k);
        if (t.time == 0 || t.time > now) continue;
        if (t.state == 0) {                  // the gap before a repeated note (drv:09C1)
            note(ki, t.note, -1);
            t.state = 3;
            t.time += 1;
            continue;
        }
        while (at(t.pos) > 0x80 && at(t.pos) != 0xFF) {   // control pairs (drv:09F9)
            if (at(t.pos) == 0x81) {         // a program change: its operand isn't read
                if (t.state < 3) note(ki, t.note, -1);
                t.state = 3;
                program(ki, ki < 4 ? ki : 1);
            } else if (at(t.pos) == 0x84) {
                t.velocity = 0x7F;
            }
            t.pos += 2;
        }
        if (at(t.pos) == 0xFF) {             // the track ends (drv:0B67)
            t.time = 0;
            if (t.state < 3) note(ki, t.note, -1);
            t.state = 3;
            ended = true;
            continue;
        }
        const std::uint8_t n = at(t.pos), d = at(t.pos + 1);
        const std::uint16_t x = duration(d);  // drv:0A81..0AC0
        t.time += x / div_;
        t.rem = static_cast<std::uint16_t>(t.rem + x % div_);
        if (t.rem >= div_) {
            t.time += 1;
            t.rem = static_cast<std::uint16_t>(t.rem - div_);
        }
        t.pos += 2;
        if (n == 0x80) {                     // a rest
            if (t.state < 3) note(ki, t.note, -1);
            t.state = 3;
            continue;
        }
        if (t.state != 2) {
            if (t.state < 3) note(ki, t.note, -1);
            note(ki, n, t.velocity);
        }
        t.note = n;
        if (d & 0x40) t.state = 2;           // tied: the next note doesn't strike
        else if (at(t.pos) == n) {           // the same note next: a one-tick gap
            t.time -= 1;
            t.state = 0;
        } else t.state = 1;
    }
    if (ended && std::all_of(tracks_.begin(), tracks_.end(), [](const Track& t) { return t.time == 0; })) {
        // drvDS:0296 = -1 (no loop: drv:0940): Silence, the song over.
        silence();
        playing_ = false;
    }
}

}  // namespace cotd
