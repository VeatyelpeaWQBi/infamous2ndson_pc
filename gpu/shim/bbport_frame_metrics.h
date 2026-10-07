// Windows gameplay telemetry: bounded numeric history, no formatting on the flip thread.
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace BbFrameMetrics {
inline constexpr const char* Columns =
    "draws,dispatches,submissions,command_cpu_ns,compile_count,compile_ns,"
    "image_bytes,buffer_bytes,protect_ns,refresh_ns,staging_ns,"
    "recorder_wait_ns,host_copy_wait_ns,copy_thread_wait_ns,gpu_tick_wait_ns,"
    "bind_ns,pipeline_select_ns";
using Counters = std::array<std::uint64_t,17>;
struct Frame {
    std::uint64_t sequence{}, tick_ms{}, interval_ns{};
    Counters delta{};
};
template <std::size_t CapacityValue>
struct HistoryBuffer {
    static constexpr std::size_t Capacity=CapacityValue;
    std::array<Frame,Capacity> frames{};
    Counters previous{};
    std::uint64_t sequence{}, previous_ns{};
    bool initialized{};
    void Reset() {
        previous={}; previous_ns=0; sequence=0; initialized=false;
    }
    void Push(std::uint64_t now_ns, std::uint64_t tick_ms, const Counters& values) {
        if (!initialized) {
            previous=values; previous_ns=now_ns; initialized=true; return;
        }
        Frame frame{++sequence,tick_ms,now_ns>=previous_ns ? now_ns-previous_ns : 0};
        for (std::size_t i=0;i<values.size();++i)
            frame.delta[i]=values[i]>=previous[i] ? values[i]-previous[i] : 0;
        frames[(sequence-1)%Capacity]=frame;
        previous=values; previous_ns=now_ns;
    }
    std::uint64_t First() const { return sequence>Capacity ? sequence-Capacity+1 : 1; }
};
using History = HistoryBuffer<1024>;
// 16,384 frames is about 4.5 minutes at 60 FPS and remains bounded. This
// buffer is populated only while the user explicitly records with F11.
using RecordingHistory = HistoryBuffer<16384>;
} // namespace BbFrameMetrics
