// SPDX-License-Identifier: GPL-3.0-or-later
// The sealed Log Book text (CLAUDE.md hard rule 1's one exception, the
// owner's, session 64; DIVERGENCE.md D-0083): LOGBOOK.TXT encrypted with
// a key derived from the bytes of the player's own START.EXE and GAME.OVR,
// so only a player with the original files reads it. ChaCha20 (RFC 8439)
// and HMAC-SHA-256 (RFC 2104), the port's own; encrypt-then-MAC.
//
// The key: SHA-256("cotd-seal-v1", START.EXE's bytes, GAME.OVR's bytes),
// which the files' published hashes don't give; from it the cipher's key
// SHA-256("enc", K) and the tag's key SHA-256("mac", K). The nonce is the
// tag key's HMAC of the text (its first 12 bytes), so sealing the same
// text gives the same file. The file: "COTDSEAL" + version 1 + the nonce
// + the cipher text + the tag (HMAC of all before it).
#pragma once

#include <optional>
#include <string>

#include "core/common.hpp"

namespace cotd {

// nullopt for a file another copy sealed, or a damaged one.
std::optional<std::string> open_sealed(const Bytes& sealed, const Bytes& start_exe, const Bytes& game_ovr);

// The primitives, for their published test vectors (`cotd seal-check`,
// tests/test_seal.py): RFC 8439 2.4's ChaCha20 (a 32-byte key, a 12-byte
// nonce, the block counter to start from) and RFC 2104's HMAC-SHA-256 (any
// key length; one over 64 bytes hashed first).
Bytes chacha20_xor(const Bytes& key, const Bytes& nonce, std::uint32_t counter, Bytes data);
Bytes hmac_sha256(const Bytes& key, const Bytes& msg);

}  // namespace cotd
