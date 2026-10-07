// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <span>
#include <xxhash.h>
#include "common/types.h"
namespace Vulkan {
inline u64 TextureSetKey(std::span<const u64> hashes,u64 stage) {
    const u64 hash=XXH3_64bits_withSeed(hashes.data(),hashes.size_bytes(),stage);
    // Preserve the low bit: forcing it to 1 wastes half of a power-of-two table.
    return hash ? hash : 1;
}
}
