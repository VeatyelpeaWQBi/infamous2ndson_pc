// Opt-in, bounded recent GPU operations; no allocations per operation.
#pragma once
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <windows.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#include "common/types.h"

namespace BbDiagnostics {
struct Event { u64 tick{},sequence{},a{},b{},c{},d{}; u32 thread{}; const char* kind{}; };
struct State {
    std::mutex mutex;
    std::array<Event,512> events{};
    u64 sequence{},presents{},last_present{},last_flush{};
    u32 width{},height{};
    std::string directory;
    bool failure_reported{};
    State() { if (const char* path=std::getenv("BB_DEBUG_DIR"); path && *path) directory=path; }
};
inline State& Get() { static State state; return state; }
inline bool Enabled() { return !Get().directory.empty(); }
inline void Record(const char* kind,u64 a=0,u64 b=0,u64 c=0,u64 d=0) {
    auto& s=Get(); if (s.directory.empty()) return;
    std::scoped_lock lock{s.mutex};
    const u64 sequence=++s.sequence;
    s.events[(sequence-1)%s.events.size()]={GetTickCount64(),sequence,a,b,c,d,GetCurrentThreadId(),kind};
}
// Safe to call after a host fault: never wait for another GPU thread's lock.
inline void Flush(bool force=false) {
    auto& s=Get(); if (s.directory.empty()) return;
    std::unique_lock lock{s.mutex,std::try_to_lock}; if (!lock.owns_lock()) return;
    const u64 now=GetTickCount64();
    if (!force && now-s.last_flush<1000) return;
    s.last_flush=now;
    const auto publish=[&](const char* name,auto write) {
        const std::string target=s.directory+"/"+name, pending=target+".next";
        FILE* f=std::fopen(pending.c_str(),"w");
        if (!f) return false;
        write(f);
        const bool ok=std::ferror(f)==0;
        const bool closed=std::fclose(f)==0;
        return ok && closed && MoveFileExA(pending.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING);
    };
    const bool trace=publish("gpu-recent.txt",[&](FILE* f) {
        std::fprintf(f,"tick_ms sequence thread kind a b c d; last %zu operations\n",s.events.size());
        const u64 first=s.sequence>s.events.size() ? s.sequence-s.events.size()+1 : 1;
        for (u64 seq=first;seq<=s.sequence;++seq) {
            const auto& e=s.events[(seq-1)%s.events.size()];
            std::fprintf(f,"%llu %llu %u %s %llx %llx %llx %llx\n",e.tick,e.sequence,e.thread,e.kind,e.a,e.b,e.c,e.d);
        }
    });
    const bool heartbeat=publish("gpu-heartbeat.json",[&](FILE* f) {
        std::fprintf(f,"{\"tick_ms\":%llu,\"last_present_ms\":%llu,\"presents\":%llu,\"operations\":%llu,\"width\":%u,\"height\":%u}\n",
            now,s.last_present,s.presents,s.sequence,s.width,s.height);
    });
    if ((!trace || !heartbeat) && !s.failure_reported) {
        s.failure_reported=true;
        std::fprintf(stderr,"DEBUG: GPU diagnostic write failed in %s\n",s.directory.c_str());
    }
}
inline void Presented(u32 width,u32 height) {
    auto& s=Get(); if (s.directory.empty()) return;
    {
        std::scoped_lock lock{s.mutex}; ++s.presents;
        s.last_present=GetTickCount64(); s.width=width; s.height=height;
    }
    Record("present",width,height);
    Flush();
}
} // namespace BbDiagnostics
