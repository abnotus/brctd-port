// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/window.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <vector>

#include "app/audio.hpp"
#include "app/synth.hpp"
#include "app/settings.hpp"
#include "core/common.hpp"
#include "game/game.hpp"

namespace cotd {

// Each gamepad button's key (D-0085): the settings file's pad_<button>,
// else the defaults; -1 for none (the guide button opens the options).
// The triggers (session 66) give none unless the settings name one.
std::array<int, game::kPadKeys> pad_keymap(const Settings* settings) {
    std::array<int, game::kPadKeys> keys{};
    keys.fill(-1);
    static const std::pair<const char*, const char*> kDefaults[] = {
        {"a", "enter"}, {"b", "esc"}, {"x", "space"}, {"y", "y"}, {"back", "esc"}, {"start", "enter"},
        {"leftshoulder", "pgup"}, {"rightshoulder", "pgdn"}, {"leftstick", "home"}, {"rightstick", "end"},
        {"dpup", "up"}, {"dpdown", "down"}, {"dpleft", "left"}, {"dpright", "right"},
        {"lefttrigger", "none"}, {"righttrigger", "none"}};
    for (const auto& [button, key] : kDefaults) {
        std::string name = key;
        if (settings)
            if (auto v = settings->get(std::string("pad_") + button)) name = *v;
        try {
            const auto k = game::parse_keys(name);
            if (k.size() == 1 && !game::is_click(k[0]) && !game::is_burst(k[0]) && !game::is_pad(k[0]))
                keys[static_cast<std::size_t>(game::pad_button(button))] = k[0];
        } catch (const Error&) {   // a setting that names no key: the button gives none
        }
    }
    return keys;
}

// The left stick's key (session 67, D-0085) for its position (SDL's axis
// values, y down) and the key it gives now (held, -1 in the middle): an
// arrow, or for a diagonal the keypad's Home, PgUp, End or PgDn (combat
// moves, travel). Past half its travel it gives a key; under 3/8 it is in
// the middle again. A diagonal when the smaller axis is over 0.6 of the
// larger, held while over 0.4, so a push near an arrow's line stays that
// arrow. -1 for no new key.
int stick_key(int& held, int x, int y) {
    const int ax = std::abs(x), ay = std::abs(y), hi = std::max(ax, ay), lo = std::min(ax, ay);
    if (held < 0 ? hi <= 16384 : hi < 12288) {
        held = -1;
        return -1;
    }
    const bool was_diagonal = held == game::kHome || held == game::kPgUp || held == game::kEnd || held == game::kPgDn;
    int k;
    if (was_diagonal ? lo * 5 > hi * 2 : lo * 5 > hi * 3)
        k = y < 0 ? (x < 0 ? game::kHome : game::kPgUp) : (x < 0 ? game::kEnd : game::kPgDn);
    else if (ax >= ay)
        k = x > 0 ? game::kRight : game::kLeft;
    else
        k = y > 0 ? game::kDown : game::kUp;
    if (k == held) return -1;
    held = k;
    return k;
}

// True while the left stick's report that gave axis event e goes on: a
// pad's report moves both axes in events of one timestamp, and its
// direction is read after the last of them (session 67), so a corner
// pushed is one key, not an arrow and then the corner.
bool stick_report_goes_on(const SDL_Event& e) {
    std::array<SDL_Event, 8> next;
    const int n = SDL_PeepEvents(next.data(), static_cast<int>(next.size()), SDL_PEEKEVENT,
                                 SDL_EVENT_GAMEPAD_AXIS_MOTION, SDL_EVENT_GAMEPAD_AXIS_MOTION);
    for (int i = 0; i < n; ++i)
        if (next[i].gaxis.which == e.gaxis.which && next[i].gaxis.timestamp == e.gaxis.timestamp &&
            (next[i].gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || next[i].gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY))
            return true;
    return false;
}

struct WindowInput::Impl {
    // The music (D-0067): made at its first start; the driver's timer makes
    // the DOS clock run four times fast while installed (clock_cs).
    std::unique_ptr<Audio> audio;
    bool music_on = false;
    long long music_t0 = 0;        // the BIOS tick count when the driver came in
    long long clock_extra = 0;     // the ticks the clock has gained so far
    SDL_Window* win = nullptr;
    bool fullscreen = false;       // full screen as last asked (or reported by the window system)
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    WindowOptions opt;
    std::vector<std::uint32_t> rgb = std::vector<std::uint32_t>(Framebuffer::kWidth * Framebuffer::kHeight);
    bool options_pending = false;   // Shift+F12: the next read opens the options screen (D-0065)
    std::string code_offer;         // the passcode F1 types (D-0084), "" when none
    // Gamepads (D-0085): each button gives a key the original reads (the
    // settings file's pad_<button> = a key name, else the defaults in
    // pad_setup); the d-pad and the left stick give the arrows; the guide
    // button opens the options screen. Nothing else reaches the game, so
    // a key log holds only keys and replays in the original unchanged.
    std::array<int, game::kPadKeys> pad_keys{};
    int stick_x = 0, stick_y = 0;   // the left stick's direction (-1, 0, 1), for text entry
    int stick_ax = 0, stick_ay = 0, stick_held = -1;   // its axes and its key now (stick_key)
    std::array<bool, 2> trigger{};  // each trigger past half its travel
    // Text entry (session 66): at a text or number prompt (Input::
    // text_entry), with the option on, the d-pad's and the left stick's Up
    // and Down type a character and then change it (Backspace and the next
    // or previous one of the cycle: A..Z then 0..9, or the digits), Right
    // keeps it (the next Up or Down types a new one), Left is Backspace;
    // the Y button types the passcode on offer (D-0084). Only keys reach
    // the game, which draws them as typed ones, so the key log replays in
    // the original.
    int entry = game::kEntryNone;
    int picked = -1;   // the cycle's index of the character typed last; -1 none
    // A field edited in place (kEntryField, session 68: MODIFY's name,
    // 0x47B53): the game reports its text and cursor before each read.
    // Up and Down change the character under the cursor to the next or
    // previous of a..z, 0, space, sent in lower case (upper-case A and S
    // move the cursor there, a K ends MODIFY; the font draws both cases
    // alike) and then put the cursor back on it: Backspace when the name
    // grew by a space (the cursor was on its last character), else A once
    // fewer than the name is long (A moves on, wrapping to the first).
    // Right is A, or on the last character (not a space, the name shorter
    // than 15) that character typed again, which grows the name by a space
    // under the cursor; Left is Backspace (the field's DelChar, as its
    // Left and Del). A press while keys are still queued waits for them
    // (and the field's next report): its keys come from that state.
    std::string field_text;
    int field_cur = 1;
    std::deque<int> field_due;
    void field_take() {
        if (buffer.empty() && !field_due.empty()) {
            const int dir = field_due.front();
            field_due.pop_front();
            field_press(dir);
        }
    }
    void field_press(int dir) {
        static const std::string kCycle = "abcdefghijklmnopqrstuvwxyz0 ";
        const int len = static_cast<int>(field_text.size()), n = static_cast<int>(kCycle.size());
        if (field_cur < 1 || field_cur > len) return;
        const unsigned char ch = static_cast<unsigned char>(field_text[static_cast<std::size_t>(field_cur - 1)]);
        if (dir == 3) {
            type(game::kBackspace);
        } else if (dir == 4) {
            const int again = std::tolower(ch);   // typed again: what the field types, not a command
            const bool types = again == '0' || (again >= 0x20 && again <= 0x7A && !(again >= '1' && again <= '9') &&
                                                again != '\\');
            type(field_cur == len && len < 15 && ch != ' ' && types ? again : 'A');
        } else {
            const auto at = kCycle.find(static_cast<char>(std::tolower(ch)));
            const int i = at == std::string::npos ? (dir == 1 ? 0 : 25)
                                                  : (static_cast<int>(at) + (dir == 1 ? 1 : n - 1)) % n;
            type(static_cast<unsigned char>(kCycle[static_cast<std::size_t>(i)]));
            if (field_cur == 15) return;               // the cursor stays at the field's end
            if (field_cur == len) {
                type(game::kBackspace);                // the space it grew by
            } else {
                for (int k = 1; k < len; ++k) type('A');
            }
        }
    }
    void pad_setup() { pad_keys = pad_keymap(opt.settings); }
    // With injected keys (tests, replays) only the virtual pad counts: a
    // real pad on the machine would type into the run (session 65).
    bool foreign_pad([[maybe_unused]] const SDL_Event& e) const {
        return false;
    }
    void type(int k) {
        if (buffer.size() < static_cast<std::size_t>(game::kBiosBuffer)) buffer.push_back(k);
    }
    // Text entry's part of a gamepad event: true when it took the event.
    bool pad_entry(const SDL_Event& e) {
        if (entry == game::kEntryNone || foreign_pad(e)) return false;
        int dir = 0;   // 1 Up, 2 Down, 3 Left, 4 Right
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: dir = 1; break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: dir = 2; break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dir = 3; break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dir = 4; break;
                case SDL_GAMEPAD_BUTTON_NORTH:
                    if (code_offer.empty()) break;
                    for (const char c : code_offer) type(static_cast<unsigned char>(c));
                    picked = -1;
                    return true;
                default: break;
            }
            if (!dir) {
                picked = -1;   // another button's key: the character stays
                return false;
            }
        } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            const int b = e.gbutton.button;
            return b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
                   b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ||
                   (b == SDL_GAMEPAD_BUTTON_NORTH && !code_offer.empty());
        } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                   (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
            const int d = e.gaxis.value > 16384 ? 1 : e.gaxis.value < -16384 ? -1 : 0;
            int& was = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? stick_x : stick_y;
            if (d == was) return true;
            was = d;
            if (!d) return true;
            dir = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? (d > 0 ? 4 : 3) : (d > 0 ? 2 : 1);
        } else {
            return false;
        }
        if (typing && opt.drop_typing_keys) return true;
        if (entry == game::kEntryField) {
            field_due.push_back(dir);
            field_take();
            return true;
        }
        static const std::string kLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", kDigits = "0123456789";
        const std::string& cycle = entry == game::kEntryDigits ? kDigits : kLetters;
        const int n = static_cast<int>(cycle.size());
        if (dir == 3) {
            type(game::kBackspace);
            picked = -1;
        } else if (dir == 4) {
            picked = -1;
        } else {
            if (picked >= 0) type(game::kBackspace);
            // The first press: the cycle's first character, or for Down the
            // last letter (the last digit).
            picked = picked < 0 ? (dir == 1 ? 0 : entry == game::kEntryDigits ? n - 1 : 25)
                                : (picked + (dir == 1 ? 1 : n - 1)) % n;
            type(static_cast<unsigned char>(cycle[static_cast<std::size_t>(picked)]));
        }
        return true;
    }
    // The key a gamepad event gives (-1: none; kOptionsKey for the guide).
    int pad_key(const SDL_Event& e) {
        if (foreign_pad(e)) return -1;
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
            SDL_OpenGamepad(e.gdevice.which);
            return -1;
        }
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (SDL_Gamepad* g = SDL_GetGamepadFromID(e.gdevice.which)) SDL_CloseGamepad(g);
            return -1;
        }
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) return game::kOptionsKey;
            return e.gbutton.button < game::kPadButtons ? pad_keys[e.gbutton.button] : -1;
        }
        if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
            (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            // A trigger's key once it passes half its travel; again after
            // it is let back under a quarter.
            const int t = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 1;
            if (!trigger[t] && e.gaxis.value > 16384) {
                trigger[t] = true;
                return pad_keys[static_cast<std::size_t>(game::kPadLeftTrigger + t)];
            }
            if (trigger[t] && e.gaxis.value < 8192) trigger[t] = false;
            return -1;
        }
        if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
            (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
            // Both axes as one direction (stick_key), read after the last
            // event of the pad's report.
            (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? stick_ax : stick_ay) = e.gaxis.value;
            return stick_report_goes_on(e) ? -1 : stick_key(stick_held, stick_ax, stick_ay);
        }
        return -1;
    }
    // The mouse pointer (Input::pointer): drawn over the frame where the
    // host's pointer is, as the driver draws it in mode 13h: each pixel
    // under a screen-mask 0 bit cleared, under a cursor-mask 1 bit XORed
    // with 0x0F (DIVERGENCE.md D-0045).
    std::array<std::uint16_t, 32> masks{};
    bool pointer_on = false;
    std::vector<std::uint8_t> frame = std::vector<std::uint8_t>(Framebuffer::kWidth * Framebuffer::kHeight);
    std::array<std::uint32_t, 256> pal{};
    // The BIOS keyboard buffer (session 24): every key the host delivers,
    // its repeats included (the host's typematic), goes in as it arrives,
    // up to 15; a key that finds it full is dropped, as the BIOS drops
    // it. The game reads from it and FlushKeys empties it (Game::
    // flush_keys, at the original's flush points). Clicks are not keys:
    // they wait as presses (D-0045), read after any buffered key, as
    // GetKey reads a waiting key before it asks the mouse.
    std::deque<int> buffer;
    std::deque<int> clicks;
    // Text.Print's typewriter runs (Input::typing): with the option on,
    // the keys and clicks that arrive are dropped (D-0059).
    bool typing = false;
    // The window's rect while it is neither full screen nor maximized:
    // what the settings file keeps.
    int keep_x = 0, keep_y = 0, keep_w = 0, keep_h = 0;
    bool placed = false;
    std::string title;   // the caption without the combat speed's note
    // The frame's pointer moved (or left): a redraw is due, done once per
    // batch of events and at most every 16 ms (a full present per motion
    // event backs up on a slow presenter: the owner's WSL lag, session 31).
    bool pointer_dirty = false;
    Uint64 last_draw_ns = 0;
    // An event, or `ms` (< 0: no limit) without one; while the pointer's
    // redraw is due, at most until it can be done (take_events draws it).
    void wait_event(Sint32 ms) {
        if (pointer_dirty) {
            const Uint64 since = SDL_GetTicksNS() - last_draw_ns;
            const Sint32 left = since >= 16000000ull ? 0 : static_cast<Sint32>((16000000ull - since) / 1000000 + 1);
            if (ms < 0 || left < ms) ms = left;
        }
        if (ms < 0) SDL_WaitEvent(nullptr);
        else SDL_WaitEventTimeout(nullptr, ms);
    }
    // The caption: the title, the combat speed when it isn't the
    // original's (D-0054).
    void set_title() {
        std::string t = title;
        if (opt.speed != 1.0) t += " [combat speed " + game::combat_speed_name(opt.speed) + "]";
        SDL_SetWindowTitle(win, t.c_str());
    }

    // The sound's tests: with injected keys the window plays no sound
    // unless asked (inject_sound), and plays it into a file (sound_out).
    bool sound_held() const {
        return false;
    }
    bool sound_device() const {
        return true;
    }
    void layout() {
        // Integer scaling of the exact framebuffer, or 4:3 when corrected.
        const int h = opt.aspect ? 240 : Framebuffer::kHeight;
        SDL_SetRenderLogicalPresentation(ren, Framebuffer::kWidth, h,
                                         opt.aspect ? SDL_LOGICAL_PRESENTATION_LETTERBOX
                                                    : SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    }
    // A point of the window in game pixels (0..319, 0..199), or false
    // outside the picture.
    bool game_point(float wx, float wy, int& x, int& y) {
        float lx = 0, ly = 0;
        if (!SDL_RenderCoordinatesFromWindow(ren, wx, wy, &lx, &ly)) return false;
        if (opt.aspect) ly = ly * Framebuffer::kHeight / 240.0f;
        if (lx < 0 || ly < 0) return false;
        x = static_cast<int>(lx);
        y = static_cast<int>(ly);
        return x < Framebuffer::kWidth && y < Framebuffer::kHeight;
    }
    // The click a button press gives (left 1, right 2), or -1.
    int click_of(const SDL_Event& e) {
        if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || !pointer_on) return -1;
        const int b = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
        int x, y;
        if (!b || !game_point(e.button.x, e.button.y, x, y)) return -1;
        return game::click_key(b, x, y);
    }
    void compose() {
        for (std::size_t i = 0; i < rgb.size(); ++i) rgb[i] = pal[frame[i]];
        float wx, wy;
        int px, py;
        if (!pointer_on || !(SDL_GetMouseFocus() == win) ) return;
        SDL_GetMouseState(&wx, &wy);
        if (!game_point(wx, wy, px, py)) return;
        for (int r = 0; r < 16; ++r)
            for (int b = 0; b < 16; ++b) {
                const int x = px + b, y = py + r;
                if (x >= Framebuffer::kWidth || y >= Framebuffer::kHeight) continue;
                const std::size_t i = static_cast<std::size_t>(y) * Framebuffer::kWidth + static_cast<std::size_t>(x);
                std::uint8_t v = frame[i];
                if (!(masks[static_cast<std::size_t>(r)] & (0x8000 >> b))) v = 0;
                if (masks[static_cast<std::size_t>(16 + r)] & (0x8000 >> b)) v ^= 0x0F;
                rgb[i] = pal[v];
            }
    }
    // The window's rect for the settings file, while it is neither full
    // screen nor maximized (those keep the rect from before).
    void keep_rect() {
        const SDL_WindowFlags f = SDL_GetWindowFlags(win);
        if (f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) return;
        SDL_GetWindowPosition(win, &keep_x, &keep_y);
        SDL_GetWindowSize(win, &keep_w, &keep_h);
    }
    void toggle_fullscreen() {
        // SDL3 applies the change asynchronously: the window's flag flips
        // only when the window system has done it, so the state asked for
        // is kept here (the options screen shows it at once) and the
        // enter/leave events keep it true to the window.
        keep_rect();
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(win, fullscreen);
        draw();
    }
    // The close button: the quit dialog (D-0051), a native message box, not
    // a game screen. True: quit. Where no box can be shown, quit as before.
    bool ask_quit() {
        static const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Keep playing"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Quit"},
        };
        const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, win, "Countdown to Doomsday",
                                     "Quit without saving?\n\nAnything not saved in the game is lost.", 2, buttons,
                                     nullptr};
        int id = 1;
        if (!SDL_ShowMessageBox(&box, &id)) return true;
        return id == 1;
    }
    // Every pending SDL event handled: keys into the buffer, clicks
    // queued, the window's own events (F11, F12, redraws, quit). Defined after
    // game_key.
    void take_events();
    void draw() {
        pointer_dirty = false;
        draw_frame();
        last_draw_ns = SDL_GetTicksNS();
    }
    void draw_frame() {
        compose();
        SDL_UpdateTexture(tex, nullptr, rgb.data(), Framebuffer::kWidth * 4);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_FRect dst{0, 0, static_cast<float>(Framebuffer::kWidth), opt.aspect ? 240.0f : 200.0f};
        SDL_RenderTexture(ren, tex, nullptr, &dst);
        SDL_RenderPresent(ren);
    }
};

