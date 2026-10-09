// SPDX-License-Identifier: GPL-2.0-or-later
// AYOUB skip_empty_draws, with independent checks (no count-product overflow).
#pragma once
#include "common/types.h"
namespace Vulkan {
constexpr bool EmptyDraw(u32 vertices,u32 instances) {return !vertices||!instances;}
constexpr bool EmptyDispatch(u32 x,u32 y,u32 z) {return !x||!y||!z;}
}
