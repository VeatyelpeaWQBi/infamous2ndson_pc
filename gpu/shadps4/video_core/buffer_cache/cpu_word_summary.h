// SPDX-License-Identifier: GPL-2.0-or-later
// Conservative dirty-word index: a zero word can skip a repeated 1024-page scan.
#pragma once
#include <atomic>
#include "video_core/buffer_cache/region_definitions.h"
namespace VideoCore {
class CpuWordSummary {
    static constexpr u64 Words=NUM_PAGES_PER_REGION/64;
    static_assert(Words<64);
    std::atomic<u64> dirty;
    static u64 Mask(size_t first,size_t end) {
        if(first>=end || end>NUM_PAGES_PER_REGION) return 0;
        const size_t lo=first/64,hi=(end-1)/64;
        return ((u64{1}<<(hi-lo+1))-1)<<lo;
    }
public:
    explicit CpuWordSummary(bool initially_dirty=true)
        : dirty{initially_dirty ? (u64{1}<<Words)-1 : 0} {}
    // The region lock serializes writers. Publish potential dirtiness BEFORE setting bits.
    void Mark(size_t first,size_t end) { dirty.fetch_or(Mask(first,end),std::memory_order_release); }
    void Refresh(const RegionBits& bits,size_t first,size_t end) {
        const u64 mask=Mask(first,end);
        if(!mask) return;
        u64 present{};
        for(size_t word=first/64;word<=(end-1)/64;++word)
            if(bits.AnyInRange(word*64,(word+1)*64)) present|=u64{1}<<word;
        const u64 old=dirty.load(std::memory_order_relaxed);
        dirty.store((old & ~mask)|present,std::memory_order_release);
    }
    bool MightBeDirty(size_t first,size_t end) const {
        return (dirty.load(std::memory_order_acquire)&Mask(first,end))!=0;
    }
};
}