namespace {

// The display a window of size w x h opens on, and its top left there.
// A position kept in the settings file is used while its centre lies on a
// display (a monitor unplugged since: no); otherwise the window is
// centred on the display holding the pointer, else on the primary one, so
// it never opens across two monitors' seam.
struct Placement {
    SDL_DisplayID display = 0;
    int x = 0, y = 0;
};

SDL_DisplayID display_at(int x, int y) {
    // The display whose bounds hold the point, or 0 (SDL_GetDisplayForPoint
    // gives the nearest one instead).
    const SDL_Point p{x, y};
    int n = 0;
    SDL_DisplayID* ds = SDL_GetDisplays(&n);
    SDL_DisplayID found = 0;
    for (int i = 0; i < n && !found; ++i) {
        SDL_Rect b{};
        if (SDL_GetDisplayBounds(ds[i], &b) && SDL_PointInRect(&p, &b)) found = ds[i];
    }
    SDL_free(ds);
    return found;
}

Placement place(int w, int h, const Settings* settings) {
    Placement p;
    if (settings) {
        const auto x = settings->get_int("window_x"), y = settings->get_int("window_y");
        if (x && y) {
            p.display = display_at(*x + w / 2, *y + h / 2);
            if (p.display) {
                SDL_Rect u{};
                SDL_GetDisplayUsableBounds(p.display, &u);
                // Keep the title bar reachable on that display.
                p.x = std::clamp(*x, u.x, std::max(u.x, u.x + u.w - w));
                p.y = std::clamp(*y, u.y, std::max(u.y, u.y + u.h - h));
                return p;
            }
        }
    }
    float mx = 0, my = 0;
    SDL_GetGlobalMouseState(&mx, &my);
    p.display = display_at(static_cast<int>(mx), static_cast<int>(my));
    if (!p.display) p.display = SDL_GetPrimaryDisplay();
    SDL_Rect u{};
    if (!p.display || !SDL_GetDisplayUsableBounds(p.display, &u)) return Placement{};
    p.x = u.x + std::max(0, (u.w - w) / 2);
    p.y = u.y + std::max(0, (u.h - h) / 2);
    return p;
}

}  // namespace

