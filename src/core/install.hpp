// SPDX-License-Identifier: GPL-3.0-or-later
// The player's game folder against the known releases (core/release_files.inc,
// generated from fixtures.toml): each file missing or differing, whether the
// folder looks like another version or a damaged copy, and a hashes file the
// player can send so a release can be added (names, sizes and SHA-256 hashes
// only; no game data). `cotd identify DIR`; the refusal of an unidentified
// release (core/gametext.hpp identify_release).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.hpp"

namespace cotd {

// The release the engine runs (its text offsets: core/gametext.cpp).
inline constexpr const char* kSupportedRelease = "ctd-dos-v1_00-a";

struct FolderFile {
    std::string name;            // as on disk
    std::uintmax_t size = 0;
    std::string sha256;          // empty when not hashed (too large)
    std::string known;           // "RELEASE NAME" of the known file it equals, or empty
};

struct InstallReport {
    bool folder = false;         // the folder exists
    bool game = false;           // it holds some known release's files (by name or content)
    bool runnable = false;       // START.EXE and GAME.OVR are the supported release's
    bool exact = false;          // and every other file of it matches too
    std::string release;         // the supported release's id when runnable
    std::string verdict;         // one paragraph: what the folder looks like
    std::vector<std::string> lines;   // per file: missing, differs, found under another name
    std::vector<FolderFile> files;    // every file in the folder (the player's saves left out)
    std::string version;         // START.EXE's version string, when it could be read

    // The explanation: the verdict, then the per-file lines.
    std::string text() const;
    // The hashes file's text (fixtures.toml's format): BUILD names the port.
    std::string hashes(const std::string& build) const;
};

InstallReport examine_install(const std::string& dir);

// The id of the known release whose START.EXE and GAME.OVR have these
// hashes, or empty.
std::string release_of(const std::string& exe_sha256, const std::string& ovr_sha256);

// An unidentified release: the message is the report's text.
struct UnidentifiedRelease : Error {
    explicit UnidentifiedRelease(InstallReport r);
    InstallReport report;
};

// Write the report's hashes file to PATH (its folder made); false when it
// can't. Callers write one only for a folder that holds some of the game
// (`game`), never a list of the player's unrelated files.
bool write_hashes(const InstallReport& r, const std::string& path, const std::string& build);

}  // namespace cotd
