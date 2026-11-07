// SPDX-License-Identifier: GPL-3.0-or-later
// Game state and the ECL interpreter (docs/formats/ecl.md), headless.
//
// What no shipped data reaches and the port leaves out (ECL OP_1D,
// PARTY_STAT, CALL 0xB203; effect handlers past the table) raises
// Error("... not implemented") instead of guessing (DIVERGENCE.md D-0082).
// Evidence for each routine is cited next to its code; the state names
// follow the original's data segment (DS:xxxx) so dumps can be compared
// with oracle observations.
#pragma once

#include <array>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/common.hpp"
#include "core/dax.hpp"
#include "core/files.hpp"
#include "core/real48.hpp"
#include "game/combat_rules.hpp"

#include <fstream>
#include "core/framebuffer.hpp"
#include "core/image.hpp"
#include "core/gametext.hpp"
#include "core/logbook_txt.hpp"
#include "game/input.hpp"
#include "ecl/script.hpp"

namespace cotd::game {

inline void data_note(const char*, int, int = 0) {}

constexpr std::size_t kRecSize = 0x103;   // character record
constexpr std::size_t kItemSize = 0x3E;   // item node
constexpr std::size_t kSpellSize = 9;     // effect/spell node
// D-0073 (session 61): an effect node whose +4 (Effect.Remove's handler
// flag) is set must name a type whose removal the port runs: rejected,
// naming `file`, the node's offset and +4, for a type outside the handler
// table DS:699D (0, past 0x20) or 0x1F's (not ported).
void check_effect_nodes(const std::vector<Bytes>& nodes, const std::string& file);

// A character or monster record with its lists. The record's pointer
// fields (+0xB4 stash, +0xF3, +0xF7 items, +0xFB effects, +0xFF next) are
// held as the vectors below; their bytes in `rec` stay as loaded.
struct Character {
    Bytes rec = Bytes(kRecSize, 0);
    // The record's pointer fields as last read from disk (+0xB4, +0xF3,
    // +0xF7, +0xFB, +0xFF), 0 for none: written back for non-nil lists
    // (DIVERGENCE.md D-0010). In `rec` they are nil after loading, as in
    // the original (CharLoad 0x4E7A4..).
    std::array<std::uint32_t, 5> disk_ptrs{};
    std::vector<Bytes> items;    // +0xF7, list order
    std::vector<Bytes> stash;    // +0xB4
    std::vector<Bytes> effects;  // +0xFB
    Bytes cb;                    // +0xF3: the combat block (0x12 bytes), empty when nil
    Character* cb_target = nullptr;   // cb+8: the last target (far pointer), nil = nullptr
    // Recalc's readied weapon / armour (+0xEB / +0xEF in memory): indices
    // into `items`, -1 for nil.
    int weapon = -1, armour = -1;

    std::uint8_t& b(std::size_t off) { return rec.at(off); }
    std::uint8_t b(std::size_t off) const { return rec.at(off); }
    std::uint16_t w(std::size_t off) const { return le16(&rec.at(off)); }
    void setw(std::size_t off, std::uint32_t v) { put16(&rec.at(off), v); }
    std::uint32_t d(std::size_t off) const { return le32(&rec.at(off)); }
    void setd(std::size_t off, std::uint32_t v) { put32(&rec.at(off), v); }
    std::string name() const;   // Pascal string at +0, at most 15
};

std::string str_long(std::int32_t v);   // Text.StrLong 0653:13E8
std::string str_byte(int v);            // Text.StrByte 0653:1370

// Keys as the headless driver supplies them: ASCII, or 0x100 + scan code
// for extended keys (arrows etc.).
enum Key : int {
    kEnter = 0x0D, kEsc = 0x1B, kBackspace = 0x08,
    kHome = 0x147, kUp = 0x148, kPgUp = 0x149, kLeft = 0x14B, kCenter = 0x14C,
    kRight = 0x14D, kEnd = 0x14F, kDown = 0x150, kPgDn = 0x151, kIns = 0x152, kDel = 0x153,
};

// The player quits to DOS (08AD:0000).
struct QuitToDos {};

// D-0081 (the fix script-load): a LOAD in a script's TEAM_MENU ends the
// running script; the run loop starts again with the loaded game.
// Also, always on, a LOAD_ECL the old block goes on to whose block the loaded
// file lacks: the original hangs there (Game::load_block).
struct RestartRun {};

// The script stops the game with a diagnostic (Q7, DIV by zero).
struct Stopped : Error {
    using Error::Error;
};

// A ship record of 0x6C bytes (docs/notes/space_combat.md): the party's
// in vars 0x4D00..0x4D6B, the enemy's for one SPACE_COMBAT.
struct Ship {
    std::uint8_t* p = nullptr;
    int w(int off) const;                              // a signed word
    void setw(int off, int v);
    int cur(int i) const { return w(0x16 + 2 * i); }   // system i now
    int max(int i) const { return w(0x22 + 2 * i); }   // its maximum
    void set_cur(int i, int v) { setw(0x16 + 2 * i, v); }
    std::uint8_t* slot(int w) const { return p + 0x3C + 6 * w; }   // weapon slot
    std::string name() const { return std::string(reinterpret_cast<const char*>(p + 1), std::min<int>(p[0], 15)); }
};

class Game {
public:
    explicit Game(const GameFiles& files);

    // --- setup --------------------------------------------------------
    void set_seed(std::uint32_t seed) { seed_ = seed; }
    // false: skip the load-time half of the quirk policy (the run-time
    // checks still apply). Used to run scripts that contain a rejected
    // construct on a path that doesn't execute (oracle script t1, Q1).
    // The fixes of the original's faults (game/input.hpp Fixes; off by default).
    void set_fixes(const Fixes& f) { fixes_ = f; }
    // The Log Book in the game (D-0083): LOGBOOK.TXT's entry printed at
    // the LOGBOOK prompt as the game's text, its maps drawn from the
    // game's data. Only the entries the shipped LOGBOOK calls name are shown.
    void set_logbook_from_text(const std::string& name, const std::string& text);
    const LogbookText* logbook_text() const { return logbook_text_.get(); }
    static bool logbook_named(int entry);
    // The passcode option (D-0084, off by default): at the shipped
    // passcode prompts the answer is typed for the player.
    void set_passcodes(bool on) { passcodes_ = on; }
    void set_input(Input& in) {
        input_ = &in;
        show_pointer();
    }
    void show_pointer();                                 // the window's mouse pointer (DS:346C)
    // SkillTotal ovl23 +0x2E3F (game/skills.cpp); also the combat rules'
    // SkillCheck input (cotd combat-rule).
    int skill_total(const Character& c, int k) const;
    // SavingThrow ovl22 +0x0679 (game/combat_rules.cpp) on the game's
    // RNG, with the effects' hook 0x0C (game.cpp).
    bool save(Character& c, int type, int bonus);
    const Framebuffer& screen() const { return screen_; }
    // LOAD SAVED GAME (ovl17 +0x1AC8): SAVGAMx.DAT + CHRDATxn.SAV/.STF.
    void load_save(const std::string& savgam_path);
    // SaveGame (ovl17 +0x220C) to slot letter 'A'..'J' in the save
    // directory: SAVGAMx.DAT and each member's CHRDATxn files.
    void save_game(char slot);
    // The team menu's SAVE CURRENT GAME (0x46DC3): DS:3BE8 := DS:3BE9,
    // SaveGame, DS:3BE8 := 0. After load_save it writes the game back.
    // BEGIN ADVENTURING: Ecl.RunLoop (ovl2 +0x3209) up to its first wait
    // for player input.
    void run_with_loads();   // begin_run_loop, again after a RestartRun (D-0081)
    // The whole program from launch (main 0x1001D): Game.Init, the intro,
    // the title menu, then the team menu and the adventure in turn. The
    // save directory is the port's (DIVERGENCE.md D-0019).
    void run_program(const std::string& save_dir);

    // --- state for dumps ---------------------------------------------
    std::uint32_t seed() const { return seed_; }
    // The battlefield (game/combat.cpp): [DS:68A2]+7, 50 x 25 tiles; the
    // view origin (+2/+3); the icon table DS:60B3 + 4k (k = 1..); the
    // generator's Random calls (its temporary stream: site, range, result).
    struct Icon {
        int x = 0, y = 0, size = 0;
        Character* rec = nullptr;   // DS:6745 + 4k
    };
    static constexpr int kFieldW = 50, kFieldH = 25;

private:
    friend class Interp;
    friend struct ShipScreen;
    friend struct SpaceFight;

    // --- files ---------------------------------------------------------
    const DaxFile& dax(const std::string& name);
    std::optional<Bytes> dax_entry(const std::string& name, int id);
    std::string ecl_file_name(const char* stem) const;    // stem + Str(DS:4898) + ".DAX"
    std::string continue_item() const;                   // WAIT's menu item (DS:0F00)

