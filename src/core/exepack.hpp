// SPDX-License-Identifier: GPL-3.0-or-later
// Microsoft EXEPACK: the load image of a packed MZ executable, as the
// EXEPACK stub in START.EXE builds it (docs/notes/unpack.md;
// tools/unexepack.py is the reference). Relocations are not applied: the
// engine reads only initial data from the image.
#pragma once

#include "core/common.hpp"

namespace cotd {

// The unpacked load image (without the MZ header); Error if the file
// isn't an EXEPACK-packed MZ executable.
Bytes exepack_image(const Bytes& exe);

}  // namespace cotd
