// SPDX-License-Identifier: GPL-3.0-or-later
// Combat rules as pure functions (docs/notes/combat.md): no screen, no
// combat state beyond what each rule reads. Every Random call is logged
// with the site the RNG trace names for it (tools/rng_log.py: the return
// address of the RollDice call, "ovlN +0xOFF"), so a rule test can match a
// recorded fight's trace call for call (tests/oracle/combat.toml).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd::game::combat {

class LowMem;

struct Call {
    std::string site;   // the rule's RollDice/Random call site
    int range = 0;      // Random's argument
    int result = 0;     // Random's result (0..range-1)
};

inline void rng_note(std::uint32_t, int, int) {}

// TP 5.5 Random (System 0BE4:1150) and Game.RollDice (ovl23 +0x3025) with
// a log of every Random call.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : seed_(seed) {}
    std::uint32_t seed() const { return seed_; }
    int random(int n, const std::string& site);
    int roll_dice(int count, int sides, const std::string& site);
    const std::vector<Call>& calls() const { return calls_; }

private:
    std::uint32_t seed_;
    std::vector<Call> calls_;
};

// Initiative, ovl11 +0x0000 (0x40900), for one record. dex_adj is
// DS:0CB9[DEX] (DEX = record +0x11); surprise is var 0x7ECB. An inactive
// record (+0xD5 = 0) gets cb+1 = 0 and no RNG.
struct Initiative {
    int cb1 = 0;   // "has a turn": the initiative value
    int cb5 = 0;   // tie-break
};
Initiative initiative(const Bytes& rec, int dex_adj, int surprise, Rng& rng);

// The to-hit modifier AttackRoll adds (ovl22 +0x0583, 0x5562D..0x5566B):
// var 0x7F70 for a monster attacker (+0xD6 != 0); for the party var 0x7F71
// plus +2/+1/0/-1/-2 for the LEVEL setting var 0x4BB6 = 0..4.
int attack_modifier(const Bytes& attacker, int var7f70, int var7f71, int level);

// AttackRoll, ovl22 +0x0583 (0x555D3): d20 (1 misses, 20 counts as 100);
// hit if roll + attacker +0xD8 + modifier >= ac_value.
struct AttackRoll {
    int roll = 0;
    bool hit = false;
    bool hook10 = false;   // the attacker's hook 10 ran (every roll but a 1)
};
// effect_0d: the attacker has effect 0x0D; hook 10 (ovl22 +0x021D ->
// ovl10 +0x00BB) then takes 4 off the roll (and off the attacker's AC
// bytes +0xD9/+0xDA, not returned here), and a roll below 0 misses
// (0x55690). The target's hook 0x10 has no handler.
// target_effect_1: the target has effect 1; the attack (ovl11 +0x0D82,
// 0x41682..0x4169D) then hits even when the roll misses (ovl23 +0x06ED,
// HasEffect(target, 1)): `hit` is the attack's outcome.
AttackRoll attack_roll(const Bytes& attacker, int ac_value, int modifier, Rng& rng, bool effect_0d = false,
                       bool target_effect_1 = false);

// DamageRoll's dice, ovl11 +0x0125 (0x40A25) through ovl22 +0x06FB:
// RollDice(attacker +0xDC+slot, +0xDE+slot) + signed +0xE0+slot, 0 if not
// positive; slot 1 = the weapon slot, 2 = the second natural attack.
// backstab (DS:6BD5 = 2): x((level + 7) div 4) for Rogues (class 5),
// else x2, as a byte. The weapon-kind cancellations after it aren't here.
int damage_roll(const Bytes& attacker, int slot, bool backstab, Rng& rng);

// SkillCheck, ovl23 +0x2E94 (0x58DA4), over the records it checks in order
// (one, or the whole list with its "all" flag): totals[i] is SkillTotal
// (ovl23 +0x2E3F) of record i for the skill, a byte. Per record: t := the
// total as a signed byte, scaled by the difficulty (0 x4, 1 x2, 2 x1, 3
// div 2, 4 div 4; the divisions unsigned on the 16-bit word), margin :=
// t - 1d100; the best margin wins (the first on a tie). Result: best >= 0:
// 3 if best >= t div 2, else 2; best < 0: 1 if |best| <= t, else 0 (t
// unsigned). Quirk (reproduced): t is the last record's, not the best
// one's. No record: the original reads its uninitialised locals; rejected
// (D-0031).
struct SkillCheckResult {
    int result = 0;   // 0..3; >= 2: a success
    int best = -1;    // index of the best record
};
SkillCheckResult skill_check(const std::vector<int>& totals, int difficulty, Rng& rng);