WindowInput::WindowInput(const std::string& title, WindowOptions opt) : impl_(new Impl) {
    impl_->opt = opt;
    impl_->title = title;
    SDL_SetMainReady();
    // Closing the window asks first (the quit dialog, D-0051): SDL must
    // not turn the close request into a quit by itself.
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) throw Error(std::string("SDL_Init: ") + SDL_GetError());
    // Gamepads (D-0085): without the subsystem the window works as before.
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    const Settings* st = opt.settings;
    int w = Framebuffer::kWidth * 3, h = Framebuffer::kHeight * 3;
    if (st && st->get_int("window_w") && st->get_int("window_h")) {
        w = std::max(Framebuffer::kWidth, *st->get_int("window_w"));
        h = std::max(Framebuffer::kHeight, *st->get_int("window_h"));
    }
    const Placement at = place(w, h, st);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          at.display ? at.x : static_cast<int>(SDL_WINDOWPOS_CENTERED));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          at.display ? at.y : static_cast<int>(SDL_WINDOWPOS_CENTERED));
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    impl_->win = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!impl_->win) throw Error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    impl_->ren = SDL_CreateRenderer(impl_->win, nullptr);
    if (!impl_->ren) throw Error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    impl_->keep_x = at.x;
    impl_->keep_y = at.y;
    impl_->keep_w = w;
    impl_->keep_h = h;
    impl_->placed = at.display != 0;
    if (st && st->get_int("fullscreen").value_or(0)) {
        impl_->fullscreen = true;
        SDL_SetWindowFullscreen(impl_->win, true);
    }
    impl_->set_title();
    SDL_ShowWindow(impl_->win);
    impl_->tex = SDL_CreateTexture(impl_->ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
                                   Framebuffer::kWidth, Framebuffer::kHeight);
    SDL_SetTextureScaleMode(impl_->tex, SDL_SCALEMODE_NEAREST);
    impl_->layout();
    impl_->pad_setup();
    SDL_StartTextInput(impl_->win);
}

