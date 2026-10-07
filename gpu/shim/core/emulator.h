// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: shared title identity initialization for the Windows renderer.
#pragma once
#include "../../bbgpu.h"
#include "common/elf_info.h"
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info=Common::ElfInfo::Instance();
        info.initialized=true;
        info.game_serial=config.serial ? config.serial : "UNKNOWN";
        info.title=config.title ? config.title : "";
        info.sdk_ver=config.sdk_version;
        info.psf_attributes.raw=config.psf_attributes;
    }
};
}
