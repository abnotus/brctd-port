// SPDX-License-Identifier: GPL-3.0-or-later
#include "game/combat_rules.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace cotd::game::combat {

namespace {
const std::string kInitD10 = "ovl11 +0x008C";     // RollDice at 0x40987
const std::string kInitD127 = "ovl11 +0x00F7";    // RollDice at 0x409F2
const std::string kToHit = "ovl22 +0x0598";       // RollDice at 0x555E3
const std::string kDamage = "ovl22 +0x0714";      // RollDice at 0x5575F

int s8(std::uint8_t v) { return static_cast<std::int8_t>(v); }
}  // namespace


int Rng::random(int n, const std::string& site) {
    // System.Random 0BE4:1150 (tools/tprandom.py).
    const std::uint32_t before = seed_;
    seed_ = seed_ * 0x08088405u + 1u;
    const int r = n ? static_cast<int>((seed_ >> 16) % static_cast<std::uint32_t>(n)) : 0;
    calls_.push_back({site, n, r});
    rng_note(before, n, r);
    return r;
}

int Rng::roll_dice(int count, int sides, const std::string& site) {
    // Game.RollDice ovl23 +0x3025: byte sum of count x (Random(sides) + 1).
    int sum = 0;
    for (int i = 0; i < count; ++i) sum += random(sides, site) + 1;
    return sum & 0xFF;
}

Initiative initiative(const Bytes& rec, int dex_adj, int surprise, Rng& rng) {
    Initiative out;
    if (rec[0xD5] == 0) return out;   // 0x40969: inactive, cb+1 := 0
    // 0x4097A..0x409A6: byte(DEX adjustment + 1d10 + 1), at least 2 (signed).
    out.cb1 = s8(static_cast<std::uint8_t>(dex_adj + rng.roll_dice(1, 10, kInitD10) + 1));
    if (out.cb1 < 2) out.cb1 = 2;
    // 0x409AB: -6 when surprised (var 0x7ECB and (side + 1)).
    if (surprise & (s8(rec[0xD6]) + 1)) out.cb1 = s8(static_cast<std::uint8_t>(out.cb1 - 6));
    if (out.cb1 < 0 || out.cb1 > 20) out.cb1 = 0;   // 0x409D0: the turn is lost
    out.cb5 = rng.roll_dice(1, 0x7F, kInitD127);
    return out;
}

int attack_modifier(const Bytes& attacker, int var7f70, int var7f71, int level) {
    if (attacker[0xD6] != 0) return s8(static_cast<std::uint8_t>(var7f70));
    int m = var7f71;
    switch (level) {
        case 0: m += 2; break;
        case 1: m += 1; break;
        case 3: m -= 1; break;
        case 4: m -= 2; break;
        default: break;
    }
    return s8(static_cast<std::uint8_t>(m));
}

AttackRoll attack_roll(const Bytes& attacker, int ac_value, int modifier, Rng& rng, bool effect_0d,
                       bool target_effect_1) {
    AttackRoll out;
    out.roll = s8(static_cast<std::uint8_t>(rng.roll_dice(1, 20, kToHit)));
    out.hit = target_effect_1;                // 0x4168B: ovl23 +0x06ED after a miss
    if (out.roll <= 1) return out;            // 0x555EB: a 1 misses
    if (out.roll == 20) out.roll = 100;       // 0x555F5
    out.hook10 = true;
    if (effect_0d) out.roll = s8(static_cast<std::uint8_t>(out.roll - 4));   // hook 10: ovl10 +0x00BB
    if (out.roll < 0) return out;             // 0x55690
    // 0x55697..0x556BA: roll (signed) + THAC0 field (unsigned) + modifier
    // (signed) against the AC value (unsigned byte), signed compare.
    out.hit = out.hit || out.roll + attacker[0xD8] + modifier >= (ac_value & 0xFF);
    return out;
}

