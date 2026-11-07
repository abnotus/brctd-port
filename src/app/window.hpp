// SPDX-License-Identifier: GPL-3.0-or-later
// The interactive window (SDL3): shows the framebuffer, reads the keys,
// the mouse, plays the sound and keeps the settings.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/framebuffer.hpp"
#include "game/input.hpp"

namespace cotd {

struct WindowClosed {};   // the player closed the window

class Settings;

struct WindowOptions {
    // Aspect correction (DIVERGENCE.md D-0012): stretch 320x200 to 4:3,
    // as a CRT showed mode 13h. Off by default: the exact framebuffer,
    // integer-scaled with square pixels. F9 toggles it while playing.
    bool aspect = false;
    // The combat speed option (D-0054): the factor on combat's waits (1.0:
    // the original's). F8 cycles 1.0, 1.25, 1.5 and fast (0.25).
    double speed = 1.0;
    // The text options (session 27, the owner's): Text.Print's typewriter
    // or the whole text at once (D-0057); keys and clicks that arrive
    // while text types out are dropped (D-0059).
    game::TextSpeed text = game::TextSpeed::Typewriter;
    bool drop_typing_keys = false;
    // The step pace (D-0064): ms after each seen computer combat step, 0 off.
    int step_pace_ms = 0;
    // The team menu's JOYSTICK/MOUSE INITIALIZE as the port's Options
    // (D-0065): on in play, off with injected keys unless asked.
    bool options_menu = false;
    // A gamepad's text entry (D-0085, session 66): at a text or number
    // prompt the d-pad picks characters; on in play, off with injected keys.
    bool pad_text = false;
    // Sound (D-0067): the music on the AdLib (BUCK.CFG's "A") or Silent
    // ("S": tests and replays), and its volume 0 (muted) .. 10.
    bool sound = false;
    int volume = 6;
    // The sound effects with the sound on, the effect unit read from
    // START.EXE's load image (core/speaker.hpp): the original's Tandy 1000
    // versions (D-0093, the default in play), their AdLib versions (D-0069),
    // the original's PC speaker (D-0068, tests) or off (by the option, or
    // with injected keys).
    enum class Effects { Off, Speaker, Tandy, Adlib };
    Effects effects = Effects::Tandy;
    Bytes speaker_image;
    // The port's settings file (app/settings.hpp): the window's last
    // position, size and full-screen state are read from it and written
    // back; null: none (tests).
    Settings* settings = nullptr;
};

// The Effects option's values: "tandy" (D-0093), "adlib" (D-0069),
// "speaker" (D-0068, tests), "off"; "on" (the setting before D-0069) is
// "tandy", the default since session 78.
WindowOptions::Effects parse_effects(const std::string& value);

// Each gamepad button's key (D-0085): pad_<button> in the settings file,
// else the defaults; -1 for none.
std::array<int, game::kPadKeys> pad_keymap(const Settings* settings);


// A message for the player who started `play` (session 72, M2): an error
// box (an unidentified folder, a save folder that can't be written, an
// error that ends the game), since a console window closes with the
// process. Shown only where a player is: never with injected keys or the
// offscreen or dummy video driver (tests, replays); false when not shown.
bool show_player_message(const std::string& text, bool error);

// The game's key source in a window: each read shows the screen and waits
// for a key (game::Input).
class WindowInput : public game::Input {
public:
    explicit WindowInput(const std::string& title, WindowOptions opt = {});
    ~WindowInput() override;
    int read_key(const Framebuffer& screen, const char* what) override;
    void pause(const Framebuffer& screen, int ms) override;
    bool wait(const Framebuffer& screen, int ms, const std::function<void()>& second = nullptr) override;
    long clock_cs() override;      // the host's time of day (-1 in tests)
    int read_key_for(const Framebuffer& screen, const char* what, int ms) override;
    bool key_waiting() override;   // a key read_key would return is queued
    void flush() override;         // queued keys dropped
    void pointer(const std::array<std::uint16_t, 32>& masks, bool on) override;
    double speed_factor() override;        // the combat speed option
    game::TextSpeed text_speed() override;     // the text option (D-0057)
    int step_pace_ms() override;               // the step pace (D-0064)
    std::string option(const std::string& key) override;   // the options screen (D-0065)
    void set_option(const std::string& key, const std::string& value) override;
    void options_open(bool on) override;       // closing it writes the settings file
    bool options_menu() override;
    void typing(bool on) override;             // keys dropped while text types (D-0059)
    void music_start(const Bytes& song) override;   // the title's music (D-0067)
    void music_stop() override;
    void sound(int n) override;                     // the effects (D-0068)
    int sound_line() override;                      // 3 with the Tandy's effects (D-0093)
    void offer_code(const std::string& code) override;   // F1 types it (D-0084)
    void text_entry(int kind) override;                  // the pad's picker (D-0085)
    void entry_field(const std::string& text, int cursor) override;   // the field it edits (session 68)
    void present(const Framebuffer& screen);   // show the screen, no wait
    // Where the window is, for the key log: the video driver, the
    // displays and the window's rect (window coordinates).
    std::string describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cotd