// Battle Tactics, ovl11 +0x45FA (0x44EFA), once per round. Each record in
// list order that is active (+0xD5), Okay, dodging or jetpacked (+0xD4 in
// {1, 0x0A, 0x0D}), without effect 0x0E (HasEffect ovl23 +0x18FD) and
// whose side (+0xD6) hasn't succeeded yet this round makes a SkillCheck
// of skill 0x1F (Battle Tactics), difficulty 2; a result >= 2 marks the
// side (DS:6BD3 + side), which Recalc turns into +1 on the THAC0 field
// +0xD8 (ovl23, 0x57493).
struct TacticsMember {
    bool active = false;
    int status = 0;
    int side = 0;
    bool effect_0e = false;
    int total = 0;    // SkillTotal(skill 0x1F)
};
std::array<bool, 2> battle_tactics(const std::vector<TacticsMember>& list, Rng& rng);

// Leadership, ovl11 +0x43A9 (0x44CA9), at the start of a fight. First
// every record's +0xD7 (computer-controlled) := +0x4C (the NPC flag) on
// the party side, 1 for the enemy. Then for each leader L in list order
// (party side, not an NPC, Leadership points +0x95 > 0, Okay) and each NPC
// N in list order (party side, +0x4C != 0, still +0xD7 != 0, Okay, INT
// +0x13 > 3, without effect 3): SkillCheck(L, skill 0x49, difficulty 2);
// a result >= 2 gives N to the player (+0xD7 := 0). Returns every
// record's +0xD7 afterwards.
struct LeadershipMember {
    int side = 0;
    int npc = 0;          // +0x4C
    int points = 0;       // +0x95, Leadership's points
    int status = 0;
    int intelligence = 0; // +0x13
    bool effect_3 = false;
    int total = 0;        // SkillTotal(skill 0x49)
};
std::vector<int> leadership(const std::vector<LeadershipMember>& list, Rng& rng);

// SavingThrow, ovl22 +0x0679 (0x556C9): d20 (DS:698A); 1 fails, 20
// passes; otherwise total := byte(roll + bonus) (bonus signed), then
// effect hook 0x0C on the record (only type 0x0D reacts), and the save passes if
// record +0x1E + type <= total, unsigned: a total below 0 wraps to 0x80..
// 0xFF and passes (quirk, reproduced). type: 0..7 (signed byte index).
struct SaveRoll {
    int roll = 0;       // the d20
    int total = 0;      // DS:698A after the bonus (0..255)
    bool success = false;
};
// effect_0d: the record has effect 0x0D; hook 0x0C then takes 4 off the
// total (a byte: below 0 it wraps and passes).
SaveRoll saving_throw(const Bytes& rec, int type, int bonus, Rng& rng, bool effect_0d = false);

// The medical phase after a fight, ovl5 +0x1449 (0x38F89), on the list's
// records in order (each 0x103 bytes, changed in place). total(rec, skill)
// is SkillTotal; effects[i] the effect types of record i; antidote(i) the
// player's use of an antidote for poisoned record i (0x38D63: an item of
// type 0x1F and a Yes; not modelled here).
//  1. Poison (0x38FEC): each record with status 7; each medic in list order
//     (class 2, status 1) makes SkillCheck(skill 0x14, difficulty 2) until
//     one passes: status 5, +0xD5 0, HP 0. None: an antidote does the
//     same, else status 8 (coma), +0xD5 0.
//  2. Wounds (0x3928C): each record P with status in {1, 4, 5}, side 0 and
//     HP < max HP (+0x45; words, unsigned); four tiers in turn, and in each
//     every member M in list order with status 1, side != 1 and HP != 0
//     (unsigned; P itself too) while P isn't done: the heal h from dice
//     first (tier 0 First Aid, skill 0x0D: 1d6, at most up to HP 6; tier 1
//     skill 0x13: 3d8+3, difficulty 4 when M is P; tier 2 skill 0x12:
//     2d8+1; tier 3 skill 0x11: 1d8; tiers 1-3 doubled), capped at max HP
//     - HP; h <= 0: no check; else SkillCheck(skill, difficulty): success
//     adds h, status 1, +0xD5 1, and in tiers 1-3 P is done.
//     Then a record with effect 3: side 1 writes through its freed combat
//     block (rejected, D-0033); otherwise HP < 45: status 1, +0xD5 1, HP 45.
// Returns the closing message: 0 no injuries, 1 no medics, 2 none
// treated, 3 healing completed (0x39677; single reading).

