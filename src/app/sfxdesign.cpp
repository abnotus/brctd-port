// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/sfxdesign.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "app/synth.hpp"
#include "core/files.hpp"
#include "sfx_designs.hpp"

extern "C" {
#include "opl3.h"
}

namespace cotd::lab {

// --- designs -------------------------------------------------------------------

Instrument default_instrument() {
    // Both operators held while the key is down (EG type), instant attack,
    // the fastest release (the speaker stops at once); the speaker's square
    // wave (odd harmonics 1/n): the modulator a quarter-sine pulse at the
    // carrier's frequency, attenuation 30, feedback 1. Fitted by a search
    // over the OPL2's settings against the speaker's own spectrum
    // (docs/notes/sound.md "Sound lab", the instruments).
    return {{0x21, 0x21, 0x1E, 0x00, 0xF0, 0xF0, 0x0F, 0x0F, 0x03, 0x00, 0x02}};
}

std::vector<SpeakerEngine::State> effect_states(const Bytes& image, int id) {
    if (id < kFirstId || id > kLastId) throw Error("sound lab: no effect " + std::to_string(id));
    SpeakerEngine eng(image, "START.EXE");
    eng.start(id - 1);
    std::vector<SpeakerEngine::State> out;
    while (eng.active()) out.push_back(eng.tick());
    return out;
}

namespace {

constexpr int kInaudible = -2;   // divisor 0: each effect's first tick (a quirk, sound.md)

int pitch_of(const SpeakerEngine::State& s) {
    if (s.bits != 3) return -1;
    if (s.divisor == 0) return kInaudible;
    const double hz = 1193182.0 / s.divisor;
    const int p = static_cast<int>(std::lround(69 + 12 * std::log2(hz / 440)));
    return std::clamp(p, 0, kTopPitch);
}

}  // namespace

std::vector<Note> draft(const std::vector<SpeakerEngine::State>& states, int step) {
    step = std::max(1, step);
    std::vector<Note> raw;
    for (std::size_t i = 0; i < states.size(); i += static_cast<std::size_t>(step)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(step), states.size() - i));
        raw.push_back({pitch_of(states[i]), n});
    }
    // An inaudible sounding tick takes the next note's pitch (else the
    // last's), so the draft sounds from the speaker's first tick; then
    // equal neighbours merge.
    for (std::size_t i = raw.size(); i-- > 0;)
        if (raw[i].pitch == kInaudible)
            raw[i].pitch = i + 1 < raw.size() && raw[i + 1].pitch >= 0 ? raw[i + 1].pitch : -1;
    std::vector<Note> out;
    for (const Note& n : raw) {
        if (!out.empty() && out.back().pitch == n.pitch) out.back().ticks += n.ticks;
        else out.push_back(n);
    }
    return out;
}

double note_hz(int pitch) { return 440.0 * std::pow(2.0, (pitch - 69) / 12.0); }

// The file: TOML's subset this needs. A comment header, then per effect
//   [[effect]]
//   id = 7
//   verdict = "approved"        # or "not yet"
//   shift = 0                   # semitones
//   level = 0                   # dB over the speaker's loudness
//   step = 1                    # the draft's step in ticks
//   instrument = [0x21, ...]    # 11 registers (Instrument)
//   notes = [[69, 5], [-1, 3]]  # only when edited: [pitch or -1, ticks]

namespace {

struct Value {
    enum Kind { Int, Str, List } kind = Int;
    long n = 0;
    std::string s;
    std::vector<Value> items;
};

struct Parser {
    const std::string& t;
    std::size_t i = 0;
    std::string where;

    [[noreturn]] void fail(const std::string& what) const { throw Error(where + ": " + what); }
    void space() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    }
    Value value() {
        space();
        if (i >= t.size()) fail("a value is missing");
        Value v;
        if (t[i] == '"') {
            const std::size_t e = t.find('"', i + 1);
            if (e == std::string::npos) fail("a string isn't closed");
            v.kind = Value::Str;
            v.s = t.substr(i + 1, e - i - 1);
            i = e + 1;
        } else if (t[i] == '[') {
            ++i;
            v.kind = Value::List;
            space();
            if (i < t.size() && t[i] == ']') {
                ++i;
                return v;
            }
            for (;;) {
                v.items.push_back(value());
                space();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    ++i;
                    break;
                }
                fail("a list isn't closed");
            }
        } else {
            std::size_t used = 0;
            try {
                v.n = std::stol(t.substr(i), &used, 0);
            } catch (const std::exception&) {
                fail("not a number");
            }
            i += used;
        }
        return v;
    }
};

