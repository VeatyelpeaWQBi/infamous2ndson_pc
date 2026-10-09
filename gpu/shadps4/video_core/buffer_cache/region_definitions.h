// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <atomic>
#include <cstdlib>

#include "common/bit_array.h"
#include "common/types.h"

namespace VideoCore {
// AYOUB upload epochs invalidate small stream/code reuse on submissions,
// guest waits, CPU write faults and emulator writes.
inline std::atomic<u64> upload_epoch{1},host_write_epoch{1};
inline bool UploadEpochEnabled() {
    static const bool enabled=[] {
        for(const char* key:{"BB_SHADER_PARAMS_MEMO","BB_STREAM_DEDUP"}) {
            const auto* value=std::getenv(key);if(value&&value[0]=='1')return true;
        }
        return false;
    }();return enabled;
}
inline void NextUploadEpoch() {if(UploadEpochEnabled())upload_epoch.fetch_add(1,std::memory_order_release);}
inline void NoteHostWrite() {if(UploadEpochEnabled())host_write_epoch.fetch_add(1,std::memory_order_release);}
inline u64 CurrentUploadEpoch() {return upload_epoch.load(std::memory_order_acquire);}
inline u64 CurrentHostWriteGen() {return host_write_epoch.load(std::memory_order_acquire);}

constexpr u64 TRACKER_PAGE_BITS = 12; // 4K pages
constexpr u64 TRACKER_BYTES_PER_PAGE = 1ULL << TRACKER_PAGE_BITS;

constexpr u64 TRACKER_HIGHER_PAGE_BITS = 22; // each region is 4MB
constexpr u64 TRACKER_HIGHER_PAGE_SIZE = 1ULL << TRACKER_HIGHER_PAGE_BITS;
constexpr u64 TRACKER_HIGHER_PAGE_MASK = TRACKER_HIGHER_PAGE_SIZE - 1ULL;
constexpr u64 NUM_PAGES_PER_REGION = TRACKER_HIGHER_PAGE_SIZE / TRACKER_BYTES_PER_PAGE;

enum class Type {
    CPU,
    GPU,
};

using RegionBits = Common::BitArray<NUM_PAGES_PER_REGION>;

} // namespace VideoCore
