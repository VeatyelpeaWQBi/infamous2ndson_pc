// SPDX-License-Identifier: GPL-2.0-or-later
// A GPU readback snapshot is reusable only until a write or CPU ownership change.
#pragma once
#include "common/types.h"
namespace VideoCore {
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
