// SPDX-License-Identifier: GPL-2.0-or-later
// AYOUB specialization memo with exact input comparison and complete fetch-code checks.
#pragma once
#include "shader_recompiler/specialization.h"
namespace Vulkan {
using SpecializationWords=boost::container::small_vector<u32,256>;
inline SpecializationWords SpecializationInput(const Shader::Info& info) {
    SpecializationWords words;
    for(const auto& desc:info.buffers) {
        const auto sharp=desc.GetSharp(info);auto raw=std::bit_cast<std::array<u32,4>>(sharp);
        words.push_back(bool(sharp));raw[0]=0;raw[1]&=~0xffu;
        words.insert(words.end(),raw.begin(),raw.end());
    }
    for(const auto& desc:info.images) {
        const auto sharp=desc.GetSharp(info);auto raw=std::bit_cast<std::array<u32,8>>(sharp);
        words.push_back(bool(sharp));raw[0]=0;raw[1]&=~0x3fu;
        words.insert(words.end(),raw.begin(),raw.end());
    }
    for(const auto& desc:info.fmasks) {const auto sharp=desc.GetSharp(info);
        words.push_back(bool(sharp));words.push_back(sharp.width);words.push_back(sharp.height);}
    for(const auto& desc:info.samplers) {const auto sharp=desc.GetSharp(info);
        const auto raw=std::bit_cast<std::array<u32,4>>(sharp);words.insert(words.end(),raw.begin(),raw.end());}
    return words;
}
struct SpecializationMemo {
    SpecializationWords words;
    Shader::RuntimeInfo runtime{};Shader::Backend::Bindings bindings{};
    std::vector<u32> fetch_code;size_t permutation{};bool valid{};
    bool Matches(const Shader::Info& info,const Shader::RuntimeInfo& current,
                 Shader::Backend::Bindings start,const SpecializationWords& key,
                 const Shader::StageSpecialization& spec) const {
        if(!valid||runtime!=current||bindings!=start||words!=key)return false;
        if(!info.has_fetch_shader)return fetch_code.empty();
        if(!spec.fetch_shader_data||fetch_code.empty())return false;
        const auto* code=Shader::Gcn::GetFetchShaderCode(info,info.fetch_shader_sgpr_base);
        if(std::memcmp(code,fetch_code.data(),fetch_code.size()*4))return false;
        const auto& attributes=spec.fetch_shader_data->attributes;
        if(attributes.size()!=spec.vs_attribs.size())return false;
        for(size_t i=0;i<attributes.size();++i) {
            const auto& attr=attributes[i];const auto sharp=attr.GetSharp(info);
            Shader::VsAttribSpecialization expected{};
            if(sharp) {
                using Step=Shader::Gcn::VertexAttribute::InstanceIdType;
                const auto rate=attr.GetStepRate();
                if(rate!=Step::None)expected.divisor=rate==Step::OverStepRate0?current.sw.vs.step_rate_0:
                    rate==Step::OverStepRate1?current.sw.vs.step_rate_1:1;
                expected.num_class=AmdGpu::GetNumberClass(sharp.GetNumberFmt());expected.dst_select=sharp.DstSelect();
            }
            if(expected!=spec.vs_attribs[i])return false;
        }
        return true;
    }
    void Store(const Shader::Info& info,const Shader::RuntimeInfo& current,Shader::Backend::Bindings start,
               SpecializationWords key,const Shader::StageSpecialization& spec,size_t index) {
        valid=false;fetch_code.clear();
        if(info.has_fetch_shader) {
            if(!spec.fetch_shader_data)return;
            const u32 bytes=spec.fetch_shader_data->size;
            if(!bytes||bytes>16384||bytes%4)return;
            const auto* code=Shader::Gcn::GetFetchShaderCode(info,info.fetch_shader_sgpr_base);
            fetch_code.assign(code,code+bytes/4);
        }
        words=std::move(key);runtime=current;bindings=start;permutation=index;valid=true;
    }
};
} // namespace Vulkan
