// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace cotd {

namespace {

// UTF-8 strings <-> paths (C++20: u8path is deprecated, u8string is char8_t).
fs::path to_path(const std::string& s) { return fs::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::string from_path(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

std::string hex(std::uint32_t v, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", digits, static_cast<unsigned>(v));
    return buf;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// A path the host can't convert (on Windows, bytes that aren't UTF-8)
// is a file that can't be opened, not a library exception (session 78).
static fs::path checked_path(const std::string& path, const char* what) {
    try {
        return to_path(path);
    } catch (const std::exception&) {
        throw Error(std::string(what) + path);
    }
}

Bytes read_file(const std::string& path) {
    std::ifstream f(checked_path(path, "cannot open "), std::ios::binary);
    if (!f) throw Error("cannot open " + path);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

void write_file(const std::string& path, const Bytes& data) {
    std::ofstream f(checked_path(path, "cannot write "), std::ios::binary);
    if (!f) throw Error("cannot write " + path);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f) throw Error("cannot write " + path);
}

GameFiles::GameFiles(std::string dir) : dir_(std::move(dir)) {}


std::string GameFiles::path(const std::string& name) const {
    fs::path exact = to_path(dir_) / to_path(name);
    if (fs::exists(exact)) return from_path(exact);
    // DOS names are case-insensitive; installs copied to Linux may differ.
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(to_path(dir_), ec))
        if (upper(from_path(e.path().filename())) == upper(name))
            return from_path(e.path());
    return from_path(exact);
}

bool GameFiles::exists(const std::string& name) const {
    return fs::exists(to_path(path(name)));
}

Bytes GameFiles::read(const std::string& name) const { return read_file(path(name)); }

}  // namespace cotd
