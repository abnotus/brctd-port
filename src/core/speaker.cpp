// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/speaker.hpp"

#include <algorithm>

namespace cotd {

namespace {

// The unit's segment in START.EXE's load image (0906:0000) and the part
// its routines touch: the records (0149..0x2E0), the tables and the
// speaker's streams (up to 0x0DB3), the Tandy part after them.
constexpr std::uint32_t kSegment = 0x0906 * 16;
constexpr std::uint32_t kSize = 0x1800;

// Unit addresses (0906:xxxx; docs/notes/sound.md).
constexpr std::uint32_t kCurrent = 0x011B;    // the effect playing (0: none)
constexpr std::uint32_t kFetching = 0x011D;   // the record EventFetch serves
constexpr std::uint32_t kRecords = 0x011F;    // the record base (0149 here)
constexpr std::uint32_t kNotes = 0x0121;      // the note table (02C9 here)
constexpr std::uint32_t kChannelEffect = 0x0125;
constexpr std::uint32_t kEnable = 0x0129;     // SetSpeakerEnable's byte (1)
constexpr std::uint32_t kTable = 0x047B;      // 21 effects x 4 stream pointers
constexpr std::uint32_t kAudible = 0x02E1;    // SpeakerUpdate's chosen record
constexpr std::uint32_t kRecord0 = 0x0149;
constexpr std::uint32_t kRecordSize = 0x30;

}  // namespace

SpeakerEngine::SpeakerEngine(const Bytes& image, std::string where, Device device)
    : where_(std::move(where)), device_(device) {
    if (device_ == Device::Tandy) {   // StartTandy 0906:16CA, TandyUpdate 0906:1408 (session 78)
        records_ = 0x02E3;
        notes_ = 0x0463;
        table_ = 0x0523;
    }
    if (image.size() < kSegment + kSize)
        throw Error(where_ + ": the load image ends before the effect unit (0906:0000)");
    // 0906:00D6 (0x19136): the unit's init: DS:3BC2 := 0906:00F7, the watchdog word := 0; its tables are read from here
    seg_.assign(image.begin() + kSegment, image.begin() + kSegment + kSize);
    // Every effect played to its end once (on a scratch copy): the quirk
    // policy's load-time check (docs/notes/sound.md "The port").
    const std::vector<std::uint8_t> pristine = seg_;
    for (int e = 1; e < kEffects; ++e) {
        start(e);
        int t = 0;
        while (active()) {
            if (device_ == Device::Tandy) tandy_tick();
            else tick();
            if (++t > 5000)
                throw Error(where_ + ": effect " + std::to_string(e) + " (Sound.Play id " +
                            std::to_string(e + 1) + ") doesn't end within 5,000 ticks");
        }
    }
    seg_ = pristine;
    effect_ = 0;
}

std::uint8_t SpeakerEngine::rb(std::uint32_t o) const {
    o &= 0xFFFF;
    if (o >= seg_.size())
        throw Error(where_ + ": effect " + std::to_string(effect_) + " reads 0906:" + hex(o, 4) +
                    ", outside the unit's tables and streams");
    return seg_[o];
}

std::uint16_t SpeakerEngine::rw(std::uint32_t o) const {
    return static_cast<std::uint16_t>(rb(o) | rb(o + 1) << 8);
}

void SpeakerEngine::ww(std::uint32_t o, std::uint32_t v) {
    o &= 0xFFFF;
    if (o + 1 >= seg_.size())
        throw Error(where_ + ": effect " + std::to_string(effect_) + " writes 0906:" + hex(o, 4) +
                    ", outside the unit's tables and streams");
    seg_[o] = static_cast<std::uint8_t>(v);
    seg_[o + 1] = static_cast<std::uint8_t>(v >> 8);
}

void SpeakerEngine::start(int e) {
    // StartSpeaker(e) 0906:165E: the effect's stream pointer per channel;
    // a channel with one gets its record cleared, the stream and a wait of
    // 1 (fetched at the next tick).
    effect_ = e;
    ww(kCurrent, static_cast<std::uint32_t>(e));
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint32_t di = records_ + kRecordSize * i;
        const std::uint16_t p = rw(table_ + 8 * static_cast<std::uint32_t>(e) + 2 * i);
        if (p == 0) continue;
        for (std::uint32_t k = 0; k < kRecordSize; k += 2) ww(di + k, 0);
        ww(di + 2, p);
        seg_[kChannelEffect + i] = static_cast<std::uint8_t>(e);
        ww(di, 1);
    }
}