    // --- variables (ReadVar ovl15 +0x0DB6, SetVar ovl15 +0x0BA5) ---------
    std::uint32_t read_var(std::uint32_t addr, int size);
    void set_var(std::uint8_t type, std::uint32_t value, std::uint32_t addr);
    std::string get_string_var(std::uint32_t addr);
    void set_string_var(std::uint32_t addr, const std::string& s);
    std::uint8_t& v0(std::uint32_t addr) { return vars0_.at(addr - 0x4B00); }
    std::uint8_t& v1(std::uint32_t addr) { return vars1_.at(addr - 0x7C00); }

    // --- party -----------------------------------------------------------
    Character* member(int index);                        // 0-based, or nullptr
    int index_of(const Character* c) const;
    void join_party(std::unique_ptr<Character> c);       // ovl17 +0x194D
    void remove_char(bool keep_count, bool free_icon);   // ovl13 +0x1C47
    void recalc(Character& c);                           // ovl23 +0x138D (game/sheet.cpp)
    int calc_move(Character& c);                         // ovl23 +0x166C
    std::string item_name(Bytes& item, bool ready_prefix = false);   // ovl23 +0x0124 (writes the node's name)
    int spec_count(const Character& c, int type) const;  // ovl23 +0x0711
    // The character sheet (game/sheet.cpp; docs/notes/frontend.md).
    void draw_sheet();                                   // ovl18 +0x00E3
    void sheet_ability(int i, bool hilite);              // ovl18 +0x09A9
    void sheet_credits();                                // ovl18 +0x05C4
    void sheet_combat();                                 // ovl18 +0x0620
    void sheet_career_skills();                          // ovl18 +0x08A9
    void draw_char_name(const Character& c, int col, int row, bool possessive = false);   // ovl25 +0x22BE
    void draw_ac(const Character& c, int col, int row);                     // ovl25 +0x2399
    void draw_hp(const Character& c, int col, int row, bool hilite, bool with_max);   // ovl25 +0x2424
    // Skills (game/skills.cpp).
    int skill_ability(int k) const;
    int skill_flag(int k) const;
    std::string skill_name(int k) const;
    int career_skill(int cls, int i) const;
    bool skill_prereq_ok(const Character& c, int k) const;   // ovl23 +0x2C47
    int free_skill_level(const Character& c, int k) const;   // ovl19 +0x0000
    int skill_bonus(const Character& c, int k) const;        // ovl23 +0x2DFC
    void init_skills(Character& c);                          // ovl13 +0x0663
    void init_career_skills(Character& c);                   // ovl13 +0x05FF
    void build_skill_lists(const Character& c);              // ovl19 +0x0086
    void allocate_skills(int career, int general, int max);  // ovl19 +0x0D90
    void weapon_specialize(bool must_choose);                // ovl18 +0x0AA5
    // Character creation (game/chargen.cpp) and icons (game/icons.cpp).
    void roll_hp_level(Character& c);                        // ovl13 +0x2A72
    void train_creation(Character& c);                       // ovl13 +0x2BFD, creation mode
    void train_character();                                  // ovl13 +0x2BFD, the team menu's TRAIN
    void modify_character();                                 // ovl13 +0x1156
    void modify_draw(Character& c, int field, int cur, bool on);   // its nested Draw +0x0F45
    int min_hp(const Character& c) const;                    // ovl13 +0x298F
    int max_hp(const Character& c) const;                    // ovl13 +0x29EC
    void purge_character();                                  // ovl13 +0x186D
    void change_menu();                                      // ovl12 +0x0B3B (CHANGE)
    void change_level();                                     // ovl12 +0x0236
    void change_messages();                                  // ovl12 +0x0349
    std::string speed_name(int v) const;                     // the Messages speed's name (DS:0990, DS:09B4)
    void options_screen();                                   // the port's options (D-0065, options.cpp)
    void change_order();                                     // ovl12 +0x0820
    void move_up();                                          // ovl12 +0x053B
    void move_down();                                        // ovl12 +0x067C
    std::string ds_pascal(std::size_t off) const;            // a DS Pascal string by its own length
    void train_level(Character& c, bool creation);           // TrainCharacter's level-up (0x49730)
    std::int32_t xp_for_level(const Character& c, int level) const;   // DS:31A2
    void set_default_icon(Character& c);                     // ovl13 +0x1D8A
    void give_starting_items(Character& c);                  // ovl14 +0x03F9
    int pick_from_list(Text header, const std::vector<std::string>& names);
    void load_icon_table();                                  // ovl13 +0x1E99
    void draw_icon_cell(int cx, int cy, int icon, int frame, bool hi);   // ovl13 +0x2250
    std::unique_ptr<Character> load_monster(int id);     // ovl17 +0x15FD
    void apply_damage(Character& c, int dmg);            // ovl15 +0x1D2C
    void take_damage(Character& c, int dmg);             // ovl23 +0x1968
    int roll_dice(int count, int sides);                 // ovl23 +0x3025
    std::uint32_t random(std::uint32_t n);               // System.Random 0BE4:1150
    bool percent(int n) { return n > static_cast<int>(random(100)); }   // ovl23 +0x3070

