// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/logbook_txt.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/common.hpp"

namespace cotd {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// "ENTRY n" (case-blind): n, or -1 when the line isn't a heading; -2 for
// a heading whose number is out of range.
int heading(const std::string& line) {
    const std::string t = trim(line);
    if (t.size() < 7) return -1;
    std::string w;
    for (int i = 0; i < 5; ++i) w += static_cast<char>(std::toupper(static_cast<unsigned char>(t[static_cast<std::size_t>(i)])));
    if (w != "ENTRY" || t[5] != ' ') return -1;
    const std::string num = trim(t.substr(6));
    if (num.empty() || num.size() > 4) return -1;
    for (const char c : num)
        if (!std::isdigit(static_cast<unsigned char>(c))) return -1;
    const int n = std::stoi(num);
    return n >= 1 && n <= 999 ? n : -2;
}

}  // namespace


LogbookText LogbookText::from_text(const std::string& name, const std::string& text) {
    LogbookText t;
    t.path_ = name;
    std::istringstream in(text);
    t.parse(in);
    return t;
}

void LogbookText::parse(std::istream& in) {
    const std::string& path = path_;
    std::string line;
    int no = 0, cur = 0;
    LogbookEntry* e = nullptr;
    LogbookMap* map = nullptr;   // the map block being read
    std::string para;
    std::size_t chars = 0;
    bool cut = false;
    auto where = [&](int ln) { return path + " line " + std::to_string(ln) + (cur ? ", entry " + std::to_string(cur) : ""); };
    auto end_para = [&]() {
        if (e && !para.empty()) e->paragraphs.push_back(para);
        para.clear();
    };
    while (std::getline(in, line)) {
        ++no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (!t.empty() && t[0] == '#') continue;
        const int h = heading(t);
        if (h == -2) {
            end_para();
            e = nullptr;
            cur = 0;
            diag_.push_back(where(no) + ": a heading's number outside 1..999");
            continue;
        }
        if (h > 0) {
            map = nullptr;
            end_para();
            cur = h;
            chars = 0;
            cut = false;
            if (entries_.count(h)) {
                diag_.push_back(where(no) + ": entry " + std::to_string(h) + " given again (the first is kept)");
                e = nullptr;
            } else {
                e = &entries_[h];
                e->line = no;
            }
            continue;
        }
        if (!e) continue;
        if (t.empty()) {
            map = nullptr;
            end_para();
            continue;
        }
        // A map block: [MAP GEOn.DAX entry x=a..b y=c..d], then its TITLE,
        // KEY and LABEL x,y lines; any other line ends it.
        if (t.rfind("[MAP ", 0) == 0 && t.back() == ']') {
            end_para();
            LogbookMap m;
            m.line = no;
            char geo[32] = {};
            if (std::sscanf(t.c_str(), "[MAP %31s %d x=%d..%d y=%d..%d", geo, &m.entry, &m.x0, &m.x1, &m.y0, &m.y1) != 6) {
                diag_.push_back(where(no) + ": a [MAP GEOn.DAX entry x=a..b y=c..d] marker it can't read");
                map = nullptr;
                continue;
            }
            m.geo = geo;
            if (const auto f = t.find(" from="); f != std::string::npos)
                std::sscanf(t.c_str() + f + 6, "%d,%d", &m.fx, &m.fy);
            if (const auto d = t.find(" doors="); d != std::string::npos) {
                const std::size_t end = t.find_first_of(" ]", d + 7);
                std::istringstream ds(t.substr(d + 7, end - d - 7));
                for (std::string n; std::getline(ds, n, ',');) {
                    if (n.empty() || !std::isdigit(static_cast<unsigned char>(n[0]))) continue;
                    // Session 74: a number too long for an int is a diagnostic, not the end of play.
                    if (n.size() > 6) {
                        diag_.push_back(where(no) + ": a doors= number it can't read: " + n.substr(0, 12));
                        continue;
                    }
                    m.doors.push_back(std::stoi(n));
                }
            }
            if (m.x0 < 0 || m.y0 < 0 || m.x1 > 15 || m.y1 > 15 || m.x0 > m.x1 || m.y0 > m.y1) {
                diag_.push_back(where(no) + ": the map's window is off the 16 x 16 area");
                map = nullptr;
                continue;
            }
            if (m.x1 - m.x0 > 10 || m.y1 - m.y0 > 10) {
                diag_.push_back(where(no) + ": the map's window is larger than AREA's 11 x 11 squares; cut to it");
                m.x1 = std::min(m.x1, m.x0 + 10);
                m.y1 = std::min(m.y1, m.y0 + 10);
            }
            e->maps.push_back(m);
            e->paragraphs.push_back(std::string(1, kMap) + std::to_string(e->maps.size() - 1));
            map = &e->maps.back();
            continue;
        }
        if (map) {
            auto clean = [&](std::string v, int col) {
                for (std::size_t i = 0; i < v.size(); ++i)
                    if (!drawable(static_cast<unsigned char>(v[i]))) {
                        diag_.push_back(where(no) + ", column " + std::to_string(col + static_cast<int>(i) + 1) +
                                        ": a character the game's font can't draw (0x" +
                                        hex(static_cast<unsigned char>(v[i]), 2) + "), shown as a space");
                        v[i] = ' ';
                    }
                return v;
            };
            if (t.rfind("TITLE ", 0) == 0) {
                map->title = clean(t.substr(6), 6);
                continue;
            }
            if (t.rfind("KEY ", 0) == 0) {
                map->key.push_back(clean(t.substr(4), 4));   // "@X": a mark's picture
                continue;
            }
            int gx = 0, gy = 0;
            char gd = 0;
            if (t.rfind("TUBE ", 0) == 0) {
                if (std::sscanf(t.c_str(), "TUBE %d,%d", &gx, &gy) == 2) map->tubes.push_back({gx, gy});
                else diag_.push_back(where(no) + ": a TUBE x,y line it can't read");
                continue;
            }
            if (t.rfind("GUN ", 0) == 0) {
                if (std::sscanf(t.c_str(), "GUN %d,%d %c", &gx, &gy, &gd) == 3 && std::strchr("NESW", gd))
                    map->guns.push_back({gx, gy, gd});
                else
                    diag_.push_back(where(no) + ": a GUN x,y N|E|S|W line it can't read");
                continue;
            }
            LogbookMap::Arrow ar{};
            if (t.rfind("DOTS ", 0) == 0) {   // a dotted line (the book's controls)
                ar.dots = true;
                if (std::sscanf(t.c_str(), "DOTS %f,%f %f,%f", &ar.x1, &ar.y1, &ar.x2, &ar.y2) == 4)
                    map->arrows.push_back(ar);
                else
                    diag_.push_back(where(no) + ": a DOTS x1,y1 x2,y2 line it can't read");
                continue;
            }
            if (t.rfind("ARROW ", 0) == 0) {
                if (std::sscanf(t.c_str(), "ARROW %f,%f %f,%f", &ar.x1, &ar.y1, &ar.x2, &ar.y2) == 4)
                    map->arrows.push_back(ar);
                else
                    diag_.push_back(where(no) + ": an ARROW x1,y1 x2,y2 line it can't read");
                continue;
            }
            LogbookMap::Hide hd{};
            if (t.rfind("HIDE ", 0) == 0) {
                if (std::sscanf(t.c_str(), "HIDE x=%d..%d y=%d..%d", &hd.x0, &hd.x1, &hd.y0, &hd.y1) == 4)
                    map->hidden.push_back(hd);
                else
                    diag_.push_back(where(no) + ": a HIDE x=a..b y=c..d line it can't read");
                continue;
            }
            int st = 0, sn = 0;
            if (t.rfind("SYMBOL ", 0) == 0 && std::sscanf(t.c_str(), "SYMBOL type=%d %n", &st, &sn) == 1 && sn > 0) {
                const std::string raw = t.substr(static_cast<std::size_t>(sn));
                const std::string text = raw == "~" ? raw : clean(raw, sn);
                // One character; "~" draws those walls dashed instead.
                if (text.size() != 1) diag_.push_back(where(no) + ": a symbol is one character; the first is kept");
                if (!text.empty()) map->symbols.push_back({st, text.substr(0, 1)});
                continue;
            }
            int lx = 0, ly = 0, n = 0;
            if (t.rfind("LABEL ", 0) == 0 && std::sscanf(t.c_str(), "LABEL %d,%d %n", &lx, &ly, &n) == 2 && n > 0) {
                const std::string text = clean(t.substr(static_cast<std::size_t>(n)), n);
                if (lx < map->x0 || lx > map->x1 || ly < map->y0 || ly > map->y1) {
                    diag_.push_back(where(no) + ": a label off the map's window");
                } else if (lx + static_cast<int>(text.size()) - 1 > map->x1) {
                    diag_.push_back(where(no) + ": a label longer than the squares to the window's edge; cut");
                    map->labels.push_back({lx, ly, text.substr(0, static_cast<std::size_t>(map->x1 - lx + 1))});
                } else {
                    map->labels.push_back({lx, ly, text});
                }
                continue;
            }
            map = nullptr;   // any other line: the map's block is over
        }
        if (t.size() > 9 && t.rfind("[PICTURE ", 0) == 0 && t.back() == ']') {
            end_para();
            const std::string k = t.substr(9, t.size() - 10);
            bool digits = !k.empty();
            for (const char c : k) digits = digits && std::isdigit(static_cast<unsigned char>(c));
            (void)digits;
            diag_.push_back(where(no) + ": [PICTURE k] is not shown (session 64: no scan pictures); a [MAP ...] block draws a map");
            continue;
        }
        std::string s;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(t[i]);
            if (drawable(c)) {
                s += static_cast<char>(c);
                continue;
            }
            diag_.push_back(where(no) + ", column " + std::to_string(i + 1) + ": a character the game's font can't draw (0x" +
                            hex(c, 2) + "), shown as a space");
            s += ' ';
        }
        if (cut) continue;
        // A word broken at the line's end ("-" after a letter) joins the next line.
        if (para.size() >= 2 && para.back() == '-' && std::isalpha(static_cast<unsigned char>(para[para.size() - 2])))
            para.pop_back();
        else if (!para.empty())
            para += ' ';
        chars += s.size() + 1;
        if (chars > kMaxChars) {
            diag_.push_back(where(no) + ": the entry is longer than " + std::to_string(kMaxChars) +
                            " characters; the rest is left out");
            cut = true;
            continue;
        }
        para += s;
    }
    end_para();
}

const LogbookEntry* LogbookText::entry(int n) const {
    auto it = entries_.find(n);
    return it == entries_.end() ? nullptr : &it->second;
}

}  // namespace cotd
