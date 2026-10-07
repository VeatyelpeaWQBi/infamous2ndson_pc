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
#include "bbport_diagnostics.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "core/platform.h"
#include "video_core/renderer_vulkan/stencil_reference.h"
#include <thread>
#include <atomic>
#include "bbport_mouse_motion.h"

static void mouse_motion_input() {
    BbMouseMotionInput input;
    SDL_Event event{}; BbMouseMotion sample{};
    event.type=SDL_EVENT_KEY_DOWN; event.key.windowID=42; event.key.key=SDLK_F6;
    input.Event(event,42,true); assert(input.Active());
    event.key.repeat=true; input.Event(event,42,true); assert(input.Active());
    event={}; event.type=SDL_EVENT_MOUSE_MOTION; event.motion.windowID=42;
    event.motion.xrel=12; event.motion.yrel=-8;
    input.Event(event,42,true); input.Event(event,42,true); input.Read(&sample);
    assert(sample.active && sample.dx==24 && sample.dy==-16);
    input.Read(&sample); assert(sample.dx==0 && sample.dy==0); // consumed exactly once
    event.motion.windowID=99; input.Event(event,42,true); input.Read(&sample);
    assert(sample.dx==0); // another window cannot steer the game
    event={}; event.type=SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.windowID=42; event.button.button=SDL_BUTTON_LEFT;
    input.Event(event,42,true); input.Read(&sample); assert(sample.left);
    input.Event(event,42,false); input.Read(&sample); assert(!sample.active && !sample.left);
    event={}; event.type=SDL_EVENT_KEY_DOWN; event.key.windowID=42; event.key.key=SDLK_F6;
    input.Event(event,42,true); input.Read(&sample); const auto generation=sample.reset;
    event.key.key=SDLK_F7; input.Event(event,42,true); input.Read(&sample);
    assert(sample.active && sample.reset==generation+1);
    event.key.key=SDLK_ESCAPE; input.Event(event,42,true); assert(!input.Active());
    std::puts("Mouse motion: toggle, repeat suppression, window isolation, deltas, buttons, focus/menu release PASS");
}

static void stencil_reference() {
    using Vulkan::StencilReference::Select;
    // Real captured Second Son pass: NE ref 4 with compare mask 4, no writes.
    const u32 reference=Select(4,0,0,true);
    for(u32 stored=0;stored<256;++stored) {
        assert(((reference&4)!=(stored&4)) == ((4u&4)!=(stored&4)));
        assert(((reference&4)==(stored&4)) == ((4u&4)==(stored&4)));
        const u32 after=(stored&~0u)|(reference&0u);
        assert(after==stored);
    }
    assert(Select(4,0,0,false)==4);
    assert(Select(4,0,255,true)==0);
    assert(Select(4,32,255,true)==32);
    assert(Select(4,32,255,false)==4);
    // Writable REPLACE_OP keeps the prior write semantics for all byte values.
    for(u32 value=0;value<256;++value)
        assert((Select(4,value,255,true)&255)==value);
    std::puts("Stencil: masked-out replacement preserves comparison; writable replacement unchanged PASS");
}

static void irq_controller() {
    Platform::IrqController controller;
    std::array<std::thread,8> workers;
    std::atomic<unsigned> persistent{}, once{};
    std::array<unsigned,8> identities{};
    const Platform::InterruptId ids[]={Platform::InterruptId::Compute0RelMem,
        Platform::InterruptId::Compute1RelMem,Platform::InterruptId::Compute2RelMem,
        Platform::InterruptId::Compute3RelMem,Platform::InterruptId::Compute4RelMem,
        Platform::InterruptId::Compute5RelMem,Platform::InterruptId::Compute6RelMem,
        Platform::InterruptId::GfxFlip};
    for(unsigned i=0;i<workers.size();++i) workers[i]=std::thread([&,i] {
        controller.Register(ids[i],[&](auto) { ++persistent; },&identities[i]);
        for(unsigned j=0;j<1000;++j) {
            controller.RegisterOnce(ids[i],[&](auto) { ++once; });
            controller.Signal(ids[i]);
        }
        controller.Unregister(ids[i],&identities[i]);
        controller.Signal(ids[i]);
    });
    for(auto& worker:workers) worker.join();
    assert(persistent==8000 && once==8000);
    once=0;
    for(auto& worker:workers) worker=std::thread([&] {
        for(unsigned j=0;j<500;++j)
            controller.RegisterOnce(Platform::InterruptId::GfxEop,[&](auto) { ++once; });
    });
    for(auto& worker:workers) worker.join();
    for(unsigned i=0;i<4001;++i) controller.Signal(Platform::InterruptId::GfxEop);
    assert(once==4000);
    std::puts("Concurrent independent IRQs, persistent removal and shared one-time queue PASS");
}

