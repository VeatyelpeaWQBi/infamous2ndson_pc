// SPDX-License-Identifier: GPL-2.0-or-later
// Exact read-only byte snapshots. No GPU-written buffer or stale guest pointer reuse.
#pragma once
#include <array>
#include <cstring>
#include <span>
#include "common/types.h"
namespace Vulkan {
class FlatDataMemo {
public:
    static constexpr size_t MaxBytes=2048, Capacity=16;
    struct Epoch { u64 tick{},wrap{}; auto operator<=>(const Epoch&) const = default; };
    struct Value { u64 offset{}; bool reused{}; };
    // Caller owns one recording thread and supplies the stream's current epoch.
    // Copy may wrap/submit, so store the epoch AFTER the actual upload.
    template<class EpochNow,class Copy>
    Value Upload(u64 stage,std::span<const u8> bytes,EpochNow now,Copy copy,bool enabled=true) {
        if(!enabled || bytes.empty() || bytes.size()>MaxBytes) return {copy(),false};
        auto& entry=entries[(stage>>4)%Capacity];
        if(entry.stage!=stage) { entry.stage=stage; entry.valid=false; entry.misses=0; entry.cooldown=0; }
        if(entry.cooldown) { --entry.cooldown; return {copy(),false}; }
        if(entry.valid && entry.epoch==now() && entry.size==bytes.size() &&
           std::memcmp(entry.bytes.data(),bytes.data(),bytes.size())==0) {
            entry.misses=0; return {entry.offset,true};
        }
        const u64 offset=copy();
        std::memcpy(entry.bytes.data(),bytes.data(),bytes.size());
        entry.epoch=now(); entry.offset=offset; entry.size=bytes.size(); entry.valid=true;
        if(++entry.misses>=8) { entry.cooldown=64; entry.misses=0; }
        return {offset,false};
    }
private:
    struct Entry {
        u64 stage{},offset{}; Epoch epoch{}; size_t size{};
        bool valid{}; u32 misses{},cooldown{};
        std::array<u8,MaxBytes> bytes{};
    };
    std::array<Entry,Capacity> entries{};
};
}
