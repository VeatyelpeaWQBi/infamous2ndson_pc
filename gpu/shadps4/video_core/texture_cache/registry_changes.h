// SPDX-License-Identifier: GPL-2.0-or-later
// Windows renderer: bounded image-registry invalidation, owned by the cache mutex.
#pragma once
#include <array>
#include <limits>
#include <span>
#include "common/types.h"
namespace VideoCore {
struct RegistryRange {
    VAddr begin{},end{};
    static RegistryRange FromSize(VAddr address,u64 size) {
        if (!size) return {};
        if (size>std::numeric_limits<u64>::max()-address)
            return {0,std::numeric_limits<u64>::max()};
        return {address,address+size};
    }
    bool Overlaps(const RegistryRange& other) const {
        return begin<end && other.begin<other.end && begin<other.end && other.begin<end;
    }
};
class RegistryChanges {
public:
    static constexpr size_t Capacity=64;
    void Record(RegistryRange range) {
        changes[generation%Capacity]=range;
        ++generation;
    }
    // Unknown/expired history always misses; never guesses that an ID is alive.
    bool Unchanged(u64& previous,std::span<const RegistryRange> ranges) const {
        if (previous>generation || generation-previous>Capacity) return false;
        for(u64 at=previous;at<generation;++at)
            for(const auto& range:ranges)
                if(changes[at%Capacity].Overlaps(range)) return false;
        previous=generation;
        return true;
    }
    u64 Generation() const { return generation; }
private:
    std::array<RegistryRange,Capacity> changes{};
    u64 generation{};
};
}
