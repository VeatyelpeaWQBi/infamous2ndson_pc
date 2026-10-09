// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <memory>
#include <xxhash.h>
#include "common/assert.h"
#include "common/types.h"

namespace VideoCore {
// Hash the CPU upload version, not a guest read of the current GPU version.
// Bounded scratch storage preserves XXH3 identities across VMA/chunk boundaries.
class CpuBackingHasher {
public:
    template<class Copy> u64 Hash(VAddr address,u64 size,Copy&& copy) {
        ASSERT(state);
        ASSERT(XXH3_64bits_reset(state.get())==XXH_OK);
        for(u64 offset=0;offset<size;) {
            const u64 bytes=std::min<u64>(scratch.size(),size-offset);
            copy(address+offset,scratch.data(),bytes);
            ASSERT(XXH3_64bits_update(state.get(),scratch.data(),bytes)==XXH_OK);
            offset+=bytes;
        }
        return XXH3_64bits_digest(state.get());
    }
private:
    std::unique_ptr<XXH3_state_t,decltype(&XXH3_freeState)> state{XXH3_createState(),XXH3_freeState};
    std::array<u8,64*1024> scratch{};
};
}