void SpeakerEngine::fetch(std::uint16_t si) {
    // EventFetch 0906:1527 (0x1A587): the channel's stream from +02 until
    // a wait is set.
    std::uint16_t di = rw(si + 2u);
    if (di == 0) return;
    ww(kFetching, si);
    for (;;) {
        const std::uint8_t op = rb(di++);
        if (op == 0xFE) {                                   // loop (0x1A5AA)
            const std::uint8_t f = rb(di++);
            const std::uint16_t target = rw(di);
            di = static_cast<std::uint16_t>(di + 2);
            const std::uint16_t n = rw(si + f);
            if (n != 0) {
                ww(si + f, n - 1u);
                if (n - 1u == 0) continue;
            }
            di = target;
            continue;
        }
        if (op == 0xFF) {                                   // set (0x1A5FB)
            const std::uint8_t f = rb(di++);
            const std::uint16_t v = rw(di);
            di = static_cast<std::uint16_t>(di + 2);
            ww(si + f, v);
            if (f != 0) continue;
            break;                                          // a wait: the fetch ends
        }
        // Note groups (< FA), FA (clear), FB/FC (return/call), FD (another
        // record): only the unreachable effect 20 uses them.
        throw Error(where_ + ": effect " + std::to_string(effect_) + "'s stream at 0906:" +
                    hex(di - 1u, 4) + " uses opcode " + hex(op, 2) +
                    ", which no shipped effect uses (not ported)");
    }
    // The end (0x1A60B): a wait of 0 ends the channel.
    const std::uint16_t r = rw(kFetching);
    if (rw(r) == 0) {
        seg_[kChannelEffect + (r - records_) / kRecordSize] = 0;
        di = 0;
    }
    ww(r + 2u, di);
}

void SpeakerEngine::step(std::uint16_t si) {
    // ChannelStep 0906:1496 (0x1A4F6): the level and pitch slides (16-bit
    // wrap), the pitch modulation, the release countdown, the wait (the
    // fetch when it runs out), the envelope.
    ww(si + 0xAu, rw(si + 0xAu) + rw(si + 0xCu));
    ww(si + 4u, rw(si + 4u) + rw(si + 6u));
    std::uint16_t dx = 0;
    std::uint16_t ph = static_cast<std::uint16_t>(rw(si + 0x1Eu) + rw(si + 0x20u));
    if (ph != 0) {
        if (ph >= rw(si + 0x24u)) ph = static_cast<std::uint16_t>(ph - rw(si + 0x24u));
        ww(si + 0x1Eu, ph);
        const std::uint16_t at = static_cast<std::uint16_t>((static_cast<std::int16_t>(ph) >> 4) + rw(si + 0x1Cu));
        const std::int32_t prod = static_cast<std::int16_t>(rb(at) << 8) *
                                  static_cast<std::int32_t>(static_cast<std::int16_t>(rw(si + 0x22u)));
        dx = static_cast<std::uint16_t>(static_cast<std::uint32_t>(prod) >> 16);   // imul's DX
    }
    ww(si + 8u, dx + rw(si + 4u));
    if (rw(si + 0x14u) != 0) {
        ww(si + 0x14u, rw(si + 0x14u) - 1u);
        if (rw(si + 0x14u) == 0) {
            ww(si + 0x18u, 0x10);
            ww(si + 0x1Au, 1);
        }
    }
    ww(si, rw(si) - 1u);
    if (rw(si) == 0) fetch(si);
    if (rw(si + 0x1Au) != 0) {
        ww(si + 0x1Au, rw(si + 0x1Au) - 1u);
        if (rw(si + 0x1Au) == 0) {
            for (int guard = 0;; ++guard) {
                if (guard > 64)
                    throw Error(where_ + ": effect " + std::to_string(effect_) + "'s envelope doesn't end");
                const std::uint16_t bx = static_cast<std::uint16_t>(rw(si + 0x16u) + rw(si + 0x18u));
                if (rw(bx + 2u) == 0xFFFF) {
                    const std::uint16_t ax = rw(bx);
                    ww(si + 0xAu, ax);
                    if (ax == 0) ww(si + 0xCu, 0);
                    ww(si + 0x18u, rw(si + 0x18u) + 4u);
                    continue;
                }
                ww(si + 0xCu, rw(bx));
                ww(si + 0x1Au, rw(bx + 2u));
                ww(si + 0x18u, rw(si + 0x18u) + 4u);
                break;
            }
        }
    }
}

