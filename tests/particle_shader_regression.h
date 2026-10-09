// SPDX-License-Identifier: GPL-2.0-or-later
// Behavioral regressions for the selected shadPS4 / AYOUB1080p backports.
#pragma once
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include <limits>
#include "video_core/buffer_cache/readback_version.h"
#include "bbport_sampling.h"
#include "bbport_toggles.h"

static void particle_shader_cpu() {
    using namespace Shader;
    std::atomic<std::uint64_t> wait_total{0},guest_wait{0},gpu_wait{0};
    {BbStats::WaitTimer timer{wait_total,&guest_wait};}
    {BbStats::WaitTimer timer{wait_total,&gpu_wait};}
    assert(wait_total.load()==guest_wait.load()+gpu_wait.load());
    const auto categorized=wait_total.load();
    {BbStats::WaitTimer timer{wait_total};}
    assert(guest_wait.load()+gpu_wait.load()==categorized);
    BbDiagnosticSampler sampling{123};unsigned even=0,odd=0;
    for(unsigned i=0;i<65536;++i) if(sampling.Select(16)) {if(i&1) ++odd;else ++even;}
    assert(even>1800 && even<2300 && odd>1800 && odd<2300);
    // Periodic one-in-16 selection would miss every other shader stage.
    const auto before=sampling.state;assert(sampling.Select(1) && sampling.state==before);
    VideoCore::ReadbackVersion version{{{0x1000,0x2000},{0x4000,0x5000}}};
    version.Write(0x2000,0x2000);assert(version.valid); // Adjacent and the gap.
    version.Write(0x1000,0);assert(version.valid);
    version.Write(0x4000,1);assert(!version.valid); // Reject a newer GPU/CPU upload.
    version.valid=true;version.Write(~u64{0},2);assert(!version.valid);
    version=VideoCore::ReadbackVersion{{{0x1000,0x5000}}};
    version.Write(0x2004,4); // Another emitter changed only the second page.
    std::vector<std::pair<VAddr,VAddr>> retained;
    version.ForEachCurrentRange(0x1004,0x3008,[&](VAddr begin,VAddr end){retained.emplace_back(begin,end);});
    assert((retained==std::vector<std::pair<VAddr,VAddr>>{{0x1004,0x2000},{0x3000,0x400c}}));
    retained.clear();version.ForEachCurrentRange(0x2000,4096,[&](VAddr begin,VAddr end){retained.emplace_back(begin,end);});
    assert(retained.empty()); // Never publish stale bytes from the changed page.
    // The lane count also indexes another resource: removing it globally
    // corrupts that user. Cover append and both consume expression orders.
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
        Info info{};
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool); IR::IREmitter ir(block);
        const auto allocation = pattern == 0 ? ir.DataAppend(ir.Imm32(0u)) : ir.DataConsume(ir.Imm32(0u));
        const IR::U1 exec{allocation.Inst()->Arg(1)};
        const auto ballot = ir.UnpackUint2x32(ir.Ballot(exec));
        const auto hi = ir.MaskedBitCount(IR::U32{ir.CompositeExtract(ballot,1)},ir.Imm32(0u),true);
        const auto lane = ir.MaskedBitCount(IR::U32{ir.CompositeExtract(ballot,0)},hi,false);
        const auto retained = ir.IAdd(lane,ir.Imm32(123u));
        IR::U32 index;
        if (pattern == 0) index = ir.IAdd(allocation,lane);
        if (pattern == 1) index = ir.ISub(allocation,lane);
        if (pattern == 2) index = ir.ISub(ir.IAdd(allocation,ir.Imm32(-1)),lane);
        if (pattern == 3) index = ir.ISub(ir.Imm32(1u),ir.ISub(allocation,lane));
        auto* lane_user = pattern == 3 ? index.Inst()->Arg(1).Inst() : index.Inst();
        Optimization::ResourceDiscoveryList resources;
        resources.push_back({.user=allocation.Inst()});
        Optimization::ResourcePatchingPass(info,resources,Profile{});
        assert(lane_user->Arg(1).IsImmediate() && lane_user->Arg(1).U32()==0);
        assert(retained.Inst()->Arg(0)==IR::Value{lane.Inst()});
        assert(lane.Inst()->GetOpcode()==IR::Opcode::MaskedBitCount32);
    }
    // A workgroup with 128 lanes spans four NVIDIA subgroups. Shared writes
    // followed by reads need a rendezvous even though it is not a single wave.
    for (bool multi : {false,true}) {
        Info info{}; info.hw_stage=HwStage::Compute;
        RuntimeInfo runtime{}; runtime.Initialize(HwStage::Compute,SwStage::Compute);
        runtime.hw.cs.workgroup_size={128,1,1};runtime.hw.cs.shared_memory_size=512;
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);IR::IREmitter ir(block);
        ir.WriteShared(32,ir.Imm32(1u),ir.Imm32(0u));
        const auto read=ir.LoadShared(32,false,ir.Imm32(4u));
        ir.SetVectorReg(IR::VectorReg::V0,IR::U32{read});
        IR::Program program(info);program.blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
        Profile profile{};profile.needs_lds_barriers=true;profile.subgroup_size=32;
        profile.lds_barriers_multi_wave=multi;
        Optimization::SharedMemoryBarrierPass(program,runtime,profile);
        unsigned barriers=0;
        for (const auto& inst:block.Instructions()) barriers+=inst.GetOpcode()==IR::Opcode::Barrier;
        assert(multi ? barriers>=1 : barriers==0);
    }
    std::puts("Particle GDS shared lane users / consume patterns / multi-wave LDS rendezvous PASS");
}

