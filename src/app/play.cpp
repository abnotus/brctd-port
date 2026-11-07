// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/play.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include "core/exepack.hpp"
#include "core/install.hpp"
#include "core/seal.hpp"
#include "core/sha256.hpp"
#include "game/combat_rules.hpp"
#include "game/input.hpp"
#include "sealed_text.hpp"
#include "version.hpp"

namespace cotd {

bool g_player = false;

// The game folder (session 78, the owner: the release runs standalone, the
// game's files in a folder beside it); a missing one is explained with
// that path (examine_install).
std::string default_game_dir() {
    const std::string exe = Settings::executable_dir();
    return exe.empty() ? "game" : exe + "game";
}

std::filesystem::path u8_path(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }

namespace {

// A folder as the key log names it (goal 81: the log is what a player
// sends with a report, so no home folder or user name in it): relative to
// the program's folder, or to the user's data folder; else as given (a
// developer's own folder).
std::string shown_folder(const std::string& dir) {
    const std::string& own = port_folder();
    if (!own.empty() && dir.rfind(own, 0) == 0) return dir.substr(own.size());
    const std::string data = Settings::data_dir();
    if (!data.empty() && dir.rfind(data, 0) == 0) return dir.substr(data.size()) + " (in the user data folder)";
    return dir;
}

// Whether a file can be made in DIR (made if missing): one written and removed.
bool can_write(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(u8_path(dir), ec);
    if (!fs::is_directory(u8_path(dir), ec)) return false;
    const fs::path probe = u8_path(dir) / "ctd-write-check.tmp";
    bool ok;
    {
        std::ofstream f(probe, std::ios::binary);
        f << "x";
        ok = static_cast<bool>(f);
    }
    fs::remove(probe, ec);
    return ok;
}

// A player's first start with the port's saves folder (session 78): the
// original's saves in GAME/SAVE copied in once, so a game played in DOS
// goes on; the game folder is only read. Returns what was done, or "".
std::string carry_over_saves(const std::string& game_dir, const std::string& saves) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(u8_path(saves), ec)) return "";
    // The game's SAVE folder, in any letter case, as the game's files are
    // found (core/files.cpp; goal 81's review: `save` was missed on Linux).
    fs::path old;
    for (const auto& e : fs::directory_iterator(u8_path(game_dir), ec)) {
        std::string n = e.path().filename().string();
        for (char& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (n == "SAVE" && e.is_directory(ec)) {
            old = e.path();
            break;
        }
    }
    if (old.empty()) return "";
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(old, ec))
        if (e.is_regular_file(ec)) files.push_back(e.path());
    if (files.empty()) return "";
    fs::create_directories(u8_path(saves), ec);
    int n = 0;
    for (const auto& f : files)
        if (fs::copy_file(f, u8_path(saves) / f.filename(), fs::copy_options::skip_existing, ec)) ++n;
    const auto sub = old.filename().u8string();
    return "copied " + std::to_string(n) + " files of the original's saves from \"" + shown_folder(game_dir) + "/" +
           std::string(sub.begin(), sub.end()) + "\" into \"" + shown_folder(saves) + "\" (once; the game folder is left as it was)";
}

// Why the save folder DIR can't hold the game's saves, or "" when it can:
// made if missing, as the game would, and a file written and removed.
std::string save_dir_problem(const std::string& dir) {
    namespace fs = std::filesystem;
    const fs::path p = u8_path(dir);
    std::error_code made, ec;
    fs::create_directories(p, made);
    std::string why;
    if (!fs::is_directory(p, ec)) {
        why = made ? made.message() : "it isn't a folder";
    } else {
        const fs::path probe = p / "ctd-write-check.tmp";
        {
            std::ofstream f(probe, std::ios::binary);
            f << "x";
            if (!f) why = "a file can't be written there";
        }
        fs::remove(probe, ec);
    }
    if (why.empty()) return "";
    const fs::path abs = fs::absolute(p, ec);
    const auto u = abs.u8string();
    return "The save folder \"" + (ec ? dir : std::string(u.begin(), u.end())) + "\" can't be written (" + why +
           "). The game keeps its saved games and characters there."
        ;
}

