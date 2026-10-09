// SPDX-License-Identifier: GPL-2.0-or-later
// Offline analysis using the same GCN instruction decoder as the runtime.
#pragma once
#include "shader_recompiler/frontend/decode.h"

static void gcn_disassemble(const std::filesystem::path& input,
                            const std::filesystem::path& output) {
    const auto bytes=std::filesystem::file_size(input);
    if(bytes<8 || bytes>65536 || bytes%4 ||
       std::filesystem::absolute(input)==std::filesystem::absolute(output))
        throw std::runtime_error("Invalid GCN input/output");
    std::vector<u32> code(bytes/4+4,0);
    std::ifstream file(input,std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(code.data()),bytes))
        throw std::runtime_error("Truncated GCN input");
    std::ofstream text(output);
    Shader::Gcn::GcnDecodeContext decoder;
    Shader::Gcn::GcnCodeSlice slice(code.data(),code.data()+bytes/4);
    while(slice.position()<code.data()+bytes/4) {
        const auto offset=4*(slice.position()-code.data());
        const auto before=slice.position();
        const auto instruction=decoder.decodeInstruction(slice);
        if(slice.position()<=before || slice.position()>code.data()+bytes/4)
            throw std::runtime_error("GCN instruction exceeded input bounds");
        text << fmt::format("{:06x} {}",offset,magic_enum::enum_name(instruction.opcode));
        for(u32 i=0;i<instruction.dst_count;++i)
            text << fmt::format(" dst{}={}:{:#x}",i,magic_enum::enum_name(instruction.dst[i].field),instruction.dst[i].code);
        for(u32 i=0;i<instruction.src_count;++i)
            text << fmt::format(" src{}={}:{:#x}",i,magic_enum::enum_name(instruction.src[i].field),instruction.src[i].code);
        text << '\n';
        if(instruction.opcode==Shader::Gcn::Opcode::S_ENDPGM) break;
    }
    if(!text) throw std::runtime_error("GCN export failed");
}