static void particle_shader_gpu() {
    using namespace Shader;
    using namespace Vulkan;
    SetUnhandledExceptionFilter(+[](EXCEPTION_POINTERS* exception)->LONG {
        CONTEXT context=*exception->ContextRecord;
        std::fprintf(stderr,"Particle regression exception %#lx\n",exception->ExceptionRecord->ExceptionCode);
        for(unsigned i=0;i<12 && context.Rip;++i) {
            std::fprintf(stderr,"  %#llx\n",context.Rip);
            DWORD64 image_base=0,frame=0;PVOID data=nullptr;
            auto* entry=RtlLookupFunctionEntry(context.Rip,&image_base,nullptr);
            if(entry) RtlVirtualUnwind(UNW_FLAG_NHANDLER,image_base,context.Rip,entry,&context,&data,&frame,nullptr);
            else {context.Rip=*reinterpret_cast<const DWORD64*>(context.Rsp);context.Rsp+=8;}
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    Instance instance(0,false);Scheduler scheduler(instance);
    const auto device=instance.GetDevice();
    VideoCore::Buffer output(instance,0,4096,VideoCore::MemoryType::HostCached);
    const vk::DescriptorSetLayoutBinding binding{0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute};
    auto set=Check(device.createDescriptorSetLayoutUnique({.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=1,.pBindings=&binding}));
    const vk::DescriptorSetLayout set_handle=*set;
    auto layout=Check(device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&set_handle}));
    const auto run=[&](std::span<const u32> code) {
        std::fprintf(stderr,"Particle kernel: %zu SPIR-V words\n",code.size());
        auto module=Check(device.createShaderModuleUnique({.codeSize=code.size_bytes(),.pCode=code.data()}));
        auto pipeline=Check(device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,
            .module=*module,.pName="main"},.layout=*layout}));
        std::memset(output.mapped_data.data(),0,4096);output.Flush(0,4096);
        const vk::DescriptorBufferInfo buffer{output.Handle(),0,4096};
        const vk::WriteDescriptorSet write{.dstBinding=0,.descriptorCount=1,
            .descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer};
        const auto cmd=scheduler.CommandBuffer();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,write);
        cmd.dispatch(1,1,1);
        const vk::MemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask=vk::AccessFlagBits2::eShaderWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&barrier});
        scheduler.Finish();output.Invalidate(0,4096);
    };
    const auto emit=[&](Info& info,RuntimeInfo& runtime,IR::Program& program) {
        info.buffers.push_back({.used_types=IR::Type::U32,.buffer_type=BufferType::SharedMemory,.is_written=true});
        Profile profile{};profile.supported_spirv=0x00010600;profile.support_int64=true;
        profile.max_shared_memory_size=65536;profile.subgroup_size=32;
        profile.needs_lds_barriers=true;profile.lds_barriers_multi_wave=true;
        if(program.post_order_blocks.empty()) program.post_order_blocks=program.blocks;
        Optimization::SharedMemoryBarrierPass(program,runtime,profile);
        Optimization::CollectShaderInfoPass(program,profile);
        Backend::Bindings bindings{};
        return Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    };
    {
        Info info{};info.hw_stage=HwStage::Compute;info.sw_stage=SwStage::Compute;info.shared_types=IR::Type::U32;
        RuntimeInfo runtime{};runtime.Initialize(info.hw_stage,info.sw_stage);
        runtime.hw.cs.workgroup_size={128,1,1};runtime.hw.cs.shared_memory_size=512;
        Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);IR::IREmitter ir(block);
        const auto id=ir.GetAttributeU32(IR::Attribute::LocalInvocationId);
        const auto offset=ir.ShiftLeftLogical(id,ir.Imm32(2u));
        ir.WriteShared(32,ir.IAdd(id,ir.Imm32(1u)),offset);
        const auto neighbor=ir.ShiftLeftLogical(ir.BitwiseXor(id,ir.Imm32(32u)),ir.Imm32(2u));
        const auto value=ir.LoadShared(32,false,neighbor);
        ir.StoreBufferU32(1,ir.Imm32(0u),id,value,{});ir.Epilogue();
        IR::Program program(info);program.blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
        std::fprintf(stderr,"Particle LDS emission\n");
        const auto code=emit(info,runtime,program);
        for(unsigned repeat=0;repeat<32;++repeat) {
            run(code);
            const auto* words=reinterpret_cast<const u32*>(output.mapped_data.data());
            for(unsigned i=0;i<128;++i) assert(words[i]==(i^32u)+1);
        }
    }
    {
        Info info{};info.hw_stage=HwStage::Compute;info.sw_stage=SwStage::Compute;
        RuntimeInfo runtime{};runtime.Initialize(info.hw_stage,info.sw_stage);runtime.hw.cs.workgroup_size={1,1,1};
        Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);
        std::vector<Gcn::GcnInst> instructions;
        const std::array<std::pair<float,float>,4> inputs{{{0,std::numeric_limits<float>::infinity()},
            {-0.0f,std::numeric_limits<float>::quiet_NaN()},{std::numeric_limits<float>::infinity(),0},{2,3}}};
        const auto literal=[](float value) {return Gcn::InstOperand{.field=Gcn::OperandField::LiteralConst,
            .type=Gcn::ScalarType::Float32,.code=std::bit_cast<u32>(value)};};
        unsigned index=0;
        for(auto opcode:{Gcn::Opcode::V_MUL_LEGACY_F32,Gcn::Opcode::V_MAC_LEGACY_F32,Gcn::Opcode::V_MAD_LEGACY_F32}) {
            for(const auto [a,b]:inputs) {
                const Gcn::InstOperand dst{.field=Gcn::OperandField::VectorGPR,.type=Gcn::ScalarType::Float32,.code=index++};
                Gcn::GcnInst init{};init.opcode=Gcn::Opcode::V_MOV_B32;init.category=Gcn::InstCategory::VectorALU;
                init.src[0]=literal(7);init.dst[0]=dst;instructions.push_back(init);
                Gcn::GcnInst inst{};inst.opcode=opcode;inst.category=Gcn::InstCategory::VectorALU;
                inst.src[0]=literal(a);inst.src[1]=literal(b);inst.src[2]=literal(7);inst.dst[0]=dst;
                instructions.push_back(inst);
            }
        }
        const Profile translation_profile{};
        Gcn::Translator translator(info,runtime,translation_profile);
        translator.Translate(&block,0,IR::Condition::True,instructions);
        IR::IREmitter ir(block);
        for(unsigned i=0;i<12;++i) ir.StoreBufferU32(1,ir.Imm32(0u),ir.Imm32(i),
            ir.GetVectorReg<IR::U32>(IR::VectorReg(i)),{});
        ir.Epilogue();IR::Program program(info);program.blocks.push_back(&block);program.post_order_blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
        Optimization::SsaRewritePass(program);
        Optimization::DeadCodeEliminationPass(program);
        std::fprintf(stderr,"Particle legacy emission\n");
        run(emit(info,runtime,program));
        const auto* values=reinterpret_cast<const float*>(output.mapped_data.data());
        for(unsigned i=0;i<12;++i) assert(values[i]==(i<4 ? 0.f : 7.f)+(i%4==3 ? 6.f : 0.f));
    }
    std::puts("Particle GPU cross-subgroup shared exchange / translated legacy zero-Inf-NaN arithmetic PASS");
}