    // --- combat (game/combat.cpp; docs/notes/combat.md) ------------------
    void combat_op(bool solo);                           // ECL COMBAT ovl2 +0x198E / COMBAT_SOLO
    void combat_run();                                   // ovl6 +0x00EF
    void combat_end(int saved_move);                     // ovl2 0x3228A: COMBAT's tail
    void combat_setup();                                 // ovl8 +0x242A
    void field_select();                                 // ovl8 +0x16B8
    void field_ground(int env);                          // ovl8 +0x0BC3
    void field_wild(int env);                            // ovl8 +0x126F (Mars, Venus)
    void field_feature(int kind, int count);             // ovl8 +0x0F29
    bool field_free(int x, int y) const;                 // ovl8 +0x0D89
    void field_fill(int base, int range);                // ovl8 +0x0CE1
    int field_cell(int x, int y, int d) const;           // ovl8 +0x06F2
    int field_edge(int x, int y, int d) const;           // ovl8 +0x0774
    void alloc_blocks();                                 // ovl8 +0x17AF
    void place_all();                                    // ovl8 +0x2089
    void leadership();                                   // ovl11 +0x43A9
    // The combat screen (game/combat_screen.cpp).
    void load_combat_icon(const std::string& name, int id, int slot);   // ovl31 +0x0166
    void load_field_tiles(const std::string& name, int first, int count);   // ovl31 +0x0022
    void field_blit(const Bytes& px, int w, int h, int x, int y, bool masked, int key, bool mirror);
    void field_present();                                // 0267:183A
    void draw_combat_icon(int rx, int ry, int mode, int frame, int slot);   // ovl31 +0x03C9
    int field_tile_at(int x, int y) const;
    int field_underlying(int x, int y, int v) const;     // ovl30 +0x14F2
    void field_draw_tile(int rx, int ry, int mx, int my);   // ovl30 +0x00F1
    bool footprint(int size, int i, int& dx, int& dy) const;   // ovl30 +0x0000
    int icon_index(const Character* c) const;           // ovl30 +0x0DCB
    int occupant(int x, int y) const;                    // DS:6267
    bool in_view(int rx, int ry) const;                  // ovl30 +0x0883
    bool icon_visible(const Character* c, bool all) const;   // ovl30 +0x08B4
    void draw_icon_of(int k);
    bool field_scroll(int x, int y, int margin);         // ovl30 +0x096E
    void field_erase_icon(int rx, int ry, int k);        // ovl30 +0x072B
    void field_redraw_square(int x, int y);              // ovl30 +0x04D3
    void field_draw_square(int x, int y);                // ovl30 +0x0356
    void field_view(int x, int y, int margin, int dir);  // ovl30 +0x0AFC
    void focus_on(Character& c, int margin, bool cursor);   // ovl30 +0x1466
    void combat_layout();                                // 015F:01F1
    void combat_redraw();                                // ovl23 +0x2AF9
    void print_in(int l, int t, int r, int b, int fg, bool clear, const std::string& s);
    void name_message(Character& c, const std::string& msg, int row, bool flag);   // ovl23 +0x0A78
    void clear_text();                                   // ovl23 +0x0BA7
    void kill_with(Character& c, int status, const std::string& msg);   // ovl22 +0x0000
    void combat_panel(Character& c);                     // ovl23 +0x04AC
    // The fight's rounds and the player's turn (game/combat_turn.cpp).
    void count_sides();                                  // ovl23 +0x1A6A
    void tactics_round();                                // ovl11 +0x45FA
    void initiative_of(Character& c);                    // ovl11 +0x0000
    void recount_attacks(Character& c);                  // ovl11 +0x0B02
    void build_turn_list();                              // ovl6 +0x020A
    void combat_turn(Character& c);                      // ovl6 +0x0385
    void player_menu(Character& c);                      // ovl6 +0x0554
    int combat_menu(Character& c);                       // ovl6 +0x09A4
    bool can_aid(Character& c);                          // ovl18 +0x2C18
    void aid_cmd(Character& c);                          // ovl18 +0x2D90
    void intimidate(Character& c);                       // ovl11 +0x426A
    void apply_effect(Character& c, int type, int dur, int p3, int p4, bool only_if_failed, bool saved,
                      const std::string& msg);           // ovl22 +0x0B13
    bool aim(Character& actor, int range, int mode, bool allow_empty, bool fire, bool& ok, bool& chosen,
             Character*& tptr, int& rx, int& ry, int tx, int ty);   // ovl11 +0x32F0
    bool end_turn(Character& c);                         // ovl23 +0x1CAE
    void end_of_round();                                 // ovl6 +0x0CEC
    void enemy_hp_percent();                             // ovl11 +0x2CD7
    void combat_cleanup();                               // ovl6 +0x0020
    int cb_0f(const Character& c);                       // cb+0x0F, or 0000:000F without a block
    bool has_effect(const Character& c, int type) const; // ovl23 +0x18FD
    void effect_hook(Character& c, int event, const char* where);   // ovl22 +0x021D
    void effect_handler(Character& c, int type, int event, const char* where, const Bytes* node);   // DS:699D's
    void removal_handler(Character& c, const Bytes& node, const char* where);   // ovl22 0x55246 (N11)
    void effect_remove(Character& c, int type, const char* where);  // ovl22 +0x00AF
    void zero_g(Character& c);                           // ovl11 +0x47CE
    // Geometry, targets and attacks (game/combat_attack.cpp).
    int pos_x(const Character& c) const;                 // ovl30 +0x0D53
    int pos_y(const Character& c) const;                 // ovl30 +0x0D7B
    int icon_size(const Character& c) const;             // ovl30 +0x0DA3
    int dir_to(int x1, int y1, int x2, int y2) const;    // ovl30 +0x178D
    int dir_rec(const Character& a, const Character& b) const;   // ovl30 +0x199E
    bool trace(int x1, int y1, int& x2, int& y2, int& d);   // ovl29 +0x03E3
    int trace_cell(int x, int y);                        // the byte Trace reads (D-0075)
    bool cone(int ax, int ay, int tx, int ty, int d) const;  // ovl29 +0x0541
    void scan(int x, int y, int limit, int dir, int size);   // ovl29 +0x08CF
    int collect_enemies(Character& c, int range);        // ovl23 +0x1ABE
    int distance(Character& a, Character& b);            // ovl23 +0x1BDA
    Bytes* weapon_of(Character& c);                      // +0xEB
    int item_byte(int type, int k) const;                // ITEMS DS:4A34 + 9 type + k
    void note_readied(const Character&, int, const char*) {}
    int weapon_range(Character& c);                      // ovl18 +0x2B88
    Bytes* find_readied(Character& c, int cls);          // ovl18 +0x1E8C
    bool weapon_heavy(Character& c);                     // ovl23 +0x2B2F
    bool weapon_area(Character& c);                      // ovl23 +0x2B72
    bool weapon_ranged(Character& c);                    // ovl23 +0x2BA7
    bool weapon_loaded(Character& c);                    // ovl23 +0x2BEA
    bool can_fire(Character& c);                         // ovl6 +0x04F3
    void target_cmd(Character& actor, int range, int a12, bool fire, bool& done);   // ovl11 +0x3BF2
    void show_range(int d, int wr);                      // ovl11 +0x2EEB
    bool can_attack(Character& a, Character& t);         // ovl11 +0x2DEE
    void note_attacker(Character& a, Character& t);      // ovl11 +0x0F45
    void attack_command(Character& a, Character* t, bool flag, bool& done);   // ovl11 +0x2241
    void attack(Character& a, Character& t, bool flag, bool& done);           // ovl11 +0x0C32
    bool behind(Character& a, Character& t);             // ovl11 +0x41BC
    bool backstab(Character& a, Character& t);           // ovl11 +0x410C
    void ac_modifiers(Character& a, Character& t, int& ac);   // ovl11 +0x2592
    void damage_roll(Character& a, Character& t, int slot);   // ovl11 +0x0125
    void attack_result(Character& a, Character& t, int apply, int show, bool hit);   // ovl11 +0x03B5
    void face(Character& c, int dir, int frame, bool no_draw);   // ovl30 +0x0C5C
    void marker_from(int slot, int sub, int frame, bool mirror);   // 0267:2E33
    void anim_icon(int x1, int y1, int x2, int y2, int frames, int delay);   // ovl23 +0x0BE1
    void projectile(int x1, int y1, int x2, int y2, const Bytes& w);       // ovl11 +0x2B26
    void death_anim(Character& c);                       // ovl30 +0x0F97
    void clear_on_down(Character& c);                    // ovl22 +0x089D
    // The AI (game/combat_ai.cpp).
    void ai_turn(Character& c);                          // ovl7 +0x000F
    bool ai_check_key(Character& c);                     // ovl7 +0x11AE
    bool ai_idle(Character& c);                          // ovl7 +0x115E
    bool ai_morale(Character& c);                        // ovl7 +0x1281
    int fastest_enemy(Character& c);                     // ovl11 +0x2D5D
    void remove_from_combat(Character& c, int status, const std::string& msg);   // ovl22 +0x07D0
    bool flee(Character& c);                             // ovl11 +0x0A44
    bool guard_cmd(Character& c);                        // ovl11 +0x2A32
    bool pick_target(Character& c, bool allow_swap, bool& done);   // ovl7 +0x1A70
    void choose_weapon(Character& c, bool allow_area, bool& out);  // ovl7 +0x164A
    void ready_toggle(Character& owner, int i, bool& flag);        // ovl18 +0x1F12
    bool ai_act(Character& c);                           // ovl7 +0x0DA4
    bool ai_can_step(Character& c, int i, int base, bool& off);   // ovl7 +0x089D
    void ai_move_step(Character& c);                     // ovl7 +0x09BF
    bool area_target(Character& c, bool fire, int& value);         // ovl7 +0x019F
    void area_attack(Character& c, int x, int y, bool& done);     // ovl11 +0x3270
    void terrain_hazard(Character& c);                   // ovl22 +0x0436
    // The area weapon (game/combat_area.cpp).
    real48::Real random_real();                          // System.RandomReal 0BE4:1166
    void remove_item(Character& c, int i);               // ovl23 +0x0863
    void remove_item_of(Character& rec, Character& owner, int i);   // RemoveItem(rec, owner's node)
    void marker_four(int slot);                          // 0267:2FE1
    void weapon_special(Character& c, int sp);           // ovl11 +0x16B0
    void explosion(int sp, int radius);                  // ovl11 0x41BC0 (nested)
    void blast_terrain(int radius, const Character* holder, int hx, int hy, int& cx, int& cy);   // ovl11 0x41960 (nested)
    void field_set_tile(int x, int y, int t);            // ovl30 +0x1647
    void area_damage(Character& v, int dmg, int kind, bool saved);   // ovl22 +0x094A
    void effect_message(Character& c, int k, const std::string& msg);   // ovl23 +0x171A
    // The player's Move (game/combat_move.cpp).
    void step_query(const Character& c, int dir, int& occ, int& tile, bool& hazard) const;   // ovl30 +0x0E23
    bool place_at(Character& c, int x, int y);           // ovl30 +0x1272
    void combat_step(Character& c, int dir);             // ovl11 +0x081E
    void guard_check(Character& mover);                  // ovl11 +0x06A7
    void jetpack(Character& c);                          // ovl11 +0x2667
    void move_cmd(Character& c, bool& done);             // ovl6 +0x0E85
    // After a fight (game/combat_after.cpp).
    void after_combat();                                 // ovl5 +0x20D8
    void free_booty();                                   // ovl5 +0x0000
    void combat_outcome();                               // ovl5 +0x02E5
    void xp_total();                                     // ovl5 +0x005B
    void add_xp(std::int32_t xp);                        // ovl5 +0x023E
    void combat_removal();                               // ovl5 +0x1F6B
    void medical_phase();                                // ovl5 +0x1449
    void xp_report(std::int32_t xp);                     // ovl5 +0x079C
    void booty_menu();                                   // ovl5 +0x1C5A

