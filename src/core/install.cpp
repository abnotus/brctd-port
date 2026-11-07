// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/install.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>
#include <string_view>

#include "core/exepack.hpp"
#include "core/files.hpp"
#include "core/sha256.hpp"

namespace fs = std::filesystem;

namespace cotd {

namespace {

struct KnownRelease {
    const char* id;
    const char* title;
    const char* version;   // the version string's last word ("v1.00")
};

struct KnownFile {
    const char* release;
    const char* name;
    std::uintmax_t size;
    const char* sha256;
};

#include "core/release_files.inc"

constexpr std::uintmax_t kHashLimit = 64u << 20;   // larger files are listed, not hashed

fs::path to_path(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string from_path(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

const KnownRelease& release(std::string_view id) {
    for (const KnownRelease& r : kKnownReleases)
        if (id == r.id) return r;
    throw Error("install: no release " + std::string(id));
}

// The last word of a version string ("v1.00").
std::string last_word(const std::string& v) {
    const auto sp = v.rfind(' ');
    return sp == std::string::npos ? v : v.substr(sp + 1);
}

// The player's own saves: left out of the hashes file (they differ per player).
bool player_save(const std::string& upper_name) {
    return upper_name.rfind("SAVGAM", 0) == 0 || upper_name.rfind("CHRDAT", 0) == 0;
}

// START.EXE's version string: the Pascal string at its unpacked image's
// offset 0 (fixtures.toml's identity evidence), printable ASCII only.
std::string version_string(const std::string& path) {
    try {
        const Bytes image = exepack_image(read_file(path));
        if (image.empty() || image[0] == 0 || image[0] > 40 || image.size() < 1u + image[0]) return "";
        std::string s(image.begin() + 1, image.begin() + 1 + image[0]);
        while (!s.empty() && s.back() == ' ') s.pop_back();   // v1.00's ends in a space
        for (char c : s)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return "";
        return s;
    } catch (const std::exception&) {
        return "";
    }
}

std::string short_hash(const std::string& h) { return h.empty() ? "not hashed" : h.substr(0, 16) + "..."; }

}  // namespace

std::string release_of(const std::string& exe_sha256, const std::string& ovr_sha256) {
    for (const KnownRelease& r : kKnownReleases) {
        bool exe = false, ovr = false;
        for (const KnownFile& f : kKnownFiles) {
            if (std::string(f.release) != r.id) continue;
            if (std::string(f.name) == "START.EXE") exe = exe_sha256 == f.sha256;
            if (std::string(f.name) == "GAME.OVR") ovr = ovr_sha256 == f.sha256;
        }
        if (exe && ovr) return r.id;
    }
    return "";
}

InstallReport examine_install(const std::string& dir) {
    InstallReport r;
    const KnownRelease& sup = release(kSupportedRelease);
    const std::string vname = sup.version;
    std::error_code ec;
    if (!fs::is_directory(to_path(dir), ec)) {
        const fs::path abs = fs::absolute(to_path(dir), ec);   // a relative "game" means little to a player
        r.verdict = "The game folder \"" + (ec ? dir : from_path(abs)) + "\" doesn't exist" +
                    (fs::exists(to_path(dir), ec) ? " as a folder" : "") +
                    ". Put the game's files (START.EXE, GAME.OVR and the rest) in that folder.";
        return r;
    }
    r.folder = true;

    // The folder's files (any case), hashed; the subfolders that hold a START.EXE.
    std::map<std::string, std::size_t> by_upper;   // upper-case name -> index in r.files
    std::vector<std::string> subfolders;
    for (const auto& e : fs::directory_iterator(to_path(dir), ec)) {
        const std::string name = from_path(e.path().filename());
        if (e.is_directory(ec)) {
            if (fs::exists(e.path() / "START.EXE", ec) || fs::exists(e.path() / "start.exe", ec)) subfolders.push_back(name);
            continue;
        }
        if (!e.is_regular_file(ec)) continue;
        FolderFile f;
        f.name = name;
        f.size = e.file_size(ec);
        if (f.size <= kHashLimit) {
            try {
                f.sha256 = sha256_hex(read_file(from_path(e.path())));
            } catch (const std::exception&) {
                f.sha256.clear();   // unreadable: listed with no hash
            }
        }
        for (const KnownFile& k : kKnownFiles)
            if (!f.sha256.empty() && f.sha256 == k.sha256 && f.size == k.size) {
                f.known = std::string(k.release) + " " + k.name;
                if (upper(name) == k.name) break;   // prefer the same name's entry
            }
        r.files.push_back(f);
    }
    std::sort(r.files.begin(), r.files.end(), [](const FolderFile& a, const FolderFile& b) { return upper(a.name) < upper(b.name); });
    for (std::size_t i = 0; i < r.files.size(); ++i) by_upper[upper(r.files[i].name)] = i;
    std::sort(subfolders.begin(), subfolders.end());

    const auto find = [&](const std::string& name) -> const FolderFile* {
        const auto it = by_upper.find(name);
        return it == by_upper.end() ? nullptr : &r.files[it->second];
    };
    if (const FolderFile* exe = find("START.EXE")) {
        fs::path p = to_path(dir) / to_path(exe->name);
        r.version = version_string(from_path(p));
    }

    // Each release's files: present by name, equal by content.
    struct Count {
        int files = 0, named = 0, equal = 0;
        bool exe = false, ovr = false;
    };
    std::map<std::string, Count> count;
    for (const KnownFile& k : kKnownFiles) {
        Count& c = count[k.release];
        ++c.files;
        const FolderFile* f = find(k.name);
        if (!f) continue;
        ++c.named;
        const bool eq = f->sha256 == k.sha256 && f->size == k.size;
        if (eq) ++c.equal;
        if (std::string(k.name) == "START.EXE") c.exe = eq;
        if (std::string(k.name) == "GAME.OVR") c.ovr = eq;
    }

    // The supported release's files, one line each that is missing or differs.
    int missing = 0, differ = 0, other_size = 0;
    for (const KnownFile& k : kKnownFiles) {
        if (std::string(k.release) != sup.id) continue;
        const FolderFile* f = find(k.name);
        if (!f) {
            ++missing;
            std::string line = "  missing  " + std::string(k.name) + " (" + vname + ": " + std::to_string(k.size) + " bytes)";
            for (const FolderFile& o : r.files)
                if (o.sha256 == k.sha256 && o.size == k.size) {
                    line += ": it is here as " + o.name + ", rename it";
                    break;
                }
            r.lines.push_back(line);
        } else if (f->sha256 != k.sha256 || f->size != k.size) {
            ++differ;
            if (f->size != k.size) ++other_size;
            r.lines.push_back("  differs  " + f->name + ": " + std::to_string(f->size) + " bytes, sha256 " +
                              short_hash(f->sha256) + " (" + vname + ": " + std::to_string(k.size) + " bytes, sha256 " +
                              short_hash(k.sha256) + ")" + (f->size == k.size ? ", the same size" : ""));
        }
    }
    const Count& cs = count[sup.id];
    for (const auto& [id, c] : count)
        if (c.named || c.equal) r.game = true;
    for (const FolderFile& f : r.files)
        if (!f.known.empty()) r.game = true;
    std::vector<std::string> extra;
    for (const FolderFile& f : r.files) {
        bool known = false;
        for (const KnownFile& k : kKnownFiles)
            if (std::string(k.release) == sup.id && upper(f.name) == k.name) known = true;
        if (!known) extra.push_back(f.name);
    }
    if (cs.equal > 0 && (missing || differ))
        r.lines.push_back("  " + std::to_string(cs.equal) + " of " + std::to_string(cs.files) + " files match " + vname);
    if (!extra.empty() && (missing || differ)) {
        std::string line = "  not the game's files (ignored): ";
        for (std::size_t i = 0; i < extra.size() && i < 8; ++i) line += (i ? ", " : "") + extra[i];
        if (extra.size() > 8) line += " and " + std::to_string(extra.size() - 8) + " more";
        r.lines.push_back(line);
    }

    // What the folder looks like.
    std::string other;   // another known release (Matrix Cubed) this folder is
    for (const KnownRelease& k : kKnownReleases) {
        const Count& c = count[k.id];
        if (std::string(k.id) != sup.id && (c.exe || c.equal * 2 > c.files)) other = k.id;
    }
    if (cs.exe && cs.ovr) {
        r.runnable = true;
        r.release = sup.id;
        r.exact = !missing && !differ;
        r.verdict = r.exact ? "Identified: " + std::string(sup.title) + ", " + vname + " (" + sup.id + "): all " +
                                  std::to_string(cs.files) + " files match."
                            : "START.EXE and GAME.OVR are " + vname + "'s (" + sup.id + "), so the game runs, but other files are missing or "
                                  "differ (" + std::to_string(missing + differ) + " of " + std::to_string(cs.files) +
                                  "): a damaged or incomplete copy. Copy them again from the original disks or "
                                  "install.";
    } else if (!other.empty()) {
        r.lines.clear();   // not this game: its files against v1.00 say nothing
        const KnownRelease& k = release(other);
        r.verdict = "This folder holds " + std::string(k.title) + " (" + k.id + "), not Countdown to Doomsday; the port "
                    "runs only " + std::string(sup.title) + ".";
    } else if (cs.named == 0 && cs.equal == 0) {
        r.lines.clear();   // every file missing: the verdict says it
        r.verdict = "This folder holds none of the game's files.";
        if (!subfolders.empty())
            r.verdict += " START.EXE is in its subfolder \"" + subfolders.front() + "\": put that folder's files here.";
        else
            r.verdict += " Put the game's files here: START.EXE, GAME.OVR and the rest.";
    } else {
        r.verdict = "START.EXE and GAME.OVR are not those of a release the port knows. ";
        if (!r.version.empty() && last_word(r.version) != vname)
            r.verdict += "START.EXE says \"" + r.version + "\": another version of the game, which the port "
                         "doesn't know yet.";
        else if (other_size * 2 > cs.named)
            r.verdict += "It looks like another version or release of the game: most files have sizes " + vname +
                         "'s don't.";
        else
            r.verdict += "It looks like a damaged or incomplete copy of " + vname + ": " + std::to_string(missing) +
                         " missing, " + std::to_string(differ) + " differ. Copy them again from the original "
                         "disks or install.";
        r.verdict += " The port reads the game's own text at places known for each release, so it runs only on one "
                     "it knows.";
    }
    return r;
}

std::string InstallReport::text() const {
    std::string s = verdict + "\n";
    for (const std::string& l : lines) s += l + "\n";
    return s;
}

std::string InstallReport::hashes(const std::string& build) const {
    std::ostringstream h;
    h << "# ctd " << build << ": a game folder's files, for adding a release.\n";
    h << "# Names, sizes and SHA-256 hashes only, no game data; the player's saves are\n";
    h << "# left out. Send this file with your report.\n";
    std::string v = verdict;
    if (v.size() > 200) v = v.substr(0, 200) + "...";
    h << "# verdict: " << v << "\n\n";
    h << "[release.unknown]\n";
    if (!version.empty()) h << "version_string = \"" << version << "\"\n";
    for (const FolderFile& f : files) {
        if (player_save(upper(f.name))) continue;
        std::string name;
        for (char c : f.name) name += (c == '"' || c == '\\') ? '_' : c;
        h << "\n[release.unknown.files.\"" << name << "\"]\n";
        h << "size = " << f.size << "\n";
        if (!f.sha256.empty()) h << "sha256 = \"" << f.sha256 << "\"\n";
        else h << "# not hashed (over 64 MB or unreadable)\n";
        if (!f.known.empty()) h << "# equals " << f.known << "\n";
    }
    return h.str();
}

UnidentifiedRelease::UnidentifiedRelease(InstallReport r)
    : Error((r.folder ? "unidentified release: " : "no game folder: ") + r.text()), report(std::move(r)) {}

bool write_hashes(const InstallReport& r, const std::string& path, const std::string& build) {
    try {
        const std::string t = r.hashes(build);
        const fs::path parent = to_path(path).parent_path();
        if (!parent.empty()) fs::create_directories(parent);
        write_file(path, Bytes(t.begin(), t.end()));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace cotd
