// SPDX-License-Identifier: GPL-2.0-or-later
// AYOUB1080p 153a5aa GetStageParams: cache code metadata, never user data.
#pragma once
#include "video_core/amdgpu/regs_shader.h"
namespace Vulkan {
class ShaderParamsMemo {
    struct Entry {VAddr address{};u64 epoch{},host{},hash{};u32 words{};};
    std::array<Entry,64> entries{};
public:
    template<typename Registers>
    Shader::ShaderParams Get(const Registers& regs,u64 epoch,u64 host) {
        const auto* code=regs.template Address<const u32*>();
        const VAddr address=reinterpret_cast<VAddr>(code);
        auto& entry=entries[(address*0x9E3779B97F4A7C15ULL)>>58];
        if(entry.words&&entry.address==address&&entry.epoch==epoch&&entry.host==host)
            return {.user_data=regs.user_data,.code=std::span{code,entry.words},.hash=entry.hash};
        const auto params=AmdGpu::GetParams(regs);
        entry={address,epoch,host,params.hash,u32(params.code.size())};return params;
    }
};
} // namespace Vulkan