// XP after a fight. The total (ovl5 +0x005B, 0x37B9B): the word +0xB2 of
// every enemy record (+0xD6 = 1, its combat block's +0x0F != 0) neither
// Okay (status 1) nor fled (status 6). The share: under COMBAT_SOLO
// (DS:82EA) the total; otherwise total div (var 0x7F3E - DS:82EB), DS:82EB
// being the party records with +0xD5 = 0 (0x37FDF), longint division
// (0x37D69). It is paid (ovl5 +0x023E, 0x37D7E) to CurChar under
// COMBAT_SOLO, else to every list record with +0xD5 != 0 (+0x2F, dword),
// before the medical phase. With no active member the share keeps the
// previous fight's (DS:82EC): rejected (D-0034).
struct XpEnemy {
    int status = 0;
    int block_0f = 1;
    std::uint16_t xp = 0;   // +0xB2
};
std::uint32_t xp_total(const std::vector<XpEnemy>& enemies);
std::uint32_t xp_share(std::uint32_t total, int party_count, int inactive, bool solo);

// TakeDamage, ovl23 +0x1968 (0x57878), on the HP word +0xE3 (unsigned):
// HP >= damage: HP - damage; else HP 0 with over := damage - HP. over >
// 29: dead (status 3); > 9: coma (8); > 0: dying (4; in combat cb+0x0C :=
// over); HP left 0: unconscious (5); otherwise the status stays. Down:
// +0xD5 := 0, HP 0, and in combat the side's count and cb+1 drop.
// `over` counts from the current HP only: a dying record hit again starts
// over (quirk, reproduced).
struct Hurt {
    std::uint16_t hp = 0;
    int status = 0;    // -1: unchanged
    int over = 0;
    bool down = false;
};
Hurt take_damage(std::uint16_t hp, std::uint16_t damage);

// The booty's credits after a fight (DS:6980, a longint): what TREASURE
// put there, plus, in the XP total's loop (ovl5 +0x005B, 0x37C2F), the
// credits +0x2B of every counted enemy (as for XP) whose combat block has
// +0x0F = 1; nothing under COMBAT_SOLO (0x37C25). Wraps like a longint.
struct BootyEnemy {
    int status = 0;
    int block_0f = 1;
    std::uint32_t credits = 0;   // +0x2B
};

// Pool, ovl21 +0x0205 (0x54D25): every list record with +0xE7 in {0, 0xB3}
// puts its credits +0x2B into the booty and keeps none (DS:6B54 := 1).
// Returns the booty; credits are changed in place.
std::int32_t pool(std::int32_t booty, const std::vector<std::uint8_t>& e7, std::vector<std::uint32_t>& credits);

// --- Undefined behaviour in combat code (quirk policy; DIVERGENCE.md) ------

// Diagnostics for undefined behaviour that shipped data triggers (CLAUDE.md
// rule 4): the port goes on with the reference machine's value and logs
// one line, once per D-entry per run (the sink; default: standard error,
// "ctd: D-00nn: ..."). reset_diag() forgets which entries were logged.
void set_diag_sink(std::function<void(const std::string&)> sink);
// Logs "<entry>: <line>" through the sink, once per entry per run.
void diag(const std::string& entry, const std::string& line);
// As diag, logged once per `key` instead of once per entry (a second kind of
// line under one D-entry: D-0075's layouts that differ, session 79).
void diag_keyed(const std::string& key, const std::string& entry, const std::string& line);

