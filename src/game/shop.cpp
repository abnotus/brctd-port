// SPDX-License-Identifier: GPL-3.0-or-later
// The SHOP opcode and the shop's screens (overlay 14): the stock as the
// booty list, Buy, Ammo, Sell through Gear, the pooled funds.
// docs/notes/shop.md; the delegated reading analysis/re/shop.md (local),
// the parts marked "main" read again in the main session (session 26).
#include <string>

#include "game/game.hpp"

namespace cotd::game {

namespace {

// Text.PadLeft 0653:15B2: spaces in front up to `width` (StrStore 40).
std::string pad_left(std::string s, std::size_t width) {
    if (s.size() < width) s.insert(0, width - s.size(), ' ');
    if (s.size() > 40) s.resize(40);
    return s;
}

// A Pascal string written into an item node's +0x00 (string[40]); the
// bytes past it keep their values.
void store_name(Bytes& node, const std::string& s, std::uint8_t length) {
    const std::size_t n = std::min<std::size_t>(s.size(), 40);
    std::copy(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n), node.begin() + 1);
    node[0] = length;
}

constexpr std::size_t kShopFirst = 0x0017;   // DS:0018.. (shop n's first index, [1..])
constexpr std::size_t kShopCount = 0x0027;   // DS:0028.. (its count)
constexpr std::size_t kShopTypes = 0x0037;   // DS:0038.. (the item types)
constexpr std::size_t kAmmoPrice = 0x0C46;   // DS:0C46 + 2 type: a round's (or a clip's) price

}  // namespace

void Game::op_shop(int n) {
    // Ecl.OpShop ovl2 +0x2BD4 (0x33424..0x33586, main): the booty list
    // DS:6984 freed; for k = first .. first + count - 1 (bytes) the type
    // DS:[0x37 + k]: unless ITEM0's record says 0xFF, a node with ITEM0's
    // 16 bytes at +0x2E, +0x29 := 0, prepended (so the last type comes
    // first). Its +0x00..+0x28 are SHOP's stack bytes (D-0009). No Random
    // call. Then ShopMenu.
    loot_.clear();
    const int first = text_.ds8(kShopFirst + static_cast<std::size_t>(n));
    const int last = (first + text_.ds8(kShopCount + static_cast<std::size_t>(n)) - 1) & 0xFF;
    if (first <= last)
        for (int k = first;; ++k) {
            const std::size_t t = text_.ds8(kShopTypes + static_cast<std::size_t>(k));
            // Types 1..254 past ITEM0's records read 0xFF: the start-up
            // loader fills the table with 0xFF first (0x4D48C); 0 and 255
            // read run-time memory (undefined), which no shipped shop has.
            if (t == 0 || t == 255)
                throw Error("D-0090: SHOP " + std::to_string(n) + " (ovl2 0x334BC): item type " + std::to_string(t) +
                            " reads memory outside ITEM0's table");
            if (16 * t <= item0_.size() && item0_[16 * (t - 1)] != 0xFF) {
                Bytes node(kItemSize, 0);
                std::copy(item0_.begin() + static_cast<std::ptrdiff_t>(16 * (t - 1)),
                          item0_.begin() + static_cast<std::ptrdiff_t>(16 * t), node.begin() + 0x2E);
                loot_.insert(loot_.begin(), node);
            }
            if (k == last) break;
        }
    shop_menu();
}

void Game::shop_menu() {
    // ShopMenu ovl14 +0x0F0C (0x4B2BC..0x4B5CF, main): state 1 (the old
    // one restored after), the first Redraw without the layout (DS:82E8),
    // the roster, the pool DS:6980 := 0, the stock's names; then the menu
    // until Exit. The key is DS:6B49 whether extended or not (only 'P'
    // tests DS:6B1E): F7 is Ammo, F8 Buy, Del Sell, Up and Down choose
    // CurChar. Esc does nothing.
    const std::uint8_t saved = mode_;
    mode_ = 1;
    f82e8_ = 0;
    redraw_screen();
    f82e8_ = 1;
    draw_roster();
    loot_value_ = 0;
    for (Bytes& node : loot_) item_name(node);
    bool done = false;
    while (!done) {
        const bool cred = loot_value_ > 0;             // BootyLeft ovl21 +0x04E4
        std::string items = text_[Text::ShItems];
        if (cred) items += text_[Text::ShTakeDivvy];
        items += text_[Text::ShExit];
        f8433_ = 1;                                    // 0x4B3B9
        const MenuKey mk = menu_screen("", items, true, false);
        f8433_ = 0;
        switch (mk.key) {
            case 'A':
                shop_ammo();
                break;
            case 'B':
                shop_buy();
                redraw_screen();
                draw_roster();
                break;
            case 'S': {
                highlight_ = 0xFF;                     // DS:6B1D, DS:6B48 := 0xFF: Gear opens on Sell
                menu_sum_ = 0xFF;
                std::uint8_t flag = 0;
                view_gear(flag);
                redraw_screen();
                draw_roster();
                break;
            }
            case 'V':
                view_character(f6b1e_);
                redraw_screen();
                draw_roster();
                break;
            case 'T':
                take_credits();
                fill_rect(1, 0x11, 0x26, 0x16, 0);    // Text.ClearRect
                break;
            case 'P':
                if (!mk.ext) {
                    pool_credits();
                } else {
                    roster_row(cur_, false);
                    select_char(mk.key);
                    roster_row(cur_, true);
                }
                break;
            case 'D':
                divvy_credits();
                break;
            case 'E':
                if (loot_value_ <= 0) {
                    done = true;
                } else {
                    // 0x4B4DF: money left in the pool.
                    print_in(1, 0x11, 0x26, 0x16, 0x0A, true, text_[Text::ShLeaving]);
                    print_in(1, 0x11, 0x26, 0x16, 0x0F, false, text_[Text::ShGoBack]);
                    if (yes_no("", 1) == 'N') {
                        done = true;                   // the pool stays (the tip)
                        print_in(1, 0x11, 0x26, 0x16, 0x0F, true, text_[Text::ShThanks]);
                        game_delay();
                    } else {
                        fill_rect(1, 0x11, 0x26, 0x16, 0);
                    }
                }
                break;
            case 0x48:                                 // Up
                roster_row(cur_, false);
                select_char(mk.key);
                roster_row(cur_, true);
                break;
            default:
                break;
        }
    }
    mode_ = saved;                                     // 0x4B5C6: nothing redrawn
}