    // --- area ------------------------------------------------------------
    std::uint8_t map_byte(int x, int y) const;           // ovl28 +0x05E3
    int wall_at(int dir, int y, int x) const;            // ovl28 +0x04E0
    void load_walldef(int id, int slot);                 // ovl28 +0x0ED1
    void refresh_map_byte();                             // ovl15 +0x1F79
    void step_forward();                                 // ovl15 +0x1A58

    // --- text window and input (0653:xxxx) -------------------------------
    int next_key(const char* what);
    int next_key_for(const char* what, int ms);   // -1 when ms ran out
    int read_key(const char* what);               // Crt.ReadKey: no clicks
    int get_key(int k);                           // GetKey's mouse part
    void print(const std::string& s, bool clear, int fg = 0x0A);   // Text.Print 0653:056C
    void clear_window();
    void press_continue(int fg = 0, int bg = 15);        // 0653:0C02 wait (fg, bg)
    void press_continue(int fg, int bg, const std::string& text);   // 0653:0C02 with its own text
    void set_window(int l, int t, int r, int b, int fill);   // 0653:115F
    void line_home();                                    // 0653:112F: the line printer to the window's top
    void print_line(int fg, const std::string& s);       // 0653:118A
    void pause_continue();                               // 0653:1291
    int menu(const std::vector<std::string>& items, bool single, bool anim = false);   // ovl15 +0x16DD
    int yes_no(const std::string& prompt = "", int highlight = 1);   // ovl25 +0x1F6D: 'Y' or 'N'
    // refused: characters the prompt doesn't take (D-0092); nullptr for
    // the original's 0x21..0x7A.
    std::string input_string(const std::string& prompt, int fg, int bg, int max,
                             int entry = kEntryLetters,
                             const char* refused = nullptr);   // 0653:07E5; entry: the pad's picker (D-0085)
    struct InputCursor { int pos; };   // InputString's column (local [bp-2A])
    std::string input_string_keys(int max, InputCursor* cursor, int entry = kEntryLetters,
                                  const char* refused = nullptr);

    // --- screen (game/screen.cpp; docs/notes/screen.md) --------------------
    struct MenuKey {
        int key = 0;        // upper-cased key, hotkey, 0 (Esc), ' ', or scan
        bool ext = false;   // an extended key (arrows, remapped digits)
    };
    void init_screen();                                   // ovl9 init: font, sets 4/6, DAC 16..31
    void draw_char(int col, int row, int fg, int bg, unsigned char ch, bool map = true);   // 0653:026B
    void draw_string(int col, int row, int fg, int bg, const std::string& s);   // 0653:0424
    void fill_cells(int col, int row, int colour, int n);  // 0653:03D9
    void clear_cells(int x1, int y1, int x2, int y2);      // 015F:029C (colour 0)
    void fill_rect(int x1, int y1, int x2, int y2, int colour);   // Text.FillRect 0653:0E58
    void border_tile(int t, int col, int row);
    void border_line(int x1, int y1, int x2, int y2, int start, int end);   // ovl33 +0x047E
    void layout_outer();                                   // 015F:00C6
    void layout_sheet();                                   // 015F:0241
    void layout_bigpic();                                  // 015F:016F
    void layout_adventure();                               // 015F:014F
    void redraw_screen();                                  // ovl23 +0x26BE
    void draw_roster() { draw_roster(cur_.detached ? nullptr : cur_.p); }   // ovl25 +0x20F9 (CurChar white)
    void draw_roster(const Character* white);
    void roster_row(const Character* c, bool white);       // ovl23 +0x02DB
    void set_roster_rect(int col);
    int roster_colour(const Character& m) const;           // ovl25 +0x22BE
    void roster_line(const Character& m, int col, int r, int fg);
    void draw_status();                                    // ovl23 +0x2820
    void draw_view_area();                                 // ovl26 +0x00A6
    void draw_view();                                      // View.Draw ovl28 +0x065E
    void show_picture(int p);                              // ovl2 0x31125 PICTURE helper
    void load_big_pic(int p);                              // LoadBigPic ovl27 +0x0CB6
    void draw_big_pic();                                   // DrawBigPic ovl27 +0x0D67
    void draw_map_cursor();                                // ovl16 +0x0000
    void restore_map_cursor();                             // ovl16 +0x00E5
    void map_step();                                       // MapStep 0x453CC
    int map_input();                                       // AdventureInput ovl12 +0x0FE4, state 3
    void load_anim(const std::string& name, int id);       // LoadAnimPicture ovl27 +0x02D0
    void sprite_step(int sprite, int pic, int dist);       // ovl15 +0x0611
    void show_sprite(int frame);                           // Show3DSprite ovl27 +0x0C2D
    int steps_ahead(int x, int y, int dir, std::uint32_t site = 0);   // ovl15 +0x057D; site: D-0071
    void pause_ms(int ms);                                 // Crt.Delay (window: a real wait)
    void game_delay();                                     // Game.Delay 0653:0E3B: var 0x4BFC x 100 ms
    void message_chars(std::size_t n);                     // message text in a fight (tests)
    void step_pace();                                      // a seen computer step's pause (D-0064)
    void flush_keys();                                     // Input.FlushKeys 08AD:03E4 (game/textwin.cpp)
    // The travel screen, state 9 (travel.cpp).
    std::uint8_t& ship(int off);                           // the ship record, var 0x4B85 + off
    void screen_tile(int col, int row, int n);             // View.DrawTile, toBuf 0
    void travel_load();                                    // ovl24 +0x0109
    void travel_free();                                    // ovl24 +0x0099
    void travel_frame();                                   // ovl24 +0x01FE
    void travel_map();                                     // ovl24 +0x0294
    void travel_ship();                                    // ovl24 +0x0000
    void draw_band();                                      // DrawPicture's state-9 band
    void rest(int n);                                      // ovl20 +0x042D
    void travel_move();                                    // TravelMove(0) ovl12 +0x0C76
    int travel_input();                                    // AdventureInput, state 9
    // The ship fight (space_combat.cpp).
    void space_combat(int a1, int a3, int a4);            // SPACE_COMBAT ovl3 +0x3600
    void load_ship_images();                               // ovl24 +0x016A
    void free_ship_images();                               // ovl24 +0x00CC
    void ship_image(int k);                                // SHIPS #(10+k) at its cell, to the screen
    void redraw_ship_screen();                             // Screen.Redraw, state 8 (ovl23 0x586CC)
    // The backbuffer DS:3C7A as a ship fight uses it: 320x200 (0x3F770),
    // drawn with DrawImage 0267:0D06 (rows 0..167 marked dirty, DS:3C92,
    // their spans DS:3E8A..3FDA) and copied by Present 0267:183A, which
    // writes only the dirty spans, at (8, 8).
    void bb_draw(const Bytes& px, int w, int h, int x, int y, bool masked);
    void bb_present();
    void backbuffer_image(int x8, int y8, const Image& im);   // DrawImage, opaque, at a cell
    void load_ship(std::uint8_t* dest, int idx);           // LoadShip ovl17 +0x26F3
    void ship_repair();                                    // ShipRepair ovl5 +0x0CFE
    void dying_round();                                    // Dying ovl23 +0x30F3
    void load_back(int id);                                // ovl28 +0x0E1C
    void set_dac(int first, int count, const std::array<Rgb, 256>& from);
    void load_palette(const Image& im);                    // into DS:908B
    void clear_row24();                                    // 0653:1359
    // MenuScreen ovl25 +0x0F15: draws prompt + items on row 0x18 and reads
    // one key (see the definition for the key rules).
    MenuKey menu_screen(const std::string& prompt, const std::string& items, bool remap, bool anim = false);
    MenuKey modify_pick(bool& done, int key);                // ovl25 +0x0B58 (MenuScreen, DS:6B77)
    MenuKey map_click(bool& done, int enter_key);          // ovl25 +0x0725 (combat's Move / Aim)
    void menu_anim_step(bool& anim, bool entry);           // MenuScreen 0x5AB67..0x5AC0F
    void draw_picture(int k);                              // DrawPicture ovl27 +0x0023 (3, 3, 1, k)
    void free_picture();                                   // ovl27 +0x0B04
    // The animated picture's state (LoadAnimPicture; MenuScreen steps it):
    // DS:6A61 the restart frame, DS:6A62 the last frame, DS:6A63 the
    // current one (written only by MenuScreen: it carries over between
    // pictures), DS:6A45 + k each frame's time in tenths.
    int anim_loop_ = 0, anim_last_ = 0, anim_cur_ = 0;
    std::vector<int> anim_delays_;
    // MenuScreen's clock (SS:[BP-0x204]): set on entry and at each frame
    // change; -1 where no time passes (headless).
    long anim_t0_ = -1;
    long anim_elapsed() const;   // centiseconds since anim_t0_, 0 headless
    int menu_anim_wait() const;  // ms until the next frame, -1 for none
    // MenuScreen's timeout (DS:6B1F, a dword of centiseconds; DS:6B23 the
    // key it answers): only the title sets one (120 s, 30 s after a
    // demo; main 0x10125, 0x10237), measured from anim_t0_.
    long menu_timeout_cs_ = 0;
    int menu_timeout_key_ = 0;
    int menu_timeout_wait() const;   // ms until the timeout, -1 for none
    void draw_menu_line(const std::string& prompt, const std::string& items);

