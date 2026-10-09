// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <functional>
#include <span>
#include <vector>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

struct UffdImpl;
struct SignalImpl;

class PageManager {
    // PAGE_SIZE and PAGE_BITS conflicts with machine/param.h definitions on freebsd!
    // Use the same page size as the tracker.
    static constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    struct ProtectionRange {VAddr address; u64 size;};
    using ProtectionBatch = std::vector<ProtectionRange>;
    using ProtectionCallback = std::function<void(VAddr,u64,u32)>;
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    // Host-only fixture: permits testing the production watcher/protection
    // protocol on ordinary Windows pages, without a guest process.
    explicit PageManager(ProtectionCallback callback);
    ~PageManager();

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);
    bool BeginDeferredProtection();
    ProtectionBatch TakeDeferredProtection();
    ProtectionBatch EndDeferredProtection();
    void RefreshDeferredProtection(std::span<const ProtectionRange> ranges) const;

    /// Updates watches in the pages touching the specified region.
    template <bool track>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

private:
    friend struct UffdImpl;
    friend struct SignalImpl;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
