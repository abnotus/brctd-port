// SPDX-License-Identifier: GPL-3.0-or-later
// The program from launch: Game.Init, the intro, the title menu, and the
// team menu with its sub-flows (docs/notes/frontend.md). Character
// creation is game/chargen.cpp; the sheet, skills and icon screens are
// game/sheet.cpp, game/skills.cpp and game/icons.cpp.
#include <algorithm>
#include <cstring>
#include <filesystem>

#include "game/game.hpp"

namespace cotd::game {

namespace fs = std::filesystem;

// --- start-up -------------------------------------------------------------------------

void Game::game_init() {
    // Game.Init ovl9 +0x0101: the game-state part (0x3F94E..0x3FBE4); the
    // screen part is init_screen (constructor). The variable blocks are
    // zero-filled, then:
    std::fill(vars0_.begin(), vars0_.end(), 0);
    std::fill(vars1_.begin(), vars1_.end(), 0);
    std::fill(vars2_.begin(), vars2_.end(), 0);
    v0(0x4BB6) = 2;     // [3BC8]+0xB6 (0x3F969)
    v0(0x4BE6) = 1;     // +0xE6, the area type
    v0(0x4BF2) = 0;     // +0xF2
    x_ = 7;             // DS:6A2F..6A33 := 0, then 7, 13, 0 (0x3FA57..0x3FA72)
    y_ = 13;
    facing_ = 0;
    wall_ahead_ = 0;
    map_byte_ = 0;
    walls_table_.fill(0);   // DS:6A34, 16 bytes
    anim_name_.clear();     // DS:6AFC := '', DS:6B0E := 0xFF
    anim_id_ = 0xFF;
    ds3bee_ = 1;            // DS:3BEE, DS:3BEC := 1 (0x3FAB1)
    demo_ = 0;              // DS:3BEA := 0 (0x3FAF7; Game.Reset leaves it)
    typing_ = 1;            // DS:3BF8 := 1 (0x3FAC0)
    debug_ = 0;             // DS:3BF0 := 0 (0x3FABB)
    ds3bec_ = 1;
    loaded_game_ = 0;       // DS:3BF1
    party_.clear();         // DS:4EC5, DS:4EC1 := nil
    for (ForeignSlot& f : foreign_slot_) f = {};
    cur_ = nullptr;
    prev_cur_ = nullptr;    // DS:4EBD := nil (0x3FAE2 / 0x3FE67)
    pc_ = 0x8000;           // DS:3BE2
    v0(0x4BFC) = 4;         // [3BC8]+0xFC: Game.Delay's factor
    ecl_file_ = 1;          // DS:4898
    area_map_ = 0;          // DS:6A2E
    v1(0x7F3E) = 0;         // [3BCC]+0x33E
    list_top_ = 1;          // DS:6B1B := 1 (0x3FB1B)
    party_lost_ = 0;        // DS:3BF6
    saved_ = 0;             // DS:499B
    c6cfd_ = 0xE8;          // DS:6CFD, DS:6CFC (0x3FB67)
    c6cfc_ = 0xDB;
    first_area_ = 0;        // DS:8301
    highlight_ = 1;         // DS:6B1D
    mode_ = 4;              // DS:3BE8, DS:3BE9
    prev_mode_ = 0;
    f3bed_ = 0;
    move_mode_ = 0;         // DS:8303
    f82f6_ = f82f7_ = f82f9_ = 0;
    // DS:6B48 is not set by Game.Init: 0 from the BSS.
    menu_sum_ = 0;
    // The team menu's enable bytes: their initial data (DS:0A49 + 0x2A*i).
    for (int i = 0; i < 13; ++i) team_enable_[static_cast<std::size_t>(i)] = text_.ds8(0x0A49 + 0x2A * i);
    new_game_defaults();
}

void Game::new_game_defaults() {
    // NewGameDefaults ovl9 +0x0000 (0x3F400): the menu colours and flags,
    // then LoadShip (ovl17 +0x26F3, called at 0x3F468 with id 10: its
    // loading line, SHIPS.DAX entry 10 + 0x28) into var 0x4D00
    // ([3BC8]+0x200): 0x6C bytes, with the ship's name (CS:26E1,
    // string[15]) stored over its start (0x4FED3, id > 4).
    menu_prompt_fg_ = 13;
    menu_text_fg_ = 10;
    menu_hot_fg_ = 15;
    f82f1_ = 0;  // DS:82F1, DS:82F2 (0x3F415): a ship fight's pending medical and repair, gone with the
    f82f2_ = 0;  // game: found at sweep 484268 (a fight's stale repair after a ship fight's lost party)
    areas_.clear();                  // DS:6B4F, DS:6B51 (0x3F421): the list dropped, not freed
    tactics_won_.fill(0);            // DS:6BD3, DS:6BD4
    f6b74_ = 0;                      // DS:6B74
    f6b4b_ = 0;                      // DS:6B4B
    fill_ = 0;   // DS:5F44
    load_ship(&vars0_.at(0x200), 10);
    // The ship record at var 0x4B85 (DS:4A26 := [3BC8]+0x85).
    v0(0x4B85 + 0x36) = 0;
    v0(0x4B85 + 0x34) = 0;
    const std::size_t k = 0x249A + 3 * (static_cast<std::size_t>(text_.ds8(0x254A)) + text_.ds8(0x255C));
    v0(0x4B85 + 0x39) = text_.ds8(k);
    v0(0x4B85 + 0x3A) = text_.ds8(k + 1);
    v0(0x4B85 + 0x38) = 0x57;
}

Image Game::load_title(int id) {
    // Gfx.LoadImage("title", id, 0, 0, img) (0267:04D8): its palette goes
    // to DS:908B.
    auto raw = dax_entry("TITLE.DAX", id);
    if (!raw) throw Fatal("TITLE.DAX: no entry " + std::to_string(id));
    Image im = decode_generic(*raw, "TITLE.DAX #" + std::to_string(id));
    load_palette(im);
    flush_keys();                                      // 0x12BED
    return im;
}

bool Game::wait_seconds(int n) {
    // Intro.WaitSeconds ovl1 +0x0000: FlushKeys (0x300C6), n seconds of the
    // DOS clock or until KeyPressed, FlushKeys again (0x30157): the key is
    // never read. True if a key ended it.
    // Each second the wait sounds DS:21F0's effect (10, 0x3010E); the
    // n-th second's never: the wait ends first (the speaker trace,
    // docs/notes/sound.md).
    flush_keys();
    steps_unread_ = 0;
    const bool key = input_ && input_->wait(screen_, n * 1000, [this] { sound(text_.ds16(0x21F0)); });
    flush_keys();
    return key;
}

void Game::intro() {
    // Intro ovl1 +0x0640 (0x30700): TITLE #1, #2 + #3, the credits, the
    // logo. Screen.Clear (0653:0CFA) zeroes the whole screen.
    screen_.clear(0);
    {
        const Image t = load_title(1);
        set_dac(16, 16, pal_908b_);
        screen_.blit(t.frames.at(0), t.width, t.height, 0, 7 * 8);
    }
    if (!wait_seconds(5)) {
        // A key during the first wait skips #2 and #3 (0x30780).
        const Image t2 = load_title(2);
        screen_.clear(0);
        screen_.blit(t2.frames.at(0), t2.width, t2.height, 2 * 8, 1 * 8);
        const Image t3 = load_title(3);
        screen_.blit(t3.frames.at(0), t3.width, t3.height, 3 * 8, 21 * 8);
        wait_seconds(5);
    }
    // Credits ovl1 +0x02E1: DAC 16..31 from DS:8D8B, the credits frame
    // (015F:0123: ClearCells(1,1,38,22) and the outer frame), 21 lines.
    set_dac(16, 16, pal_8d8b_);
    screen_.clear(0);
    clear_cells(1, 1, 38, 22);
    layout_outer();
    struct Credit { Text t; int col, row, fg; };
    static constexpr Credit kCredits[] = {
        {Text::Credit0, 2, 1, 14},   {Text::Credit1, 11, 3, 15},  {Text::Credit2, 14, 4, 10},
        {Text::Credit3, 13, 6, 15},  {Text::Credit4, 4, 7, 10},   {Text::Credit5, 1, 9, 15},
        {Text::Credit6, 4, 10, 10},  {Text::Credit7, 4, 11, 10},  {Text::Credit8, 22, 11, 15},
        {Text::Credit9, 4, 12, 10},  {Text::Credit10, 4, 13, 10}, {Text::Credit11, 1, 14, 15},
        {Text::Credit12, 22, 14, 10}, {Text::Credit13, 4, 15, 10}, {Text::Credit14, 4, 16, 10},
        {Text::Credit15, 4, 17, 10}, {Text::Credit16, 4, 18, 10}, {Text::Credit17, 4, 19, 10},
        {Text::Credit18, 4, 20, 10}, {Text::Credit19, 4, 21, 10}, {Text::Credit20, 4, 22, 10}};
    for (const Credit& c : kCredits) draw_string(c.col, c.row, c.fg, 0, text_[c.t]);
    wait_seconds(20);
    screen_.clear(0);
    // Logo ovl1 +0x05BE: TITLE #4 at (48,72) with its palette on 16..31.
    const Image logo = load_title(4);
    screen_.clear(0);
    set_dac(16, 16, pal_908b_);
    screen_.blit(logo.frames.at(0), logo.width, logo.height, 6 * 8, 9 * 8);
    // MusicStart ovl1 +0x00A5 (Music.Play 07DA:0029): the driver and the
    // title's song, MUSIC.DAX #2 (docs/notes/sound.md); the window plays it.
    music_start();
}

void Game::music_start() {
    // Intro.MusicStart (ovl1 +0x00A5): MUSIC.DAX #2 loaded by 07DA:0208
    // (0x17FA8: Dax.Load, DS:488D, size DS:4891) whatever the sound setting
    // (session 78: the port tested it first; only a MUSIC.DAX without the
    // song tells the two apart), then Music.Play(2).
    const auto song = dax("MUSIC.DAX").load(2);
    if (!song) throw Error("MUSIC.DAX: no entry 2 (the title's song)");
    if (f487c_ == 5) return;                           // Music.Play 07DA:002C (0x17DCC): after ^S, none
    if (sfx_installed_) sfx_command(0xFF);             // Music.Play's SfxCommand(0xFF) (0x17DD5)
    if (!input_) return;
    input_->music_start(*song);
}

void Game::sound(int n) {
    // Sound.Play(n) (07DA:0000; docs/notes/sound.md "The effects"): with
    // the unit not installed SfxInit first (0x19109: DS:487D := DS:487C;
    // none after ^S, DS:487C = 5), then SfxCommand(n).
    if (!sfx_installed_) {
        f487d_ = f487c_;
        if (f487c_ != 5) sfx_installed_ = true;
    }
    sfx_command(n);
}

void Game::sfx_command(int n) {
    // SfxCommand(n) (0906:0000): recorded (ecl-run's dump, "sounds"), and
    // played by the window (Input::sound). It returns at once after ^S
    // (DS:487C = 5, 0x1906E..); 0xFF removes an installed unit.
    if (f487c_ == 5) return;
    if (n == 0xFF) {
        if (!sfx_installed_) return;
        sfx_installed_ = false;
    }
    if (input_) input_->sound(n);
}

void Game::dax_cut() {
    // Dax.Load's SoundSuspend (0800:0000, 0x18000): an installed unit gets
    // SfxCommand(0xFF), which removes it (the effect cut); SoundSuspend
    // saves DS:4895 in DS:4894 only after that cleared it, so SoundResume
    // (0x1802D) doesn't install it again: the next Sound.Play does. After
    // ^S the command does nothing, the unit stays, and SoundResume's SfxInit
    // copies DS:487C to DS:487D.
    if (!sfx_installed_) return;
    sfx_command(0xFF);
    if (sfx_installed_) f487d_ = f487c_;
}

void Game::toggle_debug() {
    // Input.GetKey 0x18D7E..0x18DBA: ^D flips DS:3BF0; on, Assign and Rewrite
    // debug.txt (DS:8312) in the current directory, the game's own (the
    // port: beside the save folder, D-0091); off, Close. The key goes on to
    // the caller as ^S's does.
    debug_ = debug_ ? 0 : 1;
    if (debug_) debug_file_.open(fs::path(save_dir_).parent_path() / "debug.txt", std::ios::binary | std::ios::trunc);
    else debug_file_.close();
}

void Game::debug_show(const std::string& caption, int a, int b, int c) {
    // ovl32 +0x0004 (0x60C84), called only with DS:3BF0 set in effect (0x60C9E):
    // row 0 cleared, "a , b , c" in yellow, the caption's PressContinue, row 0
    // cleared. The numbers are bytes.
    if (!debug_) return;
    fill_rect(0, 0, 0x27, 0, 0);
    const std::string sep = text_[Text::DebugSep];
    draw_string(0, 0, 0x0E, 0, std::to_string(a & 0xFF) + sep + std::to_string(b & 0xFF) + sep + std::to_string(c & 0xFF));
    press_continue(0x0E, 0, caption);
    fill_rect(0, 0, 0x27, 0, 0);
}

void Game::debug_slot(int slot) {
    // LoadTileSet 0x60DE6..0x60E10: with DS:3BF0, a line in debug.txt.
    if (debug_ && debug_file_) debug_file_ << text_[Text::DebugSlot] << slot << "\r\n" << std::flush;
}

void Game::ctrl_key(int k) {
    // Input.GetKey 0x18D05..0x18D72; the key is still returned to the
    // caller. ^S (0x13): the effects off (DS:487D := DS:487C, SfxCommand(0)
    // if installed, DS:487C := 5), or back on when DS:487D is 3 or 4
    // (Tandy, PC speaker; SfxCommand(1) if installed). The port's sound
    // line is the AdLib's 2, or the Tandy's 3 while the effects play on
    // the Tandy (D-0093: InputSource::sound_line, read at the press), so
    // with the Tandy's effects a second ^S turns them back on, as BUCK.CFG's
    // "T" does. ^U (0x15): DS:487E flipped; becoming 0 stops the music (it
    // starts at 1: the first ^U stops the title's).
    if (k == 0x13) {
        if (f487c_ != 5) {
            f487d_ = input_ ? static_cast<std::uint8_t>(input_->sound_line()) : f487c_;
            if (sfx_installed_) sfx_command(0);
            f487c_ = 5;
        } else if (f487d_ == 3 || f487d_ == 4) {
            f487c_ = f487d_;
            if (sfx_installed_) sfx_command(1);
        }
    } else if (k == 0x15) {
        f487e_ = f487e_ ? 0 : 1;
        if (!f487e_ && input_) input_->music_stop();
    }
}

void Game::run_program(const std::string& save_dir) {
    // main (0x1001D). StartupConfig (01B5:02ED) is replaced by the port's
    // fixed configuration: VGA, keyboard, Silent, this save directory
    // (D-0019).
    save_dir_ = save_dir;
    game_init();
    intro();
    // The title menu (0x10125..0x1015A): MenuScreen with a 120-second
    // timeout (DS:6B1F := 12000 centiseconds) that answers 'D' (DS:6B23),
    // cleared after it (0x10177). 'D' is the DEMO (DS:3BEA), anything else
    // PLAY.
    menu_timeout_cs_ = 12000;
    menu_timeout_key_ = 'D';
    const MenuKey mk = menu_screen(text_[Text::TitlePrompt], text_[Text::TitleItems], false);
    menu_timeout_cs_ = 0;
    menu_timeout_key_ = 0;
    set_dac(16, 16, pal_8d8b_);   // 0x10172
    if (mk.key == 'D') demo_ = 1;                      // 0x1018B
    for (;;) {
        // The main loop (0x10190..0x102B4): Music.Stop and the driver's
        // unload first.
        // 07DA:006F (0x17E0F): Music.Stop: the driver's fn 3 when loaded (DS:4895 = 2)
        if (input_) input_->music_stop();
        ecl_file_ = 1;                                 // 0x101A1 / 0x101B2
        if (demo_) {
            v0(0x4BFC) = 9;                            // 0x101AA: the slowest messages
        } else {
            team_menu();                               // 0x101BE
            redraw_screen();                           // 0x101F6: Screen.Redraw
        }
        run_with_loads();                              // 0x10210 (SetBandColours: no effect); D-0081
        game_reset();                                  // 0x10215: Game.Reset ovl9 +0x0919
        if (!demo_) continue;
        // 0x10224: after a demo, the intro again and the title menu with a
        // 30-second timeout answering 'D' (0x10237: DS:6B1F := 3000, cleared
        // at 0x10290); 'D' runs another demo, anything else the team menu.
        sound(text_.ds16(0x21DA));                     // 0x10228: Sound.Play(0xFF)
        intro();
        flush_keys();                                  // 0x10232
        menu_timeout_cs_ = 3000;
        menu_timeout_key_ = 'D';
        walls_table_[0] = 0;                           // word DS:6A34 := 0 (0x10248)
        const MenuKey again = menu_screen(text_[Text::TitlePrompt], text_[Text::TitleItems], false);
        menu_timeout_cs_ = 0;
        menu_timeout_key_ = 0;
        set_dac(16, 16, pal_8d8b_);                    // 0x10289
        demo_ = again.key == 'D' ? 1 : 0;              // 0x1029B
        sound(text_.ds16(0x21DC));                     // 0x102AF: Sound.Play(0)
    }
}

void Game::begin_state() {
    // BEGIN's DS:3BE8 := DS:3BE9 (0x46DEA).
    mode_ = prev_mode_;
    if (mode_ == 0 || mode_ == 5) {
        // D-0077: a game saved from EXIT's "not saved" No (0x46E9B, DS:3BE8
        // = 0); the original's AdventureInput then reads no key (0x46716:
        // run s52_state0_begin). The state the file recorded before the
        // team menu instead. State 5 (the fight, 0x39EF5) is saved only by
        // the heap's warning in a fight (D-0061), which the reference
        // machine never reaches; the original then plays the area's event
        // again with a one-row text window and starts its fight (session
        // 80: runs s80_state5_begin*); the port takes the same way out as
        // for state 0.
        const int was = mode_;
        const std::uint8_t m = file_prev_mode_;
        mode_ = (m == 3 || m == 4 || m == 9 || m == 10) ? m : 4;
        if (was == 0)
            combat::diag("D-0077", "BEGIN (0x46DEA) in game state 0, saved from EXIT (0x46E9B): state " +
                                       std::to_string(mode_) + " instead (the original reads no key)");
        else
            combat::diag("D-0077", "BEGIN (0x46DEA) in game state 5, saved in a fight (D-0061): state " +
                                       std::to_string(mode_) + " instead (the original plays the event again)");
    }
}

void Game::game_reset() {
    // Game.Reset ovl9 +0x0919 (0x3FD19..0x3FF52), main-session reading:
    // Game.Init's state part again (the fields the port models; buffers it
    // doesn't model are left out), with the facing 2 and the walls table
    // DS:6A34 partly set, then the screen's row 24 and NewGameDefaults.
    // The team menu's enable bytes are not reset (unlike Init).
    std::fill(vars0_.begin(), vars0_.end(), 0);
    std::fill(vars1_.begin(), vars1_.end(), 0);
    std::fill(vars2_.begin(), vars2_.end(), 0);
    v0(0x4BB6) = 2;
    v0(0x4BE6) = 1;
    v0(0x4BF2) = 0;
    x_ = 7;                 // DS:6A2F..6A33 := 0, then 7, 13, 2 (0x3FDCB)
    y_ = 13;
    facing_ = 2;
    wall_ahead_ = 0;
    map_byte_ = 0;
    walls_table_[2] = 0;    // DS:6A38 := 0, DS:6A3A := 1 (words; the first pair is left)
    walls_table_[3] = 1;
    for (std::size_t k = 4; k < 8; ++k) walls_table_[k] = 0xFFFF;   // DS:6A3C..6A43
    loaded_game_ = 0;       // DS:3BF1
    if (f387c_ != &startup_partner_ && f387c_ != &stray_partner_) {
        // DS:387C on a freed record (D-0056): Down from it gives the
        // party's first, as a nil +0xFF does (session 80, run
        // s80_d0056_after_wipe: the party's end, TEST created and added,
        // the Depot's Trade: nobody white, then TEST).
        f387c_ = &removed_partner_;
        removed_next_ = nullptr;
    }
    party_.clear();         // DS:4EC5, DS:4EC1 := nil
    for (ForeignSlot& f : foreign_slot_) f = {};
    cur_ = nullptr;
    prev_cur_ = nullptr;    // DS:4EBD := nil (0x3FAE2 / 0x3FE67)
    pc_ = 0x8000;           // DS:3BE2
    v0(0x4BFC) = 4;
    ecl_file_ = 1;          // DS:4898
    ecl_file_prev_ = 1;     // DS:4899
    area_map_ = 0;          // DS:6A2E
    v1(0x7F3E) = 0;
    list_top_ = 1;          // DS:6B1B
    f82f6_ = f82f7_ = f82f9_ = 0;
    party_lost_ = 0;        // DS:3BF6 (0x3FEBA)
    typing_ = 1;            // DS:3BF8 := 1 (0x3FE45)
    debug_ = 0;             // DS:3BF0 := 0 (0x3FE40)
    f6b14_ = 0xFF;          // DS:6B14 := 0xFF (0x3FEFB)
    anim_name_.clear();     // DS:6AFC
    anim_id_ = 0xFF;        // DS:6B0E
    saved_ = 0;             // DS:499B
    c6cfd_ = 0xE8;
    c6cfc_ = 0xDB;
    f6bda_ = 1;             // DS:6BDA
    clear_row24();          // 0653:1359
    highlight_ = 1;         // DS:6B1D
    mode_ = 4;              // DS:3BE8, DS:3BE9
    prev_mode_ = 0;
    move_mode_ = 0;         // DS:8303
    f3bed_ = 0;
    new_game_defaults();    // ovl9 +0x0000
}

// --- the team menu ------------------------------------------------------------------------

void Game::layout_state0() {
    // Screen.LayoutState0 015F:00D8: ClearCells(0,0,0x26,0x16), the outer
    // frame (its byte parameter is unused).
    clear_cells(0, 0, 38, 22);
    layout_outer();
}

void Game::team_menu() {
    // TeamMenu ovl13 +0x019E (0x46AAE).
    prev_mode_ = mode_;
    mode_ = 0;
    int idx = 0;   // [bp-1E]: reset only here
    for (;;) {
        layout_state0();
        static constexpr std::uint8_t kNoParty[13] = {1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1};
        if (cur_) {
            draw_roster();
            for (int i : {1, 2, 3, 5, 7, 9, 10, 12}) team_enable_[static_cast<std::size_t>(i)] = 1;
            team_enable_[4] = v1(0x7EA8) != 0;
            team_enable_[8] = 0;
            team_enable_[11] = 0;
        } else {
            for (int i : {1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12})
                team_enable_[static_cast<std::size_t>(i)] = kNoParty[i];
        }
        std::vector<ListNode> list;
        for (int i = 0; i < 13; ++i) {
            if (!team_enable_[static_cast<std::size_t>(i)]) continue;
            // JOYSTICK/MOUSE INITIALIZE (record 11, shown only with no party:
            // DS:0C17 := 1 at 0x46B6A, 0 at 0x46B31) is the port's Options
            // with the option on (D-0044, D-0065): the port's own word.
            if (i == 11 && input_ && input_->options_menu()) list.push_back({"Options", false});
            else list.push_back({text_.ds_str(0x0A20 + 0x2A * i, 40), false});
        }
        if (cur_.detached && !list.empty()) detached_name_ = list.front().str;   // D-0055
        bool redraw = true;
        int picked = -1;
        f6b4c_ = 0;                                    // 0x46C0C
        f8433_ = 1;
        list_menu(text_[Text::TeamPrompt], text_[Text::TeamSelect], 9, 0x0C, 0x26, 0x16, list, false, redraw, idx,
                  picked);
        f8433_ = 0;                                    // 0x46C60
        // The choice is the first character of the picked line (0x46C65).
        // Esc leaves no line, and the original reads byte 0000:0001: the
        // high byte of the INT 0 handler's offset, CS:00CE set by TP's
        // System init (0x1BE93), a constant in START.EXE. So it's 0 and
        // matches nothing (docs/notes/frontend.md); kept in the low-memory
        // table with the other bytes nil pointers reach.
        const char choice0 = picked >= 0 && !list[static_cast<std::size_t>(picked)].str.empty()
                                ? list[static_cast<std::size_t>(picked)].str[0]
                                : static_cast<char>(low_.peek(0x01));
        // 0x46C6F (main): a click on CurChar's roster row (DS:6B4C) views
        // that member instead of A/B/C/E/L/S (CS:0132); I, V and R skip
        // PickCharacter.
        const char choice = f6b4c_ && std::strchr("ABCELS", choice0) && choice0 ? 'V' : choice0;
        if (choice != 'E' && choice != 'S') saved_ = 0;   // 0x46C9E
        switch (choice) {
            case 'C':
                create_character();
                break;
            case 'L':
                if (team_enable_[8]) load_menu();
                break;
            case 'S':
                if (team_enable_[9] && !party_.empty()) {
                    mode_ = prev_mode_;
                    save_game_menu();
                    mode_ = 0;
                }
                break;
            case 'B':
                // 0x46DEA..0x46E1F: DS:8433 := 1 (0x46DF5), then BEGIN with
                // a party or in the demo (DS:3BEA, 0x46E03; the main loop
                // never opens the team menu in the demo, 0x101AA).
                if (team_enable_[10]) f8433_ = 1;
                if (team_enable_[10] && (!party_.empty() || demo_)) {
                    begin_state();
                    redraw_screen();
                    v1(0x7EA8) = 0;
                    return;
                }
                break;
            case 'I':
                if (team_enable_[3] && (f6b4c_ || pick_character())) icon_select();
                break;
            case 'V':
                if (team_enable_[5] && (f6b4c_ || pick_character())) view_character();
                break;
            case 'A':
                add_character();
                break;
            case 'R':
                if (team_enable_[7] && (f6b4c_ || pick_character())) remove_from_team();
                break;
            case 'E': {
                // 0x46E36.
                menu_prompt_fg_ = 14;
                int a = yes_no(text_[Text::TeamQuit], 2);
                if (a == 'Y' && !party_.empty() && !saved_) {
                    menu_prompt_fg_ = 14;
                    a = yes_no(text_[Text::TeamNotSaved], 2);
                    if (a == 'N') save_game_menu();
                }
                if (a == 'Y') throw QuitToDos{};   // 08AD:0000 (0x18AD0)
                break;
            }
            case 'M':
                if (team_enable_[1]) modify_character();   // 0x46CC4: tests Purge's byte (both are set together)
                break;
            case 'P':
                if (team_enable_[2]) purge_character();    // 0x46CD6: tests Modify's byte
                break;
            case 'T':
                if (team_enable_[4]) train_character();   // 0x46D0A
            case 'J':
                // JOYSTICK/MOUSE INITIALIZE (ovl13 +0x2F94): the item stays
                // where it is, and choosing it does nothing: keyboard and
                // mouse are always on (the owner's decision, D-0044).
                break;
            case 'O':
                // The same item as the port's Options (D-0065).
                options_screen();
                break;
            default:
                break;
        }
    }
}

void Game::show_pointer() {
    // Game.Init's SetCursor (INT 33h fn 9, hotspot (0, 0)) with the masks
    // at DS:346C (VGA; DS:34AC otherwise), shown while there is a driver.
    if (!input_) return;
    std::array<std::uint16_t, 32> masks{};
    for (std::size_t i = 0; i < masks.size(); ++i) masks[i] = text_.ds16(0x346C + 2 * i);
    input_->pointer(masks, mouse_on_);
}

// --- ListMenu (ovl25 +0x1AF3) ------------------------------------------------------------

void Game::list_skip_headers(const std::vector<ListNode>& list, int& index, int rows, bool down) {
    // ovl25 +0x185C (0x5B39C): up to `rows` times past heading lines.
    for (int n = 0; n < rows; ++n) {
        if (index < 0 || index >= static_cast<int>(list.size()) || !list[static_cast<std::size_t>(index)].header) break;
        list_move(list, index, rows, down);   // the move without its own skip
    }
}

void Game::list_move(const std::vector<ListNode>& list, int& index, int rows, bool down) {
    // MoveCursor ovl25 +0x1A1E (0x5B55E): wraps within the visible page.
    const int total = static_cast<int>(list.size());
    if (down) {
        ++index;
        if (index > list_top_ + rows - 1) index = list_top_;
        if (index > total - 1) index = list_top_;
    } else {
        --index;
        if (index < list_top_) index = list_top_ + rows - 1;
        if (index > total - 1) index = total - 1;
    }
}

void Game::list_draw_page(const std::vector<ListNode>& list, int x1, int y1, int x2, int y2) {
    // DrawPage ovl25 +0x14C5 (0x5B005): headings in DS:6B45, items in
    // DS:6B46, each blanked to x2.
    // The rows of the lines drawn, for clicks: DS:6B23 + k (k = 1..25),
    // 0xFF for headings and past the last line.
    std::fill(list_rows_.begin() + 1, list_rows_.end(), 0xFF);
    clear_cells(x1, y1, x2, y2);
    const int width = x2 - x1 + 1;
    int row = y1;
    for (int i = list_top_, k = 1; i >= 0 && i < static_cast<int>(list.size()) && row <= y2; ++i, ++row, ++k) {
        const ListNode& n = list[static_cast<std::size_t>(i)];
        if (!n.header && k <= 0x19) list_rows_[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(row);
        draw_string(x1, row, n.header ? menu_prompt_fg_ : menu_text_fg_, 0, n.str);
        const int len = static_cast<int>(n.str.size());
        if (len < width) draw_string(x1 + len, row, 0, 0, std::string(static_cast<std::size_t>(width - len), ' '));
    }
}

void Game::list_highlight(const std::vector<ListNode>& list, int i, int x1, int y1, bool on) {
    // Highlight ovl25 +0x16C5 / Unhighlight +0x1763: the trimmed text,
    // black on DS:6B47, or back in its colour.
    if (i < 0 || i >= static_cast<int>(list.size())) return;
    const ListNode& n = list[static_cast<std::size_t>(i)];
    const std::string& s = n.str;
    const int len = static_cast<int>(s.size());
    int f = 1;
    // ovl25 +0x1633 (0x5B173): the first non-space column (nested in ListMenu)
    while (f < len && s[static_cast<std::size_t>(f - 1)] == ' ') ++f;
    int l = len;
    // ovl25 +0x167F (0x5B1BF): the last non-space column (nested in ListMenu)
    while (l > 1 && s[static_cast<std::size_t>(l - 1)] == ' ') --l;
    if (l < f) return;
    const std::string t = s.substr(static_cast<std::size_t>(f - 1), static_cast<std::size_t>(l - f + 1));
    const int row = y1 + i - list_top_;
    if (on) draw_string(x1 + f - 1, row, 0, menu_hot_fg_, t);
    // Unhighlight ovl25 +0x1763 (0x5B2A3): the trimmed text back in DS:6B45 (heading) or DS:6B46
    else draw_string(x1 + f - 1, row, n.header ? menu_prompt_fg_ : menu_text_fg_, 0, t);
}

int Game::list_menu(const std::string& prompt, const std::string& select, int x1, int y1, int x2, int y2,
                    std::vector<ListNode>& list, bool exit_item, bool& redraw, int& index, int& picked) {
    // ListMenu ovl25 +0x1AF3 (0x5B633); keyboard path.
    if (list.empty()) {
        index = 0;
        picked = -1;
        list_c0_ = 0x7F;
        list_c1_ = static_cast<std::int8_t>(0x80);
        return 0;
    }
    int prev_key = 'N';                                // [bp-0x5A]
    list_c0_ = static_cast<std::int8_t>(x1);           // DS:6B3D / 6B3E (0x5B694)
    list_c1_ = static_cast<std::int8_t>(x2);
    const int rows = y2 - y1 + 1;
    const int total = static_cast<int>(list.size());
    int heads = 0;
    while (heads < total && list[static_cast<std::size_t>(heads)].header) ++heads;
    if (total <= rows) list_top_ = 0;
    if (list_top_ > index) {
        list_top_ = index;
        redraw = true;
    }
    if (list_top_ > total) {
        list_top_ = 0;
        redraw = true;
    }
    ++index;   // then a move up: the index clamped into the page
    list_move(list, index, rows, false);
    list_skip_headers(list, index, rows, false);
    if (redraw) list_draw_page(list, x1, y1, x2, y2);
    redraw = false;
    auto page = [&](bool down) {
        // Page ovl25 +0x198C (0x5B4CC).
        const int off = index - list_top_;
        if (down) {
            list_top_ += rows;
            if (list_top_ > total - rows) list_top_ = total - rows;
        } else {
            list_top_ -= rows;
            if (list_top_ < 0) list_top_ = 0;
        }
        index = list_top_ + off;
        list_skip_headers(list, index, rows, down);
        list_draw_page(list, x1, y1, x2, y2);
    };
    for (;;) {
        list_highlight(list, index, x1, y1, true);
        note_transcript("[list " + std::to_string(index) + ": " + list[static_cast<std::size_t>(index)].str + "]");
        std::string line = select;
        const bool next = total - rows > list_top_;
        const bool prev = list_top_ > heads;
        if (next) line += text_[Text::ListNext];
        if (prev) line += text_[Text::ListPrev];
        if (exit_item) line += text_[Text::ListExit];
        f6b4b_ = 1;                                    // 0x5B870
        const MenuKey mk = menu_screen(prompt, line, true);
        f6b4b_ = 0;
        list_highlight(list, index, x1, y1, false);
        const int r = static_cast<std::uint8_t>(mouse_y_ / 8), c = static_cast<std::uint8_t>(mouse_x_ / 8);   // 0x5B89A
        // A key from a click pages or chooses only on the menu row (0x18);
        // elsewhere it moves the highlight to the clicked line, or chooses
        // the line already highlighted (0x5B97C..0x5BA46, main).
        const bool menu_row = !f8435_ || r == 0x18;
        auto leave = [&] {
            list_c0_ = 0x7F;
            list_c1_ = static_cast<std::int8_t>(0x80);
        };
        if (mk.ext) {
            if (mk.key == 0x48) {
                list_move(list, index, rows, false);
                list_skip_headers(list, index, rows, false);
            } else if (mk.key == 0x50) {
                list_move(list, index, rows, true);
                list_skip_headers(list, index, rows, true);
            } else if (mk.key == 0x49) {
                if (prev) page(false);
            } else if (mk.key == 0x51) {
                if (next) page(true);
            }
            prev_key = mk.key;
            continue;
        }
        if (mk.key == 'P' || mk.key == 'N') {
            if (menu_row) page(mk.key == 'N');
            prev_key = mk.key;
            continue;
        }
        if (mk.key == kEsc || mk.key == 'E' || mk.key == 0) {
            picked = -1;
            leave();
            return 0;
        }
        if (f8435_ && r != 0x18) {
            if (c >= list_c0_ && c <= list_c1_)
                for (int k = 1; k <= 0x19; ++k) {
                    if (list_rows_[static_cast<std::size_t>(k)] != r) continue;
                    const int i = k - 1 + list_top_;
                    if (i == index) {
                        picked = index;                // 0x5B9E0: the columns stay set
                        return mk.key;
                    }
                    list_highlight(list, index, x1, y1, false);
                    index = i;
                    list_highlight(list, index, x1, y1, true);
                }
            prev_key = mk.key;
            continue;
        }
        if (f8435_ && prev_key != mk.key) {            // a command clicked once arms it
            prev_key = mk.key;
            continue;
        }
        picked = index;
        leave();
        return mk.key;
    }
}

// --- pickers and slot menus -------------------------------------------------------------------

bool Game::pick_character() {
    // PickCharacter ovl18 +0x2ADB (0x52A7B): MenuScreen(CS:2A9F, CS:2AAF);
    // Up/Down move CurChar (the old row redrawn plain, the new one white);
    // until {0, 'E', 'S'} (CS:2ABB).
    // DS:8433 := 1 for the menus, restored at the end (0x52A81, 0x52B1E).
    const std::uint8_t saved8433 = f8433_;
    f8433_ = 1;
    int key = 0;
    for (;;) {
        const MenuKey mk = menu_screen(text_[Text::PickPrompt], text_[Text::PickItems], true);
        if (mk.ext) {
            roster_row(cur_, false);
            select_char(mk.key);
            roster_row(cur_, true);
            continue;
        }
        key = mk.key;
        if (key == 0 || key == 'E' || key == 'S') break;
    }
    roster_row(cur_, true);
    f8433_ = saved8433;
    return key == 'S';
}

bool Game::save_dir_ready() {
    // SaveDirReady ovl17 +0x0448 (0x4DB68): the save directory, made when
    // missing (oracle s31). The floppy prompts don't apply.
    std::error_code ec;
    if (fs::is_directory(save_dir_, ec)) return true;
    fs::create_directories(save_dir_, ec);
    if (ec) throw Error("cannot make the save directory " + save_dir_ + ": " + ec.message());
    return true;
}

void Game::load_menu() {
    // LoadSavedGame ovl17 +0x1AC8 (0x4F1E8), the slot menu: only slots
    // whose savgamX.dat exists; none: return silently.
    if (!save_dir_ready()) return;
    std::string items;
    for (char c = 'A'; c <= 'J'; ++c)
        if (save_file_exists(std::string("SAVGAM") + c + ".DAT")) items += std::string(1, c) + " ";
    if (items.empty()) return;
    items.pop_back();
    highlight_ = 1;     // DS:6B1D := 1; DS:6B48 := 0xFF
    menu_sum_ = 0xFF;
    for (;;) {
        // remap off: an extended key comes back as its scan code (Up 'H',
        // Home 'G', PgUp 'I'), a slot if that file exists (quirk, reproduced).
        const MenuKey mk = menu_screen(text_[Text::LoadWhich], items, false);
        if (mk.key == 0) return;
        if (mk.key >= 'A' && mk.key <= 'J' &&
            save_file_exists(std::string("SAVGAM") + static_cast<char>(mk.key) + ".DAT")) {
            load_save(save_path(std::string("SAVGAM") + static_cast<char>(mk.key) + ".DAT"));
            clear_row24();   // 0653:1359
            return;
        }
    }
}

bool Game::save_game_menu() {
    // SaveGame ovl17 +0x220C (0x4F92C): the slot menu until a key in {0,
    // 'A'..'J'} (CS:2159); an extended key counts by its scan code, so
    // Home, Up and PgUp pick G, H and I (0x4F96C; reproduced).
    int key = 0;
    for (;;) {
        const MenuKey mk = menu_screen(text_[Text::SaveWhich], text_[Text::SaveSlots], mode_ == 2);
        key = mk.key;
        if (key == 0 || (key >= 'A' && key <= 'J')) break;
    }
    if (!key) return false;
    save_dir_ready();
    save_game(static_cast<char>(key));
    return true;
}

// --- ADD and REMOVE --------------------------------------------------------------------------

void Game::add_character() {
    // AddCharacter ovl13 +0x1A15 (0x48325).
    clear_cells(1, 1, 0x26, 0x16);
    // ovl17 +0x0016 Lister (0x4D736): the FindFirst/FindNext list; its ".SAV" suffix branch (0x4D89F) can't fire, the one caller passes *.who.
    // RosterList ovl17 +0x037E / Lister +0x0016: every *.who of exactly
    // 0x103 bytes, its name padded to 15 with spaces; not listed: +0xC2 >
    // 0x7F, or a party member's padded name equal to it. The original
    // lists in DOS directory order; the port sorts by file name, which is
    // the order the oracle (DOSBox-X) gives (D-0021).
    std::vector<ListNode> names;
    std::vector<std::string> files;
    if (save_dir_ready()) {
        std::vector<fs::path> who;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(save_dir_, ec)) {
            const std::string n = upper(e.path().filename().string());
            if (n.size() > 4 && n.substr(n.size() - 4) == ".WHO" && e.is_regular_file()) who.push_back(e.path());
        }
        std::sort(who.begin(), who.end(), [](const fs::path& a, const fs::path& b) {
            return upper(a.filename().string()) < upper(b.filename().string());
        });
        for (const fs::path& f : who) {
            const Bytes rec = read_file(f.string());
            if (rec.size() != kRecSize) continue;
            const std::string nm(rec.begin() + 1, rec.begin() + 1 + std::min<int>(rec[0], 15));
            std::string padded = nm + std::string(15 - nm.size(), ' ');
            if (rec[0xC2] > 0x7F) continue;
            bool in_party = false;
            for (const auto& m : party_) {
                const std::string mn = m->name();
                if (mn + std::string(15 - std::min<std::size_t>(mn.size(), 15), ' ') == padded) in_party = true;
            }
            if (in_party) continue;
            names.push_back({padded, false});
            std::string base = f.stem().string();
            files.push_back(base);
        }
    }
    if (names.empty()) return;
    int idx = 0, picked = -1;
    bool redraw = true;
    // count [bp-1E] and the Exit flag [bp-1C] are uninitialised locals in
    // the original; the port starts count at 0 and always offers Exit, as
    // observed (D-0021).
    int count = 0;
    int key = 0;
    do {
        key = list_menu(text_[Text::AddPrompt], text_[Text::AddItem], 1, 2, 0x26, 0x16, names, true, redraw, idx,
                        picked);
        if ((key == kEnter || key == 'A') && picked >= 0 && names[static_cast<std::size_t>(picked)].str[0] != '*') {
            clear_row24();
            std::unique_ptr<Character> rec = char_load(files[static_cast<std::size_t>(picked)], ".WHO");
            std::string& line = names[static_cast<std::size_t>(picked)].str;
            line = (text_[Text::AddMark] + line).substr(0, 40);
            count = 0;
            if (party_.empty()) {
                v1(0x7F3E) = 0;
                join_party(std::move(rec));
                load_member_icon();                    // ovl17 +0x065F (0x48462)
            } else {
                bool found = false;
                for (const auto& m : party_) {
                    if (m->name() == rec->name()) { found = true; break; }
                    if (m->b(0xE7) < 0x80) ++count;
                }
                const bool player = rec->b(0xE7) < 0x80;
                if (!found && ((player && count < 6) || (!player && v1(0x7F3E) < 8))) {
                    join_party(std::move(rec));
                    load_member_icon();                // ovl17 +0x065F (0x484ED)
                    if (player) ++count;
                } else {
                    line.erase(0, 2);   // Delete(str, 1, 2): the mark again
                }
            }
        }
    } while (key != 'E' && key != 0 && count <= 5 && v1(0x7F3E) <= 7);
}

void Game::load_member_icon() {
    // LoadMemberIcon ovl17 +0x065F (0x4DD7F): CurChar's CHARS icon into its
    // slot (SetEclFile(1) around it: CHARS has no area digit), FlushKeys.
    // Without it a member added to a freed slot showed the removed
    // member's icon (session 71, run s71_venus71_B).
    if (!cur_) return;
    load_combat_icon("CHARS", cur_->b(0xB8), cur_->b(0xB9));
    flush_keys();                                      // 0x4DDC2
}

void Game::remove_from_team() {
    // TeamMenu 'R' (0x46D56) after PickCharacter: an NPC (+0xE7 >= 0x80)
    // goes to Purge; otherwise CharSave(CurChar, '') and, if it saved,
    // RemoveChar(0, 1).
    if (!cur_) return;
    if (cur_->b(0xE7) >= 0x80) {                       // 0x46DA9: an NPC is purged (a second pick)
        purge_character();
        return;
    }
    const int i = index_of(cur_);
    if (char_save_roster(*cur_, i + 1 == static_cast<int>(party_.size()))) remove_char(false, true);
}

void Game::row_message(const std::string& s, int fg, int bg) {
    // 0653:0DE1 (0x17311): row 24 cleared (FillCells), the text (at most
    // 40) in fg/bg, Game.Delay, the row cleared again.
    fill_cells(0, 0x18, 0, 0x28);
    draw_string(0, 0x18, fg, bg, s.substr(0, 40));
    message_chars(std::min<std::size_t>(s.size(), 40));
    game_delay();
    fill_cells(0, 0x18, 0, 0x28);
}

void Game::show_message(const std::string& s) {
    // ShowMessage ovl23 +0x0A18 (0x56928): row 24 in green, Game.Delay
    // (var 0x4BFC * 100 ms, no key), then the row cleared.
    clear_cells(0, 0x18, 0x27, 0x18);
    draw_string(0, 0x18, 10, 0, s.substr(0, 40));
    message_chars(std::min<std::size_t>(s.size(), 40));
    game_delay();
    clear_cells(0, 0x18, 0x27, 0x18);
}

}  // namespace cotd::game
