// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/audio.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include <map>

#include "app/sfxdesign.hpp"
#include "app/synth.hpp"
#include "core/music.hpp"
#include "game/combat_rules.hpp"

extern "C" {
#include "opl3.h"
}

namespace cotd {

namespace {
constexpr int kRate = kAudioRate;   // the chip's own sample rate (OPL3_Generate)

// The stream's lock, released on a throw too (session 74's wrap-up: a
// rejection while it was held left it locked, and the window's destructor
// then destroyed the stream under it).
struct StreamLock {
    SDL_AudioStream* s;
    explicit StreamLock(SDL_AudioStream* stream) : s(stream) {
        if (s) SDL_LockAudioStream(s);
    }
    ~StreamLock() {
        if (s) SDL_UnlockAudioStream(s);
    }
    StreamLock(const StreamLock&) = delete;
    StreamLock& operator=(const StreamLock&) = delete;
};
}

struct Audio::Impl {
    SDL_AudioStream* stream = nullptr;
    opl3_chip chip{};
    std::unique_ptr<MusicDriver> driver;
    double ticks = 0;          // driver ticks owed, in fractions
    int volume = 10;
    std::vector<std::int16_t> buf;
    std::unique_ptr<SpeakerVoice> spk;   // the effect unit on the speaker (app/synth.hpp)
    // The same unit on the Tandy's tables (D-0093): both get every command,
    // so a change of device in the options is heard at once.
    std::unique_ptr<TandyVoice> tnd;
    Fx device = Fx::Speaker;
    // The effects on the AdLib (D-0069): the approved designs on their own
    // OPL2, the effect unit's commands still deciding what plays; the
    // speaker is muted while an AdLib version plays.
    bool adlib_fx = false;
    Bytes image;
    std::map<int, lab::Design> designs;   // built in (app/sfxdesign.hpp), on first use
    bool designs_built = false, designs_failed = false;
    std::string designs_text, designs_where;   // tests: instead of the built-in file
    std::map<int, double> gains;          // loudness_gain per id, on first use
    std::unique_ptr<lab::AdlibVoice> fx;
    bool spk_muted = false;
    // A rejection on the audio thread (the music driver's or the effect
    // unit's, D-0082: no shipped data reaches one): that source stops, the
    // rest plays on, and the message waits here for the game's thread
    // (report_faults), which owns the key log. Guarded by the stream lock.
    std::vector<std::string> faults;