SpeakerEngine::State SpeakerEngine::tick() {
    // SpeakerUpdate 0906:1393 (0x1A3F3): each channel with a wait steps;
    // the first audible one (level +0A set, still waiting) is sounded.
    if (rw(kCurrent) == 0) return {};
    ww(kAudible, 0);
    ww(kNotes, 0x02C9);
    ww(kRecords, kRecord0);
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint16_t si = static_cast<std::uint16_t>(kRecord0 + kRecordSize * i);
        if (rw(si) == 0) continue;
        step(si);
        if (rw(kAudible) == 0 && rw(si + 0xAu) != 0 && rw(si) != 0) ww(kAudible, si);
    }
    if (seg_[kEnable] == 0 || rw(kAudible) == 0) return {};
    const std::uint16_t r = rw(kAudible);
    return {rw(r + 0xAu) & 3, rw(r + 8u)};
}

bool SpeakerEngine::active() const {
    for (std::uint32_t i = 0; i < 4; ++i)
        if (rw(records_ + kRecordSize * i) != 0) return true;
    return false;
}

SpeakerEngine::TandyState SpeakerEngine::tandy_tick() {
    // TandyUpdate 0906:1408 (0x1A468): with an effect, each Tandy record
    // with a wait steps (notes 0463, records 02E3); then, the unit enabled
    // (byte 0129), every record's frequency and level are written; else
    // the four volumes off.
    TandyState s;
    if (rw(kCurrent) == 0) return s;
    ww(kNotes, notes_);
    ww(kRecords, records_);
    for (std::uint32_t i = 0; i < 4; ++i) {
        const std::uint16_t si = static_cast<std::uint16_t>(records_ + kRecordSize * i);
        if (rw(si) != 0) step(si);
    }
    if (seg_[kEnable] == 0) return s;
    s.silent = false;
    for (std::uint32_t i = 0; i < 4; ++i) {
        s.freq[i] = rw(records_ + kRecordSize * i + 8u);
        s.level[i] = rw(records_ + kRecordSize * i + 0xAu);
    }
    return s;
}

std::vector<std::uint8_t> SpeakerEngine::TandyState::bytes() const {
    if (silent) return {0x9F, 0xBF, 0xDF, 0xFF};   // 0x1A4E5
    std::vector<std::uint8_t> out;
    for (int c = 0; c < 4; ++c) {                   // 0x1A4A6..0x1A4E2
        const std::uint16_t f = static_cast<std::uint16_t>(freq[c] >> 2);
        const std::uint8_t dl = static_cast<std::uint8_t>(0x80 + 0x20 * c);
        out.push_back(static_cast<std::uint8_t>(((f & 0xFF) >> 4) ^ dl));
        if (c < 3) out.push_back(static_cast<std::uint8_t>(f >> 8));
        const std::uint16_t v = static_cast<std::uint16_t>(0xFFFF - level[c]);
        out.push_back(static_cast<std::uint8_t>(((v >> 12) ^ dl) + 0x10));
    }
    return out;
}

void SfxUnit::play(int id) {
    // Sound.Play 07DA:0000 (the music's stop is the game's, Music.Stop):
    // SfxInit when not installed (TimerInstall; StartSpeaker(0)).
    if (!installed_) {
        installed_ = true;
        engine_.start(0);
    }
    command(id);
}

// 0906:1736 (0x1A796): SetSpeakerEnable: 0906:0129 := the argument; only ever 1, its image value
void SfxUnit::command(int n) {
    // SfxCommand 0906:0000.
    if (n == 0) {
        engine_.start(0);
    } else if (n == 0xFF) {
        if (installed_) {
            engine_.start(0);
            installed_ = false;
        }
    } else if (n >= 2 && n <= SpeakerEngine::kEffects) {
        engine_.start(n - 1);
    }
}

}  // namespace cotd
