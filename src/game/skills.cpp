// SPDX-License-Identifier: GPL-3.0-or-later
// Skills: the rules (docs/notes/skills.md; model tools/skills.py), the
// allocation screens AllocateSkills (ovl19 +0x0D90) and WeaponSpecialize
// (ovl18 +0x0AA5) (docs/notes/frontend.md).
#include <algorithm>

#include "game/game.hpp"

namespace cotd::game {

namespace {

constexpr std::size_t kSkillTable = 0x13EB;   // DS: 27-byte records: name string[24], flag +0x19, ability +0x1A
constexpr std::size_t kCareerList = 0x0EA5;   // DS: [class*8 + i], i = 1..8
constexpr std::size_t kPageTitles = 0x1373;   // DS: string[20] per page (0x15 bytes)

}  // namespace

// --- rules -----------------------------------------------------------------------------

int Game::skill_ability(int k) const { return text_.ds8(kSkillTable + 27 * static_cast<std::size_t>(k) + 0x1A); }
int Game::skill_flag(int k) const { return text_.ds8(kSkillTable + 27 * static_cast<std::size_t>(k) + 0x19); }
std::string Game::skill_name(int k) const { return text_.ds_str(kSkillTable + 27 * static_cast<std::size_t>(k), 24); }
int Game::career_skill(int cls, int i) const { return text_.ds8(kCareerList + static_cast<std::size_t>(cls) * 8 + static_cast<std::size_t>(i)); }

bool Game::skill_prereq_ok(const Character& c, int k) const {
    // ovl23 +0x2C47; all comparisons "greater than".
    auto g = [&](int i) { return static_cast<int>(c.b(0x4C + static_cast<std::size_t>(i))); };
    switch (k) {
        case 3: case 5: return g(2) > 9;
        case 4: return g(1) > 29 && g(2) > 24;
        case 6: return g(1) > 9 && g(2) > 9;
        case 15: return g(1) > 39;
        case 26: return g(77) > 19 || g(68) > 19 || g(38) > 19;
        case 43: return g(27) > 9;
        case 46: return g(36) > 19 && g(27) > 24;
        case 47: return g(36) > 14 && g(27) > 24;
        case 75: return g(74) > 19;
        case 84: return g(30) > 9;
        default: break;
    }
    if (k >= 16 && k <= 23) {
        if (c.b(0x28) != 2) return false;
        if (k == 18) return g(17) > 29;
        if (k == 19) return g(18) > 39;
        if (k == 20) return g(23) > 14;
        if (k == 22) return g(23) > 24;
    }
    return true;
}

int Game::free_skill_level(const Character& c, int k) const {
    // FreeSkillLevel ovl19 +0x0000.
    if (!skill_prereq_ok(c, k)) return 0;
    const int cls = c.b(0x28);
    if (cls == 1 && k > 50 && k < 60) return 10;
    if (cls == 5) {
        for (int r : {7, 8, 61, 62, 63, 65, 75, 76, 82, 83})
            if (k == r) return 10;
    }
    return 0;
}

int Game::skill_bonus(const Character& c, int k) const {
    // SkillBonus ovl23 +0x2DFC: the governing ability's current score.
    return skill_prereq_ok(c, k) ? c.b(0x10 + static_cast<std::size_t>(skill_ability(k))) : 0;
}

int Game::skill_total(const Character& c, int k) const {
    // SkillTotal ovl23 +0x2E3F: 0 with no points or +0xD5 = 0.
    const int pts = c.b(0x4C + static_cast<std::size_t>(k));
    if (!pts || !c.b(0xD5)) return 0;
    return (pts + skill_bonus(c, k)) & 0xFF;   // shown with StrByte
}

void Game::init_skills(Character& c) {
    // InitSkills ovl13 +0x0663: k = 1..85 (85 writes record byte 0xA1:
    // D-0001).
    for (int k = 1; k <= 85; ++k) c.b(0x4C + static_cast<std::size_t>(k)) = static_cast<std::uint8_t>(free_skill_level(c, k));
}

void Game::init_career_skills(Character& c) {
    // InitCareerSkills ovl13 +0x05FF (0x46F0F): the 8 career skills.
    for (int i = 1; i <= 8; ++i) {
        const int k = career_skill(c.b(0x28), i);
        c.b(0x4C + static_cast<std::size_t>(k)) = static_cast<std::uint8_t>(free_skill_level(c, k));
    }
}

// --- AllocateSkills ----------------------------------------------------------------------

void Game::build_skill_lists(const Character& c) {
    // BuildSkillLists ovl19 +0x0086 (0x531E6). Page 0 is the career list;
    // pages 1, 3..6 the general skills by governing ability, in id order;
    // page 2 all general skills (view mode).
    skill_list_.fill(0);
    skill_first_.fill(0);
    skill_last_.fill(0);
    skill_first_[0] = 1;
    skill_last_[0] = 8;
    const int cls = c.b(0x28);
    for (int i = 1; i <= 8; ++i) skill_list_[static_cast<std::size_t>(i)] = career_skill(cls, i);
    int cur = skill_ability(1);
    skill_first_[static_cast<std::size_t>(cur)] = 9;
    int off = 8;
    open_general_ = 0;
    for (int k = 1; k <= 84; ++k) {
        bool career = false;
        for (int i = 1; i <= 8; ++i) career = career || career_skill(cls, i) == k;
        if (career || (k >= 16 && k <= 23) || (skill_view_ && skill_total(c, k) == 0) || skill_flag(k) == 0) {
            --off;
            continue;
        }
        if (c.b(0x4C + static_cast<std::size_t>(k)) > 0) ++open_general_;
        skill_list_[static_cast<std::size_t>(off + k)] = k;
        if (skill_ability(k) != cur) {
            skill_last_[static_cast<std::size_t>(cur)] = off + k - 1;
            cur = skill_ability(k);
            skill_first_[static_cast<std::size_t>(cur)] = off + k;
        }
    }
    skill_last_[static_cast<std::size_t>(cur)] = off + 84;
    skill_first_[2] = 9;
    skill_last_[2] = off + 84;
}

void Game::allocate_skills(int career, int general, int max) {
    // AllocateSkills ovl19 +0x0D90 (0x53EF0), keyboard path.
    Character& c = *cur_;
    skill_view_ = max == 0;
    const bool view = skill_view_;
    build_skill_lists(c);
    if (view && open_general_ == 0) {
        show_message(c.name() + text_[Text::NoGeneral]);
        return;
    }
    const std::uint8_t saved_mode = mode_;
    if (!view) mode_ = 0x0B;
    const int limit = c.b(0x29) == 2 ? 7 : open_general_ + 1;
    bool exit = false;
    int page = view ? 2 : 0;
    auto rows_of = [&](int p) { return skill_last_[static_cast<std::size_t>(p)] - skill_first_[static_cast<std::size_t>(p)] + 1; };
    int nrows = rows_of(page);
    int cur = 1, old = 1;
    int shown = career;
    bool redraw = false;
    std::array<int, 85> start{};
    for (int k = 1; k <= 84; ++k) start[static_cast<std::size_t>(k)] = c.b(0x4C + static_cast<std::size_t>(k));
    auto rec = [&](int k) -> std::uint8_t& { return c.b(0x4C + static_cast<std::size_t>(k)); };
    auto skill_at = [&](int i) { return skill_list_[static_cast<std::size_t>(skill_first_[static_cast<std::size_t>(page)] + i - 1)]; };

    auto points_line = [&](int pts) {
        // ovl19 +0x02B4: Str(pts) + " " at (23,1) in white.
        draw_string(23, 1, 15, 0, std::to_string(pts & 0xFF) + " ");
    };
    auto draw_row = [&](int i, bool hl, int cur_row) {
        // DrawSkillRow ovl19 +0x0396 (0x534F6).
        const int k = skill_at(i);
        const int row = view ? i + 3 : i + 5;
        int colour = skill_prereq_ok(c, k) ? 10 : 14;
        if (hl && i == cur_row && !view) {
            clear_cells(1, 0x15, 0x26, 0x16);
            if (!skill_prereq_ok(c, k)) {
                // The "Needs" lines (ovl19 +0x02FE), from row 0x15.
                int line = 0x15;
                auto need = [&](int s, int n) {
                    if (rec(s) < n) {
                        draw_string(1, line, 12, 0, text_[Text::SkNeeds] + std::to_string(n) + text_[Text::SkPointsIn] +
                                                        skill_name(s).substr(0, 18));
                        ++line;
                    }
                };
                switch (k) {
                    case 3: case 5: need(2, 10); break;
                    case 4: need(2, 25); need(1, 30); break;
                    case 6: need(2, 10); need(1, 10); break;
                    case 15: need(1, 40); break;
                    case 18: need(17, 30); break;
                    case 19: need(18, 40); break;
                    case 20: need(23, 15); break;
                    case 22: need(23, 25); break;
                    case 43: need(27, 10); break;
                    case 46: need(36, 20); need(27, 25); break;
                    case 47: need(36, 15); need(27, 25); break;
                    case 75: need(74, 20); break;
                    case 84: need(30, 10); break;
                    default: break;
                }
            }
            colour = 13;
        }
        draw_string(1, row, colour, 0, skill_name(k));
        if (view) {
            draw_string(26, row, colour, 0, std::to_string(skill_total(c, k)));
        } else {
            draw_string(23, row, colour, 0, std::to_string(rec(k)) + " ");
            draw_string(29, row, colour, 0, std::to_string(skill_bonus(c, k)) + " ");
            draw_string(35, row, colour, 0, std::to_string(skill_total(c, k)) + " ");
        }
    };
    auto draw_page = [&](int pts, bool full) {
        // DrawSkillPage ovl19 +0x0723 (0x53883).
        if (full) {
            layout_state0();
            if (view) {
                draw_string(2, 2, 10, 0, text_[Text::SkGeneralTitle]);
            } else {
                draw_string(1, 1, 10, 0, text_[page == 0 ? Text::SkCareerPts : Text::SkGeneralPts]);
                points_line(pts);
                draw_string(1, 2, 10, 0, text_[Text::SkMaxPer]);
                draw_string(23, 2, 15, 0, std::to_string(max));
                draw_string(1, 4, 10, 0, text_.ds_str(kPageTitles + 0x15 * static_cast<std::size_t>(page), 20));
                draw_string(21, 4, 10, 0, text_[Text::SkColumns]);
            }
            for (int i = 1; i <= nrows; ++i) draw_row(i, false, cur);
        }
        if (cur != old) draw_row(old, true, cur);
        draw_row(cur, true, cur);
    };
    auto page_next = [&](bool next) {
        // SkillPageNext ovl19 +0x0B77 (0x53CD7).
        if (next) {
            static constexpr int kNext[7] = {6, 5, 0, 1, 6, 4, 3};
            if (page == 0) shown = general;
            page = kNext[page];
        } else {
            if (page == 0) return;
            static constexpr int kPrev[7] = {0, 3, 0, 6, 5, 1, 4};
            page = kPrev[page];
        }
        nrows = rows_of(page);
        redraw = true;
        old = 1;
        cur = 1;
    };
    auto dependents = [&](int k, bool add) {
        // SkillDependents ovl19 +0x09D8 / DepCheck +0x08AE (0x53A0E).
        bool rebuild = false;
        auto dep = [&](int t, int d) {
            const int f = free_skill_level(c, d);
            if (add) {
                if (rec(k) == t) {
                    rec(d) = static_cast<std::uint8_t>(f);
                    start[static_cast<std::size_t>(d)] = f;
                    rebuild = true;
                }
            } else if (rec(k) == t - 1) {
                start[static_cast<std::size_t>(d)] = f;
                const int diff = static_cast<std::int8_t>(rec(d) - f);
                if (diff > 0) {
                    shown += diff;
                    if (page == 0) career += diff;
                    else general += diff;
                }
                rec(d) = 0;
                rebuild = true;
            }
        };
        switch (k) {
            case 1: dep(10, 6); dep(30, 4); dep(40, 15); break;
            case 2: dep(10, 6); dep(10, 5); dep(25, 4); dep(10, 3); break;
            case 17: dep(30, 18); break;
            case 18: dep(40, 19); break;
            case 23: dep(25, 22); dep(15, 20); break;
            case 27: dep(25, 47); dep(25, 46); dep(10, 43); break;
            case 30: dep(10, 84); break;
            case 36: dep(15, 47); dep(20, 46); break;
            case 38: if (rec(68) < 10 && rec(77) < 10) dep(10, 26); break;
            case 68: case 77: dep(10, 26); break;
            case 74: dep(20, 75); break;
            default: break;
        }
        return rebuild;
    };

    draw_page(career, true);
    redraw = false;
    do {
        std::string items;
        if (!view) {
            items += text_[Text::SkAddSub];
            if (page != 0) items += text_[Text::SkPrevNext];
        }
        items += text_[Text::SkDone];
        MenuKey mk = menu_screen("", items, true);
        if (static_cast<std::int8_t>(f6b44_) > 0 && static_cast<std::int8_t>(f6b44_) <= nrows) {
            // 0x54095 (main): a click on a skill row (DS:6B44, MenuScreen's
            // SkillRowPick) selects it; left adds, right subtracts.
            mk.ext = false;
            if (mk.key == kEnter) {
                mk.key = 'A';
                highlight_ = 1;
            } else if (mk.key == kEsc) {
                mk.key = 'S';
                highlight_ = 2;
            } else {
                mk.key = 0;
            }
            if (f6b44_ != cur) {
                old = cur;
                cur = f6b44_;
                draw_page(shown, redraw);
                redraw = false;
            }
            f6b44_ = 0xFF;
        }
        const int k = skill_at(cur);
        if (mk.ext) {
            if (mk.key == 0x48) {
                old = cur;
                cur = cur == 1 ? nrows : cur - 1;
            } else if (mk.key == 0x50) {
                old = cur;
                cur = cur == nrows ? 1 : cur + 1;
            } else if (mk.key == 0x51 || mk.key == 0x49) {
                if (page != 0 && page != 2) page_next(mk.key == 0x51);
            } else if (mk.key == 0x1B) {
                exit = true;
            }
            draw_page(shown, redraw);
            redraw = false;
        } else if (mk.key == 'A') {
            // ADD (0x541DA).
            const int fl = free_skill_level(c, k);
            if (page == 0) {
                if (career == 0) show_message(text_[Text::SkNoCareer]);
                else if (!skill_prereq_ok(c, k)) show_message(text_[Text::SkPrereq]);
                else if (rec(k) >= start[static_cast<std::size_t>(k)] + max) show_message(text_[Text::SkMaxLevel]);
                else {
                    --career;
                    --shown;
                    ++rec(k);
                    if (dependents(k, true)) {
                        redraw = true;
                        build_skill_lists(c);
                        nrows = rows_of(page);
                    }
                }
            } else {
                if (general == 0) show_message(text_[Text::SkNoGeneralPts]);
                else if (rec(k) >= 80) show_message(text_[Text::SkMaxGeneral]);
                else if (rec(k) >= start[static_cast<std::size_t>(k)] + max) show_message(text_[Text::SkMaxLevel]);
                else if (!skill_prereq_ok(c, k)) show_message(text_[Text::SkPrereq]);
                else if (rec(k) == fl && static_cast<std::int8_t>(open_general_) >= limit) show_message(text_[Text::SkNoNew]);
                else {
                    if (rec(k) == fl) ++open_general_;
                    --general;
                    --shown;
                    ++rec(k);
                    if (dependents(k, true)) {
                        redraw = true;
                        build_skill_lists(c);
                        nrows = rows_of(page);
                    }
                }
            }
            draw_page(shown, redraw);
            redraw = false;
            points_line(shown);
        } else if (mk.key == 'S') {
            // SUBTRACT (0x5447E).
            if (rec(k) > start[static_cast<std::size_t>(k)]) {
                if (page == 0) ++career;
                else ++general;
                ++shown;
                --rec(k);
                if (rec(k) == free_skill_level(c, k)) --open_general_;   // on any page (quirk)
                if (dependents(k, false)) {
                    redraw = true;
                    build_skill_lists(c);
                    nrows = rows_of(page);
                }
                draw_page(shown, redraw);
                redraw = false;
                points_line(shown);
            }
        } else if (mk.key == 'D' || mk.key == 0 || mk.key == 0x1B) {
            // DONE (0x54542).
            if (page == 0) {
                if (career == 0 || yes_no(text_[Text::SkCareerUnspent], 2) == 'Y') {
                    page_next(true);
                    draw_page(shown, redraw);
                    redraw = false;
                }
            } else {
                exit = true;
            }
        } else if (mk.key == 'P' || mk.key == 'N') {
            page_next(mk.key == 'N');
            draw_page(shown, redraw);
            redraw = false;
        }
        if (exit && general != 0 && yes_no(text_[Text::SkGeneralUnspent], 2) == 'N') exit = false;
    } while (!exit);
    mode_ = saved_mode;
}

// --- WeaponSpecialize ----------------------------------------------------------------------

int Game::spec_count(const Character& c, int type) const {
    // SpecCount ovl23 +0x0711: j in 1..10 with record +0xB9+j = type.
    int n = 0;
    for (int j = 1; j <= 10; ++j)
        if (c.b(0xB9 + static_cast<std::size_t>(j)) == type) ++n;
    return n;
}

void Game::weapon_specialize(bool must_choose) {
    // WeaponSpecialize ovl18 +0x0AA5 (0x50A45).
    Character& c = *cur_;
    layout_state0();
    draw_string(6, 3, 10, 0, text_[Text::WpnTitle]);
    // The templates: ITEM0 types 15 down to 1 (prepended), except 0xFF.
    std::vector<int> types;
    for (int i = 15; i >= 1; --i)
        if (item_type(static_cast<std::size_t>(i)) && item_type(static_cast<std::size_t>(i))[0] != 0xFF) types.push_back(i);
    // No type to choose (ITEM0.DAX missing: a full entry without them is
    // rejected at load, check_item0): ListMenu would return with no key for
    // ever, as the original's does (0x50E49..0x50E55; D-0090).
    if (must_choose && types.empty())
        throw Error("D-0090: ITEM0.DAX #1: no item types 1..15; WeaponSpecialize (ovl18 +0x0AA5) would never read a key");
    int index = 0;
    bool redraw = true;
    bool chosen = false;
    for (;;) {
        std::string items = text_[Text::WpnTrain];
        std::string prompt = text_[Text::WpnPrompt];
        std::vector<ListNode> list;
        std::vector<int> shown;
        for (int t : types) {
            const std::uint8_t* rec16 = item_type(static_cast<std::size_t>(t));
            const int n = spec_count(c, rec16[0]);
            if (!must_choose && n == 0) continue;
            Bytes node(kItemSize, 0);
            std::copy(rec16, rec16 + 16, node.begin() + 0x2E);
            node[0x38] = 0;
            std::string name = item_name(node);
            if (n > 0) {
                while (name.size() < 25) name += text_[Text::WpnPad];
                name += text_[Text::WpnOpen] + std::to_string(n) + text_[Text::WpnClose];
            }
            list.push_back({name, false});
            shown.push_back(t);
        }
        if (chosen || !must_choose) {
            prompt.clear();
            items.clear();
        }
        int picked = -1;
        const int key = list_menu(prompt, items, 3, 5, 0x26, 0x16, list, !must_choose || chosen, redraw, index, picked);
        if (key == 'T' && picked >= 0) {
            const int id = item_type(static_cast<std::size_t>(shown[static_cast<std::size_t>(picked)]))[0];
            if (spec_count(c, id) < 3) {
                for (int j = 0; j < 10; ++j)
                    if (c.b(0xBA + static_cast<std::size_t>(j)) == 0) {
                        c.b(0xBA + static_cast<std::size_t>(j)) = static_cast<std::uint8_t>(id);
                        chosen = true;
                        break;
                    }
            } else {
                show_message(text_[Text::WpnMax]);
            }
        }
        if ((key == 0 || key == 0x1B || key == 'E') && (chosen || !must_choose)) return;
    }
}

}  // namespace cotd::game
