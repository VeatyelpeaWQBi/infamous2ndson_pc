// SPDX-License-Identifier: GPL-2.0-or-later
// Narrow adaptation of AYOUB1080p 153a5aa's recorded pipeline BindCache.
#pragma once
#include "common/types.h"
namespace Vulkan {
class GraphicsBindingMemo {
    u64 command{}, pipeline{};
public:
    void Invalidate() {command = pipeline = 0;}
    bool NeedsBind(u64 cmd, u64 handle) {
        if (cmd && handle && command == cmd && pipeline == handle) return false;
        command = cmd; pipeline = handle; return true;
    }
};
} // namespace Vulkan