// The bytes of the first KiB of memory (the interrupt vectors, the BIOS
// data) that the game reads or writes through nil pointers. One table for
// the whole run, so a write (D-0024's 0000:0034, D-0033's 0000:000F)
// reaches every later read (D-0025's 0000:002E, D-0029's 0000:000F, ...).
// Seeded with the reference machine's bytes, the same in every memory dump
// (runs s17t_fe_cg1, s18_dsaves_mem, s19_f2_mem): 0x01 = 0, 0x0F = 0
// (INT 3 is 0070:000E), 0x2E = 0 (INT 0Bh F000:CE60), 0x34 = 0x60
// (INT 0Dh F000:CE60), 0x38 = 0xC0, 0xD5 = 0xCA; session 23: 0x11 = 0 (INT 4),
// 0x7F = 0xC0 (INT 1Fh), the same in all 11 dumps. A byte not measured is
// rejected. The machine keeps a write until it reboots (TP restores only
// its own vectors at exit); the port keeps it for the process (D-0037).
// Every byte and whether shipped data reaches it: docs/notes/combat.md
// "Low memory".
class LowMem {
public:
    LowMem();
    // The byte at addr (logged under `entry` as rule 4 asks).
    int read(int addr, const std::string& entry, const std::string& line);
    void write(int addr, int value, const std::string& entry, const std::string& line);
    int peek(int addr) const;

private:
    std::array<int, 0x400> b_;
};

// ChooseWeapon's tail (ovl7 +0x164A, 0x3CAE4..0x3CB4F). The original
// writes bit 7 of byte +0x34 through the readied weapon (cur0) and the
// chosen one (best); with either nil the write lands in 0000:0034 (the
// INT 0Dh vector), which no game state sees: the port drops it and logs
// D-0024. Then, with no heavy weapon chosen (out = 0) and an unreadied
// kind-6 item (hand6) but none readied, hand6 is toggled (Game.ReadyItem)
// if the kind of best's type, DS:4A36[9 type] (ITEMS byte 2), is 8. With
// best nil the type is read at 0000:002E: the reference machine's byte 0
// (memory dumps of runs s17t_fe_cg1 and s18_dsaves_mem), so type 0's kind
// from the player's ITEMS, and D-0025 is logged. items: the ITEMS file
// (9 bytes per type). Returns whether hand6 is toggled.
bool choose_weapon_tail(bool cur0_nil, bool best_nil, int best_type, bool hand6, bool hand6_ready, bool out,
                        const Bytes& items, class LowMem& low);

// AreaTarget's scoring base (ovl7 +0x019F, 0x3B8D3..0x3B957) for the
// area weapon's type; 0x23..0x26 roll dice. Other types read an
// uninitialised local: unreachable from shipped code (ScoreArea is called
// only for types {17, 18, 34..38}, all handled), rejected (D-0026).

// WeaponSpecial's message (ovl11 +0x16B0, 0x42078..0x420E4): "launches a
// missile" (true) or "throws a grenade" (false, a kind-8 weapon with no
// readied launcher). For kinds 7 and 9 the original reads a stale stack
// byte (BP-0x38); the port takes "missile", as for every other kind, and
// logs D-0027 (measured in session 19: "missile").
bool area_message_missile(int kind, bool launcher_found);

// Place (ovl8 +0x1A30, 0x3E810): its result flag (BP-2) is written only by
// TryCell (0x3EC6C) or the both-out branch (0x3EB77); reading it unwritten
// (0x3EC72) is rejected (D-0028). Throws Error when !written.

// After combat with no fight (ovl5 +0x02E5): records without a combat
// block are tested for side through a nil pointer, reading 0000:000F; the
// port uses the reference machine's value, 0 (party side), and logs
// D-0029.
int side_of_record_without_block(class LowMem& low);

// Divvy (ovl21 +0x02C4, 0x54DE4): the booty's credits divided among the
// list members with +0xE7 in {0, 0xB3} (the count, 0x54D8E), paid to those
// with +0xE7 < 0x80: the quotient's low word each, then one more credit
// each in list order while the remainder lasts. Credits <= 0: nothing.
// No counted member: the original's LongDivMod divides by zero (runtime
// error 200); rejected (D-0030). Returns what each member gains.
std::vector<std::uint32_t> divvy(std::int32_t credits, const std::vector<std::uint8_t>& e7);

}  // namespace cotd::game::combat