    // --- program and team menu (game/frontend.cpp) -----------------------------
    struct ListNode {
        std::string str;      // +0x00 string[40]
        bool header = false;  // +0x29: a heading line, not selectable
    };
    void game_init();                                      // ovl9 +0x0101 (game state)
    void new_game_defaults();                              // ovl9 +0x0000
    void game_reset();                                     // ovl9 +0x0919
    void intro();                                          // ovl1 +0x0640
    void music_start();                                    // ovl1 +0x00A5: the title's music
    void sound(int n);                                     // Sound.Play 07DA:0000
    void sfx_command(int n);                               // SfxCommand 0906:0000
    void ctrl_key(int k);                                  // GetKey's ^S and ^U
    void dax_cut();                                        // Dax.Load's SoundSuspend / SoundResume
    bool wait_seconds(int n);                              // ovl1 +0x0000
    Image load_title(int id);                              // Gfx.LoadImage("title", id)
    void layout_state0();                                  // 015F:00D8
    void begin_run_loop();                                 // Ecl.RunLoop ovl2 +0x3209
    void create_character();                               // ovl13 +0x06FE (game/chargen.cpp)
    void icon_select();                                    // ovl13 +0x25BB (game/icons.cpp)
    // ViewCharacter ovl18 +0x0F75 (game/sheet.cpp) with the caller's exit
    // flag: Gear sets it when a heavy weapon readied in combat ends the turn.
    void view_character(std::uint8_t& exit_flag);
    void view_character() {
        std::uint8_t flag = 0;
        view_character(flag);
    }
    // --- VIEW's Gear and Trade (game/gear.cpp) -----------------------------------
    void view_gear(std::uint8_t& exit_flag);               // ovl18 +0x1968
    bool can_modify(const Bytes& item);                    // ovl18 +0x1191
    void gear_trade(Character& owner, int i);              // ovl18 +0x20C4
    void halve(Character& c, int i);                       // ovl18 +0x21FD
    void sell(Character& owner, int i);                    // ovl18 +0x2594
    void view_trade();                                     // ovl18 +0x283C
    void take_booty();                                     // ovl5 +0x0B3F
    void take_credits();                                   // ovl21 +0x0416
    void pool_credits();                                   // Pool ovl21 +0x0205
    void divvy_credits();                                  // Divvy ovl21 +0x02C4
    // --- the shop (game/shop.cpp) ------------------------------------------------
    void op_shop(int n);                                   // SHOP n, ovl2 +0x2BD4
    void shop_menu();                                      // ovl14 +0x0F0C
    std::int32_t shop_price(const Bytes& item);            // ovl14 +0x00D1
    void shop_funds();                                     // ovl14 +0x0020
    int shop_buy_list(int& index, int& picked, bool& redraw);   // ovl14 +0x012E
    void shop_buy();                                       // ovl14 +0x04C9
    void shop_ammo_calc(const Bytes& w, int key, int& max, int& amount, int& price) const;   // ovl14 +0x06E9
    void shop_ammo();                                      // ovl14 +0x089C
    std::uint8_t f82e8_ = 1;         // DS:82E8: Redraw in state 1 draws the layout (0 for the shop's first)
    std::int32_t input_credits(std::int32_t max, const std::string& prompt, int fg);   // ovl21 +0x0000
    void take_gear();                                      // ovl5 +0x09EF
    bool give_item(Character& c, const Bytes& item);       // ovl14 +0x02F7
    void merge_stack(Character& c, std::size_t i);         // ovl18 +0x22C9
    void add_character();                                  // ovl13 +0x1A15
    void load_member_icon();                               // ovl17 +0x065F
    void remove_from_team();                               // TeamMenu 'R' (0x46D56)
    void show_message(const std::string& s);               // ovl23 +0x0A18
    void row_message(const std::string& s, int fg, int bg);   // 0653:0DE1
    void begin_state();              // BEGIN's state (0x46DEA; D-0077)
    void team_menu();                                      // ovl13 +0x019E
    int list_menu(const std::string& prompt, const std::string& select, int x1, int y1, int x2, int y2,
                  std::vector<ListNode>& list, bool exit_item, bool& redraw, int& index, int& picked);   // ovl25 +0x1AF3
    void list_draw_page(const std::vector<ListNode>& list, int x1, int y1, int x2, int y2);
    void list_highlight(const std::vector<ListNode>& list, int i, int x1, int y1, bool on);
    void list_move(const std::vector<ListNode>& list, int& index, int rows, bool down);
    void list_skip_headers(const std::vector<ListNode>& list, int& index, int rows, bool down);
    bool pick_character();                                 // ovl18 +0x2ADB
    void load_menu();                                      // ovl17 +0x1AC8 (the slot menu)
    bool save_game_menu();                                 // ovl17 +0x220C (the slot menu)
    bool save_dir_ready();                                 // ovl17 +0x0448

    // --- adventure (game/adventure.cpp) ---------------------------------------
    int adventure_input();                                 // ovl12 +0x0FE4
    void select_char(int scan);                            // ovl18 +0x299F
    void select_member(const std::string& prompt) {      // PICK_CHARACTER: &CurChar (D-0055: a member chosen ends a detached CurChar)
        select_member(prompt, false, false, cur_.p, true);
    }
    void select_member(const std::string& prompt, bool exit, bool alt, Character*& var, bool view);   // ovl18 +0x2F14
    bool passable(int x, int y, int f) const;              // ovl28 +0x03B1
    void edge_test();                                      // ovl12 +0x0000
    void step_party();                                     // ovl12 +0x00A9
    void pass_time(int n, int unit);                       // ovl20 +0x0388
    void timed_effects(int n, int unit);                   // ovl20 +0x0000
    void save_menu();                                      // ovl12 0x461DF
    void run_loop();                                       // Ecl.RunLoop main loop

    // --- script ----------------------------------------------------------
    void load_block(int id);                             // LoadEcl ovl15 +0x042A
    void ecl_init();                                     // EclInit ovl15 +0x02DB
    void run_ecl(std::uint16_t addr);                    // RunEcl ovl2 +0x30CB
    void enter_block();                                  // ovl2 +0x3117
    void step();                                         // one instruction
    void ecl_exit();                                     // EXIT (ovl2 0x30850); also RETURN with no GOSUB

    std::string save_dir_;          // DS:489A (BUCK.CFG's save path)
    Bytes name_table_ = Bytes(0x148, 0);   // SAVGAMx.DAT 0xA0D.. as loaded (D-0011)
    std::uint8_t ds3bec_ = 0, ds3bee_ = 0; // from/to var 0x4BFF
    std::uint8_t saved_ = 0;        // DS:499B "game saved"
    void delete_roster_files(const Character& c);   // 0x4DDD2
    void char_save(Character& c, const std::string& base, bool last, const char* ext = ".SAV");   // 0x4DF60
    bool char_save_roster(Character& c, bool last);   // CharSave(c, '') with its prompts
    std::unique_ptr<Character> char_load(const std::string& base, const char* ext);   // 0x4E6D9
    std::string make_file_name(const std::string& name) const;   // 0800:0647 of StripChar(name, '.')
    bool save_file_exists(const std::string& name) const;
    std::string save_path(const std::string& name) const;
    const GameFiles& files_;
    GameText text_;              // the original's UI strings (core/gametext.hpp)
    std::map<std::string, std::unique_ptr<DaxFile>> dax_cache_;
    Bytes items_table_;          // ITEMS: 9-byte records at DS:4A34
    Bytes item0_;                // ITEM0.DAX #1: 16-byte records, DS:4EB9 + 16*type (type 1 first)
    void check_item0() const;    // rejects an entry WeaponSpecialize can't run on (D-0090)
    // ITEM0 record of `type` (1..), or nullptr when the file has none.
    const std::uint8_t* item_type(std::size_t type) const {
        return type >= 1 && 16 * type <= item0_.size() ? &item0_[16 * (type - 1)] : nullptr;
    }

