// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_set>
// bbport: multi-wave LDS synchronization from AYOUB1080p 153a5aa.
// WaveMemoryBarrierPass is intentionally not imported: its tail-path barrier
// relies on driver behavior for exited invocations instead of uniform execution.
#include "common/logging/log.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
// Returns the number of barriers inserted.
static u32 EmitBarrierInBlock(IR::Block* block) {
    u32 num_barriers{};
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
                ++num_barriers;
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
                ++num_barriers;
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        ir.Barrier();
        ++num_barriers;
    }
    return num_barriers;
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    ir.Barrier();
}

static bool IsDivergent(const IR::U1& cond) {
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// Values that can differ between the invocations of a workgroup spanning several host
// subgroups: invocation ids, subgroup operations (only uniform within one subgroup) and atomics
// (each invocation receives its own previous value).
static bool IsVaryingSource(const IR::Inst* inst) {
    const IR::Opcode opcode = inst->GetOpcode();
    if ((opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) ||
        (opcode >= IR::Opcode::BufferAtomicIAdd32 &&
         opcode <= IR::Opcode::BufferAtomicFCmpSwap32) ||
        (opcode >= IR::Opcode::ImageAtomicIAdd32 && opcode <= IR::Opcode::ImageAtomicCmpSwap32)) {
        return true;
    }
    switch (opcode) {
    case IR::Opcode::GetAttributeU32: {
        const IR::Attribute attribute = inst->Arg(0).Attribute();
        return attribute == IR::Attribute::LocalInvocationId ||
               attribute == IR::Attribute::LocalInvocationIndex ||
               attribute == IR::Attribute::GlobalInvocationId ||
               attribute == IR::Attribute::SubgroupLtMask;
    }
    case IR::Opcode::LaneId:
    case IR::Opcode::Ballot:
    case IR::Opcode::BallotFindLsb:
    case IR::Opcode::ReadLane:
    case IR::Opcode::ReadFirstLane:
    case IR::Opcode::WriteLane:
    case IR::Opcode::QuadBroadcast:
    case IR::Opcode::MaskedBitCount32:
    case IR::Opcode::DataAppend:
    case IR::Opcode::DataConsume:
        return true;
    default:
        return false;
    }
}

static bool IsDivergentStrict(const IR::U1& cond) {
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               if (IsVaryingSource(inst)) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

static bool IsDivergent(const IR::U1& cond, bool strict) {
    return strict ? IsDivergentStrict(cond) : IsDivergent(cond);
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static u32 EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                   NodeSet& divergence_end, u32& divergence_depth, bool strict) {
    const IR::U1 cond = data.if_node.cond;
    u32 num_barriers{};
    if (IsDivergent(cond, strict)) {
        if (divergence_depth == 0) {
            EmitBarrierAtBlockStart(data.if_node.merge);
            ++num_barriers;
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
    return num_barriers;
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list, bool strict) {
    NodeSet divergent_loops;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (IsDivergent(node.data.repeat.cond, strict)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (IsDivergent(node.data.break_node.cond, strict)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    if (shared_memory_size == 0 || !profile.needs_lds_barriers) {
        return;
    }
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup: its 64 invocations run in lockstep on GCN, not on hosts with smaller subgroups.
    const bool single_wave = threadgroup_size == GcnSubgroupSize;
    // Workgroups of several GCN waves synchronize their waves with barriers, but code may still
    // rely on the 64 invocations of each wave running in lockstep to share data in LDS without
    // barriers (sorting and reduction steps shorter than a wave). Hosts with smaller subgroups
    // split each wave, so those steps race: synchronize the uniform parts of such shaders too.
    const bool multi_wave = profile.lds_barriers_multi_wave &&
                            threadgroup_size > GcnSubgroupSize &&
                            threadgroup_size > profile.subgroup_size;
    if (!single_wave && !multi_wave) {
        return;
    }
    // Conditions only uniform within a subgroup are divergent in multi wave workgroups.
    const bool strict = multi_wave;
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    u32 num_barriers{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list, strict);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            num_barriers +=
                EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth, strict);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge);
                ++num_barriers;
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            num_barriers += EmitBarrierInBlock(node.data.block);
        }
    }
    if (multi_wave && num_barriers != 0) {
        LOG_INFO(Render_Recompiler,
                 "Compute shader {:#x}: {} LDS barriers for {} invocations ({} bytes of LDS) "
                 "sharing data within GCN waves",
                 program.info.pgm_hash, num_barriers, threadgroup_size, shared_memory_size);
    }
}

} // namespace Shader::Optimization
