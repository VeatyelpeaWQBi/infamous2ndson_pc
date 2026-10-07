// Opt-in, bounded recent GPU operations; no allocations per operation.
#pragma once
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <memory>
#include "bbport_frame_metrics.h"
#include <windows.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#include "common/types.h"

namespace BbDiagnostics {
struct Event { u64 tick{},sequence{},a{},b{},c{},d{}; u32 thread{}; const char* kind{}; };
struct State {
    std::mutex mutex;
    std::mutex writer_mutex; // never held by Record/Presented/Frame
    std::array<Event,512> events{};
    u64 sequence{},presents{},last_present{},last_flush{};
    u32 width{},height{};
    std::string directory;
    bool failure_reported{};
    BbFrameMetrics::History frames;
    BbFrameMetrics::RecordingHistory recording_frames;
    u64 written_frame{},frame_file_bytes{},dropped_frames{};
    bool recording{},record_pending{};
    u64 recording_id{},record_start_sequence{},record_end_sequence{};
    u64 recording_start_tick{},recording_end_tick{},pending_recording_id{};
    State() { if (const char* path=std::getenv("BB_DEBUG_DIR"); path && *path) directory=path; }
};
inline State& Get() { static State state; return state; }
inline bool Enabled() { return !Get().directory.empty(); }
inline bool ToggleRecording() {
    auto& s=Get(); if (s.directory.empty()) return false;
    std::scoped_lock lock{s.mutex};
    if (!s.recording) {
        s.recording=true;
        s.recording_id++;
        s.recording_frames.Reset();
        s.record_start_sequence=1;
        s.recording_start_tick=GetTickCount64();
        std::fprintf(stderr,"PERF_RECORDING START id=%llu tick_ms=%llu\n",
                     static_cast<unsigned long long>(s.recording_id),
                     static_cast<unsigned long long>(s.recording_start_tick));
    } else {
        s.recording=false;
        s.record_end_sequence=s.recording_frames.sequence;
        s.recording_end_tick=GetTickCount64();
        s.pending_recording_id=s.recording_id;
        s.record_pending=true;
        std::fprintf(stderr,"PERF_RECORDING STOP id=%llu tick_ms=%llu\n",
                     static_cast<unsigned long long>(s.recording_id),
                     static_cast<unsigned long long>(s.recording_end_tick));
    }
    std::fflush(stderr);
    return s.recording;
}
inline void Record(const char* kind,u64 a=0,u64 b=0,u64 c=0,u64 d=0) {
    auto& s=Get(); if (s.directory.empty()) return;
    std::scoped_lock lock{s.mutex};
    const u64 sequence=++s.sequence;
    s.events[(sequence-1)%s.events.size()]={GetTickCount64(),sequence,a,b,c,d,GetCurrentThreadId(),kind};
}
// Two events per draw, one lock/clock/thread-id lookup; retain the existing trace format.
inline void RecordDraw(u64 indices,u64 instances,u64 indexed,u64 pipeline,u64 vs,u64 ps) {
    auto& s=Get(); if (s.directory.empty()) return;
    const auto tick=GetTickCount64(); const auto thread=GetCurrentThreadId();
    std::scoped_lock lock{s.mutex};
    auto seq=++s.sequence;
    s.events[(seq-1)%s.events.size()]={tick,seq,indices,instances,indexed,pipeline,thread,"draw"};
    seq=++s.sequence;
    s.events[(seq-1)%s.events.size()]={tick,seq,vs,ps,0,0,thread,"shaders"};
}
inline void Frame(const BbFrameMetrics::Counters& counters) {
    auto& s=Get(); if(s.directory.empty()) return;
    const auto now=std::chrono::steady_clock::now().time_since_epoch();
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::scoped_lock lock{s.mutex};
    const auto tick=GetTickCount64();
    s.frames.Push(ns,tick,counters);
    if (s.recording) s.recording_frames.Push(ns,tick,counters);
}
// Safe to call after a host fault: never wait for another GPU thread's lock.
inline void Flush(bool force=false) {
    auto& s=Get(); if (s.directory.empty()) return;
    std::unique_lock writer{s.writer_mutex,std::try_to_lock}; if (!writer.owns_lock()) return;
    const u64 now=GetTickCount64();
    if (!force && now-s.last_flush<1000) return;
    std::unique_lock lock{s.mutex,std::try_to_lock}; if (!lock.owns_lock()) return;
    const auto events=s.events;
    const auto frames=s.frames;
    std::unique_ptr<BbFrameMetrics::RecordingHistory> recording_frames;
    const auto sequence=s.sequence,presents=s.presents,last_present=s.last_present;
    const auto width=s.width,height=s.height;
    const bool record_pending=s.record_pending;
    const u64 record_start=s.record_start_sequence,record_end=s.record_end_sequence,
              record_start_tick=s.recording_start_tick,record_end_tick=s.recording_end_tick,
              record_id=s.pending_recording_id;
    if (record_pending) {
        recording_frames=std::make_unique<BbFrameMetrics::RecordingHistory>(s.recording_frames);
        s.record_pending=false;
    }
    s.last_flush=now;
    lock.unlock(); // filesystem latency must never stall render producers
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
        std::fprintf(f,"tick_ms sequence thread kind a b c d; last %zu operations\n",events.size());
        const u64 first=sequence>events.size() ? sequence-events.size()+1 : 1;
        for (u64 seq=first;seq<=sequence;++seq) {
            const auto& e=events[(seq-1)%events.size()];
            std::fprintf(f,"%llu %llu %u %s %llx %llx %llx %llx\n",e.tick,e.sequence,e.thread,e.kind,e.a,e.b,e.c,e.d);
        }
    });
    const bool heartbeat=publish("gpu-heartbeat.json",[&](FILE* f) {
        std::fprintf(f,"{\"tick_ms\":%llu,\"last_present_ms\":%llu,\"presents\":%llu,\"operations\":%llu,\"width\":%u,\"height\":%u}\n",
            now,last_present,presents,sequence,width,height);
    });
    bool frame_ok=true;
    if (frames.sequence>s.written_frame) {
        const std::string path=s.directory+"/frames.csv";
        constexpr u64 Limit=2*1024*1024; // current + one backup <= 4 MiB
        const u64 first=std::max<u64>(s.written_frame+1,frames.First());
        const u64 dropped=s.dropped_frames+(first-s.written_frame-1);
        FILE* f=nullptr;
        for (u64 seq=first;seq<=frames.sequence;++seq) {
            const auto& row=frames.frames[(seq-1)%frames.Capacity];
            char line[1024];
            int used=std::snprintf(line,sizeof(line),"%llu,%llu,%llu,%llu",row.sequence,row.tick_ms,row.interval_ns,dropped);
            for (auto value:row.delta) used+=std::snprintf(line+used,sizeof(line)-used,",%llu",static_cast<unsigned long long>(value));
            line[used++]='\n';
            if (s.frame_file_bytes+used>Limit) {
                if(f) { std::fclose(f); f=nullptr; }
                if(!MoveFileExA(path.c_str(),(path+".1").c_str(),MOVEFILE_REPLACE_EXISTING)) { frame_ok=false; break; }
                s.frame_file_bytes=0;
            }
            if(!f) {
                f=std::fopen(path.c_str(),s.frame_file_bytes ? "ab" : "wb");
                if(!f) { frame_ok=false; break; }
                if(!s.frame_file_bytes) {
                    const int n=std::fprintf(f,"sequence,tick_ms,interval_ns,dropped_frames,%s\n",BbFrameMetrics::Columns);
                    if(n<0) { frame_ok=false; break; }
                    s.frame_file_bytes=n;
                }
            }
            if(std::fwrite(line,1,used,f)!=size_t(used)) { frame_ok=false; break; }
            s.frame_file_bytes+=used; s.written_frame=seq; s.dropped_frames=dropped;
        }
        if(f && std::fclose(f)!=0) frame_ok=false;
    }
    if (record_pending) {
        const std::string path=s.directory+"/performance-recording-"+
            std::to_string(static_cast<unsigned long long>(record_id))+".csv";
        const auto& captured=*recording_frames;
        const u64 first=std::max<u64>(record_start,captured.First());
        const u64 last=std::min<u64>(record_end,captured.sequence);
        const u64 lost=first>record_start ? first-record_start : 0;
        FILE* f=std::fopen(path.c_str(),"wb");
        bool ok=f!=nullptr;
        if (ok) {
            ok=std::fprintf(f,"# recording_id=%llu start_tick_ms=%llu end_tick_ms=%llu dropped_frames=%llu\n",
                static_cast<unsigned long long>(record_id),
                static_cast<unsigned long long>(record_start_tick),
                static_cast<unsigned long long>(record_end_tick),
                static_cast<unsigned long long>(lost))>0;
            if (ok) ok=std::fprintf(f,"sequence,tick_ms,interval_ns,%s\n",BbFrameMetrics::Columns)>0;
            for (u64 seq=first;ok && seq<=last;++seq) {
                const auto& row=captured.frames[(seq-1)%captured.Capacity];
                std::fprintf(f,"%llu,%llu,%llu",static_cast<unsigned long long>(row.sequence),
                             static_cast<unsigned long long>(row.tick_ms),
                             static_cast<unsigned long long>(row.interval_ns));
                for (auto value:row.delta) std::fprintf(f,",%llu",static_cast<unsigned long long>(value));
                std::fputc('\n',f);
                ok=std::ferror(f)==0;
            }
            ok=std::fclose(f)==0 && ok;
        }
        if (ok) std::fprintf(stderr,"PERF_RECORDING SAVED id=%llu frames=%llu dropped=%llu path=%s\n",
            static_cast<unsigned long long>(record_id),
            static_cast<unsigned long long>(last>=first ? last-first+1 : 0),
            static_cast<unsigned long long>(lost),path.c_str());
        else std::fprintf(stderr,"DEBUG: performance recording write failed path=%s\n",path.c_str());
        std::fflush(stderr);
    }
    if ((!trace || !heartbeat || !frame_ok) && !s.failure_reported) {
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
}
// Owned by the window thread. No disk operations on rendering/presentation threads.
class Writer {
    std::jthread thread;
public:
    Writer() {
        if(Enabled()) thread=std::jthread([](std::stop_token stop) {
            SetThreadDescription(GetCurrentThread(),L"bb:Diagnostics");
            while(!stop.stop_requested()) {
                Flush(); std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            Flush(true);
        });
    }
};
} // namespace BbDiagnostics