    // Script variables and memory.
    Bytes vars0_ = Bytes(0x400, 0);   // [DS:3BC8], vars 0x4B00..0x4EFF
    Bytes vars1_ = Bytes(0x400, 0);   // [DS:3BCC], vars 0x7C00..0x7FFF
    Bytes vars2_ = Bytes(0x200, 0);   // [DS:3BD0], vars 0x7A00..0x7BFF
    std::unique_ptr<ecl::Script> script_;   // [DS:3BD4], vars 0x8000..
    std::array<std::uint16_t, 5> entries_{};   // DS:3BD8..3BE0
    std::uint16_t pc_ = 0;                  // DS:3BE2
    std::uint16_t op_at_ = 0;               // the running instruction's address (diagnostics)
    std::uint64_t steps_unread_ = 0;        // ECL steps since the game last read, polled or waited for a key (D-0079)
    static constexpr std::uint64_t kStepsUnread = 1000000;
    std::set<std::string> validated_;       // blocks that passed load_block's checks (name and bytes)
    std::uint8_t op_ = 0, prev_op_ = 0;     // DS:6D13, DS:6D12
    std::vector<std::uint16_t> gosub_;      // list at DS:6D04
    std::array<std::uint8_t, 6> flags_{};   // DS:6D0C..6D11: EQ NE LT GT LE GE
    std::uint16_t loop_pc_ = 0;             // DS:8306
    std::vector<std::string> str_slots_;    // DS:6DDC + 0x100*j

    // Run-state flags.
    std::uint8_t stop_ = 0;          // DS:357E
    std::uint8_t new_block_ = 0;     // DS:357F
    std::uint8_t party_lost_ = 0;    // DS:3BF6
    std::uint8_t loaded_game_ = 0;   // DS:3BF1
    std::uint8_t file_prev_mode_ = 4;   // the loaded file's +0xA06 (DS:3BE9 when saved): D-0077's state
    std::uint8_t set_char_ = 0;      // DS:3580
    std::array<std::uint8_t, 3> area_flags_{};   // DS:3581..3583
    std::uint8_t no_char_ = 0;       // DS:8300
    std::uint8_t first_area_ = 0;    // DS:8301
    std::uint8_t f82e6_ = 0, f82e7_ = 0, f82f6_ = 0, f82f7_ = 0, f82f8_ = 0, f82f9_ = 0,
                 f82fb_ = 0, f82fc_ = 0, f82fd_ = 0, f82fe_ = 0, f3bed_ = 0;
    std::uint8_t c6cfc_ = 0, c6cfd_ = 0;   // DS:6CFC, DS:6CFD
    std::uint8_t ecl_file_ = 1;      // DS:4898
    std::uint8_t ecl_file_prev_ = 1; // DS:4899 (0800:0AAE puts it back)
    std::uint8_t block_id_ = 0;      // DS:82F4
    int traced_file_ = 0;            // tools only: the ECL file the running block came from

    // Area.
    std::int8_t x_ = 0, y_ = 0;      // DS:6A2F, 6A30
    std::uint8_t facing_ = 0;        // DS:6A31
    std::uint8_t wall_ahead_ = 0;    // DS:6A32
    std::uint8_t map_byte_ = 0;      // DS:6A33
    std::uint8_t mode_ = 0, prev_mode_ = 0;   // DS:3BE8, DS:3BE9
    std::uint8_t map_mode_ = 0;      // DS:6B15
    std::array<std::uint16_t, 8> walls_table_{};   // DS:6A34: (id, slot) x 4
    Bytes geo_ = Bytes(0x400, 0);    // [DS:6A26]
    Bytes walls_ = Bytes(4 * 0x30C, 0);   // [DS:6A22]
    int picture_ = -1;               // last PICTURE / SET_MODE picture
    // Screen state.
    Image font_;                     // 8X8D1 #201 (DS:5F32)
    std::array<std::unique_ptr<Image>, 7> tile_sets_;   // DS:5F5A + 4k
    std::unique_ptr<Image> back_far_, back_near_;       // DS:8486 (id + 0x80), DS:8482 (id)
    std::unique_ptr<Image> event_pic_;                   // the PIC on screen
    // LoadAnimPicture ovl27 +0x02D0: the last load (DS:6AFC name, DS:6B0E
    // id, DS:6A44 "a sprite"), and a sprite's frames DS:6A8C[] as drawn
    // (sprite_view: key 0x0D).
    std::string anim_name_;
    int anim_id_ = -1;
    bool anim_sprite_ = false;
    std::vector<Bytes> sprite_frames_;
    // The view square of the backbuffer DS:3C7A (screen (24,24), 88x88):
    // View.Draw leaves its render there; Show3DSprite draws into it.
    Bytes view_buf_ = Bytes(88 * 88, 0);
    std::uint8_t sprite_id_ = 0;      // DS:6D15
    std::uint8_t sprite_pic_ = 0xFF;  // DS:6D16: the PIC at distance 0
    std::uint8_t approach_mask_ = 0;  // DS:6BD6: for COMBAT's placement
    std::uint8_t f82dc_ = 0;          // DS:82DC: a sprite is loaded
    std::uint8_t f82dd_ = 0;          // DS:82DD: a picture is shown
    std::array<Rgb, 256> pal_908b_{}, pal_8d8b_{}, pal_878b_{};   // palette buffers
    std::uint8_t f959b_ = 0;         // DS:959B: view palette pending
    std::uint8_t f6b70_ = 0;         // DS:6B70
    std::uint8_t area_map_ = 0;      // DS:6A2E: overhead map instead of the view
    std::uint8_t move_mode_ = 0;     // DS:8303
    std::uint8_t f82fa_ = 0;         // DS:82FA: text window cleared since the last print
    int menu_prompt_fg_ = 13, menu_text_fg_ = 10, menu_hot_fg_ = 15;   // DS:6B45, 6B46, 6B47

