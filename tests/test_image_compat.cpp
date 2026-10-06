// SPDX-License-Identifier: GPL-2.0-or-later
// Production 1D/array/cube shader lowering and BC texture allocation/transfer.
#include "vulkan_test.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "common/slot_vector.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include <spirv/unified1/spirv.hpp11>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/blit_helper.h"
#include <vk_mem_alloc.h>

static void shaders(const std::filesystem::path& directory) {
    using namespace Shader;
    std::filesystem::create_directories(directory);
    const AmdGpu::ImageType types[]={AmdGpu::ImageType::Color1D,AmdGpu::ImageType::Color1DArray,AmdGpu::ImageType::Cube};
    for (unsigned i=0;i<3;++i) {
        Info info{}; info.hw_stage=HwStage::Fragment; info.sw_stage=SwStage::Fragment;
        info.has_image_query=true;
        auto image=AmdGpu::Image::Null(false); image.type=u64(types[i]); image.width=15; image.height=0;
        image.depth=i==1 ? 1 : 0;
        ImageResource texture{}; texture.is_array=i!=0;
        std::memcpy(texture.sharp_fetch.immediates.data(),&image,sizeof(image));
        info.images.push_back(texture); info.samplers.push_back(SamplerResource{});
        RuntimeInfo runtime{}; runtime.Initialize(info.hw_stage,info.sw_stage);
        runtime.hw.fs.color_buffers[0].num_format=AmdGpu::NumberFormat::Float;
        runtime.hw.fs.color_buffers[1].num_format=AmdGpu::NumberFormat::Float;
        Common::ObjectPool<IR::Inst> pool; IR::Block block(pool); IR::IREmitter ir(block);
        IR::Value coords=ir.Imm32(0.5f);
        if (i==1) coords=ir.CompositeConstruct(ir.Imm32(0.5f),ir.Imm32(1.0f));
        if (i==2) coords=ir.CompositeConstruct(ir.Imm32(1.5f),ir.Imm32(1.5f),ir.Imm32(0.0f));
        const auto color=ir.ImageSampleExplicitLod(ir.Imm32(0u),coords,ir.Imm32(0.0f),{},{});
        const auto dimensions=ir.ImageQueryDimension(ir.Imm32(0u),ir.Imm32(0u),ir.Imm1(false),{});
        for (u32 c=0;c<4;++c) {
            info.stores.Set(IR::Attribute::RenderTarget0,c);
            info.stores.Set(IR::Attribute::RenderTarget1,c);
            ir.SetAttribute(IR::Attribute::RenderTarget0,IR::F32(ir.CompositeExtract(color,c)),c);
            ir.SetAttribute(IR::Attribute::RenderTarget1,IR::F32(ir.ConvertUToF(32,32,ir.CompositeExtract(dimensions,c))),c);
        }
        ir.Epilogue(); IR::Program program(info); program.blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
        Profile profile{}; profile.supported_spirv=0x00010600; profile.support_int64=true;
        Backend::Bindings bindings{};
        const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
        unsigned count=0;
        for (size_t at=5;at<code.size();) {
            const unsigned length=code[at]>>16; assert(length && at+length<=code.size());
            if ((code[at]&65535)==unsigned(spv::Op::OpTypeImage)) {
                assert(code[at+3]==unsigned(spv::Dim::Dim2D));
                assert(code[at+5]==(i==0 ? 0u : 1u)); ++count;
            }
            at+=length;
        }
        assert(count==1);
        const auto path=directory/(std::string("image-")+std::to_string(i)+".spv");
        std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(code.data()),code.size()*4);
        assert(file.good());
    }
    std::puts("Image compatibility: 1D/1D-array/cube sampling and size-query SPIR-V emitted as 2D PASS");
}

static void textures() {
    using namespace Vulkan;
    Instance instance(0,false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    Scheduler scheduler(instance); Runtime runtime(instance,scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    VkBuffer staging{}; VmaAllocation allocation{}; VmaAllocationInfo mapped{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=128,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage=VMA_MEMORY_USAGE_AUTO,.requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ac,&staging,&allocation,&mapped)==VK_SUCCESS);
    const vk::Format formats[]={vk::Format::eBc1RgbaUnormBlock,vk::Format::eBc1RgbaSrgbBlock,
        vk::Format::eBc4UnormBlock,vk::Format::eBc5UnormBlock};
    for (const auto format:formats) for (u32 layers:{1u,2u}) {
        const u32 block_bytes=format==vk::Format::eBc5UnormBlock ? 16 : 8;
        VideoCore::ImageInfo ci; ci.type=layers==1 ? AmdGpu::ImageType::Color1D : AmdGpu::ImageType::Color1DArray;
        ci.pixel_format=format; ci.size={4,1,1}; ci.resources={1,layers}; ci.num_bits=block_bytes*8; ci.props.is_block=true;
        VideoCore::Image image(instance,runtime,views,ci);
        assert(image.backing->image.image_ci.imageType==vk::ImageType::e2D);
        VideoCore::ImageViewInfo vi; vi.type=ci.type; vi.format=format;
        vi.range.extent={1,layers};
        VideoCore::ImageView view(instance,vi,image);
        assert(view.image_view);
        auto* bytes=static_cast<u8*>(mapped.pMappedData);
        const u32 size=block_bytes*layers; u8 expected[32]{};
        for (u32 j=0;j<size;++j) bytes[j]=expected[j]=u8(j+layers*17);
        auto cmd=scheduler.CommandBuffer();
        vk::ImageMemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eTransferDstOptimal,
            .image=image.GetImage(),.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,layers}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier},d);
        const vk::BufferImageCopy region{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,layers},.imageExtent={4,1,1}};
        cmd.copyBufferToImage(staging,image.GetImage(),vk::ImageLayout::eTransferDstOptimal,region,d);
        barrier.srcStageMask=vk::PipelineStageFlagBits2::eTransfer; barrier.srcAccessMask=vk::AccessFlagBits2::eTransferWrite;
        barrier.dstAccessMask=vk::AccessFlagBits2::eTransferRead;
        barrier.oldLayout=vk::ImageLayout::eTransferDstOptimal; barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier},d);
        auto readback=region; readback.bufferOffset=64;
        cmd.copyImageToBuffer(image.GetImage(),vk::ImageLayout::eTransferSrcOptimal,staging,readback,d);
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host},d);
        scheduler.Finish(); assert(std::memcmp(bytes+64,expected,size)==0);
        std::printf("BC format %d layers %u allocation/view/upload/readback PASS\n",int(format),layers);
    }
    vmaDestroyBuffer(instance.GetAllocator(),staging,allocation);
}
int main(int argc,char** argv) {
    if (argc==3 && !std::strcmp(argv[1],"--shaders")) shaders(argv[2]);
    else textures();
}
