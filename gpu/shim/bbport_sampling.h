// Independent pseudo-random timing samples avoid aliasing alternating VS/PS work.
#pragma once
#include <cstdint>
struct BbDiagnosticSampler {
    std::uint32_t state;
    constexpr explicit BbDiagnosticSampler(std::uint32_t seed) : state{seed ? seed : 1} {}
    bool Select(unsigned stride) {
        if(stride<=1) return true;
        state^=state<<13;state^=state>>17;state^=state<<5;
        return (state & (stride-1))==0;
    }
};