std::int32_t Game::shop_price(const Bytes& item) {
    // Price ovl14 +0x00D1 (0x4A481, main): the value (+0x39, word) times
    // var 0x7F6D (byte) divided by 16 in LongInt; var 0 the value itself.
    const std::int32_t value = item[0x39] | item[0x3A] << 8;
    const std::int32_t m = v1(0x7F6D);
    return m == 0 ? value : value * m / 16;
}

void Game::shop_funds() {
    // ShowFunds ovl14 +0x0020 (0x4A3D0, main): StrInt (plain), padded to 10.
    draw_string(1, 0x14, 15, 0, text_[Text::ShFunds] + pad_left(std::to_string(static_cast<std::int32_t>(cur_->d(0x2B))), 10));
    draw_string(1, 0x15, 15, 0, text_[Text::ShPooled] + pad_left(std::to_string(loot_value_), 10));
}

int Game::shop_buy_list(int& index, int& picked, bool& redraw) {
    // BuyList ovl14 +0x012E (0x4A4DE..0x4A699, main for the lines): each
    // stock line is 38 spaces, the price (StrInt) ending at position 38,
    // the name over the left; it is written into the node (length 0x26),
    // then ListMenu(ShItemsPrompt, ShBuy, 1, 1, 0x26, 0x12, exit), DS:6B1D :=
    // 0 first; the names rebuilt after.
    std::vector<ListNode> list;
    for (Bytes& node : loot_) {
        if ((node[0x39] | node[0x3A] << 8) == 0) {     // 0x4A511: a value of 0 becomes 1
            node[0x39] = 1;
            node[0x3A] = 0;
        }
        std::string s = text_[Text::ShSpaces];
        const std::string t = std::to_string(shop_price(node));
        s.replace(38 - t.size(), t.size(), t);
        const std::string name(node.begin() + 1, node.begin() + 1 + node[0]);
        if (name.size() > 38) throw Error("BuyList (ovl14 0x4A5A5): a name longer than its line");
        s.replace(0, name.size(), name);
        store_name(node, s, 0x26);
        list.push_back({s, false});
    }
    highlight_ = 0;                                    // 0x4A604
    const int key = list_menu(text_[Text::ShItemsPrompt], text_[Text::ShBuy], 1, 1, 0x26, 0x12, list, true, redraw,
                              index, picked);
    for (Bytes& node : loot_) item_name(node);
    return key;
}

void Game::shop_buy() {
    // Buy ovl14 +0x04C9 (0x4A879..0x4AA78, main for the payments):
    // LayoutState0, then the funds and the list until a key other than
    // 'B' or Enter. CurChar pays from its own credits; otherwise its whole
    // purse and the pool; else ShNoMoney. The item goes through
    // GiveItem (nothing charged when it is refused).
    layout_state0();
    bool redraw = true;                                // DS:3876 := 1
    int index = 0, picked = loot_.empty() ? -1 : 0;
    for (;;) {
        shop_funds();
        const int key = shop_buy_list(index, picked, redraw);
        if (key != 'B' && key != kEnter) return;
        if (picked < 0) return;
        const Bytes& sel = loot_[static_cast<std::size_t>(picked)];
        std::int32_t price = shop_price(sel);
        Character& c = *cur_;
        const std::int32_t purse = static_cast<std::int32_t>(c.d(0x2B));
        bool paid = false;
        if (price <= purse) {
            if (give_item(c, sel)) {
                c.setd(0x2B, static_cast<std::uint32_t>(purse - price));
                paid = true;
            }
        } else if (price <= purse + loot_value_) {
            if (give_item(c, sel)) {
                price -= purse;
                c.setd(0x2B, 0);
                loot_value_ -= price;
                paid = true;
            }
        } else {
            show_message(text_[Text::ShNoMoney]);
        }
        if (paid) {
            data_note("buy", sel[0x2E]);                // tools only: --data-trace
            const std::string name(sel.begin() + 1, sel.begin() + 1 + sel[0]);
            show_message(c.name() + text_[sel[0x38] > 0 ? Text::ShBuys : Text::ShBuysA] + name);
        }
    }
}

