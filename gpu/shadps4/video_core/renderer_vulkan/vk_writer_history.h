// SPDX-License-Identifier: GPL-2.0-or-later
// Conservative, bounded producer history for AYOUB's second readback queue.
#pragma once
#include <array>
#include <algorithm>
#include "common/types.h"
namespace Vulkan {
class BufferWriterHistory {
    struct Write {u64 buffer,begin,end,tick;};
    std::array<Write,4096> writes{};
    size_t count{};u64 floor{};
public:
    void Unknown(u64 tick) {floor=std::max(floor,tick);}
    void WriteRange(u64 buffer,u64 offset,u64 size,u64 tick) {
        if(!size)return;
        if(offset+size<offset){Unknown(tick);return;}
        if(count&&writes[count-1].buffer==buffer&&writes[count-1].tick==tick&&
           offset<=writes[count-1].end&&offset+size>=writes[count-1].begin) {
            auto& last=writes[count-1];last.begin=std::min(last.begin,offset);
            last.end=std::max(last.end,offset+size);return;
        }
        if(count==writes.size()) {
            for(size_t i=0;i<count;++i)Unknown(writes[i].tick);
            count=0;
        }
        writes[count++]={buffer,offset,offset+size,tick};
    }
    u64 LastWriter(u64 buffer,u64 offset,u64 size) const {
        if(offset+size<offset)return ~u64{0};
        u64 last=floor;
        for(size_t i=0;i<count;++i) {
            const auto& w=writes[i];
            if(w.buffer==buffer&&w.begin<offset+size&&offset<w.end)last=std::max(last,w.tick);
        }
        return last;
    }
};
} // namespace Vulkan