WindowInput::~WindowInput() {
    if (impl_->win) {
        impl_->keep_rect();
        if (Settings* st = impl_->opt.settings) {
            st->set("window_x", impl_->keep_x);
            st->set("window_y", impl_->keep_y);
            st->set("window_w", impl_->keep_w);
            st->set("window_h", impl_->keep_h);
            st->set("fullscreen", impl_->fullscreen ? 1 : 0);
        }
    }
    // The sound first: its stream must go before SDL_Quit (session 35: a
    // stream destroyed after it, with the Impl, ended the program before
    // the settings file was written).
    impl_->audio.reset();
    if (impl_->tex) SDL_DestroyTexture(impl_->tex);
    if (impl_->ren) SDL_DestroyRenderer(impl_->ren);
    if (impl_->win) SDL_DestroyWindow(impl_->win);
    SDL_Quit();
}


std::string WindowInput::describe() const {
    std::string s = std::string("driver ") + (SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?") + ";";
    int n = 0;
    SDL_DisplayID* ds = SDL_GetDisplays(&n);
    for (int i = 0; i < n; ++i) {
        SDL_Rect b{};
        SDL_GetDisplayBounds(ds[i], &b);
        s += " display " + std::to_string(ds[i]) + " " + std::to_string(b.x) + "," + std::to_string(b.y) + " " +
             std::to_string(b.w) + "x" + std::to_string(b.h) + " scale " +
             std::to_string(SDL_GetDisplayContentScale(ds[i])).substr(0, 4) + ";";
    }
    SDL_free(ds);
    int x = 0, y = 0, w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowPosition(impl_->win, &x, &y);
    SDL_GetWindowSize(impl_->win, &w, &h);
    SDL_GetWindowSizeInPixels(impl_->win, &pw, &ph);
    s += " window " + std::to_string(x) + "," + std::to_string(y) + " " + std::to_string(w) + "x" + std::to_string(h) +
         " (pixels " + std::to_string(pw) + "x" + std::to_string(ph) + ") on display " +
         std::to_string(SDL_GetDisplayForWindow(impl_->win));
    return s;
}

void WindowInput::present(const Framebuffer& fb) {
    // 6-bit DAC values to 8 bits.
    auto c8 = [](std::uint8_t v) -> std::uint32_t { return static_cast<std::uint32_t>((v << 2) | (v >> 4)); };
    const auto& dac = fb.dac();
    for (std::size_t i = 0; i < 256; ++i) impl_->pal[i] = c8(dac[i].r) << 16 | c8(dac[i].g) << 8 | c8(dac[i].b);
    std::copy(fb.pixels().begin(), fb.pixels().end(), impl_->frame.begin());
    impl_->draw();
}

void WindowInput::pointer(const std::array<std::uint16_t, 32>& masks, bool on) {
    // The driver's SetCursor and Show/Hide: drawn into the frame (D-0045);
    // with no driver the host's own pointer shows.
    impl_->masks = masks;
    impl_->pointer_on = on;
    if (on) SDL_HideCursor();
    else SDL_ShowCursor();
}

game::TextSpeed WindowInput::text_speed() { return impl_->opt.text; }

int WindowInput::step_pace_ms() { return impl_->opt.step_pace_ms; }

bool WindowInput::options_menu() { return impl_->opt.options_menu; }

std::string WindowInput::option(const std::string& key) {
    // The options screen's values (D-0065), as the settings file names them.
    const WindowOptions& o = impl_->opt;
    if (key == "text") return game::text_speed_name(o.text);
    if (key == "text_keys") return o.drop_typing_keys ? "wait" : "original";
    if (key == "combat_speed") return game::combat_speed_name(o.speed);
    if (key == "step_pace") return o.step_pace_ms ? "on" : "off";
    if (key == "aspect") return o.aspect ? "4:3" : "off";
    if (key == "fullscreen") return impl_->fullscreen ? "on" : "off";
    if (key == "sound") return o.sound ? "adlib" : "off";
    if (key == "volume") return std::to_string(o.volume);
    if (key == "effects")
        return o.effects == WindowOptions::Effects::Adlib     ? "adlib"
               : o.effects == WindowOptions::Effects::Tandy   ? "tandy"
               : o.effects == WindowOptions::Effects::Speaker ? "speaker"
                                                              : "off";
    // The gamepad (D-0085, session 66): its text entry and each button's key.
    if (key == "pad_text") return o.pad_text ? "on" : "off";
    if (key.rfind("pad_", 0) == 0)
        if (const int b = game::pad_button(key.substr(4)); b >= 0) {
            const int k = impl_->pad_keys[static_cast<std::size_t>(b)];
            return k < 0 ? "none" : game::key_name(k);
        }
    return {};
}

namespace {
Audio::Fx fx_device(WindowOptions::Effects e) {
    return e == WindowOptions::Effects::Adlib   ? Audio::Fx::Adlib
           : e == WindowOptions::Effects::Tandy ? Audio::Fx::Tandy
                                                : Audio::Fx::Speaker;
}
}  // namespace

WindowOptions::Effects parse_effects(const std::string& value) {
    if (value == "tandy" || value == "on") return WindowOptions::Effects::Tandy;
    if (value == "adlib") return WindowOptions::Effects::Adlib;
    if (value == "speaker") return WindowOptions::Effects::Speaker;
    if (value == "off") return WindowOptions::Effects::Off;
    throw Error("effects (tandy, adlib, speaker or off): '" + value + "'");
}

void WindowInput::set_option(const std::string& key, const std::string& value) {
    // A change from the options screen: applied at once, and kept in the
    // settings file when the screen closes (D-0049, D-0065).
    WindowOptions& o = impl_->opt;
    if (key == "text") o.text = game::parse_text_speed(value);
    else if (key == "text_keys") o.drop_typing_keys = value == "wait";
    else if (key == "step_pace") o.step_pace_ms = value == "on" ? game::kStepPaceMs : 0;
    else if (key == "sound") o.sound = value == "adlib";
    else if (key == "effects") {
        o.effects = parse_effects(value);
        if (impl_->audio) {
            impl_->audio->effects_device(fx_device(o.effects));
            if (o.effects == WindowOptions::Effects::Off) impl_->audio->sound(0xFF);   // what plays stops
        }
    }
    else if (key == "volume") {
        o.volume = std::clamp(std::stoi(value), 0, 10);
        if (impl_->audio) impl_->audio->set_volume(o.volume);
    }
    else if (key == "combat_speed") {
        o.speed = game::parse_combat_speed(value);
        impl_->set_title();
    } else if (key == "aspect") {
        o.aspect = value == "4:3";
        impl_->layout();
        impl_->draw();
    } else if (key == "fullscreen") {
        if ((value == "on") != impl_->fullscreen) impl_->toggle_fullscreen();
        if (o.settings) o.settings->set("fullscreen", value == "on" ? 1 : 0);
        return;
    } else if (key == "pad_text") {
        o.pad_text = value == "on";
    } else if (key.rfind("pad_", 0) == 0 && game::pad_button(key.substr(4)) >= 0) {
        int k = -1;
        try {
            const auto ks = game::parse_keys(value);
            if (ks.size() == 1 && !game::is_click(ks[0]) && !game::is_burst(ks[0]) && !game::is_pad(ks[0])) k = ks[0];
        } catch (const Error&) {   // "none"
        }
        impl_->pad_keys[static_cast<std::size_t>(game::pad_button(key.substr(4)))] = k;
    } else {
        return;
    }
    if (o.settings) o.settings->set(key, option(key));
}

void WindowInput::options_open(bool on) {
    // The settings file is written when the options screen closes, not
    // only on the way out (the owner's, session 35).
    if (!on && impl_->opt.settings) impl_->opt.settings->save();
}

void WindowInput::typing(bool on) {
    // Text.Print's typewriter starts or ends (D-0059). At its end, what
    // arrived during the last character is still dropped.
    if (on) {
        impl_->typing = true;
        return;
    }
    if (impl_->typing && impl_->opt.drop_typing_keys) impl_->take_events();
    impl_->typing = false;
}


double WindowInput::speed_factor() { return impl_->opt.speed; }

void WindowInput::pause(const Framebuffer& fb, int ms) {
    // Crt.Delay: show the screen and wait; keys typed meanwhile go into
    // the buffer (the original reads none during the delay).
    present(fb);
    const Uint64 end = SDL_GetTicks() + static_cast<Uint64>(ms);
    for (;;) {
        impl_->take_events();
        const Uint64 now = SDL_GetTicks();
        if (now >= end) break;
        impl_->wait_event(static_cast<Sint32>(std::min<Uint64>(end - now, 10)));
    }
}

bool WindowInput::wait(const Framebuffer& fb, int ms, const std::function<void()>& second) {
    // The intro's WaitSeconds (ovl1 +0x0000): show the screen; a key in the
    // buffer (or a click: KeyPressed counts a held button) ends the wait.
    // The key stays; WaitSeconds' FlushKeys takes it (Game::wait_seconds).
    // `second` at each whole second that passes but the last.
    present(fb);
    const Uint64 start = SDL_GetTicks(), end = start + static_cast<Uint64>(ms);
    Uint64 next = start + 1000;
    for (;;) {
        impl_->take_events();
        if (!impl_->buffer.empty()) return true;
        if (!impl_->clicks.empty()) {
            impl_->clicks.pop_front();
            return true;
        }
        const Uint64 now = SDL_GetTicks();
        if (now >= end) return false;
        if (now >= next) {
            if (second) second();
            next += 1000;
        }
        impl_->wait_event(static_cast<Sint32>(std::min<Uint64>({end - now, next - now, 50})));
    }
}

namespace {

// The game key an event gives (read_key's mapping), or -1.
int game_key(const SDL_Event& e) {
    if (e.type == SDL_EVENT_TEXT_INPUT) {
        // Printable characters as typed (shift included).
        const unsigned char c = static_cast<unsigned char>(e.text.text[0]);
        if (c >= 0x20 && c < 0x7F && e.text.text[1] == 0) return c;
        return -1;
    }
    if (e.type != SDL_EVENT_KEY_DOWN) return -1;
    // The keypad gives digits: Game.Init forces NumLock on (0x3F5C1;
    // DIVERGENCE.md D-0019). With the host's NumLock on, SDL also sends
    // the digit as text input, which delivers it; otherwise it's here.
    if (e.key.key >= SDLK_KP_1 && e.key.key <= SDLK_KP_0) {
        if (e.key.mod & SDL_KMOD_NUM) return -1;
        return e.key.key == SDLK_KP_0 ? '0' : '1' + static_cast<int>(e.key.key - SDLK_KP_1);
    }
    // Ctrl+S and Ctrl+U: the BIOS's ASCII 13h and 15h, GetKey's sound keys
    // (docs/notes/sound.md); Ctrl+D, 04h, its debug flag (D-0091); no other
    // Ctrl letter gives a key (D-0068).
    if ((e.key.mod & SDL_KMOD_CTRL) && !(e.key.mod & SDL_KMOD_ALT)) {
        if (e.key.key == SDLK_S) return 0x13;
        if (e.key.key == SDLK_U) return 0x15;
        if (e.key.key == SDLK_D) return 0x04;
    }
    // Alt + a key: the BIOS's ASCII 0 + its code, as INT 16h AH=00h gives
    // them: the letters (combat's Alt-Q), the digits (7800h..8100h), - and =
    // (8200h, 8300h). Session 78, measured (tools/bios_keys.py --kbc in the
    // oracle's DOSBox-X): Alt+1 7800h, Alt+- 8200h; Alt+], Alt+; and Alt+/
    // give the program nothing, so the window gives no Alt+punctuation.
    if (e.key.mod & SDL_KMOD_ALT) {
        char c = 0;
        if (e.key.key >= SDLK_A && e.key.key <= SDLK_Z) c = static_cast<char>('a' + (e.key.key - SDLK_A));
        else if (e.key.key >= SDLK_0 && e.key.key <= SDLK_9) c = static_cast<char>('0' + (e.key.key - SDLK_0));
        else if (e.key.key == SDLK_MINUS) c = '-';
        else if (e.key.key == SDLK_EQUALS) c = '=';
        if (c) {
            const int k = game::alt_letter_key(c);
            if (k >= 0) return k;
        }
    }
    // F1..F10 as INT 16h AH=00h reports them (ASCII 0, scan 0x3B..0x44;
    // with Shift 0x54.., Ctrl 0x5E.., Alt 0x68..; the BIOS tests Alt,
    // then Ctrl, then Shift). Callers that ignore MenuScreen's ext flag
    // read the scan as a letter: the shop's F7 Ammo, F8 Buy (session 32).
    // F11 and F12 never reach the game (tools/bios_keys.py).
    if (e.key.key >= SDLK_F1 && e.key.key <= SDLK_F10) {
        const int i = static_cast<int>(e.key.key - SDLK_F1);
        const int base = (e.key.mod & SDL_KMOD_ALT) ? 0x68 : (e.key.mod & SDL_KMOD_CTRL) ? 0x5E
                       : (e.key.mod & SDL_KMOD_SHIFT) ? 0x54 : 0x3B;
        return 0x100 + base + i;
    }
    switch (e.key.key) {
        case SDLK_RETURN: case SDLK_KP_ENTER: return game::kEnter;
        case SDLK_ESCAPE: return game::kEsc;
        case SDLK_BACKSPACE: return game::kBackspace;
        // The cursor keys as the BIOS reports them (scan codes, extended).
        case SDLK_UP: return game::kUp;
        case SDLK_DOWN: return game::kDown;
        case SDLK_LEFT: return game::kLeft;
        case SDLK_RIGHT: return game::kRight;
        case SDLK_HOME: return game::kHome;
        case SDLK_END: return game::kEnd;
        case SDLK_PAGEUP: return game::kPgUp;
        case SDLK_PAGEDOWN: return game::kPgDn;
        case SDLK_INSERT: return game::kIns;
        case SDLK_DELETE: return game::kDel;
        default: return -1;   // printable keys arrive as text input
    }
}

}  // namespace

void WindowInput::Impl::take_events() {
    if (audio) audio->report_faults();   // the audio thread's rejections, said here (D-0082)
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) throw WindowClosed{};
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (ask_quit()) throw WindowClosed{};
            continue;
        }
        if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) fullscreen = true;
        if (e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) fullscreen = false;
        if (e.type == SDL_EVENT_WINDOW_MOVED || e.type == SDL_EVENT_WINDOW_RESIZED) keep_rect();
        if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_EXPOSED) {
            draw();
            continue;
        }
        if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
            // The frame's pointer follows the host's.
            pointer_dirty = true;
            continue;
        }
        // While text types out, with the option on (D-0059): what the
        // player presses or clicks is dropped, never read.
        const bool drop = typing && opt.drop_typing_keys;
        if (pad_entry(e)) continue;                 // D-0085's text entry
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT) picked = -1;
        if (const int pk = pad_key(e); pk >= 0) {   // D-0085
            if (pk == game::kOptionsKey) options_pending = true;
            else if (!drop && buffer.size() < static_cast<std::size_t>(game::kBiosBuffer)) buffer.push_back(pk);
            continue;
        }
        if (const int c = click_of(e); c >= 0) {
            if (!drop) clicks.push_back(c);
            continue;
        }
        // The options screen (D-0065) on Shift+F12, which the BIOS never
        // returns to the game (tools/bios_keys.py --push 8800): asked of
        // the game at its next key read.
        // The passcode key (D-0084): F1 at a prompt that offers a code types
        // it into the buffer (the log keeps those keys); F1 itself never
        // reaches the game there (InputString would ignore it).
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F1 && !(e.key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT)) &&
            !code_offer.empty()) {
            if (!e.key.repeat)
                for (const char c : code_offer)
                    if (buffer.size() < static_cast<std::size_t>(game::kBiosBuffer)) buffer.push_back(static_cast<unsigned char>(c));
            continue;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F12 && (e.key.mod & SDL_KMOD_SHIFT)) {
            if (!e.key.repeat) options_pending = true;
            continue;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F12) {
            // The combat speed option (D-0054) on F12, which the BIOS never
            // returns to the game (session 32: F8 is the shop's Buy, the
            // original's, as 'B'): the original's, then faster
            // in steps (rates 1.25, 1.5, 2, fast = 4), again (the owner,
            // session 31). A speed not in the cycle (from --combat-speed)
            // goes to 1.25. Held as the factors on the waits (1 / rate).
            if (!e.key.repeat) {
                static const double kCycle[] = {1.0, 1.0 / 1.25, 1.0 / 1.5, 1.0 / 2, game::kFastCombat};
                constexpr std::size_t n = sizeof kCycle / sizeof kCycle[0];
                std::size_t i = 0;
                while (i < n && kCycle[i] != opt.speed) ++i;
                opt.speed = kCycle[i < n ? (i + 1) % n : 1];
                if (opt.settings) {
                    opt.settings->set("combat_speed", game::combat_speed_name(opt.speed));
                    opt.settings->save();
                }
                set_title();
            }
            continue;
        }
        // Aspect correction (D-0012) on Shift+F11 (F9 until session 32:
        // the game reads F9 as 'C', e.g. a ship fight's Close and the save
        // menus' slot C; the BIOS never returns F11).
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11 && (e.key.mod & SDL_KMOD_SHIFT)) {
            if (!e.key.repeat) {
                opt.aspect = !opt.aspect;
                if (opt.settings) {
                    opt.settings->set("aspect", opt.aspect ? "4:3" : "off");
                    opt.settings->save();
                }
                layout();
                draw();
            }
            continue;
        }
        // Full screen on and off (D-0052): F11, or Alt+Enter (which the
        // game never sees; it has no meaning there).
        if (e.type == SDL_EVENT_KEY_DOWN &&
            (e.key.key == SDLK_F11 ||
             ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT)))) {
            if (!e.key.repeat) toggle_fullscreen();
            continue;
        }
        const int k = game_key(e);
        if (k >= 0 && !drop && buffer.size() < static_cast<std::size_t>(game::kBiosBuffer)) buffer.push_back(k);
    }
    if (pointer_dirty && SDL_GetTicksNS() - last_draw_ns >= 16000000ull) draw();
}

