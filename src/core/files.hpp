// SPDX-License-Identifier: GPL-3.0-or-later
// The player's game directory: file lookup (DOS names, any case) and
// per-file overrides used by tests (a modified copy of one DAX file).
#pragma once

#include <map>
#include <string>

#include "core/common.hpp"

namespace cotd {

class GameFiles {
public:
    explicit GameFiles(std::string dir);


    // Full path of NAME in the game directory, or of its override.
    std::string path(const std::string& name) const;
    bool exists(const std::string& name) const;
    Bytes read(const std::string& name) const;
    const std::string& dir() const { return dir_; }

private:
    std::string dir_;
};

Bytes read_file(const std::string& path);
void write_file(const std::string& path, const Bytes& data);
std::string upper(std::string s);

}  // namespace cotd
