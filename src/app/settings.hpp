// SPDX-License-Identifier: GPL-3.0-or-later
// The port's own settings file (not BUCK.CFG, which stays the original's):
// one "key = value" per line, '#' starts a comment. The window keeps its
// last position, size and full-screen state there, and the player's
// options (combat speed, the text options). Unknown keys are kept as they
// are, so an older build doesn't drop a newer build's settings.
#pragma once

#include <map>
#include <optional>
#include <string>

namespace cotd {

class Settings {
public:
    Settings() = default;
    // Reads `path` (a missing file is an empty one); an empty path means
    // no file: nothing is read or written.
    explicit Settings(std::string path);
    ~Settings() { save(); }   // written on every way out
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    // The default file: settings.ini in the user's data folder for the
    // port (SDL_GetPrefPath: ~/.local/share/cotd, %APPDATA%\cotd), or ""
    // when there is none.
    static std::string default_path();
    // The folder that holds the running executable, with a trailing
    // separator (SDL_GetBasePath); "" when SDL can't tell.
    static std::string executable_dir();
    // The user's data folder for the port (SDL_GetPrefPath), with a
    // trailing separator; "" when there is none.
    static std::string data_dir();

    std::optional<std::string> get(const std::string& key) const;
    std::optional<int> get_int(const std::string& key) const;
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, int value) { set(key, std::to_string(value)); }
    void erase(const std::string& key);
    // Writes the file if anything changed (errors are ignored: a setting
    // that can't be kept never stops the game).
    void save();
    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::map<std::string, std::string> values_;
    bool changed_ = false;
};

}  // namespace cotd
