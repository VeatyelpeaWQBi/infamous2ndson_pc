// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbgpu is built with NDEBUG. Vulkan-Hpp adds fields to DispatchLoaderBase
// without NDEBUG, so its headers must use the same ABI in these static-library
// clients. Restore assertions immediately afterwards for the test checks.
#ifndef NDEBUG
#define NDEBUG
#define BB_RESTORE_TEST_ASSERTIONS
#endif
#include "video_core/renderer_vulkan/vk_common.h"
#ifdef BB_RESTORE_TEST_ASSERTIONS
#undef NDEBUG
#undef BB_RESTORE_TEST_ASSERTIONS
#endif
#include <cassert>
