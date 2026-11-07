// SPDX-License-Identifier: GPL-3.0-or-later
// Where the game's keys come from. The game reads a key only when it
// waits for one (the original's GetKey / Crt.ReadKey); everything between
// two reads runs without input. Implementations:
//   QueueInput  - headless: a fixed key list; optionally writes the
//                 screen at every read (a checkpoint, as the original's
//                 captures by tools/keyfeed.S); parks when the list ends.
//   WindowInput - app/window.cpp: shows the screen, waits for a key.
#pragma once

#include <array>
#include <cctype>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <vector>

#include "core/framebuffer.hpp"

namespace cotd::game {

// The text options (session 27, the owner's play-testing): how long text
// stays or takes to appear, never what is drawn or which key is read.
// Text.Print's typewriter (DIVERGENCE.md D-0057): the original's, by
// CHANGE -> Messages, or the whole text at once.
enum class TextSpeed { Typewriter, Instant };

class Input {
public:
    virtual ~Input() = default;
    // The game waits for a key; `screen` is what is on screen now.
    virtual int read_key(const Framebuffer& screen, const char* what) = 0;
    // A timed wait that reads no key (Crt.Delay): the window shows the
    // screen and waits; headless sources return at once.
    virtual void pause(const Framebuffer& screen, int ms) { (void)screen; (void)ms; }
    // A timed wait that a key cuts short (the intro's WaitSeconds, which
    // polls KeyPressed): true if a key ended it. The key stays in the
    // buffer; WaitSeconds flushes it (Game::wait_seconds). `second` is
    // called each time a whole second of it has passed, but the last
    // (WaitSeconds' sound, 0x3010E). Headless sources return false at once
    // unless their next step is offered there (QueueInput::wait), its
    // seconds all passed.
    virtual bool wait(const Framebuffer& screen, int ms, const std::function<void()>& second = nullptr) {
        (void)screen;
        for (int s = 1000; second && s < ms; s += 1000) second();
        return false;
    }
    // The time of day in centiseconds (Game.GetTime 0653:0C81), or -1 where
    // no time passes: headless sources, whose keys are answered at once
    // (the test harness's rule, docs/notes/replay.md "Time").
    virtual long clock_cs() { return -1; }
    // A key read that gives up after `ms` (MenuScreen's poll while its
    // picture animates): the key, or -1 when the time ran out. Headless
    // sources read the next key at once.
    virtual int read_key_for(const Framebuffer& screen, const char* what, int ms) {
        (void)ms;
        return read_key(screen, what);
    }
    // A key is waiting (Input 08AD:033E, polled without waiting, e.g. by
    // the AI's Quick interrupt): headless sources never have one, as
    // tools/keyfeed.S never answers that poll.
    virtual bool key_waiting() { return false; }
    // Input.FlushKeys (08AD:03E4): the keys in the BIOS buffer go (typed
    // ahead, or a held key's repeats). The game calls it at the original's
    // flush points (Game::flush_keys).
    virtual void flush() {}
    // The mouse pointer, as the driver draws it (window only; the
    // captures have none, docs/notes/mouse.md): the cursor masks given to
    // INT 33h fn 9 (16 screen-mask rows, then 16 cursor-mask rows), shown
    // while `on` (a driver, DS:8434).
    virtual void pointer(const std::array<std::uint16_t, 32>& masks, bool on) { (void)masks; (void)on; }
    // A menu's timeout ended its wait with no key (the title's 120 s): the
    // key log writes the idle step there, so the session replays.
    virtual void timed_out() {}
    // The combat speed option (D-0054): the factor on combat's waits, 1.0
    // for the original's pace (and always for headless sources unless a
    // test sets one).
    virtual double speed_factor() { return 1.0; }
    // The text option (D-0057): the original's for headless sources
    // unless a test sets one.
    virtual TextSpeed text_speed() { return TextSpeed::Typewriter; }
    // The step pace (D-0064, session 32): ms after each computer-controlled
    // combat step the player sees; 0 (the original's) for headless sources.
    virtual int step_pace_ms() { return 0; }
    // The port's options (D-0065, session 33): each by its settings-file
    // key, read and changed by the options screen (game/options.cpp);
    // options_open frames the screen's own reads (never the game's: not
    // logged); options_menu says whether the team menu's JOYSTICK/MOUSE
    // INITIALIZE is the port's Options (off in tests and replays).
    virtual std::string option(const std::string& key) { (void)key; return {}; }
    virtual void set_option(const std::string& key, const std::string& value) { (void)key; (void)value; }
    virtual void options_open(bool on) { (void)on; }
    virtual bool options_menu() { return false; }
    // A game key (upper case) that opens the options screen at the next
    // reads, as the options key does (CHANGE's M in play, where Messages
    // was); 0: none. The key log leaves it out with the screen's keys.
    virtual void options_key(int k) { (void)k; }
    // Text.Print's typewriter runs (on) or has finished (off). The window
    // drops the keys and clicks that arrive meanwhile when its option is on
    // (D-0059): they never reach a read, so a key log holds only keys that
    // were read, and replays in the original as before.
    virtual void typing(bool on) { (void)on; }
    // The music driver installed and playing `song` (a MUSIC.DAX entry:
    // Music.Play 07DA:0029), and removed (Music.Stop). Only the window
    // plays it (src/app/audio.cpp; off in tests and replays): headless runs
    // stay Silent, as the oracle's configuration (D-0019, D-0067).
    virtual void music_start(const Bytes& song) { (void)song; }
    virtual void music_stop() {}
    // The effect unit's command n (SfxCommand 0906:0000, through Sound.Play
    // 07DA:0000 or directly: 0 silence, 0xFF removed, 2..20 an effect;
    // core/speaker.hpp). Only the window plays it (src/app/audio.cpp; off
    // in tests and replays); Game::sounds() records every command.
    virtual void sound(int n) { (void)n; }
    // BUCK.CFG's sound line the effects now play as (DS:487C's values): 3
    // the Tandy's (D-0093), 2 the AdLib line's otherwise (the speaker's
    // effects, or their AdLib versions; and tests and replays). ^S keeps
    // it in DS:487D, and a second ^S turns the effects back on only for 3
    // or 4, as the original (Game::ctrl_key).
    virtual int sound_line() { return 2; }
    // The passcode the prompt waiting now checks for (D-0084), typed when
    // the player presses the code key; "" when none.
    virtual void offer_code(const std::string& code) { (void)code; }
    // The prompt reading now (D-0085's text entry, session 66): letters
    // (InputString), digits (InputNumber, INPUT_NUMBER) or none; a gamepad
    // types into it by the picker. Nothing reaches the game but keys.
    virtual void text_entry(int kind) { (void)kind; }
    // With kEntryField (session 68): the field as it stands before each
    // read, its text and its cursor (1-based): MODIFY's name.
    virtual void entry_field(const std::string& text, int cursor) { (void)text, (void)cursor; }
    // The Log Book's pages (D-0083): the keys read while on are the port's
    // own pages', not the original's, and stay out of the key log.
    virtual void quiet_keys(bool on) { (void)on; }
};

// text_entry's kinds.
constexpr int kEntryNone = 0;
constexpr int kEntryLetters = 1;
constexpr int kEntryDigits = 2;
constexpr int kEntryField = 3;   // a field edited in place at a cursor (MODIFY's name, session 68)

// The key log's mark for a poll that found a key waiting ("poll"): a
// replay answers that poll yes, and the key after the mark is the one read.
constexpr int kPollMark = 0x7F01;

// An idle step ("idle", session 26): the wait it is offered at gets no
// key, so a wait with a timeout runs out (the title's 120 s); the step is
// taken at the game's next input call of another kind, as tools/keyfeed.S
// takes it at the next INT 16h call from another site.
constexpr int kIdle = 0x7F02;

// The step pace's length (D-0064): the owner wants the monsters' moves
// slow enough to follow, as the machines of 1990 drew them.
constexpr int kStepPaceMs = 150;

// F11 and F12 ("f11", "f12", session 32): the window's own hotkeys (full
// screen, combat speed), for injected window tests only. INT 16h AH=00h
// and 01h never return them (tools/bios_keys.py), so no read gets them.
constexpr int kHostF11 = 0x7F03;
constexpr int kHostF12 = 0x7F04;

// The options screen's key ("options", session 33): Shift+F12 in the
// window, a key the BIOS never returns to the game (tools/bios_keys.py
// --push); the game's key reads open the screen (D-0065) and read on.
constexpr int kOptionsKey = 0x7F05;
constexpr int kAspectKey = 0x7F7F;   // Shift+F11 in the window, for its tests

// The passcode key ("code", session 64, D-0084): F1 in the window at a
// passcode prompt; it types the code the game offers (offer_code), as
// keys the log keeps; itself never read by the game, never logged.
constexpr int kCodeKey = 0x7F06;

// A gamepad button ("pad:NAME", session 62, D-0085), for injected window
// tests only: the window turns it into the button's event, which its
// mapping turns into a key. 0x7E00 + SDL's button number (0..25); the
// triggers (session 66), axes in SDL, follow as 26 and 27.
constexpr int kPadBase = 0x7E00;
constexpr int kPadButtons = 26;
constexpr int kPadLeftTrigger = kPadButtons, kPadRightTrigger = kPadButtons + 1;
constexpr int kPadKeys = kPadButtons + 2;
// Tests only: the left stick pushed one way and let back ("pad:stickup",
// stickdown, stickleft, stickright), 28..31; to a corner (session 67:
// stickupleft, stickupright, stickdownleft, stickdownright, both axes in
// one report), 32..35.
constexpr int kPadStick = kPadKeys;
constexpr int kPadTokens = kPadKeys + 8;
inline bool is_pad(int k) { return k >= kPadBase && k < kPadBase + kPadTokens; }
// The buttons by SDL's names (SDL_GetGamepadStringForButton order), then
// "lefttrigger" and "righttrigger", then the stick's eight tokens.
int pad_button(const std::string& name);   // -1 for none

// A mouse click as a key value (session 23): the buttons (1 left, 2
// right) and the pointer in game pixels (0..319, 0..199), as INT 33h
// fn 3 reports them to Input.GetKey; "click:X,Y" / "rclick:X,Y" in key
// lists and key logs (docs/notes/replay.md "The mouse").
constexpr int kClickBase = 0x01000000;
constexpr int click_key(int buttons, int x, int y) { return kClickBase | buttons << 20 | x << 8 | y; }
constexpr bool is_click(int k) { return k >= kClickBase; }
constexpr int click_buttons(int k) { return (k >> 20) & 3; }
constexpr int click_x(int k) { return (k >> 8) & 0x1FF; }
constexpr int click_y(int k) { return k & 0xFF; }

// A burst (session 24): n copies of a key arriving at once in the BIOS
// buffer (15 keys), as a held key's repeats pile up: "burst:N:KEY" in key
// lists (tools/keyfeed.S offers it as one step, with one checkpoint).
constexpr int kBurstBase = 0x00800000;
constexpr int kBiosBuffer = 15;   // keys the BIOS buffer holds (0040:001E, 16 words)
constexpr int burst_key(int n, int key) { return kBurstBase | n << 16 | key; }
constexpr bool is_burst(int k) { return (k & 0x7F800000) == kBurstBase; }
constexpr int burst_count(int k) { return (k >> 16) & 0x7F; }
constexpr int burst_of(int k) { return k & 0xFFFF; }

// Thrown when a headless run has no keys left: the run ends there.
struct Parked {
    std::string where;
};


// Key names, as `--keys` and the key log use them: a printable character
// as typed, or enter, esc, space, bs, up, down, left, right, home, end,
// pgup, pgdn, ins, center, hash (the '#' key); click:X,Y, rclick:X,Y;
// burst:N:KEY; NAME*N repeats a key; '#' starts a comment (to the end of
// the line).
std::vector<std::string> key_tokens(const std::string& text);
std::vector<int> parse_keys(const std::string& text);
std::string key_name(int key);
// Alt + letter c as an extended key (0x100 + the letter's scan code), -1
// for anything else.
int alt_letter_key(char c);

// The combat speed option (D-0054): "original", "fast" or a rate 0.25..10
// (times faster; session 31, the owner's choice: 1.5 = 1.5 times faster).
// Held as the factor on combat's waits, 1 / rate: fast is 0.25 (4 times).
constexpr double kFastCombat = 0.25;
double parse_combat_speed(const std::string& text);
std::string combat_speed_name(double f);
// The text option's names, as the command line and the settings file
// write them: "typewriter" / "instant".
TextSpeed parse_text_speed(const std::string& text);
std::string text_speed_name(TextSpeed t);


// The fixes of the original's faults (options, off by default: each
// changes what the game keeps, so a key log records them in its header
// and replays with them). --fixes takes a comma list of the names, "none"
// or "all".
struct Fixes {
    bool stash_load = false;      // "stash-load" (D-0013): .HID loaded with .STF
    bool stale_stash = false;     // "stale-stash" (D-0014): an empty stash deletes .HID
    bool roster_backup = false;   // "roster-backup" (D-0015): deleted roster files kept aside
    bool wipe_load = false;       // "wipe-load" (D-0081): a game saved after the party's end loads
    bool script_load = false;     // "script-load" (D-0081): a LOAD in a script's team menu restarts the run
    bool any() const { return stash_load || stale_stash || roster_backup || wipe_load || script_load; }
};
Fixes parse_fixes(const std::string& text);
std::string fixes_name(const Fixes& f);   // "none" or the comma list

// Wraps another key source and writes every key it returns to a log file,
// one per line with the prompt that read it, flushed at once (so a crash
// keeps the log).
class LoggingInput : public Input {
public:
    LoggingInput(Input& inner, const std::string& path, const std::string& header);
    ~LoggingInput() override;
    int read_key(const Framebuffer& screen, const char* what) override;
    void pause(const Framebuffer& screen, int ms) override { inner_.pause(screen, ms); }
    bool wait(const Framebuffer& screen, int ms, const std::function<void()>& second = nullptr) override { return inner_.wait(screen, ms, second); }
    long clock_cs() override { return inner_.clock_cs(); }
    int read_key_for(const Framebuffer& screen, const char* what, int ms) override;   // a key is logged
    bool key_waiting() override;          // a yes is logged as the mark "poll"
    void flush() override { inner_.flush(); }
    void pointer(const std::array<std::uint16_t, 32>& masks, bool on) override { inner_.pointer(masks, on); }
    void timed_out() override;            // logged as "idle"
    double speed_factor() override { return inner_.speed_factor(); }
    TextSpeed text_speed() override { return inner_.text_speed(); }
    int step_pace_ms() override { return inner_.step_pace_ms(); }
    std::string option(const std::string& key) override { return inner_.option(key); }
    void set_option(const std::string& key, const std::string& value) override { inner_.set_option(key, value); }
    void options_open(bool on) override {   // the screen's keys aren't logged
        open_ = on;
        inner_.options_open(on);
    }
    bool options_menu() override { return inner_.options_menu(); }
    void options_key(int k) override {
        options_key_ = k;
        inner_.options_key(k);
    }
    void typing(bool on) override { inner_.typing(on); }
    void music_start(const Bytes& song) override { inner_.music_start(song); }
    void music_stop() override { inner_.music_stop(); }
    void offer_code(const std::string& code) override { inner_.offer_code(code); }
    void text_entry(int kind) override { inner_.text_entry(kind); }
    void entry_field(const std::string& text, int cursor) override { inner_.entry_field(text, cursor); }
    void quiet_keys(bool on) override { quiet_ = on; }
    void sound(int n) override { inner_.sound(n); }
    int sound_line() override { return inner_.sound_line(); }
    void note(const std::string& line);   // a "# ..." line
    const std::string& path() const { return path_; }

private:
    Input& inner_;
    std::string path_;
    std::FILE* f_ = nullptr;
    bool open_ = false;   // the options screen is up
    bool quiet_ = false;   // a Log Book page is up
    int options_key_ = 0;
    bool unlogged(int k) const {
        return open_ || quiet_ || k == kOptionsKey || (options_key_ && k > 0 && k < 0x100 && std::toupper(k) == options_key_);
    }
};


}  // namespace cotd::game
