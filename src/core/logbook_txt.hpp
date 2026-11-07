// SPDX-License-Identifier: GPL-3.0-or-later
// LOGBOOK.TXT, the player's text of the Log Book (docs/formats/logbook_txt.md,
// DIVERGENCE.md D-0083): read and checked at start; the engine shows the
// entry a LOGBOOK call names as the game's own text.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace cotd {

// A map page (session 64): the window of a GEO entry, drawn with AREA's
// tiles, its words in the game's font over its squares.
struct LogbookMap {
    std::string geo;              // "GEO3.DAX"
    int entry = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<int> doors;       // the wall types drawn as doors (doors=a,b,...)
    int fx = -1, fy = -1;         // the square the floor is flooded from (from=x,y; default the window's middle)
    std::string title;
    std::vector<std::string> key;   // lines for the text window, under the title
    struct Label { int x, y; std::string text; };
    std::vector<Label> labels;    // squares in GEO coordinates
    struct Arrow { float x1, y1, x2, y2; bool dots = false; };
    struct Gun { int x, y; char dir; };
    std::vector<std::pair<int, int>> tubes;   // TUBE x,y: the connecting tube (the lift), a circle with T
    std::vector<Gun> guns;        // GUN x,y DIR: a weapon mount, its barrels to N, E, S or W
    std::vector<Arrow> arrows;    // ARROW x1,y1 x2,y2: an arrow, its head at the second point (squares)
    struct Hide { int x0, y0, x1, y1; };
    std::vector<Hide> hidden;     // HIDE x=a..b y=c..d: squares the book leaves blank
    struct Symbol { int type; std::string text; };
    std::vector<Symbol> symbols;  // SYMBOL type=N X: X on every drawn wall of GEO type N
    int line = 0;
};

struct LogbookEntry {
    // Paragraphs in order; a paragraph "\x01k" is the entry's picture k,
    // "\x02k" its map k (maps[k]).
    std::vector<std::string> paragraphs;
    std::vector<LogbookMap> maps;
    int line = 0;   // the heading's line in the file
};

class LogbookText {
public:
    LogbookText() = default;
    // The same from text in memory (the sealed file, D-0083), named `name`
    // in diagnostics.
    static LogbookText from_text(const std::string& name, const std::string& text);
    bool empty() const { return entries_.empty(); }
    const LogbookEntry* entry(int n) const;
    const std::map<int, LogbookEntry>& entries() const { return entries_; }
    const std::vector<std::string>& diagnostics() const { return diag_; }
    const std::string& path() const { return path_; }

    static constexpr std::size_t kMaxChars = 4000;
    // A character the game's font draws (DrawChar 0653:026B: UpCase(c)
    // mod 64, so ASCII 0x20..0x5F and a..z).
    static bool drawable(unsigned char c) { return (c >= 0x20 && c <= 0x5F) || (c >= 'a' && c <= 'z'); }
    static constexpr char kPicture = '\x01';
    static constexpr char kMap = '\x02';

private:
    void parse(std::istream& in);
    std::string path_;
    std::map<int, LogbookEntry> entries_;
    std::vector<std::string> diag_;
};

}  // namespace cotd