// The key log's header: what's needed to replay the session.
std::string log_header(const PlayConfig& c, const game::Fixes& fixes) {
    namespace fs = std::filesystem;
    std::string save;
    std::ostringstream h;
    const std::string dir = save.empty() ? c.save_dir : fs::path(save).parent_path().string();
    h << "# ctd play key log (keys one per line; '#' starts a comment)\n";
    h << "# started: " << now_text("%Y-%m-%d %H:%M:%S") << "\n";
    h << "# build: " << COTD_VERSION << "\n";
    h << "# release: " << c.release << "\n";
    h << "# game: " << shown_folder(c.game_dir) << "\n";
    if (save.empty()) {
        h << "# start: launch\n";
        h << "# save dir: " << shown_folder(c.save_dir) << "\n";
    } else {
        h << "# save: " << save << "\n";
    }
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file()) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    for (const auto& n : names)
        h << "# save file: " << n << " sha256 " << sha256_hex(read_file((fs::path(dir) / n).string())) << "\n";
    h << "# seed: 0x" << hex(c.seed, 8) << "\n";
    h << "# cheats: none\n";
    if (fixes.any()) h << "# qol: fixes " << game::fixes_name(fixes) << "\n";
    return h.str();
}

}  // namespace

const std::string& port_folder() {
    static const std::string folder = [] {
        const std::string exe = Settings::executable_dir();
        if (!exe.empty() && can_write(exe)) return exe;
        const std::string data = Settings::data_dir();
        return data.empty() ? exe : data;
    }();
    return folder;
}

long number_option(const std::string& value, long lo, long hi, const std::string& form) {
    char* end = nullptr;
    const long v = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end || v < lo || v > hi) throw Error(form + ": '" + value + "'");
    return v;
}

std::string now_text(const char* fmt) {
    const std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, std::localtime(&t));
    return buf;
}

std::uint32_t randomize_now() {
    // TP Randomize (0BE4:11D7): RandSeed := (sec:hund) shl 16 + (hour:min)
    // from the DOS clock (INT 21h/2Ch).
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const std::tm* lt = std::localtime(&t);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::uint32_t hund = static_cast<std::uint32_t>(ms / 10);
    return static_cast<std::uint32_t>(lt->tm_sec << 8 | hund) << 16 |
           static_cast<std::uint32_t>(lt->tm_hour << 8 | lt->tm_min);
}

// The built-in sealed text is the only text a player's build uses
// (session 78, the owner).
void configure_logbook(game::Game& g, const std::string& game_dir, const std::string& text) {
    if (text == "off") return;
    if (kSealedSize) {
        // The built-in sealed text (CLAUDE.md hard rule 1's one exception):
        // opened with the player's own START.EXE and GAME.OVR, in memory.
        const GameFiles files(game_dir);
        const Bytes sealed(kSealed, kSealed + kSealedSize);
        if (auto plain = open_sealed(sealed, files.read("START.EXE"), files.read("GAME.OVR")))
            g.set_logbook_from_text("the built-in Log Book", *plain);
        else
            game::combat::diag("D-0083", "the built-in Log Book doesn't open with these game files; the original's LOGBOOK");
    } else {
        // A build made without data/cotd.dat (session 72, M2): the game's
        // own line only, as the original; said once.
        std::fprintf(stderr, "ctd: logbook: no built-in text in this build (data/cotd.dat was missing when it "
                             "was built): the game shows its own line only\n");
    }
}

std::string identify_folder(const GameFiles& files, const std::string& settings_path) {
    try {
        return identify_release(files);
    } catch (const UnidentifiedRelease& e) {
        // The folder explained (core/install.hpp), its file list beside the
        // settings file for the player to send.
        std::string msg = e.what();
        if (e.report.game && !settings_path.empty()) {
            const std::string out = (std::filesystem::path(settings_path).parent_path() / "ctd-hashes.txt").string();
            if (write_hashes(e.report, out, COTD_VERSION))
                msg += "The folder's file list (names, sizes and hashes; no game data) is in " + out +
                       ": send it with your report so this version can be added.\n";
        }
        throw Error(msg);
    }
}

void prepare_saves(PlayConfig& c, bool carry) {
    if (carry) c.carried = carry_over_saves(c.game_dir, c.save_dir);
    if (!c.carried.empty()) std::fprintf(stderr, "ctd: saves: %s\n", c.carried.c_str());
    // Said before the game starts, not at its first save (session 72, M2).
    if (const std::string why = save_dir_problem(c.save_dir); !why.empty()) throw Error(why);
}

