// Local, opt-in correlation of synthesized query results with shader consumers.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace BbVisibilityTrace {
inline bool Enabled() {
    static const bool enabled=[] {
        const char* value=std::getenv("BB_OCCLUSION_TRACE");
        return value && value[0]=='1';
    }();
    return enabled;
}
inline std::mutex mutex;
inline std::array<std::uint64_t,32> pages{};
struct Binding { std::uint64_t shader,page; std::uint32_t slot; };
inline std::array<Binding,64> bindings{};
inline std::size_t page_count{},binding_count{};
inline void Result(std::uint64_t address) {
    if(!Enabled()) return;
    std::scoped_lock lock{mutex};
    const auto page=address & ~std::uint64_t{4095};
    for(std::size_t i=0;i<page_count;++i) if(pages[i]==page) return;
    if(page_count<pages.size()) pages[page_count++]=page;
}
inline void Buffer(std::uint64_t shader,std::uint32_t slot,std::uint64_t address,
                   std::uint64_t size,bool written) {
    if(!Enabled() || !size) return;
    std::scoped_lock lock{mutex};
    if(binding_count==bindings.size()) return;
    for(std::size_t i=0;i<page_count;++i) {
        const auto page=pages[i];
        if(address>=page+4096 || (address<page && size<=page-address)) continue;
        bool seen=false;
        for(std::size_t j=0;j<binding_count;++j)
            if(bindings[j].shader==shader && bindings[j].slot==slot && bindings[j].page==page)
                seen=true;
        if(seen) continue;
        bindings[binding_count++]={shader,page,slot};
        std::fprintf(stderr,"VISIBILITY_BUFFER shader=%016llx slot=%u address=%#llx bytes=%llu written=%d query_page=%#llx\n",
                     static_cast<unsigned long long>(shader),slot,
                     static_cast<unsigned long long>(address),static_cast<unsigned long long>(size),
                     int(written),static_cast<unsigned long long>(page));
        if(binding_count==bindings.size()) return;
    }
}
}