    // Party and encounters.
    std::vector<std::unique_ptr<Character>> party_;   // list at DS:4EC5
    // DS:4EC1. D-0055 (session 55, run s55_blind_s01): where CurChar is
    // restored from a DS:4A30 that names a record TeamMenu removed, the
    // original makes the freed block CurChar and the roster whitens nobody
    // until CurChar is next set; the engine keeps CurChar for every other
    // reader, marked detached, and any assignment ends it.
    struct CurChar {
        Character* p = nullptr;
        bool detached = false;
        CurChar& operator=(Character* c) {
            p = c;
            detached = false;
            return *this;
        }
        operator Character*() const { return p; }
        Character* operator->() const { return p; }
        Character& operator*() const { return *p; }
    };
    CurChar cur_;
    Character* saved_cur_ = nullptr; // DS:4A30
    // D-0074: ReadyToggle (0x51F74) after a trade's roster click puts the
    // list owner's node in CurChar's weapon / armour slot ([0] / [1]); it
    // stays until the holder's next Recalc clears both slots (0x572AF).
    // The port finds the node again in the owner's list (indices shift when
    // other items go); a readied node can't be dropped, traded or sold
    // (CanModify), so a copy of it finds it (inference).
    struct ForeignSlot { Character* holder = nullptr; Character* owner = nullptr; Bytes item; };
    ForeignSlot foreign_slot_[2];
    Bytes& foreign_node(ForeignSlot& f);
    // CurChar := DS:4A30, unless TeamMenu's REMOVE or PURGE removed that
    // record since (D-0055): then CurChar stays.
    void restore_cur();                                  // CurChar := DS:4A30 (D-0055)
    void rest_menu();                // TEAM_MENU 9: ovl12 +0x15D8
    std::uint8_t combatants_ = 0;    // DS:3584
    std::uint8_t icon_next_ = 8;     // DS:6D17
    // Combat.
    std::array<std::uint8_t, kFieldW * kFieldH> field_{};   // [DS:68A2]+7
    int field_ox_ = 0, field_oy_ = 0;                        // [DS:68A2]+2/+3
    // The wild battlefield's feature centre and forced radius (DS:37BA,
    // DS:37BB, DS:37BC; written and read only by ovl8 +0x0F29 / +0x126F).
    std::uint8_t wild_cx_ = 0, wild_cy_ = 0, wild_radius_ = 0;
    std::vector<Icon> icons_;                                // DS:60B3 + 4k, k = 1..
    std::vector<combat::Call> field_calls_;                  // the generator's calls
    struct IconFrame {
        int w = 0, h = 0;
        Bytes px;              // empty: the original's nil pointer (nothing drawn)
    };
    struct CombatIcon {
        bool loaded = false;
        IconFrame ready, action;   // DS:5F86 + 8 slot (+0: id, +4: id + 0x80), each its own image
    };
    std::array<CombatIcon, 0x40> combat_icons_{};
    std::array<Rgb, 256> pal_8a8b_{};         // DS:8A8B: the combat palette
    std::vector<Bytes> field_tiles_;          // DS:5F82: the battlefield tiles (0x3B frames)
    std::vector<bool> field_past_;            // the slots LoadTiles filled past the file's frames (D-0063)
    std::string field_past_file_;
    static constexpr int kViewPx = 168;       // the backbuffer DS:3C7A in combat
    Bytes field_buf_ = Bytes(kViewPx * kViewPx, 0);
    bool cursor_on_ = false;                  // [DS:68A2]+4
    int cursor_size_ = 1;                     // [DS:68A2]+5
    std::uint8_t f6bda_ = 0;                  // DS:6BDA: draw this turn's combatant
    std::uint8_t f6bd9_ = 0;                  // DS:6BD9: the panel is due
    struct Body {
        Character* rec = nullptr;
        int x = 0, y = 0, old = 0;   // DS:6862 + 7n: rec, x, y, the tile before
    };
    std::vector<Body> bodies_;       // DS:6862, count DS:68A1
    struct AreaFx {                  // DS:6B4F's nodes (0x1A bytes), newest first
        int x = 0, y = 0;            // +0x16 / +0x17: the centre
        int dur = 0, tile = 0;       // +0x18 rounds, +0x19 the tile it lays (1, 2, 4)
        std::array<int, 10> under{}; // +0x03 + i: the tile under cell i (1..9)
        std::array<bool, 10> on{};   // +0x0C + i: cell i laid
        int heap = -1;               // at a below_site(): its offset from the field block's end
    };
    std::vector<AreaFx> areas_;
    // D-0075 below the field where a fight's field reuses a free heap block
    // (session 71): the nodes' place in the block's rest, TP 5.5's free list.
    struct BelowSite { int file, block, pc; int field_lin; const char* run; };
    const BelowSite* below_site() const;
    bool below_on_ = false;                          // the model set up for this fight
    std::vector<std::pair<int, int>> below_free_;    // free entries [from, to), the newest first
    std::vector<int> below_img_;                     // a freed node's bytes as left (-1: not known)
    void below_start();
    int below_alloc();
    void below_free(const AreaFx& a);
    int area_node_byte(std::size_t n, int k) const;  // byte k of areas_[n]'s node (0x1A bytes)
    void area_cell(const AreaFx& a, int i, int& x, int& y) const;   // DS:303B, DS:257C / 2585
    void area_age();                                     // ovl22 +0x0CC3
    void area_reapply();                                 // ovl22 0x55C4C
    std::uint8_t round_ = 0, round_limit_ = 15;   // DS:6B7D, DS:6B7E
    std::uint8_t f82ea_ = 0;         // DS:82EA: COMBAT_SOLO
    std::uint8_t f82f1_ = 0;         // DS:82F1: a fight took place (after combat's gate)
    std::uint8_t f82f2_ = 0;         // DS:82F2: a ship fight's repairs are due
    std::uint8_t f6b14_ = 0xFF;      // DS:6B14: the big picture of state 3 (COMBAT's tail)
    std::unique_ptr<Image> big_pic_;   // DS:6B10 holding a BIGPIC (LoadBigPic)
    std::array<Rgb, 256> pal_848b_{};  // DS:848B: the big picture's palette 0x20..0xFF
    std::unique_ptr<Image> cursor_img_;   // DS:8413: CURSOR #1, the map cursor
    Bytes cursor_under_ = Bytes(64, 0);  // DS:5F7A: the 8x8 cell under the cursor
    std::uint8_t f4a24_ = 0;         // DS:4A24: the travel images are loaded
    std::uint8_t f4a25_ = 0;         // DS:4A25: the ROCKET images are loaded
    std::unique_ptr<Image> ships_bg_;   // DS:6B10: SHIPS #128, the travel map
    Bytes ship_under_;               // DS:4A2A: the 8x8 cell under the ship
    int ship_x_ = -1, ship_y_ = -1;  // DS:4A2E/4A2F: its square (x < 0: none)
    std::vector<Image> ship_images_;   // DS:499C: SHIPS #10..#38 (ship fights)
    Bytes bb_ = Bytes(320 * 200, 0);   // DS:3C7A (ship fights)
    std::array<int, 168> bb_x0_{}, bb_x1_{};   // DS:3E8A / DS:3FDA: a row's dirty span
    std::array<bool, 168> bb_dirty_{};         // DS:3C92
    Bytes enemy_ship_;               // DS:4A14: the enemy's record (empty: nil)
    Character* pilot_ = nullptr;     // DS:4A18
    std::uint8_t f4a1f_ = 8;         // DS:4A1F: the range shown (signed)
    std::uint8_t f4a20_ = 0;         // DS:4A20: the enemy is retreating
    std::uint8_t f4a21_ = 0;         // DS:4A21: ShipStatus shows the enemy's details
    std::uint8_t f4a22_ = 0, f4a23_ = 0;   // DS:4A22/4A23: speed boosts, party / enemy
    std::uint16_t xp_5ea9_ = 0;      // DS:5EA9: the ship fight's XP
    std::uint8_t f82eb_ = 0;         // DS:82EB: the party's inactive members after a fight
    std::uint8_t f82f0_ = 0;         // DS:82F0: a party member is Okay after a fight
    std::uint8_t f3586_ = 0;         // DS:3586: enemies were defeated or removed
    std::uint8_t f3587_ = 0;         // DS:3587: the party fled
    std::int32_t xp_share_ = 0;      // DS:82EC: each member's XP from the fight
    std::array<std::uint8_t, 2> side_count_{};   // DS:6988 / 6989: active records per side
    std::array<std::uint8_t, 2> tactics_won_{};  // DS:6BD3 / 6BD4: Battle Tactics this round
    std::array<Character*, 0x49> turn_list_{};   // DS:6BD7 + 4k, k = 1..0x48
    std::uint8_t turn_idx_ = 0;                  // DS:6CFB
    struct ScanEntry {
        int k = 0, cnt = 0, dir = 0;   // DS:68A4 + 3i: icon, trace count, octant
    };
    std::array<ScanEntry, 0x49> scan_{};         // entries 1..0x48
    int scan_n_ = 0;                             // DS:68A6
    bool field_open_ = false;                    // [DS:68A2]+6: terrain doesn't block traces
    std::vector<Character*> aid_list_;           // DS:387F + n: AidAvailable's list
    std::uint8_t f699a_ = 0;                     // DS:699A: morale's last value (MoveStep reads it)
    std::uint8_t f358a_ = 8, f358b_ = 0;         // DS:358A / 358B: MoveStep's last direction, turn-backs
    std::uint8_t f6bd8_ = 0;                     // DS:6BD8: the AI attacks this iteration
    std::uint8_t area_x_ = 0, area_y_ = 0;       // DS:6996 / 6997: the area attack's square
    std::uint8_t f698d_ = 0;                     // DS:698D: EffectMessage's image for the area damage
    std::uint8_t f6993_ = 0;                     // DS:6993: a weapon special took the attack
    combat::LowMem low_;                         // 0000:0000..03FF as nil pointers reach it
    std::uint8_t f698b_ = 0;                     // DS:698B: the effect type being applied
    std::uint8_t f698e_ = 0;                     // DS:698E: the attack rate being computed
    std::array<std::uint8_t, 0x49> f388d_{};     // DS:388D + i: record i has a timed effect
    // DS:60B3/60B4 + 4k as the last fight left them: Place clears only the
    // sizes, so an entry Place doesn't write keeps an earlier square.
    std::array<std::pair<int, int>, 0x100> icon_xy_{};
    std::array<std::uint8_t, 4> trace_flags_{};  // DS:38D6..38D9
    std::uint8_t behind_ = 0;                    // DS:6BD5: 1 behind, 2 backstab, 3 helpless
    std::uint8_t dmg_ = 0;                       // DS:698C
    std::array<std::uint8_t, 3> slot_attacks_{}, slot_hits_{};   // DS:6BC6 + slot, DS:6997 + slot
    std::uint8_t enemy_hp_pct_ = 0;              // DS:6BC9
    struct Marker {
        int w = 0, h = 0;
        std::array<Bytes, 4> frames;   // DS:6BCB: 24 x 24, colour key 8
    };
    Marker marker_;
    std::int32_t loot_value_ = 0;    // DS:6980
    std::vector<Bytes> loot_;        // list at DS:6984

