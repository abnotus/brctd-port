// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/seal.hpp"

#include <array>
#include <cstring>
#include <utility>

#include "core/sha256.hpp"

namespace cotd {

namespace {

using Key = std::array<std::uint8_t, 32>;

Key digest(const Bytes& b) {
    const std::string h = sha256_hex(b);
    Key k{};
    for (std::size_t i = 0; i < 32; ++i) k[i] = static_cast<std::uint8_t>(std::stoi(h.substr(2 * i, 2), nullptr, 16));
    return k;
}

Bytes cat(std::initializer_list<const Bytes*> parts) {
    Bytes out;
    for (const Bytes* p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
}

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes bytes(const Key& k) { return Bytes(k.begin(), k.end()); }

Key hmac(const Key& key, const Bytes& msg) {
    const Bytes h = hmac_sha256(bytes(key), msg);
    Key k{};
    std::memcpy(k.data(), h.data(), 32);
    return k;
}

std::uint32_t rotl(std::uint32_t v, int n) { return v << n | v >> (32 - n); }

void quarter(std::uint32_t* s, int a, int b, int c, int d) {
    s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl(s[d], 16);
    s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl(s[b], 12);
    s[a] += s[b]; s[d] ^= s[a]; s[d] = rotl(s[d], 8);
    s[c] += s[d]; s[b] ^= s[c]; s[b] = rotl(s[b], 7);
}

// ChaCha20 (RFC 8439 2.4): XOR with the key stream from block 1.
void chacha20(const Key& key, const std::uint8_t* nonce, Bytes& data) {
    data = chacha20_xor(bytes(key), Bytes(nonce, nonce + 12), 1, std::move(data));
}

struct Keys { Key enc, mac; };

Keys keys(const Bytes& exe, const Bytes& ovr) {
    const Bytes tag = bytes("cotd-seal-v1");
    const Bytes k = bytes(digest(cat({&tag, &exe, &ovr})));
    const Bytes e = bytes("enc"), m = bytes("mac");
    return {digest(cat({&e, &k})), digest(cat({&m, &k}))};
}

const char kMagic[] = "COTDSEAL";

}  // namespace

Bytes hmac_sha256(const Bytes& key, const Bytes& msg) {
    Bytes k = key.size() > 64 ? bytes(digest(key)) : key;
    k.resize(64, 0);
    Bytes ipad(64), opad(64);
    for (std::size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5C);
    }
    const Bytes inner = bytes(digest(cat({&ipad, &msg})));
    return bytes(digest(cat({&opad, &inner})));
}

Bytes chacha20_xor(const Bytes& key, const Bytes& nonce, std::uint32_t counter, Bytes data) {
    if (key.size() != 32 || nonce.size() != 12) throw Error("chacha20: a 32-byte key and a 12-byte nonce");
    std::uint32_t init[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) init[4 + i] = le32(&key[static_cast<std::size_t>(4 * i)]);
    for (int i = 0; i < 3; ++i) init[13 + i] = le32(&nonce[static_cast<std::size_t>(4 * i)]);
    for (std::size_t off = 0; off < data.size(); off += 64, ++counter) {
        std::uint32_t s[16];
        std::memcpy(s, init, sizeof s);
        s[12] = counter;
        std::uint32_t w[16];
        std::memcpy(w, s, sizeof w);
        for (int r = 0; r < 10; ++r) {
            quarter(w, 0, 4, 8, 12); quarter(w, 1, 5, 9, 13); quarter(w, 2, 6, 10, 14); quarter(w, 3, 7, 11, 15);
            quarter(w, 0, 5, 10, 15); quarter(w, 1, 6, 11, 12); quarter(w, 2, 7, 8, 13); quarter(w, 3, 4, 9, 14);
        }
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t v = w[i] + s[i];
            for (int b = 0; b < 4; ++b) {
                const std::size_t at = off + static_cast<std::size_t>(4 * i + b);
                if (at < data.size()) data[at] ^= static_cast<std::uint8_t>(v >> (8 * b));
            }
        }
    }
    return data;
}


std::optional<std::string> open_sealed(const Bytes& sealed, const Bytes& start_exe, const Bytes& game_ovr) {
    if (sealed.size() < 8 + 1 + 12 + 32 || std::memcmp(sealed.data(), kMagic, 8) != 0 || sealed[8] != 1)
        return std::nullopt;
    const Keys k = keys(start_exe, game_ovr);
    const Bytes head(sealed.begin(), sealed.end() - 32);
    const Key tag = hmac(k.mac, head);
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < 32; ++i) diff |= static_cast<std::uint8_t>(tag[i] ^ sealed[sealed.size() - 32 + i]);
    if (diff) return std::nullopt;
    Bytes body(sealed.begin() + 21, sealed.end() - 32);
    chacha20(k.enc, &sealed[9], body);
    return std::string(body.begin(), body.end());
}

}  // namespace cotd
