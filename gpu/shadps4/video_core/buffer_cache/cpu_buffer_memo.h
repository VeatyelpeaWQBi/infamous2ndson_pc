// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from AYOUB1080p 153a5aa's readonly synchronization memo.
#pragma once
#include <array>
#include "common/types.h"

namespace VideoCore {
class Buffer;
struct CpuBufferMemo {
    VAddr address{};
    u32 size{};
    const Buffer* arena{};
    std::array<u64, 2> versions{};
    bool Matches(VAddr source, u32 bytes, const std::array<u64, 2>& current,
                 const Buffer* first_arena, const Buffer* last_arena) const {
        // Arena migration changes the Vulkan handle even when guest bytes do
        // not change. Never return a formerly bound sparse-buffer handle.
        return arena && arena == first_arena && arena == last_arena &&
               address == source && size == bytes && versions == current;
    }
};
} // namespace VideoCore
