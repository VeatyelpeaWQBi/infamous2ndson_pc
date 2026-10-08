// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include "common/types.h"
namespace VideoCore {
inline u32 StoredFaultCount(u64 reported,u32 slots) {
    return slots ? static_cast<u32>(std::min<u64>(reported,slots-1)) : 0;
}
}