int damage_roll(const Bytes& attacker, int slot, bool backstab, Rng& rng) {
    const std::size_t s = static_cast<std::size_t>(slot);
    // 0x40A2B..0x40A77, DiceRoll ovl22 +0x06FB.
    int d = rng.roll_dice(attacker[0xDC + s], attacker[0xDE + s], kDamage) + s8(attacker[0xE0 + s]);
    int out = d > 0 ? (d & 0xFF) : 0;
    if (backstab) {
        // 0x40A7C: Rogues x((level + 7) div 4), others x2, as a byte.
        if (attacker[0x28] == 5) out = (out * ((s8(attacker[0x29]) + 7) / 4)) & 0xFF;
        else out = (out * 2) & 0xFF;
    }
    return out;
}

SkillCheckResult skill_check(const std::vector<int>& totals, int difficulty, Rng& rng) {
    if (totals.empty())
        throw Error("SkillCheck (ovl23 +0x2E94, 0x58EB6): no record to check: "
                    "the original reads its uninitialised locals [D-0031]");
    SkillCheckResult out;
    int best = -10000;              // 0x58DAA
    std::uint16_t t = 0;            // BP-3, a word
    for (std::size_t i = 0; i < totals.size(); ++i) {
        t = static_cast<std::uint16_t>(s8(static_cast<std::uint8_t>(totals[i])));   // 0x58E11 cbw
        switch (difficulty) {       // 0x58E15..0x58E58
            case 0: t = static_cast<std::uint16_t>(t << 2); break;
            case 1: t = static_cast<std::uint16_t>(t << 1); break;
            case 3: t = static_cast<std::uint16_t>(t / 2); break;
            case 4: t = static_cast<std::uint16_t>(t / 4); break;
            default: break;
        }
        const int r = rng.roll_dice(1, 100, "ovl23 +0x2F55");   // 0x58E62
        const int margin = static_cast<std::int16_t>(static_cast<std::uint16_t>(t - r));
        if (margin > best) {        // 0x58E74: signed, the first best kept
            best = margin;
            out.best = static_cast<int>(i);
        }
    }
    if (best >= 0)                  // 0x58EB6..0x58EE1
        out.result = best < (t / 2) ? 2 : 3;
    else                            // 0x58EE8..0x58F12
        out.result = -best > t ? 0 : 1;
    return out;
}

std::array<bool, 2> battle_tactics(const std::vector<TacticsMember>& list, Rng& rng) {
    std::array<bool, 2> done{false, false};   // DS:6BD3, DS:6BD4
    for (const TacticsMember& m : list) {
        if (!m.active) continue;                                            // 0x44F36
        if (m.status != 1 && m.status != 0x0A && m.status != 0x0D) continue;  // set CS:45BB
        if (m.effect_0e) continue;                                          // 0x44F67
        if (m.side != 0 && m.side != 1) throw Error("battle_tactics: side must be 0 or 1");
        const std::size_t side = static_cast<std::size_t>(m.side);
        if (done[side]) continue;                                           // 0x44F7E
        if (skill_check({m.total}, 2, rng).result >= 2) done[side] = true;  // 0x44F96
    }
    return done;
}

SaveRoll saving_throw(const Bytes& rec, int type, int bonus, Rng& rng, bool effect_0d) {
    SaveRoll out;
    out.roll = rng.roll_dice(1, 20, "ovl22 +0x068F");   // 0x556DA
    out.total = out.roll;
    if (out.roll == 1) return out;                      // 0x556E5
    if (out.roll == 20) {                               // 0x556F0
        out.success = true;
        return out;
    }
    out.total = (out.roll + s8(static_cast<std::uint8_t>(bonus))) & 0xFF;   // 0x556FB
    if (effect_0d) out.total = (out.total - 4) & 0xFF;                       // hook 0x0C: ovl10 +0x00BB
    const int idx = 0x1E + s8(static_cast<std::uint8_t>(type));             // 0x5571D
    if (idx < 0 || idx >= static_cast<int>(rec.size()))
        throw Error("saving_throw: save type out of the record");
    out.success = rec[static_cast<std::size_t>(idx)] <= out.total;          // 0x5572A: ja fails
    return out;
}


Hurt take_damage(std::uint16_t hp, std::uint16_t damage) {
    Hurt out;
    std::uint16_t over = 0;
    if (hp >= damage) out.hp = static_cast<std::uint16_t>(hp - damage);   // 0x57890
    else over = static_cast<std::uint16_t>(damage - hp);
    out.over = over;
    out.status = -1;
    if (over > 0x1D) out.status = 3;            // 0x578BE: dead
    else if (over > 9) out.status = 8;          // 0x578CF: coma
    else if (over > 0) out.status = 4;          // 0x578E0: dying
    else if (out.hp == 0) out.status = 5;       // 0x57907: unconscious
    out.down = out.status != -1;
    if (out.down) out.hp = 0;                   // 0x5791A
    return out;
}