static void frame_capture(const std::filesystem::path& directory) {
    using Vulkan::FrameCapture;
    std::filesystem::create_directories(directory);
    const auto trigger = directory / "capture-request";
    _putenv_s("BB_CAPTURE_TRIGGER",trigger.string().c_str());
    _putenv_s("BB_CAPTURE_DIR",directory.string().c_str());
    _putenv_s("BB_DEBUG_DIR","");
    VideoCore::ImageInfo display{}, scene{};
    display.guest_address=0x100000; display.size={1920,1080,1};
    display.pixel_format=vk::Format::eR8G8B8A8Unorm;
    scene=display; scene.guest_address=0x200000;
    const VideoCore::ImageInfo* display_ptr=&display;
    const VideoCore::ImageInfo* scene_ptr=&scene;
    FrameCapture::AddDisplayBuffer(display.guest_address);
    FrameCapture::OnFlip(display.guest_address);
    assert(!FrameCapture::Active());
    std::ofstream(trigger).put('1');
    FrameCapture::OnFlip(display.guest_address);
    assert(FrameCapture::Active() && !std::filesystem::exists(trigger));
    FrameCapture::BeginPass(&display_ptr,1,nullptr);
    FrameCapture::Draw(0xabc,0xdef,3,1);
    FrameCapture::BeginPass(&scene_ptr,1,nullptr);
    FrameCapture::Sampled(display,false);
    FrameCapture::Draw(0x123,0x456,36,2);
    float constants[256]{};
    for(unsigned i=0;i<300;++i) FrameCapture::Buffer(0x123,i,0x400000,constants,sizeof(constants));
    FrameCapture::Draw(0x123,0x456,3,1);
    FrameCapture::Dispatch(0x789,4,2,1);
    FrameCapture::BeginPass(&display_ptr,1,nullptr);
    assert(!FrameCapture::Active());
    std::filesystem::path report;
    for(const auto& entry:std::filesystem::directory_iterator(directory))
        if(entry.path().extension()==".txt") report=entry.path();
    assert(!report.empty());
    std::ifstream input(report); const std::string text((std::istreambuf_iterator<char>(input)),{});
    assert(text.find("DEBUG_FRAME_BEGIN")!=std::string::npos);
    assert(text.find("truncated=1")!=std::string::npos);
    assert(text.find("PASS draws 2 (indices 75)")!=std::string::npos);
    assert(text.find("COMPUTE dispatches 1")!=std::string::npos);
    assert(text.find("samples 0x100000")!=std::string::npos);
    assert(text.find("DEBUG_FRAME_END passes=3")!=std::string::npos);
    assert(text.size()<2*1024*1024);
    std::puts("Frame capture: trigger, complete frame boundaries, sampled resources, bounded constants PASS");
}