void Game::shop_ammo_calc(const Bytes& w, int key, int& max, int& amount, int& price) const {
    // AmmoCalc ovl14 +0x06E9 (0x4AA99..0x4ABCE): kinds 7 and 9 (set
    // CS:06C9) hold 10 and a clip is 1; the others hold 250 and a clip is
    // 10 rounds, a fill rounded up to tens; the price per unit from
    // DS:0C46 + 2 type, by the type, not the kind (0x4ABB4; no shop multiplier).
    const int kind = item_byte(w[0x2E], 2);
    const bool few = kind == 7 || kind == 9;
    max = few ? 10 : 250;
    amount = few ? 1 : 10;
    if (key == 'F') amount = max;
    const int room = (max - w[0x38]) & 0xFFFF;         // Min ovl23 +0x309C: unsigned
    if (room < amount) amount = room;
    amount &= 0xFF;
    const int units = few ? amount : (amount + 9) / 10;
    price = (units * text_.ds16(kAmmoPrice + 2 * static_cast<std::size_t>(w[0x2E]))) & 0xFFFF;
}

void Game::shop_ammo() {
    // Ammo ovl14 +0x089C (0x4AC4C..0x4B1ED): CurChar's weapons that use
    // rounds (ITEMS byte 0 = 0, byte 2 not 0 or 8, not stacking), each
    // name with its clip and fill prices appended (in CurChar's own item,
    // a quirk), in a list of their own; C buys a clip, F fills; until Esc
    // or Exit. Then Screen.Redraw and the roster.
    bool redraw = true;                                // DS:3876 := 1
    int index = 0;
    for (;;) {
        Character& c = *cur_;
        if (c.items.empty()) return;                   // 0x4AC82: no message, no redraw
        std::vector<int> weapons;
        std::vector<ListNode> list;
        for (std::size_t i = 0; i < c.items.size(); ++i) {
            Bytes& it = c.items[i];
            const int type = it[0x2E];
            if (item_byte(type, 0) != 0 || item_byte(type, 2) == 0 || item_byte(type, 2) == 8 || !(it[0x34] & 0x20))
                continue;
            weapons.push_back(static_cast<int>(i));
            std::string name = item_name(it);
            int max, amount, price;
            shop_ammo_calc(it, 'C', max, amount, price);
            std::string p = std::to_string(price);
            name = (name + pad_left(p, 0x20 > name.size() ? 0x20 - name.size() : 0)).substr(0, 40);
            shop_ammo_calc(it, 'F', max, amount, price);
            name = (name + pad_left(std::to_string(price), 6)).substr(0, 40);
            store_name(it, name, static_cast<std::uint8_t>(name.size()));
            list.push_back({name, false});
        }
        if (list.empty()) {
            show_message(c.name() + text_[Text::AmNoMissile]);
            return;                                    // the screen as it was
        }
        layout_state0();
        shop_funds();
        draw_string(1, 2, 15, 0, text_[Text::AmWeapon]);
        draw_string(0x1D, 2, 15, 0, text_[Text::AmClipFill]);
        f8433_ = 0;
        int picked = -1;
        const int key = list_menu("", text_[Text::AmItems], 1, 3, 0x26, 0x13, list, true, redraw, index, picked);
        f8433_ = 1;
        if (key == 0 || key == kEsc) break;
        if ((key == 'C' || key == 'F') && index >= 0 && index < static_cast<int>(weapons.size())) {
            Bytes& w = c.items[static_cast<std::size_t>(weapons[static_cast<std::size_t>(index)])];
            int max, amount, price;
            shop_ammo_calc(w, key, max, amount, price);
            if (w[0x38] >= max) {
                show_message(text_[Text::AmFull]);
            } else {
                std::int32_t purse = static_cast<std::int32_t>(c.d(0x2B));
                if (price > purse && price <= purse + loot_value_) {   // 0x4B01A: the pool tops the purse up
                    loot_value_ -= price - purse;
                    purse = price;
                    c.setd(0x2B, static_cast<std::uint32_t>(purse));
                }
                if (price <= purse) {
                    c.setd(0x2B, static_cast<std::uint32_t>(purse - price));
                    show_message(c.name() + text_[key == 'C' ? Text::AmClip : Text::AmReloaded]);
                    if (w[0x38] < max) {
                        int n = w[0x38] + amount;
                        if (n > max) n = max;
                        w[0x38] = static_cast<std::uint8_t>(n);
                    }
                    item_name(w);
                } else {
                    show_message(text_[Text::AmNoMoney]);
                }
            }
            redraw = true;                             // 0x4B197
        }
    }
    redraw_screen();
    draw_roster();
}

}  // namespace cotd::game