int WindowInput::read_key(const Framebuffer& fb, const char* what) { return read_key_for(fb, what, -1); }

namespace {

long long host_ticks() {
    // DOS keeps the time of day as BIOS timer ticks (1193182 / 65536 per
    // second) and derives the centiseconds from them, so the value steps
    // once per tick (about 55 ms): the host's time since midnight is
    // turned into whole ticks (D-0038).
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const std::tm* lt = std::localtime(&t);
    const long long ms = ((lt->tm_hour * 60LL + lt->tm_min) * 60LL + lt->tm_sec) * 1000LL +
                         std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    return ms * 1193182LL / (65536LL * 1000LL);
}

}  // namespace

long WindowInput::clock_cs() {
    // Game.GetTime (0653:0C81): centiseconds since midnight, from the
    // host's clock, stepped in DOS's timer ticks (D-0038). Tests
    // that inject keys pass no time, as headless runs.
    long long ticks = host_ticks();
    // While the music driver is installed its timer (PIT channel 0 at
    // divisor 0x4000, drv:02E6) drives the BIOS's INT 8 four times as
    // often, and the BIOS counts every one as a tick: DOS's time runs four
    // times fast, and stays ahead after the driver goes (drv:0309 doesn't
    // correct it; docs/notes/sound.md). The title's 120-second timeout
    // runs out in 30 s with the music on, as in the original (D-0067).
    ticks += impl_->clock_extra + (impl_->music_on ? 3 * (ticks - impl_->music_t0) : 0);
    return static_cast<long>(ticks * 65536LL * 100LL / 1193182LL);
}

