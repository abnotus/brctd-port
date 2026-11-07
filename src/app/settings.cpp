// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/settings.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cotd {

namespace {

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    const auto b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

}  // namespace

Settings::Settings(std::string path) : path_(std::move(path)) {
    if (path_.empty()) return;
    std::ifstream f(path_);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line.substr(0, line.find('#')));
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq));
        if (!k.empty()) values_[k] = trim(line.substr(eq + 1));
    }
}

std::string Settings::default_path() {
    char* p = SDL_GetPrefPath("", "ctd");
    if (!p) return "";
    std::string s = std::string(p) + "settings.ini";
    SDL_free(p);
    return s;
}

std::string Settings::data_dir() {
    char* p = SDL_GetPrefPath("", "ctd");
    if (!p) return "";
    std::string s(p);
    SDL_free(p);
    return s;
}

std::string Settings::executable_dir() {
    const char* p = SDL_GetBasePath();   // SDL owns the string (SDL3)
    return p ? std::string(p) : std::string();
}

std::optional<std::string> Settings::get(const std::string& key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::optional<int> Settings::get_int(const std::string& key) const {
    const auto v = get(key);
    if (!v) return std::nullopt;
    char* end = nullptr;
    const long n = std::strtol(v->c_str(), &end, 10);
    if (end == v->c_str() || *end) return std::nullopt;
    return static_cast<int>(n);
}

void Settings::set(const std::string& key, const std::string& value) {
    const auto it = values_.find(key);
    if (it != values_.end() && it->second == value) return;
    values_[key] = value;
    changed_ = true;
}

void Settings::erase(const std::string& key) {
    if (values_.erase(key)) changed_ = true;
}

void Settings::save() {
    if (path_.empty() || !changed_) return;
    std::error_code ec;
    const auto dir = std::filesystem::path(path_).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    std::ofstream f(path_, std::ios::trunc);
    if (!f) {
        // Never stops the game; said once, so the player knows why the
        // options aren't kept (session 72, M2).
        std::fprintf(stderr, "ctd: settings: cannot write %s; the options are not kept\n", path_.c_str());
        changed_ = false;
        return;
    }
    f << "# Countdown to Doomsday (native port): settings, written by the port.\n";
    for (const auto& [k, v] : values_) f << k << " = " << v << "\n";
    changed_ = false;
}

}  // namespace cotd