void player_options(PlayConfig& c, Settings& settings, const GameFiles& files) {
    WindowOptions& o = c.opt;
    o.settings = &settings;
    // Each the settings file's (kept by the options screen), else the play
    // default (OPTIONS.md).
    auto setting = [&](const std::string& key, const std::string& def) { return settings.get(key).value_or(def); };
    o.speed = game::parse_combat_speed(setting("combat_speed", "original"));        // D-0054
    o.text = game::parse_text_speed(setting("text", "instant"));                    // D-0057
    o.drop_typing_keys = true;                                                      // D-0059
    o.step_pace_ms = game::kStepPaceMs;                                             // D-0064
    o.sound = true;                                                                 // D-0067: the AdLib
    o.volume = static_cast<int>(std::clamp(number_option(setting("volume", "6"), LONG_MIN, LONG_MAX,
                                                         "the settings file's volume 0..10"), 0L, 10L));
    // The effects (D-0093): the Tandy versions, their AdLib versions
    // (D-0069) or off; another value of the settings file is not taken.
    std::string effects = setting("effects", "tandy");
    if (effects != "tandy" && effects != "adlib" && effects != "off") effects = "tandy";
    o.effects = parse_effects(effects);
    o.speaker_image = exepack_image(files.read("START.EXE"));
    o.options_menu = true;                                                          // D-0065
    o.pad_text = true;                                                              // D-0085
    const std::string aspect = setting("aspect", "off");                            // D-0012
    if (aspect != "off" && aspect != "4:3") throw Error("the settings file's aspect: off|4:3");
    o.aspect = aspect == "4:3";
    // The three data-loss fixes (D-0013..D-0015; session 78, the owner),
    // the Log Book (D-0083) and the passcodes (D-0084): on.
    c.fixes = "stash-load,stale-stash,roster-backup";
    c.logbook = "on";
    c.passcodes = true;
}

int run_play(const GameFiles& files, PlayConfig& c) {
    game::Game g(files);
    g.set_seed(c.seed);
    std::string title = "Countdown to Doomsday";
    const game::Fixes fixes = game::parse_fixes(c.fixes);
    g.set_fixes(fixes);
    configure_logbook(g, c.game_dir, c.logbook);
    g.set_passcodes(c.passcodes);
    WindowInput window(title, c.opt);
    // The key log: on by default (ctd-logs/ in the port's own folder,
    // session 78).
    std::string log_path = c.keylog;
    if (log_path.empty()) {
        std::filesystem::create_directories(u8_path(port_folder() + "ctd-logs"));
        log_path = port_folder() + "ctd-logs/session-" + now_text("%Y%m%d-%H%M%S") + ".txt";
    }
    std::unique_ptr<game::LoggingInput> log;
    if (log_path != "off") {
        log = std::make_unique<game::LoggingInput>(window, log_path, log_header(c, fixes));
        g.set_input(*log);
        log->note("window: " + window.describe());   // for triage (the mouse, the monitors)
        if (!c.carried.empty()) log->note("saves: " + c.carried);
    } else {
        g.set_input(window);
    }
    // Rule-4 diagnostics (once per D-entry per run): a comment line in the
    // key log, so a log sent with a report carries them; standard error
    // when there is no key log to hold them.
    bool verbose = false;
    game::combat::set_diag_sink([&](const std::string& line) {
        if (verbose || !log) std::fprintf(stderr, "ctd: %s\n", line.c_str());
        if (log) log->note("diag: " + line);
    });
    // A runnable folder whose other files are missing or differ: said once,
    // in the key log and on standard error.
    if (const InstallReport inst = examine_install(c.game_dir); !inst.exact) {
        std::fprintf(stderr, "ctd: install: %s\n", inst.verdict.c_str());
        if (log) log->note("install: " + inst.verdict);
    }
    auto finish = [&](const std::string& why) {
        if (log) log->note("end: " + why);
        return 0;
    };
    try {
        g.run_program(c.save_dir);
    } catch (const WindowClosed&) {
        return finish("window closed");
    } catch (const game::QuitToDos&) {
        return finish("quit to DOS");
    } catch (const std::exception& e) {
        if (log) log->note(std::string("stopped: ") + e.what());
        throw;
    }
    return finish("returned");
}

int play() {
    g_player = true;
    PlayConfig c;
    c.game_dir = default_game_dir();
    const GameFiles files(c.game_dir);
    c.settings_path = Settings::default_path();
    c.release = identify_folder(files, c.settings_path);
    c.save_dir = port_folder() + "saves";
    prepare_saves(c, true);
    Settings settings(c.settings_path);
    player_options(c, settings, files);
    c.seed = randomize_now();
    return run_play(files, c);
}

}  // namespace cotd
