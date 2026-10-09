// SPDX-License-Identifier: GPL-2.0-or-later
// AYOUB1080p 153a5aa BindCache semantics, on the actual local recording thread.
#pragma once
#include <array>
#include <span>
#include <cstring>
#include <algorithm>
#include "video_core/renderer_vulkan/vk_common.h"
namespace Vulkan {
class RecordedBindings {
    struct Push {
        vk::PipelineLayout layout{};vk::ShaderStageFlags stages{};u32 offset{},size{};
        std::array<std::byte,128> bytes{};
    };
    struct Descriptor {
        struct Write {u32 binding,array,count;vk::DescriptorType type;
            bool operator==(const Write&) const=default;};
        struct Info {u64 a,b,c;bool operator==(const Info&) const=default;};
        vk::PipelineLayout layout{};u32 count{~0u},infos{};
        std::array<Write,64> writes;std::array<Info,128> values;
    };
    std::array<vk::Pipeline,2> pipelines{};
    std::array<Push,2> pushes{};
    std::array<Descriptor,2> descriptors{};
    std::array<vk::VertexInputBindingDescription2EXT,32> bindings{};
    std::array<vk::VertexInputAttributeDescription2EXT,32> attributes{};
    size_t binding_count{},attribute_count{};bool vertex_known{};
    static unsigned Index(vk::PipelineBindPoint point) {return point==vk::PipelineBindPoint::eCompute?1:0;}
public:
    void Forget() {pipelines={};for(auto& p:pushes)p.size=0;for(auto& d:descriptors)d.count=~0u;vertex_known=false;}
    void ForgetDescriptors(vk::PipelineBindPoint point) {descriptors[Index(point)].count=~0u;}
    bool SkipPipeline(vk::PipelineBindPoint point,vk::Pipeline pipeline) {
        const unsigned i=Index(point);const bool same=pipeline&&pipelines[i]==pipeline;
        if(!i&&!same)vertex_known=false;pipelines[i]=pipeline;return same;
    }
    bool SkipPush(vk::PipelineLayout layout,vk::ShaderStageFlags stages,u32 offset,u32 size,const void* data) {
        const bool compute=bool(stages&vk::ShaderStageFlagBits::eCompute);
        // Mixed stages can overwrite both caches. Retain no partial knowledge.
        if(compute && stages!=vk::ShaderStageFlagBits::eCompute) {
            for(auto& p:pushes)p.size=0;return false;
        }
        auto& p=pushes[compute?1:0];
        if(!size||size>p.bytes.size()||!data){p.size=0;return false;}
        if(p.size==size&&p.offset==offset&&p.layout==layout&&p.stages==stages&&
           !std::memcmp(p.bytes.data(),data,size))return true;
        p.layout=layout;p.stages=stages;p.offset=offset;p.size=size;
        std::memcpy(p.bytes.data(),data,size);return false;
    }
    bool SkipDescriptors(vk::PipelineBindPoint point,vk::PipelineLayout layout,u32 set,
                         std::span<const vk::WriteDescriptorSet> writes) {
        auto& d=descriptors[Index(point)];Descriptor next;next.layout=layout;
        if(set||writes.size()>next.writes.size()){d.count=~0u;return false;}
        next.count=u32(writes.size());
        for(size_t w=0;w<writes.size();++w) {
            const auto& v=writes[w];
            if(v.pNext||v.descriptorCount>next.values.size()-next.infos){d.count=~0u;return false;}
            next.writes[w]={v.dstBinding,v.dstArrayElement,v.descriptorCount,v.descriptorType};
            for(u32 i=0;i<v.descriptorCount;++i) {
                Descriptor::Info value;
                switch(v.descriptorType) {
                case vk::DescriptorType::eSampler:case vk::DescriptorType::eCombinedImageSampler:
                case vk::DescriptorType::eSampledImage:case vk::DescriptorType::eStorageImage:
                case vk::DescriptorType::eInputAttachment:
                    if(!v.pImageInfo){d.count=~0u;return false;}
                    value={u64(VkSampler(v.pImageInfo[i].sampler)),u64(VkImageView(v.pImageInfo[i].imageView)),u64(v.pImageInfo[i].imageLayout)};break;
                case vk::DescriptorType::eUniformTexelBuffer:case vk::DescriptorType::eStorageTexelBuffer:
                    if(!v.pTexelBufferView){d.count=~0u;return false;}
                    value={u64(VkBufferView(v.pTexelBufferView[i])),0,0};break;
                case vk::DescriptorType::eUniformBuffer:case vk::DescriptorType::eStorageBuffer:
                case vk::DescriptorType::eUniformBufferDynamic:case vk::DescriptorType::eStorageBufferDynamic:
                    if(!v.pBufferInfo){d.count=~0u;return false;}
                    value={u64(VkBuffer(v.pBufferInfo[i].buffer)),v.pBufferInfo[i].offset,v.pBufferInfo[i].range};break;
                default:d.count=~0u;return false;
                }
                next.values[next.infos++]=value;
            }
        }
        if(d.count==next.count&&d.infos==next.infos&&d.layout==next.layout&&
           std::equal(next.writes.begin(),next.writes.begin()+next.count,d.writes.begin())&&
           std::equal(next.values.begin(),next.values.begin()+next.infos,d.values.begin()))return true;
        d.layout=next.layout;d.count=next.count;d.infos=next.infos;
        std::copy_n(next.writes.begin(),next.count,d.writes.begin());
        std::copy_n(next.values.begin(),next.infos,d.values.begin());return false;
    }
    bool SkipVertex(std::span<const vk::VertexInputBindingDescription2EXT> b,
                    std::span<const vk::VertexInputAttributeDescription2EXT> a) {
        if(b.size()>bindings.size()||a.size()>attributes.size()){vertex_known=false;return false;}
        if(vertex_known&&binding_count==b.size()&&attribute_count==a.size()&&
           std::equal(b.begin(),b.end(),bindings.begin())&&std::equal(a.begin(),a.end(),attributes.begin()))return true;
        std::copy(b.begin(),b.end(),bindings.begin());std::copy(a.begin(),a.end(),attributes.begin());
        binding_count=b.size();attribute_count=a.size();vertex_known=true;return false;
    }
};
} // namespace Vulkan
