// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

namespace Vulkan::StencilReference {
// A masked-out REPLACE_OP writes nothing. Its op value must not change the
// reference used by the stencil comparison (Second Son: NE, test=4, op=0,
// compare mask=4, write mask=0).
inline u32 Select(u32 test, u32 operation, u32 write_mask, bool replace_operation) {
    return replace_operation && (write_mask & 0xffu) ? operation : test;
}
} // namespace Vulkan::StencilReference
