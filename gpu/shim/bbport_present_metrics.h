// Actual fresh game presentations, distinct from guest flip requests.
#pragma once
#include <array>
#include <cstdint>
namespace BbPresentMetrics {
struct Row { std::uint64_t sequence{},tick_ms{},interval_ns{}; std::uint32_t width{},height{}; };
struct History {
    static constexpr size_t Capacity=4096;
    std::array<Row,Capacity> rows{};
    std::uint64_t sequence{},last_ns{};
    void Push(std::uint64_t ns,std::uint64_t tick,std::uint32_t width,std::uint32_t height) {
        if(last_ns && ns>=last_ns) {
            ++sequence; rows[(sequence-1)%Capacity]={sequence,tick,ns-last_ns,width,height};
        }
        last_ns=ns;
    }
    std::uint64_t First() const { return sequence>=Capacity ? sequence-Capacity+1 : 1; }
};
}
