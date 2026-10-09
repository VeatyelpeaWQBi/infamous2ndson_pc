// SPDX-License-Identifier: GPL-2.0-or-later
// A GPU readback snapshot is reusable only until a write or CPU ownership change.
#pragma once
#include <algorithm>
#include <vector>
#include <utility>
#include "common/types.h"
namespace VideoCore {
// Bound speculative copies during bursts of particle dispatches. This gate
// never applies to demand readbacks, whose ownership/version checks stay exact.
struct ReadbackPrefetchGate {
    u64 last_queue_ns{};
    bool queued{};
    bool Ready(u64 now,u64 interval) const {
        return !queued || !interval || now<last_queue_ns || now-last_queue_ns>=interval;
    }
    void Queued(u64 now) {last_queue_ns=now;queued=true;}
};
// Combine known CPU read windows into one download submission. Only windows in
// the current arena are included; actual copies still filter GPU-owned bytes.
template<class Slots>
auto CoalescedReadbackWindows(VAddr begin,VAddr end,VAddr arena_begin,VAddr arena_end,
                             u64 window,const Slots& slots) {
    std::vector<std::pair<VAddr,VAddr>> ranges{{begin,end}};
    for(const auto& slot:slots) {
        const auto page=slot.hint.page;
        if(!page || page<arena_begin || page>=arena_end) continue;
        const VAddr start=std::max(arena_begin,page & ~(window-1));
        const VAddr stop=start+std::min(window,arena_end-start);
        ranges.emplace_back(start,stop);
    }
    std::sort(ranges.begin(),ranges.end());
    size_t count=0;
    for(const auto range:ranges) {
        if(count && range.first<=ranges[count-1].second)
            ranges[count-1].second=std::max(ranges[count-1].second,range.second);
        else ranges[count++]=range;
    }
    ranges.resize(count);
    return ranges;
}
struct ReadbackHint {
    VAddr page{};u64 epoch{1},copied_epoch{},tick{};
    bool Matches(VAddr address,u64 size) const {return page && address>=page && size<=4096 && address-page<=4096-size;}
    bool Overlaps(VAddr address,u64 size) const {
        return page && size && address<page+4096 && (address>=page || size>page-address);
    }
    void Write(VAddr address,u64 size) {
        if(Overlaps(address,size)) ++epoch;
    }
    // Keep the old tick until it completes; the slot's GPU storage may be in flight.
    void Forget() {page=0;++epoch;}
    bool Valid() const {return tick && copied_epoch==epoch;}
    void Captured(u64 at) {copied_epoch=epoch;tick=at;}
    // The caller transfers snapshot-buffer ownership before storage is reused.
    u64 Detach() {const auto at=tick;Consumed();return at;}
    void Consumed() {++epoch;tick=0;}
};
// Bounded learning: cold startup pages must not permanently occupy the hot pool.
// Never recycle storage which the GPU may still be writing.
template<class Slots,class IsFree>
auto* SelectReadbackVictim(Slots& slots,IsFree is_free) {
    using Slot=typename Slots::value_type;
    Slot* selected=nullptr;
    for(auto& slot:slots) {
        if(slot.hint.tick && !is_free(slot.hint.tick)) continue;
        if(!slot.hint.page) return &slot;
        if(!selected || slot.last_use<selected->last_use) selected=&slot;
    }
    return selected;
}
}
