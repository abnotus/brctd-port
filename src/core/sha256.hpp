// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-256 (FIPS 180-4), for identifying the player's release files.
#pragma once

#include <string>

#include "core/common.hpp"

namespace cotd {

// Lower-case hex digest of data.
std::string sha256_hex(const Bytes& data);

}  // namespace cotd