std::int32_t pool(std::int32_t booty, const std::vector<std::uint8_t>& e7, std::vector<std::uint32_t>& credits) {
    std::uint32_t b = static_cast<std::uint32_t>(booty);
    for (std::size_t i = 0; i < e7.size() && i < credits.size(); ++i) {
        if (e7[i] != 0 && e7[i] != 0xB3) continue;                  // 0x54D47
        b += credits[i];                                             // 0x54D63
        credits[i] = 0;
    }
    return static_cast<std::int32_t>(b);
}

std::uint32_t xp_total(const std::vector<XpEnemy>& enemies) {
    std::uint32_t total = 0;
    for (const XpEnemy& e : enemies)                   // 0x37BD4..0x37C22
        if (e.status != 1 && e.block_0f != 0 && e.status != 6) total += e.xp;
    return total;
}

std::uint32_t xp_share(std::uint32_t total, int party_count, int inactive, bool solo) {
    if (solo) return total;                            // 0x37D24
    const int n = (party_count - inactive) & 0xFF;     // 0x37D3C: a byte
    if (n == 0)
        throw Error("XP (ovl5 +0x005B, 0x37D57): no active party member: the original pays the previous "
                    "fight's share (DS:82EC) [D-0034]");
    return total / static_cast<std::uint32_t>(n);      // 0x37D69 LongDivMod
}

std::vector<int> leadership(const std::vector<LeadershipMember>& list, Rng& rng) {
    std::vector<int> d7(list.size());
    for (std::size_t i = 0; i < list.size(); ++i)                 // 0x44CC0..0x44CFD
        d7[i] = list[i].side == 0 ? list[i].npc : 1;
    for (const LeadershipMember& l : list) {                      // 0x44D0B
        if (l.side != 0 || l.npc != 0 || l.points == 0 || l.status != 1) continue;
        for (std::size_t j = 0; j < list.size(); ++j) {           // 0x44D59
            const LeadershipMember& n = list[j];
            if (n.side != 0 || n.npc == 0 || d7[j] == 0 || n.status != 1 || n.intelligence <= 3 || n.effect_3)
                continue;
            if (skill_check({l.total}, 2, rng).result >= 2) d7[j] = 0;   // 0x44DD0, 0x44E80
        }
    }
    return d7;
}

namespace {
std::function<void(const std::string&)> g_diag_sink;
std::set<std::string> g_diag_logged;
}  // namespace

void diag_keyed(const std::string& key, const std::string& entry, const std::string& line) {
    if (!g_diag_logged.insert(key).second) return;     // once per run
    const std::string text = entry + ": " + line;
    if (g_diag_sink) g_diag_sink(text);
    else std::fprintf(stderr, "ctd: %s\n", text.c_str());
}

void diag(const std::string& entry, const std::string& line) { diag_keyed(entry, entry, line); }

void set_diag_sink(std::function<void(const std::string&)> sink) { g_diag_sink = std::move(sink); }

LowMem::LowMem() {
    b_.fill(-1);
    b_[0x01] = 0x00;
    b_[0x0F] = 0x00;
    b_[0x11] = 0x00;
    b_[0x7F] = 0xC0;
    b_[0x2E] = 0x00;
    b_[0x34] = 0x60;
    b_[0x38] = 0xC0;
    b_[0xD5] = 0xCA;
    // INT 7's vector segment (0000:001E, a word): 0xF000 in every recorded
    // memory dump (runs s32_wild1, s32_wildd_a8_6; 40 DOSBox-X states,
    // analysis/re/boarding.md): read and cleared by a capture's ShipRepair
    // (D-0066).
    b_[0x1E] = 0x00;
    b_[0x1F] = 0xF0;
}

int LowMem::peek(int addr) const {
    if (addr < 0 || addr >= 0x400 || b_[static_cast<std::size_t>(addr)] < 0)
        throw Error("low memory 0000:" + hex(static_cast<std::uint32_t>(addr), 4) + " was never measured on the reference machine");
    return b_[static_cast<std::size_t>(addr)];
}

