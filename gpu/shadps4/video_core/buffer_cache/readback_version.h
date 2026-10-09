// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: validate an owned GPU snapshot before publishing it to guest memory.
#pragma once
#include <vector>
#include <algorithm>
#include "common/types.h"
namespace VideoCore {
struct ReadbackVersion {
    std::vector<std::pair<VAddr,VAddr>> windows;
    bool valid=true;
    std::vector<std::vector<u8>> current_pages;
    explicit ReadbackVersion(std::vector<std::pair<VAddr,VAddr>> ranges={}) : windows{std::move(ranges)} { Capture(); }
    void Capture() {
        valid=true;current_pages.clear();
        for(const auto [begin,end]:windows) current_pages.emplace_back((end-begin+4095)/4096,1);
    }
    void Write(VAddr address,u64 size) {
        if(!size) return;
        const auto end=address+size;
        if(end<address) {
            valid=false;for(auto& pages:current_pages) std::fill(pages.begin(),pages.end(),0);return;
        }
        for(size_t i=0;i<windows.size();++i) {
            const auto [begin,finish]=windows[i];
            if(address>=finish || begin>=end) continue;
            valid=false;
            const auto first=(std::max(address,begin)-begin)/4096;
            const auto last=(std::min(end,finish)-begin+4095)/4096;
            std::fill(current_pages[i].begin()+first,current_pages[i].begin()+last,0);
        }
    }
    // Only publish pages whose version is unchanged. A write to another
    // emitter in a wide/coalesced window must not reject an unrelated counter.
    void ForEachCurrentRange(VAddr address,u64 size,auto&& func) const {
        const auto finish=address+size;
        for(size_t i=0;i<windows.size();++i) {
            const auto [begin,end]=windows[i];
            if(address>=end || begin>=finish) continue;
            const auto first=(std::max(address,begin)-begin)/4096;
            const auto last=(std::min(finish,end)-begin+4095)/4096;
            size_t page=first;
            while(page<last) {
                if(!current_pages[i][page]) {++page;continue;}
                const auto run=page;
                while(page<last && current_pages[i][page]) ++page;
                func(std::max(address,begin+run*4096),std::min({finish,end,begin+page*4096}));
            }
        }
    }
};
}