    // Text window: cursor DS:5F3E/5F3F, rectangle DS:5F40..43, fill DS:5F44.
    // DS:5F3E/5F3F: zero (the data segment's initial value) until text is
    // printed: still 0, 0 at the team menu after LOAD (run s18_dsaves_mem,
    // MEM0011), so a first message placed at column 0x26 (ApplyDamage)
    // starts at the window's (left, top), leading spaces kept (Print's
    // reset, 0x16AD7).
    int col_ = 0, row_ = 0;
    int lp_col_ = 0, lp_row_ = 0;    // DS:4A1C / 4A1D: the line printer's position
    bool lp_used_ = false;           // DS:4A1E: a line printed since the last clear
    int left_ = 1, top_ = 0x11, right_ = 0x26, bottom_ = 0x16, fill_ = 0;
    // What was printed and read, for the developer build's dumps; a
    // player's build keeps none (goal 81: it grew through a session).
    void note_transcript([[maybe_unused]] std::string line) {
    }
    std::uint8_t f487c_ = 2;          // DS:487C: the sound line (the port's AdLib, 2; 5 after ^S)
    std::uint8_t f487d_ = 2;          // DS:487D: ^S's saved line
    std::uint8_t f487e_ = 1;          // DS:487E: ^U's flag (ovl9 +0x07C6)
    bool sfx_installed_ = false;      // DS:4895 = 1: the effect unit installed
    std::uint8_t highlight_ = 1;     // DS:6B1D
    std::uint8_t menu_sum_ = 0xFF;   // DS:6B48: byte sum of the last menu's items
    int list_top_ = 0;               // DS:6B1B: ListMenu's first visible line (shared)
    // The mouse (docs/notes/mouse.md). The DS bytes are BSS: 0 at start.
    bool mouse_on_ = true;           // DS:8434: a driver (BUCK.CFG 'M'); always on (D-0019, D-0044)
    std::uint8_t f8435_ = 0;         // DS:8435: GetKey's last key was a button
    int mouse_x_ = 160, mouse_y_ = 100;   // DS:8437 / 8439: the pointer, game pixels (Game.Init)
    std::uint8_t f8433_ = 0;         // DS:8433: roster clicks allowed
    std::uint8_t roster_c0_ = 0, roster_c1_ = 0, roster_r0_ = 0, roster_r1_ = 0;   // DS:6B3F..6B42
    std::int8_t list_c0_ = 0, list_c1_ = 0;   // DS:6B3D / 6B3E: ListMenu's columns
    std::array<std::uint8_t, 26> list_rows_{};   // DS:6B23 + k: the k-th line's row (0xFF: heading)
    std::uint8_t f6b4b_ = 0;         // DS:6B4B: inside ListMenu
    std::uint8_t f6b4c_ = 0;         // DS:6B4C: the team menu's roster click on CurChar
    std::uint8_t f6b74_ = 0;         // DS:6B74: IconSelect's grid
    std::uint8_t f6bd7_ = 0, f6bd2_ = 0;   // DS:6BD7 / 6BD2: combat's Move / Aim
    std::uint8_t f6b16_ = 0;         // DS:6B16: SelectMember's alternate mode
    Character* f6b17_ = nullptr;     // DS:6B17: its member
    // DS:387C: the last trade partner (VIEW sets it to CurChar). At start-up
    // it holds 3A25:3525 (all 12 memory dumps; packed START.EXE's bytes),
    // not a record: the engine names that value by a record of its own
    // that is never in the party (D-0056).
    Character startup_partner_;
    Character* f387c_ = &startup_partner_;
    // What SelectMember's Down reads at 3A25:3525 +0xFF: the overlay
    // buffer's code bytes taken as a pointer, no record (run s60_c29_dump:
    // FEFC:BE8D). Its own +0xFF read nil there (D-0056).
    Character stray_partner_;
    // DS:387C left on a record REMOVE freed (session 69, run
    // s69_d0056_down): RemoveChar frees it and DS:387C keeps the pointer;
    // the freed record's +0xFF still held its successor there (ACE removed,
    // DOC at its +0xFF; its first bytes reused), so Down gives that
    // successor (nil: the party's first). removed_next_ is the successor at
    // the removal; nil after the party's end (Game.Reset: measured, run
    // s80_d0056_after_wipe); a LOAD maps DS:387C to the loaded record at
    // the freed one's position (save.cpp, run s80_d0056_after_load).
    Character removed_partner_;
    Character* removed_next_ = nullptr;
    // D-0055 (session 80): the record DS:4A30 names when TeamMenu frees it:
    // its successor at the removal (its +0xFF, what SelectMember's Down
    // reads), the sentinel PICK_CHARACTER starts from while CurChar is
    // detached, and the name a detached CurChar reads (the team menu's first
    // item: the list's first node in the freed block, run s80_detach_pick_enter).
    Character* detached_next_ = nullptr;
    // D-0056 (session 80): every record REMOVE freed since the last load,
    // by its old address: its successor at the removal (its +0xFF), so a
    // Down through freed records follows them (run s80_d0056_succ_gone);
    // and the list position DS:387C's freed record had, which a LOAD's
    // first-fit allocation gives to the loaded record there (runs
    // s80_d0056_after_load, s80_d0056_load_pos3: positions 1 and 3).
    std::map<const Character*, Character*> freed_next_;
    std::size_t removed_pos_ = 0;
    Character freed_cur_;
    std::string detached_name_;
    std::uint8_t f6b1e_ = 0;         // DS:6B1E: the last MenuScreen's key was extended
    std::uint8_t demo_ = 0;          // DS:3BEA: the title's DEMO runs
    std::optional<Bytes> hitek_;     // HITEK.DAX #1, decoded: D-0075
    std::uint8_t typing_ = 1;        // DS:3BF8: Text.Print types (Crt.Delay(var 0x4BFC * 3) per character)
    // Message characters drawn in a fight since the last message wait,
    // and every message wait (tests).
    int msg_chars_ = 0;
    std::vector<std::pair<int, int>> message_waits_;
    bool options_up_ = false;                                // the options screen is up (D-0065)
    int options_key_ = 0;   // a game key that opens it instead (CHANGE's M in play), 0 none
    bool is_options_key(int k) const {
        return k == kOptionsKey || (options_key_ && k > 0 && k < 0x100 && std::toupper(k) == options_key_);
    }
    std::vector<int> step_waits_;                            // step_pace's (tests)
    std::uint8_t f6b43_ = 0xFF;      // DS:6B43: the field ModifyPick hit
    std::uint8_t f6b77_ = 0;         // DS:6B77: MODIFY CHARACTER runs (MenuScreen's ModifyPick)
    const Character* prev_cur_ = nullptr;   // DS:4EBD: CurChar before a roster click
    int aim_x_ = 0, aim_y_ = 0;      // DS:6BD0 / 6BD1: Aim's cursor
    int row_click_ = 0, col_click_ = 0;   // MenuScreen's [bp-0x210] / [bp-0x20E]: the clicked cell
    std::uint8_t f6b44_ = 0;         // DS:6B44: the skill row clicked (0xFF: none)
    std::array<int, 93> skill_list_{};   // DS:3BF9 + i: the skills by page
    std::array<int, 7> skill_first_{}, skill_last_{};   // DS:3C4E, DS:3C55
    std::int8_t open_general_ = 0;   // DS:388B
    bool skill_view_ = false;        // DS:388C: AllocateSkills(0,0,0)
    std::uint8_t train_done_ = 0;    // DS:6B75: TrainCharacter stopped
    std::vector<Bytes> icon_ready_, icon_action_;   // CHARS #i, #0x80+i
    std::unique_ptr<Bytes> icon_backdrop_;          // COMSPR #20
    Fixes fixes_;
    std::unique_ptr<LogbookText> logbook_text_;
    void logbook_print(int entry);
    void draw_logbook_map(const LogbookMap& m);
    std::vector<int> logbook_shown_;
    bool printing_logbook_ = false;   // logbook_print's text: D-0089's break says nothing (D-0083 checks the file)
    bool passcodes_ = false;
    // Ctrl+D (DS:3BF0, D-0091): debug.txt (DS:8312) beside the save folder.
    std::uint8_t debug_ = 0;
    std::ofstream debug_file_;
    void toggle_debug();
    void debug_show(const std::string& caption, int a, int b, int c);   // ovl32 +0x0004
    void debug_slot(int slot);                                          // ovl33 0x60DE6
    std::optional<long long> free_bytes_;    // tools only: the disk's free space (--free-bytes)
    bool room_to_save(char slot) const;      // SaveGame 0x4F9CF..0x4FA15
    int loads_ = 0;                  // LOAD SAVED GAMEs so far (port-only: D-0081)
    // Port-only (D-0081): a LOAD in a script's TEAM_MENU and no block loaded
    // since, with the state that LOAD and its BEGIN left.
    bool script_menu_load_ = false;
    std::uint8_t script_load_block_ = 0, script_load_mode_ = 0, script_load_prev_mode_ = 0;
    std::array<std::uint8_t, 13> team_enable_{};   // DS:0A49 + 0x2A*i

    bool load_checks_ = true;
    Input* input_ = nullptr;
    Framebuffer screen_;             // the mode-13h screen
    std::uint32_t seed_ = 0;         // RandSeed DS:3572
};

}  // namespace cotd::game