    void fill(std::int16_t* out, int frames) {
        for (int i = 0; i < frames; ++i) {
            // The driver's INT 1Ch: 1193182 / 0x4000 ticks a second.
            ticks += MusicDriver::kTickHz / kRate;
            while (ticks >= 1.0) {
                ticks -= 1.0;
                if (driver) {
                    try {
                        driver->tick();
                    } catch (const std::exception& e) {
                        driver.reset();
                        for (int r = 0xB0; r <= 0xB8; ++r) OPL3_WriteReg(&chip, static_cast<std::uint16_t>(r), 0);   // key off
                        faults.push_back(std::string("the music stops: ") + e.what());
                    }
                }
            }
            double s = 0;   // the unit ticks either way
            if (spk) {
                try {
                    s = spk->next();
                } catch (const std::exception& e) {
                    spk.reset();
                    faults.push_back(std::string("the sound effects stop: ") + e.what());
                }
            }
            if (tnd) {
                try {
                    const double t = tnd->next();
                    if (device == Fx::Tandy) s = t;
                } catch (const std::exception& ex) {
                    tnd.reset();
                    if (device == Fx::Tandy) s = 0;
                    faults.push_back(std::string("the sound effects stop: ") + ex.what());
                }
            } else if (device == Fx::Tandy) {
                s = 0;
            }
            if (spk_muted) s = 0;
            std::int16_t e[2] = {0, 0};
            if (fx) {
                fx->next(e);
                if (fx->done()) fx.reset();
            }
            std::int16_t f[2];
            OPL3_Generate(&chip, f);
            for (int c = 0; c < 2; ++c) {
                const double v = (f[c] + s + e[c]) * volume / 10;
                out[2 * i + c] = static_cast<std::int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
    }

    static void SDLCALL feed(void* user, SDL_AudioStream* s, int additional, int) {
        auto* self = static_cast<Impl*>(user);
        const int frames = additional / 4;
        if (frames <= 0) return;
        self->buf.resize(static_cast<std::size_t>(frames) * 2);
        self->fill(self->buf.data(), frames);
        SDL_PutAudioStreamData(s, self->buf.data(), frames * 4);
    }
};

Audio::Audio(bool device) : impl_(std::make_unique<Impl>()) {
    OPL3_Reset(&impl_->chip, kRate);
    if (!device || !SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kRate};
    impl_->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::feed, impl_.get());
    if (impl_->stream) SDL_ResumeAudioStreamDevice(impl_->stream);
}

Audio::~Audio() {
    if (impl_->stream) SDL_DestroyAudioStream(impl_->stream);
}

bool Audio::ok() const { return impl_->stream != nullptr; }

void Audio::start(const Bytes& song) {
    StreamLock lock(impl_->stream);
    Impl* p = impl_.get();
    impl_->driver = std::make_unique<MusicDriver>(song, "MUSIC.DAX #2", [p](int reg, int value) {
        OPL3_WriteReg(&p->chip, static_cast<std::uint16_t>(reg), static_cast<std::uint8_t>(value));
    });
    impl_->driver->start();
    impl_->ticks = 0;
}

void Audio::stop() {
    StreamLock lock(impl_->stream);
    if (impl_->driver) impl_->driver->stop();
    // 07DA:0082 (0x17E22): the driver and song's memory freed (08F3:001F), once (DS:4896)
    impl_->driver.reset();
}

void Audio::effects(const Bytes& image) {
    if (impl_->spk) return;
    auto voice = std::make_unique<SpeakerVoice>(image, "START.EXE");
    auto tandy = std::make_unique<TandyVoice>(image, "START.EXE");
    StreamLock lock(impl_->stream);
    impl_->spk = std::move(voice);
    impl_->tnd = std::move(tandy);
    impl_->image = image;
}


void Audio::effects_device(Fx fx) {
    StreamLock lock(impl_->stream);
    impl_->device = fx;
    impl_->adlib_fx = fx == Fx::Adlib;
    if (!impl_->adlib_fx) {   // what plays on the AdLib stops; the speaker's own goes on
        impl_->fx.reset();
        impl_->spk_muted = false;
    }
}

void Audio::sound(int n) {
    if (!impl_->spk) return;
    // An effect's AdLib version (an approved design) is made here, on the
    // game's thread; every command cuts what the AdLib plays, as the unit
    // cuts the speaker's.
    std::unique_ptr<lab::AdlibVoice> next;
    if (impl_->adlib_fx && !impl_->designs_failed && n >= lab::kFirstId && n <= lab::kLastId) {
        try {
            if (!impl_->designs_built) {
                impl_->designs = impl_->designs_text.empty()
                                     ? lab::built_in_designs(impl_->image)
                                     : lab::game_designs(impl_->image, impl_->designs_text, impl_->designs_where);
                impl_->designs_built = true;
            }
            if (auto it = impl_->designs.find(n);
                it != impl_->designs.end() && it->second.verdict == lab::Verdict::Approved) {
                auto g = impl_->gains.find(n);
                if (g == impl_->gains.end())
                    g = impl_->gains.emplace(n, lab::loudness_gain(impl_->image, it->second)).first;
                next = std::make_unique<lab::AdlibVoice>(it->second, g->second);
            }
        } catch (const std::exception& e) {
            // The speaker plays them instead, for the rest of the run.
            impl_->designs_failed = true;
            impl_->designs.clear();
            game::combat::diag("D-0069", std::string("the AdLib effects can't be built (") + e.what() +
                                             "); the PC speaker plays the effects");
        }
    }
    StreamLock lock(impl_->stream);
    if (!impl_->spk) return;   // stopped by a rejection on the audio thread
    if (n == 0xFF) impl_->spk->unit().command(n);
    else impl_->spk->unit().play(n);
    if (impl_->tnd) {
        if (n == 0xFF) impl_->tnd->unit().command(n);
        else impl_->tnd->unit().play(n);
    }
    if (n == 0 || n == 0xFF || (n >= lab::kFirstId && n <= lab::kLastId)) {
        impl_->fx = std::move(next);
        impl_->spk_muted = impl_->fx != nullptr;
    }
}


void Audio::set_volume(int volume) {
    StreamLock lock(impl_->stream);
    impl_->volume = volume < 0 ? 0 : volume > 10 ? 10 : volume;
}

void Audio::report_faults() {
    std::vector<std::string> faults;
    {
        StreamLock lock(impl_->stream);
        faults.swap(impl_->faults);
    }
    for (const std::string& f : faults) game::combat::diag("D-0082", f);
}

}  // namespace cotd