void WindowInput::music_start(const Bytes& song) {
    // Music.Play (07DA:0029) with BUCK.CFG's "A"; Silent ("S") installs no
    // driver at all (07DA:002C tests DS:487C = 5).
    if (!impl_->opt.sound || impl_->sound_held()) return;
    if (!impl_->audio) impl_->audio = std::make_unique<Audio>(impl_->sound_device());
    impl_->audio->set_volume(impl_->opt.volume);
    impl_->audio->start(song);
    if (!impl_->music_on) {
        impl_->music_on = true;
        impl_->music_t0 = host_ticks();
    }
}

void WindowInput::offer_code(const std::string& code) { impl_->code_offer = code; }

void WindowInput::text_entry(int kind) {
    const int was = impl_->entry;
    impl_->entry = impl_->opt.pad_text ? kind : game::kEntryNone;
    impl_->picked = -1;
    if (impl_->entry != was) impl_->field_due.clear();
}

void WindowInput::entry_field(const std::string& text, int cursor) {
    impl_->field_text = text;
    impl_->field_cur = cursor;
}

void WindowInput::sound(int n) {
    // The effect unit's command (Game::sound); BUCK.CFG's "S" (the Music
    // option's Silent) has none, nor the Effects option's Off.
    if (!impl_->opt.sound || impl_->opt.effects == WindowOptions::Effects::Off ||
        impl_->sound_held() || impl_->opt.speaker_image.empty())
        return;
    if (!impl_->audio) impl_->audio = std::make_unique<Audio>(impl_->sound_device());
    impl_->audio->set_volume(impl_->opt.volume);
    impl_->audio->effects(impl_->opt.speaker_image);
    impl_->audio->effects_device(fx_device(impl_->opt.effects));
    impl_->audio->sound(n);
}

