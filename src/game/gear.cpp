// SPDX-License-Identifier: GPL-3.0-or-later
// VIEW's Gear (ovl18 +0x1968) with its item commands, and VIEW's Trade
// (ovl18 +0x283C): docs/notes/frontend.md "VIEW's Gear and Trade".
#include <string>
#include <vector>

#include "game/game.hpp"

namespace cotd::game {

void Game::view_gear(std::uint8_t& exit_flag) {
    // ViewGear ovl18 +0x1968 (0x51908..0x51E29) on CurChar; exit_flag is
    // ViewCharacter's (combat: the turn's done).
    f8433_ = 0;                                        // 0x5190F: no roster clicks (not restored)
    Character& c = *cur_;
    if (c.items.empty()) {                             // 0x51942
        show_message(c.name() + text_[Text::GrNoGear]);
        return;
    }
    int index = 0;
    bool list_redraw = true, full_redraw = true;
    exit_flag = 0;
    int key = '@';
    // The loop leaves at its top (0x51983): key 0 or 'E' (CS:18B5), the
    // exit flag, or no items left.
    while (key != 0 && key != 'E' && !exit_flag && !c.items.empty()) {
        // The menu (0x519B6..0x51AFB): Ready [Trade] Drop [Halve] Join
        // [Sell]; Trade and Sell not for an NPC that is not under control
        // (+0xE7 >= 0x80 and +0xD5), Trade not in states 5 and 8 (CS:18DB),
        // Halve below 16 items, Sell only in state 1 (the shop).
        const bool npc = c.b(0xE7) >= 0x80 && c.b(0xD5) != 0;
        std::string select = text_[Text::GrReady];
        int hi = 4;
        if (!npc && mode_ != 5 && mode_ != 8) {
            select += text_[Text::GrTrade];
            ++hi;
        }
        select += text_[Text::GrDrop];
        if (c.items.size() < 0x10) {
            select += text_[Text::GrHalve];
            ++hi;
        }
        select += text_[Text::GrJoin];
        if (!npc && mode_ == 1) select += text_[Text::GrSell];
        // 0x51B13: from the shop (DS:6B1D = 0xFF) or after a sale, the
        // highlight on the item after Join (Sell); from VIEW MenuScreen's
        // checksum puts it on Ready.
        if (highlight_ == 0xFF || key == 'S') {
            highlight_ = static_cast<std::uint8_t>(hi);
            menu_sum_ = 0xFF;
        }
        // Every name rebuilt with its ready prefix, into the item records
        // (ItemName ovl23 +0x0124, 0x51B2D); the list's nodes are the items.
        std::vector<ListNode> list;
        for (Bytes& it : c.items) list.push_back({item_name(it, true), it[0x29] != 0});
        if (full_redraw) {                             // 0x51B6C (DS:6994 has no writer)
            clear_cells(1, 1, 0x26, 0x10);             // 015F:0274
            layout_outer();
            draw_char_name(c, 1, 1, true);
            draw_string(static_cast<std::uint8_t>(c.name().size() + 4), 1, 10, 0, text_[Text::GrTitle]);
            draw_string(1, 3, 15, 0, text_[Text::GrReadyItem]);
            list_redraw = true;
            full_redraw = false;
        }
        const std::size_t before = c.items.size();     // 0x51BF0
        int picked = -1;
        key = list_menu("", select, 1, 5, 0x26, 0x16, list, true, list_redraw, index, picked);
        if (picked >= 0) {
            // ItemView ovl18 +0x1305 ('V') is not reachable: no item of the
            // menu has a V (MenuScreen matches only its items' letters).
            switch (key) {
                case 'R': {                            // 0x51C7C
                    bool heavy = false;
                    ready_toggle(c, picked, heavy);
                    exit_flag = heavy ? 1 : 0;
                    if (exit_flag) show_message(c.name() + text_[Text::GrHeavy]);
                    break;
                }
                case 'T':                              // 0x51CBD
                    if (can_modify(c.items[static_cast<std::size_t>(picked)])) gear_trade(c, picked);
                    key = ' ';
                    full_redraw = true;
                    break;
                case 'D':                              // 0x51CEA
                    if (can_modify(c.items[static_cast<std::size_t>(picked)])) {
                        const std::string name = item_name(c.items[static_cast<std::size_t>(picked)]);
                        print_in(1, 0x15, 0x26, 0x16, 14, true, text_[Text::GrYour] + name + text_[Text::GrGone]);
                        if (yes_no(text_[Text::GrDropIt], 2) == 'Y') {
                            remove_item(c, picked);
                            list_redraw = true;
                        }
                        clear_cells(1, 0x15, 0x26, 0x16);
                    } else {
                        key = ' ';
                    }
                    break;
                case 'H': halve(c, picked); break;     // 0x51DB4
                case 'J': merge_stack(c, static_cast<std::size_t>(picked)); break;   // 0x51DC6
                case 'S':                              // 0x51DD8
                    if (can_modify(c.items[static_cast<std::size_t>(picked)])) sell(c, picked);
                    else key = ' ';
                    break;
                default: break;
            }
        }
        recalc(c);                                     // 0x51DFF, after every key
        if (c.items.size() != before) list_redraw = true;
    }
}

bool Game::can_modify(const Bytes& item) {
    // CanModify ovl18 +0x1191 (0x51131): a readied item is refused
    // (CS:117F); rows 21-22 cleared either way.
    const bool ok = !(item[0x34] & 0x80);
    if (!ok) show_message(text_[Text::GrUnready]);
    clear_cells(1, 0x15, 0x26, 0x16);
    return ok;
}

namespace {

bool in_party(const std::vector<std::unique_ptr<Character>>& party, const Character* m) {
    for (const auto& p : party)
        if (p.get() == m) return true;
    return false;
}

}  // namespace

void Game::gear_trade(Character& owner, int i) {
    // GearTrade ovl18 +0x20C4 (0x52064..0x52189): the partner from DS:387C
    // (VIEW set it to CurChar), Screen.Redraw, SelectMember(CS:20A8, Exit,
    // alt); nil: nothing. the count check (+0x2939):
    // Recalc(partner), refused at 16 items. Then a copy of the node (the
    // list owner's) appended to the partner's items (ovl23 +0x0983),
    // RemoveItem(CurChar, node) (0x52109: remove_item_of),
    // Recalc(partner), and unless the partner is CurChar, MergeStack of the
    // copy with the partner as CurChar.
    Character* target = f387c_;
    // DS:387C is set by VIEW's entry and the trades. The shop's Sell opens
    // Gear without VIEW, so a fresh game's partner is the start-up value
    // (not a record), or a record no longer in the party. SelectMember then
    // whitens nobody; Exit, Esc and roster clicks work as usual; Select
    // gives the item to that "partner": the original copies it into the
    // memory there and removes it from CurChar (run s26_shop_trade2: the
    // item gone, the game goes on). The port removes it, writes nothing
    // else (D-0056).
    redraw_screen();
    select_member(text_[Text::GrWhom], true, true, target, false);
    if (!cur_ || !target) return;
    f387c_ = target;
    if (!in_party(party_, target)) {
        combat::diag("D-0056", "GearTrade (ovl18 +0x20C4, 0x520B8) to a partner that is no member (DS:387C): the "
                               "item leaves CurChar and goes to no record");
        remove_item_of(*cur_, owner, i);
        return;
    }
    recalc(*target);                                   // 0x528E5
    if (target->items.size() >= 0x10) {                // 0x52928
        show_message(text_[Text::GrTooMany]);
        return;
    }
    const Bytes copy = owner.items.at(static_cast<std::size_t>(i));
    target->items.push_back(copy);                     // 0x520F6
    remove_item_of(*cur_, owner, i);                   // 0x52109: the node keeps its index (the copy is last)
    recalc(*target);                                   // 0x52114
    if (target == cur_) return;                        // 0x5211E
    const CurChar saved = cur_;                          // 0x52158
    cur_ = target;
    merge_stack(*target, target->items.size() - 1);
    cur_ = saved;
}

void Game::halve(Character& c, int i) {
    // Halve ovl18 +0x21FD (0x5219D..0x52266): not a non-stacking item
    // (+0x34 bit 0x20) nor a count below 2 (CS:21EC); the item keeps the
    // larger half and its readied bit, a copy with the smaller half,
    // unreadied, goes right after it. It takes only the node ([BP+6]) and
    // links the copy after it (+0x2A, 0x52222..0x52249): the list's owner
    // c, which isn't CurChar after a roster click moved it (D-0074).
    Bytes& it = c.items.at(static_cast<std::size_t>(i));
    const int half = it[0x38] / 2;
    if ((it[0x34] & 0x20) || half == 0) {
        show_message(text_[Text::GrCantHalve]);
        return;
    }
    for (ForeignSlot& f : foreign_slot_)   // a slot holding this node sees its new count
        if (f.owner == &c && f.item == it) f.item[0x38] = static_cast<std::uint8_t>(it[0x38] - half);
    Bytes copy = it;
    copy[0x38] = static_cast<std::uint8_t>(half);
    copy[0x34] = it[0x34] & 0x7F;
    it[0x38] = static_cast<std::uint8_t>(it[0x38] - half);
    c.items.insert(c.items.begin() + i + 1, copy);
    if (c.weapon > i) ++c.weapon;
    if (c.armour > i) ++c.armour;
}

void Game::sell(Character& owner, int i) {
    // Sell ovl18 +0x2594 (0x52534..0x527B9), the shop's (state 1): half
    // the value (+0x39, word) times the count unless the item doesn't
    // stack, kept to 16 bits; the offer in rows 21-22, YesNo; Yes: the
    // message, RemoveItem(CurChar, node) (0x52789: remove_item_of), the
    // credits added to CurChar (0x52799). Rows 21-22 cleared.
    Bytes& it = owner.items.at(static_cast<std::size_t>(i));
    const int count = it[0x38];
    std::uint16_t price = 0;
    const std::uint16_t value = static_cast<std::uint16_t>(it[0x39] | it[0x3A] << 8);
    if (value > 0) price = static_cast<std::uint16_t>(value / 2);
    if (count > 1 && !(it[0x34] & 0x20)) price = static_cast<std::uint16_t>(count * price);
    const std::string name = item_name(it);
    std::string s, q;
    if (price > 0) {
        s = text_[Text::SlGive] + std::to_string(price) + text_[Text::SlCreditsFor] + name;
        q = text_[Text::SlWillYou] + text_[count < 2 ? Text::SlIt : Text::SlThem];
    } else if (count < 2) {
        s = text_[Text::SlYour] + name + text_[Text::SlWorthless];
        q = text_[Text::SlDispose];
    } else {
        s = text_[Text::SlYour] + name + text_[Text::SlAreWorthless];
        q = text_[Text::SlDiscard];
    }
    print_in(1, 0x15, 0x26, 0x16, 0x0E, true, s);
    if (yes_no(q, 1) == 'Y') {
        show_message(text_[price > 0 ? Text::SlSold : count < 2 ? Text::SlSons : Text::SlRecycle]);
        remove_item_of(*cur_, owner, i);
        cur_->setd(0x2B, cur_->d(0x2B) + price);
    }
    clear_cells(1, 0x15, 0x26, 0x16);
}

void Game::view_trade() {
    // ViewTrade ovl18 +0x283C (0x527DC..0x528D8): credits to a member.
    // SelectMember(DS:30CA, Exit, alt) from DS:387C; nil or CurChar ends
    // it. Otherwise the sheet, then InputNumber(CS:281C, up to CurChar's
    // credits) again and again:
    // an amount moves its low word (TransferCredits ovl21 +0x01D6); 0 or
    // Esc goes back to the choice; no credits left ends it all.
    bool done = false;
    do {
        // DS:387C has three writers (main session 30, byte search):
        // ViewCharacter's entry 0x50F3E (:= CurChar), GearTrade 0x520B8 and
        // ViewTrade 0x5283F (a SelectMember result, never nil), and
        // ViewTrade's only caller is ViewCharacter. So the partner is a
        // member unless CurChar isn't one at VIEW's start, which the
        // original reaches through D-0053 / D-0055 and the port can't.
        Character* target = f387c_;
        if (!in_party(party_, target))
            throw Error("VIEW's Trade (ovl18 +0x283C) with DS:387C not a party member: unreachable in the port "
                        "(ViewCharacter 0x50F3E sets it to CurChar; D-0053, D-0055)");
        redraw_screen();
        select_member(text_[Text::VtTradeTo], true, true, target, false);
        if (!target || target == cur_) {
            done = true;
            break;
        }
        draw_sheet();
        bool again = false;
        do {
            sheet_credits();
            f387c_ = target;
            const std::int32_t n = input_credits(static_cast<std::int32_t>(cur_->d(0x2B)), text_[Text::VtAmount], 0x0A);
            if (n > 0) {
                const std::uint32_t w = static_cast<std::uint32_t>(n) & 0xFFFF;   // the low word only (quirk)
                cur_->setd(0x2B, cur_->d(0x2B) - w);
                target->setd(0x2B, target->d(0x2B) + w);
                again = cur_->d(0x2B) == 0;
            } else {
                again = true;
            }
            done = cur_->d(0x2B) == 0;
        } while (!again);
    } while (!done);
}

}  // namespace cotd::game
