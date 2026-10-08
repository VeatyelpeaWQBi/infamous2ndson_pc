// SPDX-License-Identifier: GPL-2.0-or-later
// An owned snapshot of descriptor writes and all pointed-to infos, in one block.
#pragma once
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <vulkan/vulkan.hpp>
#include "common/types.h"
namespace Vulkan {
inline std::optional<size_t> DescriptorPackingSize(std::span<const vk::WriteDescriptorSet> writes) {
    constexpr size_t max=std::numeric_limits<size_t>::max();
    if(writes.size()>max/sizeof(vk::WriteDescriptorSet)) return {};
    size_t bytes=writes.size_bytes();
    for(const auto& w:writes) {
        // Unsupported extension-owned data retains the existing path.
        if(w.pNext) return {};
        const size_t unit=(w.pBufferInfo ? sizeof(vk::DescriptorBufferInfo):0)+
            (w.pImageInfo ? sizeof(vk::DescriptorImageInfo):0)+
            (w.pTexelBufferView ? sizeof(vk::BufferView):0);
        if(unit && w.descriptorCount>(max-bytes)/unit) return {};
        bytes+=w.descriptorCount*unit;
    }
    return bytes;
}
inline std::span<const vk::WriteDescriptorSet> PackDescriptorWrites(
    std::span<const vk::WriteDescriptorSet> writes,std::span<u8> storage) {
    const auto required=DescriptorPackingSize(writes);
    if(writes.empty()) return {};
    if(!required || *required>storage.size() ||
       reinterpret_cast<uintptr_t>(storage.data())%alignof(vk::WriteDescriptorSet)) return {};
    static_assert(alignof(vk::WriteDescriptorSet)>=alignof(vk::DescriptorBufferInfo));
    static_assert(alignof(vk::WriteDescriptorSet)>=alignof(vk::DescriptorImageInfo));
    static_assert(sizeof(vk::DescriptorBufferInfo)%alignof(vk::WriteDescriptorSet)==0);
    static_assert(sizeof(vk::DescriptorImageInfo)%alignof(vk::WriteDescriptorSet)==0);
    static_assert(sizeof(vk::BufferView)%alignof(vk::WriteDescriptorSet)==0);
    auto* packed=reinterpret_cast<vk::WriteDescriptorSet*>(storage.data());
    if(!writes.empty()) std::memcpy(packed,writes.data(),writes.size_bytes());
    u8* cursor=storage.data()+writes.size_bytes();
    const auto copy=[&](auto pointer,u32 count) {
        using T=std::remove_const_t<std::remove_pointer_t<decltype(pointer)>>;
        const size_t bytes=size_t(count)*sizeof(T);
        auto* result=reinterpret_cast<T*>(cursor);
        std::memcpy(cursor,pointer,bytes);cursor+=bytes;return result;
    };
    for(size_t i=0;i<writes.size();++i) {
        auto& w=packed[i];
        if(w.pBufferInfo) w.pBufferInfo=copy(w.pBufferInfo,w.descriptorCount);
        if(w.pImageInfo) w.pImageInfo=copy(w.pImageInfo,w.descriptorCount);
        if(w.pTexelBufferView) w.pTexelBufferView=copy(w.pTexelBufferView,w.descriptorCount);
    }
    return {packed,writes.size()};
}
}