int LowMem::read(int addr, const std::string& entry, const std::string& line) {
    const int v = peek(addr);
    diag(entry, line + " (0000:" + hex(static_cast<std::uint32_t>(addr), 4) + " = " + hex(static_cast<std::uint32_t>(v), 2) + ")");
    return v;
}

void LowMem::write(int addr, int value, const std::string& entry, const std::string& line) {
    peek(addr);
    b_[static_cast<std::size_t>(addr)] = value & 0xFF;
    diag(entry, line + " (0000:" + hex(static_cast<std::uint32_t>(addr), 4) + " := " + hex(static_cast<std::uint32_t>(value & 0xFF), 2) + ")");
}

bool choose_weapon_tail(bool cur0_nil, bool best_nil, int best_type, bool hand6, bool hand6_ready, bool out,
                        const Bytes& items, LowMem& low) {
    // 0x3CAE4: cur0+0x34 &= 0x7F; 0x3CB06: best+0x34 |= 0x80, through nil
    // into 0000:0034 (the INT 0Dh vector's low byte): kept in `low`.
    if (cur0_nil)
        low.write(0x34, low.peek(0x34) & 0x7F, "D-0024", "ChooseWeapon (ovl7 +0x164A, 0x3CAE4): a write through a nil weapon pointer");
    if (best_nil)
        low.write(0x34, low.peek(0x34) | 0x80, "D-0024", "ChooseWeapon (ovl7 +0x164A, 0x3CB06): a write through a nil weapon pointer");
    // 0x3CB16..0x3CB42: out, hand6, hand6's readied flag, then best's type.
    if (out || !hand6 || hand6_ready) return false;
    int type = best_type;
    if (best_nil)                                           // 0x3CB2D: reads 0000:002E
        type = low.read(0x2E, "D-0025", "ChooseWeapon (ovl7 +0x164A, 0x3CB2D): no weapon chosen; the item type is read "
                                        "through a nil pointer");
    const std::size_t k = 9 * static_cast<std::size_t>(type & 0xFF) + 2;   // DS:4A36 = ITEMS + 2
    return k < items.size() && items[k] == 8;               // 0x3CB3D
}


bool area_message_missile(int kind, bool launcher_found) {
    if (kind == 8) return launcher_found;   // 0x420A1..0x420DA
    if (kind == 7 || kind == 9)             // 0x4208C: a stale stack byte
        diag("D-0027", "WeaponSpecial (ovl11 +0x16B0, 0x420E4): the message flag for weapon kind " +
                           std::to_string(kind) + " is a stale stack byte: \"launches a missile\" is shown");
    return true;                            // 0x420E0
}


int side_of_record_without_block(LowMem& low) {
    return low.read(0x0F, "D-0029", "after combat (ovl5 +0x02E5): a record without a combat block is tested through "
                                    "a nil pointer");
}

std::vector<std::uint32_t> divvy(std::int32_t credits, const std::vector<std::uint8_t>& e7) {
    std::vector<std::uint32_t> out(e7.size(), 0);
    int n = 0;
    for (std::uint8_t v : e7)
        if (v == 0 || v == 0xB3) n = (n + 1) & 0xFF;   // 0x54DAF: a byte count
    if (credits <= 0) return out;                     // 0x54E0B: signed > 0
    if (n == 0)
        throw Error("Divvy (ovl21 +0x02C4, 0x54E2C): no member to divide the booty among: "
                    "the original divides by zero (runtime error 200) [D-0030]");
    const std::uint32_t q = static_cast<std::uint32_t>(credits) / static_cast<std::uint32_t>(n);
    std::uint32_t r = (static_cast<std::uint32_t>(credits) % static_cast<std::uint32_t>(n)) & 0xFFFF;
    for (std::size_t i = 0; i < e7.size(); ++i) {
        if (e7[i] >= 0x80) continue;                  // 0x54E65
        out[i] = q & 0xFFFF;                          // the quotient's low word
        if (r > 0) {
            out[i] += 1;
            --r;
        }
    }
    return out;
}

}  // namespace cotd::game::combat