long int_in(const Value& v, long lo, long hi, const std::string& what, const std::string& where) {
    if (v.kind != Value::Int || v.n < lo || v.n > hi)
        throw Error(where + ": " + what + " must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
    return v.n;
}

}  // namespace

std::map<int, Design> game_designs(const Bytes& image, const std::string& text, const std::string& where) {
    auto out = parse_designs(text, where);
    for (auto& [id, d] : out)
        if (!d.edited) d.notes = draft(effect_states(image, id), d.step);
    return out;
}

std::map<int, Design> built_in_designs(const Bytes& image) {
    return game_designs(image, kBuiltInDesigns, "data/sfx_adlib.toml (built in)");
}

std::map<int, Design> parse_designs(const std::string& text, const std::string& path) {
    std::map<int, Design> out;
    std::istringstream f(text);
    std::string line;
    int no = 0;
    Design pending;
    bool open = false, has_id = false;
    auto close = [&](const std::string& where) {
        if (!open) return;
        if (!has_id) throw Error(where + ": an [[effect]] without an id");
        if (out.count(pending.id)) throw Error(where + ": effect " + std::to_string(pending.id) + " listed twice");
        out[pending.id] = pending;
        open = false;
    };
    while (std::getline(f, line)) {
        ++no;
        const std::string where = path + ":" + std::to_string(no);
        std::string s = line;
        if (!s.empty() && s.back() == '\r') s.pop_back();
        // A comment starts at '#' outside a string.
        bool in_str = false;
        for (std::size_t k = 0; k < s.size(); ++k) {
            if (s[k] == '"') in_str = !in_str;
            if (s[k] == '#' && !in_str) {
                s.resize(k);
                break;
            }
        }
        const auto a = s.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        s = s.substr(a, s.find_last_not_of(" \t") - a + 1);
        if (s == "[[effect]]") {
            close(where);
            pending = Design{};
            has_id = false;
            open = true;
            continue;
        }
        if (!open) throw Error(where + ": a key outside an [[effect]]");
        const auto eq = s.find('=');
        if (eq == std::string::npos) throw Error(where + ": not a key = value line");
        std::string key = s.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        const std::string rest = s.substr(eq + 1);
        Parser p{rest, 0, where};
        const Value v = p.value();
        p.space();
        if (p.i != rest.size()) throw Error(where + ": text after the value");
        if (key == "id") {
            pending.id = static_cast<int>(int_in(v, kFirstId, kLastId, "id", where));
            has_id = true;
        } else if (key == "verdict") {
            if (v.kind != Value::Str || (v.s != "approved" && v.s != "not yet"))
                throw Error(where + ": verdict must be \"approved\" or \"not yet\"");
            pending.verdict = v.s == "approved" ? Verdict::Approved : Verdict::NotYet;
        } else if (key == "shift") {
            pending.shift = static_cast<int>(int_in(v, -48, 48, "shift", where));
        } else if (key == "level") {
            pending.level = static_cast<int>(int_in(v, -24, 12, "level", where));
        } else if (key == "step") {
            pending.step = static_cast<int>(int_in(v, 1, 32, "step", where));
        } else if (key == "instrument") {
            if (v.kind != Value::List || v.items.size() != pending.ins.regs.size())
                throw Error(where + ": instrument must list 11 registers");
            for (std::size_t k = 0; k < v.items.size(); ++k)
                pending.ins.regs[k] = static_cast<int>(int_in(v.items[k], 0, 255, "a register", where));
        } else if (key == "notes") {
            if (v.kind != Value::List || v.items.empty()) throw Error(where + ": notes must list at least one note");
            pending.notes.clear();
            for (const Value& n : v.items) {
                if (n.kind != Value::List || n.items.size() != 2) throw Error(where + ": a note is [pitch, ticks]");
                pending.notes.push_back({static_cast<int>(int_in(n.items[0], -1, kTopPitch, "a pitch", where)),
                                         static_cast<int>(int_in(n.items[1], 1, 10000, "a length", where))});
            }
            pending.edited = true;
        } else {
            throw Error(where + ": unknown key " + key);
        }
    }
    close(path + ":" + std::to_string(no));
    return out;
}