int WindowInput::sound_line() {
    // The effects heard on the Tandy (D-0093): BUCK.CFG's "T" (3); else the
    // AdLib line's 2. Tests and replays (no effects played) keep 2.
    const WindowOptions& o = impl_->opt;
    const bool heard = o.sound && o.effects == WindowOptions::Effects::Tandy && !impl_->sound_held() &&
                       !o.speaker_image.empty();
    return heard ? 3 : 2;
}

void WindowInput::music_stop() {
    // Music.Stop: the driver's fn 3; the clock keeps what it gained.
    if (!impl_->music_on) return;
    if (impl_->audio) impl_->audio->stop();
    impl_->clock_extra += 3 * (host_ticks() - impl_->music_t0);
    impl_->music_on = false;
}

int WindowInput::read_key_for(const Framebuffer& fb, const char*, int ms) {
    // Show the screen and wait for a key: the buffer's first, else the
    // first click, else the next to arrive; with ms >= 0, -1 once that
    // time has passed without one. Tests that inject keys don't wait.
    present(fb);
    const Uint64 end = SDL_GetTicks() + static_cast<Uint64>(ms < 0 ? 0 : ms);
    for (;;) {
        impl_->take_events();
        impl_->field_take();   // a press that waited (session 68)
        if (impl_->options_pending) {
            impl_->options_pending = false;
            return game::kOptionsKey;
        }
        if (!impl_->buffer.empty()) {
            const int k = impl_->buffer.front();
            impl_->buffer.pop_front();
            return k;
        }
        if (!impl_->clicks.empty()) {
            const int c = impl_->clicks.front();
            impl_->clicks.pop_front();
            return c;
        }
        if (ms < 0) {
            impl_->wait_event(-1);
        } else {
            const Uint64 now = SDL_GetTicks();
            if (now >= end) return -1;
            impl_->wait_event(static_cast<Sint32>(end - now));
        }
    }
}

bool WindowInput::key_waiting() {
    // KeyPressed (08AD:033E): a key in the buffer, or with the mouse a
    // held button (INT 33h fn 3): a press queued. Tests that inject keys
    // never have one waiting, as KEYFEED never answers that poll, unless
    // a burst left keys in the buffer.
    impl_->take_events();
    return !impl_->buffer.empty() || !impl_->clicks.empty();
}

void WindowInput::flush() {
    // FlushKeys (08AD:03E4): the buffer emptied (keys that arrived until
    // now included). Clicks stay (the mouse has no buffer; D-0045).
    impl_->take_events();
    impl_->buffer.clear();
}

bool show_player_message(const std::string& text, bool error) {
    const char* driver = std::getenv("SDL_VIDEODRIVER");
    if (driver && (std::string(driver) == "offscreen" || std::string(driver) == "dummy")) return false;
    return SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_WARNING, "Countdown to Doomsday",
                                    text.c_str(), nullptr);
}


}  // namespace cotd