static void diagnostic_ring(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    _putenv_s("BB_DEBUG_DIR",directory.string().c_str());
    assert(BbDiagnostics::Enabled());
    std::thread first([] { for (unsigned i=0;i<1500;++i) BbDiagnostics::Record("draw",i,1); });
    std::thread second([] { for (unsigned i=0;i<1500;++i) BbDiagnostics::Record("dispatch",i,2); });
    first.join(); second.join();
    BbDiagnostics::Presented(1920,1080);
    BbDiagnostics::Flush(true);
    std::ifstream input(directory/"gpu-recent.txt");
    std::string line; std::getline(input,line);
    unsigned count=0; u64 previous=0;
    while (std::getline(input,line)) {
        unsigned long long tick,sequence;
        assert(std::sscanf(line.c_str(),"%llu %llu",&tick,&sequence)==2);
        assert(sequence>previous); previous=sequence; ++count;
    }
    assert(count==512 && previous==3001);
    std::ifstream heartbeat(directory/"gpu-heartbeat.json");
    std::getline(heartbeat,line);
    assert(line.find("\"presents\":1")!=std::string::npos);
    assert(line.find("\"operations\":3001")!=std::string::npos);
    // Fault-time flush must not wait on another producer's held mutex.
    std::atomic<bool> locked=false,release=false;
    std::thread holder([&] {
        std::scoped_lock lock{BbDiagnostics::Get().mutex}; locked=true;
        while (!release) std::this_thread::yield();
    });
    while (!locked) std::this_thread::yield();
    const auto start=std::chrono::steady_clock::now(); BbDiagnostics::Flush(true);
    const auto elapsed=std::chrono::steady_clock::now()-start;
    release=true; holder.join();
    assert(elapsed<std::chrono::milliseconds(500));
    // A blocked diagnostic writer must not block render producers or crash flush.
    locked=false; release=false;
    std::thread io_holder([&] {
        std::scoped_lock lock{BbDiagnostics::Get().writer_mutex}; locked=true;
        while(!release) std::this_thread::yield();
    });
    while(!locked) std::this_thread::yield();
    const auto io_start=std::chrono::steady_clock::now();
    BbDiagnostics::RecordDraw(3,1,0,10,20,30);
    BbDiagnostics::Presented(1280,720); BbDiagnostics::Flush(true);
    const auto io_elapsed=std::chrono::steady_clock::now()-io_start;
    release=true; io_holder.join();
    assert(io_elapsed<std::chrono::milliseconds(500));
    BbFrameMetrics::History history;
    BbFrameMetrics::Counters values{}; values[0]=10;
    history.Push(1000000000,1000,values);
    values[0]=17; history.Push(1016000000,1016,values);
    assert(history.sequence==1 && history.frames[0].interval_ns==16000000 && history.frames[0].delta[0]==7);
    values[0]=1; history.Push(1032000000,1032,values);
    assert(history.frames[1].delta[0]==0); // reset cannot underflow
    for(unsigned i=0;i<1200;++i) history.Push(1048000000ull+i*16000000ull,1048+i*16,values);
    assert(history.sequence==1202 && history.First()==179);
    // Queue overflow is explicit; newest frames survive and serialize in order.
    auto& state=BbDiagnostics::Get();
    { std::scoped_lock lock{state.mutex}; state.frames=history; }
    BbDiagnostics::Flush(true);
    assert(state.written_frame==1202 && state.dropped_frames==178);
    assert(std::filesystem::file_size(directory/"frames.csv")>1000);
    const auto size=std::filesystem::file_size(directory/"frames.csv");
    BbDiagnostics::Flush(true); assert(std::filesystem::file_size(directory/"frames.csv")==size);
    // Writer lifecycle flushes the remaining numeric frames without a GPU.
    {
        BbDiagnostics::Writer writer;
        BbDiagnostics::Frame(values);
    }
    assert(state.written_frame==1203);
    const bool active_recording=BbDiagnostics::ToggleRecording();
    assert(active_recording);
    for (unsigned i=0;i<4;++i) BbDiagnostics::Frame(values);
    assert(!BbDiagnostics::ToggleRecording());
    BbDiagnostics::Flush(true);
    assert(std::filesystem::exists(directory/"performance-recording-1.csv"));
    const auto recording_text=std::ifstream(directory/"performance-recording-1.csv");
    assert(recording_text.good());
    std::puts("Bounded recent GPU operations / telemetry / fault flush PASS");
}

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
    {
        Scheduler deferred(instance,true);
        // Oversized copy-region arrays must remain owned until the recording
        // worker executes them, even after the caller changes its input.
        std::vector<u64> source(40000);
        for (size_t i=0;i<source.size();++i) source[i]=i*17+3;
        const auto captured=deferred.RecordData(std::span<const u64>{source});
        std::fill(source.begin(),source.end(),0);
        bool executed=false;
        deferred.Record([captured,&executed](vk::CommandBuffer) {
            for (size_t i=0;i<captured.size();++i) assert(captured[i]==i*17+3);
            executed=true;
        });
        deferred.SyncRecording();
        assert(executed);
        deferred.Finish();
        std::puts("Oversized deferred recording data ownership PASS");
    }
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
    if (argc==2 && !std::strcmp(argv[1],"--irq")) irq_controller();
    else if (argc==2 && !std::strcmp(argv[1],"--stencil")) stencil_reference();
    else if (argc==2 && !std::strcmp(argv[1],"--mouse-motion")) mouse_motion_input();
    else if (argc==3 && !std::strcmp(argv[1],"--shaders")) shaders(argv[2]);
    else if (argc==3 && !std::strcmp(argv[1],"--diagnostics")) diagnostic_ring(argv[2]);
    else if (argc==3 && !std::strcmp(argv[1],"--frame-capture")) frame_capture(argv[2]);
    else textures();
}