// --- the AdLib voice -------------------------------------------------------------

struct AdlibVoice::Chip {
    opl3_chip c{};
    std::uint8_t b0 = 0;
    void write(int reg, int v) { OPL3_WriteReg(&c, static_cast<std::uint16_t>(reg), static_cast<std::uint8_t>(v)); }
};

AdlibVoice::AdlibVoice(const Design& d, double gain)
    : chip_(new Chip), notes_(d.notes), shift_(d.shift), gain_(gain) {
    OPL3_Reset(&chip_->c, kAudioRate);
    chip_->write(0x01, 0x20);   // the waveform select
    const auto& r = d.ins.regs;
    static constexpr int kRegs[10] = {0x20, 0x23, 0x40, 0x43, 0x60, 0x63, 0x80, 0x83, 0xE0, 0xE3};   // channel 0's slots 0, 3
    for (int k = 0; k < 10; ++k) chip_->write(kRegs[k], r[static_cast<std::size_t>(k)]);
    chip_->write(0xC0, r[10]);
}

AdlibVoice::~AdlibVoice() { delete chip_; }

void AdlibVoice::tick() {
    if (ended_) return;
    if (left_ == 0) {
        if (at_ >= notes_.size()) {
            if (sounding_) chip_->write(0xB0, chip_->b0 & ~0x20);
            sounding_ = false;
            ended_ = true;
            return;
        }
        const Note& n = notes_[at_++];
        left_ = n.ticks;
        if (n.pitch < 0) {
            if (sounding_) chip_->write(0xB0, chip_->b0 & ~0x20);
            sounding_ = false;
        } else {
            // The F-number in the lowest block that holds it (49,716 Hz:
            // f = F * 49716 / 2^(20 - block)); a note after a sounding one
            // keeps the key down (no new attack).
            const double hz = note_hz(std::clamp(n.pitch + shift_, 0, kTopPitch));
            int block = 0;
            double fn = 0;
            for (; block < 8; ++block) {
                fn = hz * static_cast<double>(1 << (20 - block)) / kAudioRate;
                if (fn <= 1023) break;
            }
            block = std::min(block, 7);
            const int f = std::clamp(static_cast<int>(std::lround(fn)), 1, 1023);
            chip_->write(0xA0, f & 0xFF);
            chip_->b0 = static_cast<std::uint8_t>(0x20 | block << 2 | f >> 8);
            chip_->write(0xB0, chip_->b0);
            sounding_ = true;
        }
    }
    --left_;
}

void AdlibVoice::next(std::int16_t out[2]) {
    // The notes at the effect unit's clock, as SpeakerVoice ticks it.
    ticks_ += SpeakerEngine::kTickHz / kAudioRate;
    while (ticks_ >= 1.0) {
        ticks_ -= 1.0;
        tick();
    }
    OPL3_Generate(&chip_->c, out);
    for (int c = 0; c < 2; ++c) {
        const double v = out[c] * gain_;
        out[c] = static_cast<std::int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (ended_) ++tail_;
}

double loudness_gain(const Bytes& image, const Design& d) {
    // Both versions rendered to their ends: the RMS of each.
    SpeakerVoice spk(image, "START.EXE");
    spk.unit().play(d.id);
    double es = 0;
    long n = 0;
    do {
        const double v = spk.next();
        es += v * v;
        ++n;
    } while (spk.unit().active() || n < kAudioRate / 100);
    AdlibVoice fm(d, 1.0);
    double ef = 0;
    long m = 0;
    std::int16_t f[2];
    do {
        fm.next(f);
        ef += static_cast<double>(f[0]) * f[0];
        ++m;
    } while (!fm.ended());
    const double level = std::pow(10.0, d.level / 20.0);
    if (ef <= 0) return level;
    return std::sqrt(es / static_cast<double>(n)) / std::sqrt(ef / static_cast<double>(m)) * level;
}

bool AdlibVoice::done() const { return ended_ && tail_ >= kAudioRate / 10; }

}  // namespace cotd::lab
