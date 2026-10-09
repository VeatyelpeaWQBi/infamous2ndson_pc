// SPDX-License-Identifier: GPL-2.0-or-later
// Production 1D/array/cube shader lowering and BC texture allocation/transfer.
#include "vulkan_test.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "common/slot_vector.h"
#include "texture_gc_regression.h"
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
#include "video_core/buffer_cache/cpu_word_summary.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/cpu_buffer_memo.h"
#include "video_core/buffer_cache/fault_buffer_limits.h"
#include "video_core/buffer_cache/readback_hint.h"
#include "video_core/buffer_cache/cpu_backing_hash.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/host_shaders/fault_buffer_process_comp.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "core/platform.h"
#include "video_core/renderer_vulkan/stencil_reference.h"
#include <thread>
#include <atomic>
#include "bbport_mouse_motion.h"
#include "video_core/vk_shader_bundle.h"
#include "video_core/renderer_vulkan/vk_driver_cache.h"
#include "common/serdes.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "core/emulator.h"
#include "common/hash.h"
#include "video_core/cache_storage.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/texture_cache/registry_changes.h"
#include "video_core/renderer_vulkan/vk_frame_snapshot.h"
#include "video_core/renderer_vulkan/vk_texture_set_key.h"
#include "video_core/renderer_vulkan/vk_flat_data_memo.h"
#include "video_core/renderer_vulkan/vk_descriptor_pack.h"
#include "video_core/buffer_cache/buffer.h"
#include "bbport_benchmark_control.h"
#include "particle_shader_regression.h"
#include "gcn_analysis.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_recorded_bindings.h"
#include "video_core/renderer_vulkan/vk_shader_params_memo.h"
#include "video_core/renderer_vulkan/vk_specialization_memo.h"
#include "video_core/renderer_vulkan/vk_empty_work.h"
#include "video_core/renderer_vulkan/vk_readback_queue.h"

static void readback_writer_history_cpu() {
    Vulkan::BufferWriterHistory history;
    history.WriteRange(1,64,64,2);history.WriteRange(1,128,64,2);
    assert(history.LastWriter(1,80,16)==2&&history.LastWriter(1,160,16)==2);
    assert(history.LastWriter(2,80,16)==0&&history.LastWriter(1,0,64)==0);
    history.WriteRange(1,96,32,3);assert(history.LastWriter(1,100,4)==3);
    assert(history.LastWriter(1,160,4)==2);
    history.Unknown(4);assert(history.LastWriter(2,0,4)==4);
    for(u64 i=0;i<5000;++i)history.WriteRange(1,i*128,64,5+i);
    assert(history.LastWriter(1,0,4)>=5); // Overflow loses precision, never a required dependency.
    assert(history.LastWriter(1,~u64{0}-4,8)==~u64{0});
    std::puts("Readback writers: overlap, other buffers, BDA fallback and bounded history overflow PASS");
}
static void readback_queue_gpu() {
    _putenv_s("BB_READBACK_QUEUE","1");
    Vulkan::Instance instance(0,false);assert(instance.GetReadbackQueue());
    Vulkan::Scheduler scheduler(instance,false);Vulkan::Runtime runtime(instance,scheduler);
    VideoCore::Buffer source(instance,0,4096,VideoCore::MemoryType::DeviceLocal);
    VideoCore::Buffer result(instance,0,4096,VideoCore::MemoryType::HostCached);
    Vulkan::ReadbackQueue reader(instance,scheduler);
    const vk::BufferCopy copy{0,0,4096};
    runtime.FillBuffer(&source,0,4096,0x12345678);
    const u64 a=scheduler.CurrentTick();scheduler.Flush();
    assert(runtime.LastBufferWriter(&source,0,4096)==a);
    const u64 first=reader.Copy(source.Handle(),result.Handle(),std::span{&copy,1},a);
    // A new graphics write must wait for the copy: cannot overwrite its source early.
    runtime.FillBuffer(&source,0,4096,0x87654321);
    const u64 b=scheduler.CurrentTick();scheduler.Flush();
    reader.SemaphoreState().Wait(first);result.Invalidate(0,4096);
    assert(*reinterpret_cast<const u32*>(result.mapped_data.data())==0x12345678);
    const u64 second=reader.Copy(source.Handle(),result.Handle(),std::span{&copy,1},b);
    reader.SemaphoreState().Wait(second);result.Invalidate(0,4096);
    assert(*reinterpret_cast<const u32*>(result.mapped_data.data())==0x87654321);
    scheduler.Finish();
    std::puts("Two real Vulkan queues: producer visibility and read-before-overwrite ordering PASS");
}

static void recorded_bindings_cpu() {
    Vulkan::RecordedBindings state;
    const vk::PipelineLayout layout{VkPipelineLayout(1)};
    const vk::Pipeline graphics{VkPipeline(2)},compute{VkPipeline(3)};
    assert(!state.SkipPipeline(vk::PipelineBindPoint::eGraphics,graphics));
    assert(state.SkipPipeline(vk::PipelineBindPoint::eGraphics,graphics));
    assert(!state.SkipPipeline(vk::PipelineBindPoint::eCompute,compute));
    assert(state.SkipPipeline(vk::PipelineBindPoint::eGraphics,graphics));
    u32 value=7;
    assert(!state.SkipPush(layout,vk::ShaderStageFlagBits::eVertex,0,4,&value));
    assert(state.SkipPush(layout,vk::ShaderStageFlagBits::eVertex,0,4,&value));
    value=8;assert(!state.SkipPush(layout,vk::ShaderStageFlagBits::eVertex,0,4,&value));
    assert(!state.SkipPush(layout,vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eCompute,0,4,&value));
    assert(!state.SkipPush(layout,vk::ShaderStageFlagBits::eVertex,0,4,&value));
    vk::DescriptorBufferInfo buffer{vk::Buffer{VkBuffer(4)},32,64};
    vk::WriteDescriptorSet write{.dstBinding=0,.descriptorCount=1,
        .descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer};
    const auto apply=[&] {return state.SkipDescriptors(vk::PipelineBindPoint::eGraphics,layout,0,std::span{&write,1});};
    assert(!apply());assert(apply());
    // Same pointer, changed contents must miss; a new pointer with equal contents must hit.
    buffer.offset=64;assert(!apply());
    auto equal_buffer=buffer;write.pBufferInfo=&equal_buffer;assert(apply());
    write.dstArrayElement=1;assert(!apply());
    state.ForgetDescriptors(vk::PipelineBindPoint::eGraphics);assert(!apply());
    vk::VertexInputBindingDescription2EXT binding{.binding=0,.stride=16,.inputRate=vk::VertexInputRate::eVertex,.divisor=1};
    vk::VertexInputAttributeDescription2EXT attribute{.location=0,.binding=0,.format=vk::Format::eR32Sfloat,.offset=0};
    assert(!state.SkipVertex(std::span{&binding,1},std::span{&attribute,1}));
    assert(state.SkipVertex(std::span{&binding,1},std::span{&attribute,1}));
    attribute.offset=4;assert(!state.SkipVertex(std::span{&binding,1},std::span{&attribute,1}));
    state.Forget();assert(!apply());assert(!state.SkipPipeline(vk::PipelineBindPoint::eGraphics,graphics));
    std::puts("Recorded Vulkan state: owned descriptor contents, mixed-stage invalidation and command reset PASS");
}

static void shader_memos_cpu() {
    alignas(256) std::array<u32,64> code{};code[0]=0xBEEB03FF;code[1]=3;
    auto* binary=std::construct_at(reinterpret_cast<AmdGpu::BinaryInfo*>(code.data()+8));
    binary->signature=AmdGpu::BinaryInfo::signature_ref;binary->length=16;binary->shader_hash=42;
    AmdGpu::ShaderProgram registers{};registers.address=reinterpret_cast<uintptr_t>(code.data())>>8;
    Vulkan::ShaderParamsMemo params;
    assert(params.Get(registers,1,1).hash==42);
    binary->shader_hash=43;registers.user_data[0]=9;
    const auto hit=params.Get(registers,1,1);assert(hit.hash==42&&hit.user_data[0]==9);
    assert(params.Get(registers,2,1).hash==43);
    binary->length=20;assert(params.Get(registers,2,2).code.size()==5);
    Shader::Info info{};info.hw_stage=Shader::HwStage::Compute;info.sw_stage=Shader::SwStage::Compute;
    AmdGpu::Buffer sharp{};sharp.base_address=0x1000;sharp.num_records=16;sharp.stride=4;
    Shader::BufferResource resource{};resource.sharp_fetch.immediates=std::bit_cast<std::array<u32,4>>(sharp);
    info.buffers.push_back(resource);
    const auto key=Vulkan::SpecializationInput(info);
    sharp.base_address=0x2000;info.buffers[0].sharp_fetch.immediates=std::bit_cast<std::array<u32,4>>(sharp);
    assert(key==Vulkan::SpecializationInput(info));
    sharp.stride=8;info.buffers[0].sharp_fetch.immediates=std::bit_cast<std::array<u32,4>>(sharp);
    assert(key!=Vulkan::SpecializationInput(info));
    Shader::RuntimeInfo runtime{};Shader::StageSpecialization spec{};
    Vulkan::SpecializationMemo specialization;const auto current=Vulkan::SpecializationInput(info);
    specialization.Store(info,runtime,{},current,spec,2);
    assert(specialization.Matches(info,runtime,{},current,spec));
    Shader::Backend::Bindings changed{};changed.buffer=1;
    assert(!specialization.Matches(info,runtime,changed,current,spec));
    std::array<u32,80> fetch{};AmdGpu::UserData userdata{};
    const auto* pointer=fetch.data();std::memcpy(userdata.data(),&pointer,sizeof(pointer));
    info.user_data=userdata;info.has_fetch_shader=true;spec.fetch_shader_data.emplace();
    spec.fetch_shader_data->size=sizeof(fetch);
    specialization.Store(info,runtime,{},current,spec,3);
    assert(specialization.Matches(info,runtime,{},current,spec));
    fetch[70]=1;assert(!specialization.Matches(info,runtime,{},current,spec));
    assert(Vulkan::EmptyDraw(0,1)&&Vulkan::EmptyDraw(1,0)&&!Vulkan::EmptyDraw(~0u,~0u));
    assert(Vulkan::EmptyDispatch(1,0,1)&&!Vulkan::EmptyDispatch(1024,1024,4096));
    std::puts("Shader metadata and specialization: fresh user data, epochs, resource formats and complete fetch code PASS");
}

static void gpu_profile_async() {
    _putenv_s("BB_GPU_PROFILE","1");
    Vulkan::Instance instance(0,false);
    Vulkan::Scheduler scheduler(instance,false);
    Vulkan::GpuProfiler::Init(instance,scheduler);
    auto* profiler=Vulkan::GpuProfiler::Get();assert(profiler);
    // Keep all timestamps in an unsubmitted command buffer. The fifth frame
    // must skip instrumentation instead of waiting for results or resetting
    // queries referenced by that buffer (the old four-frame heuristic hangs).
    const auto start=std::chrono::steady_clock::now();
    for(unsigned frame=0;frame<20;++frame) {
        profiler->Mark(1,[]{return std::string{"unsubmitted diagnostic work"};});
        profiler->BeginFrame();
    }
    assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(500));
    scheduler.Finish();
    profiler->BeginFrame();
    profiler->Mark(2,[]{return std::string{"reused completed diagnostic storage"};});
    profiler->BeginFrame();scheduler.Finish();profiler->BeginFrame();
    std::puts("GPU profiler skips in-flight query storage without waiting PASS");
}

static void runtime_benchmark_control(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    auto history=std::make_unique<BbPresentMetrics::History>();
    history->Push(100,1,1920,1080); history->Push(150,2,1920,1080);
    assert(history->sequence==1 && history->rows[0].interval_ns==50);
    for(u64 i=0;i<5000;++i) history->Push(200+i,3+i,1920,1080);
    assert(history->First()>1);
    BbBenchmark::Command parsed;
    for(const char* bad:{"0 quit","-1 quit"," -1 quit","1 quit extra","1 unknown","18446744073709551616 quit"})
        assert(!BbBenchmark::Parse(bad,parsed));
    const auto path=directory/"control.txt";_putenv_s("BB_BENCH_CONTROL",path.string().c_str());
    BbBenchmark::Control control; unsigned calls=0,snapshots=0;
    std::ofstream(path)<<"1 record-start\n";
    assert(!control.Poll([&](bool value){++calls;return value;},[&]{++snapshots;}));
    assert(calls==1 && std::filesystem::exists(path.string()+".ack"));
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(!control.Poll([&](bool value){++calls;return value;},[&]{++snapshots;}));
    assert(calls==1); // Retry cannot toggle recording twice.
    const auto wait_command=[&](auto complete) {
        // GetTickCount64 has coarser granularity than sleep_for: a 25ms sleep
        // does not guarantee the production 20ms polling gate has advanced.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        bool quit=false;
        do {
            quit|=control.Poll([&](bool value){++calls;return value;},[&]{++snapshots;});
            if(complete(quit)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while(std::chrono::steady_clock::now()<deadline);
        assert(false && "benchmark command acknowledgement timed out");
    };
    std::ofstream(path)<<"2 snapshot\n";
    wait_command([&](bool quit){assert(!quit);return snapshots==1;});
    std::ofstream(path)<<"3 quit\n";
    wait_command([](bool quit){return quit;});
    _putenv_s("BB_BENCH_CONTROL","");
    std::puts("Actual-present history / owned control acknowledgements / idempotency / bounded input validation PASS");
}

// Exercise the production transfer and scheduler. An unrelated later submission
// waits on a host-controlled gate: reading the earlier snapshot must not drain it.
static void readback_prefetch_gpu() {
    using namespace VideoCore;
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    Buffer source(instance, 0, 4096, MemoryType::DeviceLocal);
    auto snapshot=std::make_unique<Buffer>(instance,0,4096,MemoryType::HostCached);
    Buffer unrelated(instance, 0, 4096, MemoryType::DeviceLocal);
    ReadbackHint hint;
    hint.page = 0x1000;
    const vk::BufferCopy copy{0, 0, 4096};
    runtime.FillBuffer(&source, 0, 4096, 0xa5a5a5a5);
    runtime.CopyBuffer(&source, snapshot.get(), std::span{&copy, 1});
    hint.Captured(scheduler.CurrentTick());
    scheduler.Flush();

    // Transfer ownership while the earlier copy can still be in flight. A
    // later prefetch uses different storage and cannot overwrite the reader's.
    const auto reader_tick=hint.Detach();
    auto reader_snapshot=std::move(snapshot);
    snapshot=std::make_unique<Buffer>(instance,0,4096,MemoryType::HostCached);

    vk::SemaphoreTypeCreateInfo timeline{.semaphoreType = vk::SemaphoreType::eTimeline};
    auto [result, gate] = instance.GetDevice().createSemaphoreUnique({.pNext = &timeline});
    assert(result == vk::Result::eSuccess);
    runtime.FillBuffer(&unrelated, 0, 4096, 0x11111111);
    Vulkan::SubmitInfo later{};
    later.AddWait(*gate, 1);
    const u64 later_tick = scheduler.CurrentTick();
    scheduler.Flush(later);

    std::atomic<bool> completed{false};
    std::thread reader([&] {
        scheduler.GetWorkSemaphore()->Wait(reader_tick);
        reader_snapshot->Invalidate(0, 4096);
        completed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool independent = completed.load(std::memory_order_acquire);
    const bool later_pending = independent && !scheduler.IsFree(later_tick);
    // Always release the gate, including failure paths, so a regression cannot
    // leave the driver or test teardown waiting for an unsignaled semaphore.
    assert(instance.GetDevice().signalSemaphore({.semaphore = *gate, .value = 1}) ==
           vk::Result::eSuccess);
    reader.join();
    scheduler.Wait(later_tick);
    assert(independent && later_pending);
    for (const auto byte : reader_snapshot->mapped_data) assert(byte == 0xa5);

    hint.Write(0x1100, 4);
    assert(!hint.Valid()); // A newer write rejects the old snapshot.
    runtime.FillBuffer(&source, 0, 4096, 0x11111111);
    runtime.CopyBuffer(&source, snapshot.get(), std::span{&copy, 1});
    hint.Captured(scheduler.CurrentTick());
    scheduler.Flush();
    scheduler.Wait(hint.tick);
    snapshot->Invalidate(0, 4096);
    for (const auto byte : snapshot->mapped_data) assert(byte == 0x11);
    for (const auto byte : reader_snapshot->mapped_data) assert(byte == 0xa5);
    hint.Consumed();
    assert(!hint.Valid());
    std::puts("GPU readback snapshot: early timeline completion, later-work independence and fresh bytes PASS");
}

static void flat_visibility() {
    Vulkan::Instance instance(0,false); Vulkan::Scheduler scheduler(instance);
    VideoCore::StreamBuffer stream(instance,scheduler,VideoCore::MemoryType::Stream,512);
    VideoCore::Buffer readback(instance,0,128,VideoCore::MemoryType::HostCached);
    auto memo=std::make_unique<Vulkan::FlatDataMemo>(); std::array<u8,128> source{};
    const auto upload=[&] {
        return memo->Upload(16,source,
            [&]{return Vulkan::FlatDataMemo::Epoch{scheduler.CurrentTick(),stream.WrapGeneration()};},
            [&]{return stream.Copy(source.data(),source.size(),16);});
    };
    assert(!upload().reused && upload().reused);
    for(u32 i=0;i<8;++i) {source[127]=u8(i+1); assert(!upload().reused);}
    assert(stream.WrapGeneration()>0);
    auto saved=upload();
    for(u32 i=0;i<128 && !saved.reused;++i) saved=upload();
    assert(saved.reused); // Stable data must recover after the changing-data cooldown.
    const auto cmd=scheduler.CommandBuffer();
    const vk::MemoryBarrier2 host_write{.srcStageMask=vk::PipelineStageFlagBits2::eHost,
        .srcAccessMask=vk::AccessFlagBits2::eHostWrite,.dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&host_write});
    const vk::BufferCopy copy{saved.offset,0,source.size()};
    cmd.copyBuffer(stream.Handle(),readback.Handle(),copy);
    const vk::MemoryBarrier2 readable{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask=vk::AccessFlagBits2::eHostRead};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&readable});
    scheduler.Finish(); readback.Invalidate(0,source.size());
    assert(std::memcmp(readback.mapped_data.data(),source.data(),source.size())==0);
    assert(!upload().reused); // A completed submission cannot reuse an unpinned old offset.
    std::puts("Flat memo real GPU transfer visibility / ring wrap / submitted tick invalidation PASS");
}

static void flat_memo() {
    using Memo=Vulkan::FlatDataMemo;
    auto memo=std::make_unique<Memo>();
    std::array<u8,16> bytes{};
    Memo::Epoch epoch{1,0}; u64 copies=0;
    const auto upload=[&](u64 stage=16) {
        return memo->Upload(stage,bytes,[&]{return epoch;},[&]{return ++copies;});
    };
    assert(!upload().reused); assert(upload().reused && copies==1);
    bytes[15]=9; assert(!upload().reused && copies==2);
    assert(upload().reused); ++epoch.tick; assert(!upload().reused);
    ++epoch.wrap; assert(!upload().reused);
    assert(!upload(16+16*Memo::Capacity).reused); // slot collision
    assert(!upload().reused && upload().reused);
    // Changes during copy must stamp the new epoch, not the request's old one.
    bytes[0]=7;
    const auto changed=memo->Upload(16,bytes,[&]{return epoch;},[&]{++epoch.wrap; return ++copies;});
    assert(!changed.reused && upload().reused);
    for(u32 i=0;i<200;++i) {bytes[0]=u8(i); assert(!upload().reused);}
    bool eventually=false;
    for(u32 i=0;i<200;++i) eventually|=upload().reused;
    assert(eventually); // Adaptive cooldown must recover for a stable workload.
    std::puts("Flat readonly memo: exact bytes, tick/wrap/collision invalidation, post-copy epoch and adaptive recovery PASS");
}

static void texture_set_distribution() {
    std::array<bool,32768> old_slots{},new_slots{};
    std::array<u64,32768> old_cache{},new_cache{};
    std::vector<u64> keys;
    for(u64 i=0;i<50000;++i) {
        const std::array hashes{i,i*17+3};
        const auto key=Vulkan::TextureSetKey(hashes,0x1234);
        assert(key); keys.push_back(key);
        old_slots[(key|1)%old_slots.size()]=true;
        new_slots[key%new_slots.size()]=true;
    }
    const auto old_count=std::count(old_slots.begin(),old_slots.end(),true);
    const auto new_count=std::count(new_slots.begin(),new_slots.end(),true);
    assert(old_count<=16384 && new_count>24000);
    u64 old_hits=0,new_hits=0;
    for(unsigned pass=0;pass<2;++pass) for(auto key:keys) {
        auto& a=old_cache[(key|1)%old_cache.size()];
        auto& b=new_cache[key%new_cache.size()];
        if(a==key) ++old_hits; if(b==key) ++new_hits;
        a=key;b=key;
    }
    assert(new_hits>old_hits);
    std::printf("Texture cache slot distribution PASS: old %zu, fixed %zu; repeated workload hits old %llu, fixed %llu\n",
        size_t(old_count),size_t(new_count),static_cast<unsigned long long>(old_hits),static_cast<unsigned long long>(new_hits));
}

static void snapshot_pixels(const std::filesystem::path& directory) {
    Vulkan::FrameSnapshotWriter::Slots slots;
    for(u32 mark=0;mark<4;++mark) {
        assert(slots.Next(true)==mark);
        for(u32 followup=0;followup<3;++followup) assert(slots.Next(false)>=4);
    }
    assert(slots.Next(true)==0);
    std::filesystem::create_directories(directory);
    std::vector<u8> pixels(3840*4*4);
    for(size_t i=0;i<pixels.size();i+=4) {pixels[i]=10;pixels[i+1]=20;pixels[i+2]=30;pixels[i+3]=255;}
    Vulkan::FrameSnapshotWriter::Job job{.pixels=pixels.data(),.tick=123,.width=3840,.height=4,
        .bgra=true,.gamma=.8f,.directory=directory};
    assert(Vulkan::FrameSnapshotWriter::Save(job,0));
    const auto path=directory/"frame-snapshot-0.bmp";
    std::vector<u8> data(std::filesystem::file_size(path));
    std::ifstream input(path,std::ios::binary);assert(input.read(reinterpret_cast<char*>(data.data()),data.size()));
    u32 width,height;std::memcpy(&width,data.data()+18,4);std::memcpy(&height,data.data()+22,4);
    assert(width==1920 && height==u32(-2));
    assert(data[54]==10 && data[55]==20 && data[56]==30);
    std::ifstream metadata(directory/"frame-snapshot-0.json");
    const std::string text{std::istreambuf_iterator<char>{metadata},std::istreambuf_iterator<char>{}};
    for(const char* field:{"\"tick_ms\":123","\"source_width\":3840","\"width\":1920",
                          "\"gamma\":0.8","\"hdr\":false","\"srgb_input\":false","\"mean_encoded_luma\":"})
        assert(text.find(field)!=std::string::npos);
    job.bgra=false;assert(Vulkan::FrameSnapshotWriter::Save(job,1));
    std::ifstream other(directory/"frame-snapshot-1.bmp",std::ios::binary);
    other.seekg(54);std::array<u8,3> color;assert(other.read(reinterpret_cast<char*>(color.data()),3));
    assert((color==std::array<u8,3>{30,20,10}));
    job.width=4097;assert(!Vulkan::FrameSnapshotWriter::Save(job,2));
    std::puts("4K source snapshot bound/downscale/BGRA-RGBA conversion/color metadata PASS");
}

static void snapshot_worker(const std::filesystem::path& directory) {
    using namespace Vulkan;
    std::filesystem::create_directories(directory);
    const auto bmp=directory/"frame-snapshot-0.bmp";std::filesystem::remove(bmp);
    Instance instance(0,false);
    vk::SemaphoreTypeCreateInfo timeline{.semaphoreType=vk::SemaphoreType::eTimeline};
    auto [result,semaphore]=instance.GetDevice().createSemaphoreUnique({.pNext=&timeline});
    assert(result==vk::Result::eSuccess);
    VkBuffer buffer{};VmaAllocation allocation{};VmaAllocationInfo mapping{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=32,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage=VMA_MEMORY_USAGE_AUTO,.requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ai,&buffer,&allocation,&mapping)==VK_SUCCESS);
    std::memset(mapping.pMappedData,127,32);
    {
        FrameSnapshotWriter writer(instance.GetDevice(),instance.GetAllocator());
        const auto start=std::chrono::steady_clock::now();
        writer.Submit({.buffer=buffer,.allocation=allocation,.pixels=mapping.pMappedData,
            .semaphore=*semaphore,.gpu_tick=1,.tick=456,.width=4,.height=2,.bgra=true,.directory=directory});
        assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(100));
        assert(!std::filesystem::exists(bmp)); // Worker cannot read before timeline completion.
        assert(instance.GetDevice().signalSemaphore({.semaphore=*semaphore,.value=1})==vk::Result::eSuccess);
    }
    assert(std::filesystem::exists(bmp));
    std::puts("Asynchronous screenshot timeline wait / producer returns before GPU / drain ownership PASS");
}

static void registry_changes() {
    using namespace VideoCore;
    RegistryChanges journal;
    const std::array ranges{RegistryRange{100,200},RegistryRange{400,500}};
    u64 generation=0;
    assert(journal.Unchanged(generation,ranges));
    journal.Record({200,400}); // Adjacent surfaces do not invalidate either texture.
    assert(journal.Unchanged(generation,ranges) && generation==1);
    journal.Record({1000,2000});
    assert(journal.Unchanged(generation,ranges) && generation==2);
    journal.Record({199,201}); // Registration or removal touching a cached surface misses.
    assert(!journal.Unchanged(generation,ranges) && generation==2);
    generation=journal.Generation();
    for(size_t i=0;i<RegistryChanges::Capacity;++i) journal.Record({1000,2000});
    const auto before=generation;
    assert(journal.Unchanged(generation,ranges));
    journal.Record({1000,2000});
    u64 expired=before;
    assert(!journal.Unchanged(expired,ranges) && expired==before);
    u64 invalid=~u64(0); assert(!journal.Unchanged(invalid,ranges));
    assert(!RegistryRange{}.Overlaps(ranges[0]));
    // Compare the bounded algorithm to a conservative complete change history.
    RegistryChanges checked; std::vector<RegistryRange> history;
    for(u64 i=0;i<256;++i) {
        const auto change=RegistryRange::FromSize((i*73)%1500,1+i%32);
        checked.Record(change); history.push_back(change);
        for(u64 distance=0;distance<=65 && distance<=history.size();++distance) {
            u64 token=checked.Generation()-distance;
            bool expected=distance<=RegistryChanges::Capacity;
            for(size_t j=token;j<history.size();++j)
                for(const auto& range:ranges) if(history[j].Overlaps(range)) expected=false;
            const bool actual=checked.Unchanged(token,ranges);
            assert(actual==expected);
            if(actual) assert(token==checked.Generation());
        }
    }
    // Texture alias lookups must include both the requested and backing regions.
    RegistryChanges aliases;
    const std::array alias_ranges{RegistryRange{100,1200}};
    u64 token=0; aliases.Record({1100,1200});
    assert(!aliases.Unchanged(token,alias_ranges));
    std::puts("Scoped texture registry reuse: unrelated/overlap/alias/history overflow PASS");
}

static void runtime_cache(const std::filesystem::path& directory) {
    using namespace Vulkan;
    std::filesystem::create_directories(directory);
    const auto source=directory/"all_shaders.xpps";
    const std::vector<u8> original{'K','C','A','P',1,2,3,4};
    { std::ofstream file(source,std::ios::binary); file.write(reinterpret_cast<const char*>(original.data()),original.size()); }
    _putenv_s("BB_GAME_PROFILE","infamous"); _putenv_s("BB_SHADER_SOURCE",source.string().c_str());
    BbGpuConfig config{}; config.serial="TEST-BUNDLE"; Core::Emulator::FillElfInfo(config);
    Instance instance(0,false); Scheduler scheduler(instance); AmdGpu::Liverpool liverpool;
    auto& database=Storage::DataBase::Instance();
    constexpr u64 pgm_hash=0x1234;
    const ComputePipelineKey key{HashCombine(pgm_hash,size_t{0})};
    {
        PipelineCache cache(instance,scheduler,&liverpool,0);
        database.Clear(); // Only the synthetic TEST-BUNDLE title cache.
        std::vector<u8> profile(sizeof(Shader::Profile));
        std::memcpy(profile.data(),&cache.GetProfile(),profile.size());
        assert(database.Save(Storage::BlobType::ShaderProfile,"profile",std::move(profile)));
        Shader::Info info{}; info.hw_stage=Shader::HwStage::Compute; info.sw_stage=Shader::SwStage::Compute;
        info.pgm_hash=pgm_hash;
        Shader::RuntimeInfo runtime{}; runtime.Initialize(info.hw_stage,info.sw_stage);
        runtime.hw.cs.workgroup_size={1,1,1};
        Common::ObjectPool<Shader::IR::Inst> pool; Shader::IR::Block block(pool);
        Shader::IR::IREmitter ir(block); ir.Epilogue();
        Shader::IR::Program program(info); program.blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=Shader::IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type=Shader::IR::AbstractSyntaxNode::Type::Return});
        Shader::Backend::Bindings bindings{};
        auto spv=Shader::Backend::SPIRV::EmitSPIRV(cache.GetProfile(),runtime,program,bindings);
        // A nonempty SRT walker is generated by real resource-table shaders.
        // Exercise its serialization, unlike the previous empty-table fixture.
        const std::array<u8,1> walker{0xc3}; // x86-64 ret; does not touch guest memory.
        info.srt_info.walker_func=Shader::RegisterWalkerCode(walker.data(),walker.size());
        info.srt_info.walker_func_size=walker.size();
        Shader::StageSpecialization spec{}; spec.info=&info; spec.runtime_info=runtime;
        RegisterShaderMeta(info,{},spec,key.value,0);
        RegisterShaderBinary(std::move(spv),pgm_hash,0);
        ComputePipeline::SerializationSupport sdata{}; RegisterPipelineData(key,sdata);
        // Close must drain, not discard, the just-enqueued writes.
        cache.Sync();
    }
    {
        PipelineCache restored(instance,scheduler,&liverpool,0);
        assert(restored.NumCachedPrograms()==1 && restored.NumCachedComputePipelines()==1);
        restored.Sync();
    }
    // The synthetic source was read-only throughout caching/preload.
    std::vector<u8> after(original.size()); std::ifstream input(source,std::ios::binary);
    assert(input.read(reinterpret_cast<char*>(after.data()),after.size()) && after==original);
    std::puts("Engine runtime compile-output packaging / drained close / real Vulkan startup prewarm / source read-only PASS");
}

static void cache_guards() {
    // SysV ABI walker: load one user-data word and write the flattened table.
    const std::array<u8,5> code{0x8b,0x07,0x89,0x06,0xc3};
    Shader::PersistentSrtInfo original_srt{};
    original_srt.walker_func=Shader::RegisterWalkerCode(code.data(),code.size());
    original_srt.walker_func_size=code.size();
    Serialization::Archive srt_ar; original_srt.Serialize(srt_ar);
    const auto saved_srt=srt_ar.TakeOff();
    assert(saved_srt.size()==sizeof(original_srt)+code.size());
    Serialization::Archive valid_srt{std::vector<u8>(saved_srt)};
    Shader::PersistentSrtInfo restored{}; assert(restored.Deserialize(valid_srt));
    assert(restored.walker_func_size==code.size() && restored.walker_func!=original_srt.walker_func);
    std::array<u32,16> user{},flat{}; user[0]=0x12345678;
    restored.walker_func(user.data(),flat.data()); assert(flat[0]==user[0]);
    Serialization::Archive truncated_srt{std::vector<u8>(saved_srt.begin(),saved_srt.end()-1)};
    bool invalid_srt=false;
    try { Shader::PersistentSrtInfo broken{}; broken.Deserialize(truncated_srt); }
    catch(const std::runtime_error&) { invalid_srt=true; }
    assert(invalid_srt);
    // Corrupt legacy cache blobs must reject before any read/allocation.
    for(size_t size=0;size<sizeof(u64);++size) {
        Serialization::Archive ar{std::vector<u8>(size)}; Serialization::Reader reader(ar);
        u64 value; bool rejected=false;
        try { reader.Read(value); } catch(const std::runtime_error&) { rejected=true; }
        assert(rejected);
    }
    Serialization::Archive ar;
    Serialization::Writer writer(ar); writer.Write(std::numeric_limits<size_t>::max());
    Serialization::Reader reader(ar);
    std::string value; bool rejected=false;
    try { reader.Read(value); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && value.empty());
    Serialization::Archive vector_ar;
    Serialization::Writer vector_writer(vector_ar); vector_writer.Write(std::numeric_limits<size_t>::max());
    Serialization::Reader vector_reader(vector_ar); std::vector<u32> values; rejected=false;
    try { vector_reader.Read(values); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && values.empty());
    // Regular and prepared paths hash the same current table. In-place T#
    // rewrites without register packets must invalidate the binding key.
    Shader::Info info{}; Shader::ImageResource resource{};
    resource.sharp_fetch.load_mask=255;
    for(u32 i=0;i<8;++i) resource.sharp_fetch.offsets[i]=i;
    auto image=AmdGpu::Image::Null(false); image.width=127;
    info.flattened_ud_buf.resize(8);
    std::memcpy(info.flattened_ud_buf.data(),&image,sizeof(image));
    const auto original=Vulkan::ImageDescHash(resource.GetSharp(info),resource);
    assert(original==Vulkan::ImageDescHash(image,resource));
    image.width=255; std::memcpy(info.flattened_ud_buf.data(),&image,sizeof(image));
    assert(original!=Vulkan::ImageDescHash(resource.GetSharp(info),resource));
    resource.is_array=true;
    assert(Vulkan::ImageDescHash(image,resource)!=Vulkan::ImageDescHash(image,Shader::ImageResource{}));
    std::puts("Truncated/oversized cache rejection and mutable resource-table key invalidation PASS");
}

static void shader_bundle(const std::filesystem::path& directory) {
    using Storage::ShaderBundle;
    const ShaderBundle::Blobs blobs{{"0xabc_0.spv",{3,2,35,7}}, {"profile.bin",{1,2}},
                                   {"0xabc_0.meta",{8}}, {"0xabc.key",{9}}};
    const auto data=ShaderBundle::Encode(123,blobs);
    ShaderBundle::Blobs decoded;
    assert(!data.empty() && ShaderBundle::Decode(data,123,decoded) && decoded==blobs);
    for(size_t i=0;i<data.size();++i) {
        auto damaged=data; damaged[i]^=128;
        assert(!ShaderBundle::Decode(damaged,123,decoded));
        assert(decoded==blobs); // Rejection is transactional.
        assert(!ShaderBundle::Decode(std::span{data}.first(i),123,decoded));
    }
    assert(!ShaderBundle::Decode(data,456,decoded));
    assert(ShaderBundle::Encode(123,{{"../game.xpps",{1}}}).empty());
    std::filesystem::create_directories(directory);
    const auto path=directory/"bundle.vkpack";
    {
        ShaderBundle bundle(path,123); bundle.Clear();
        for(const auto& [name,blob]:blobs) assert(bundle.Put(name,blob));
        assert(bundle.Put("empty.spv",{}));
        assert(!bundle.Put("../../eboot.bin",{ }));
        const std::vector<u8> oversized(ShaderBundle::Limit);
        assert(!bundle.Put("oversized.spv",oversized));
        assert(bundle.Save());
        bundle.Start();
        std::thread producer([&] { for(unsigned i=0;i<100;++i) {
            const std::array<u8,1> bytes{u8(i)}; assert(bundle.Put("live.meta",bytes));
        }});
        for(unsigned i=0;i<10;++i) assert(bundle.Save());
        producer.join(); bundle.Stop();
    }
    {
        ShaderBundle bundle(path,123); assert(bundle.Load());
        std::vector<u8> blob; assert(bundle.Get("live.meta",blob) && blob==std::vector<u8>{99});
        assert(bundle.Get("empty.spv",blob) && blob.empty());
        bundle.Clear(); assert(bundle.Save()); assert(!bundle.Get("0xabc_0.spv",blob));
    }
    ShaderBundle empty(path,123); assert(empty.Load() && empty.Snapshot().empty());
    ShaderBundle wrong_source(path,456); assert(!wrong_source.Load());
    std::puts("Runtime shader package round-trip/corruption/source isolation/capacity/concurrent save PASS");
}

static void driver_cache(const std::filesystem::path& directory) {
    using namespace Vulkan;
    Instance instance(0,false);
    std::filesystem::create_directories(directory);
    const auto path=directory/"driver.vkc";
    { DriverPipelineCache cache(instance,path); cache.Dirty(); assert(cache.Save()); }
    const auto size=std::filesystem::file_size(path);
    assert(size>=56);
    std::vector<u8> bytes(size); std::ifstream file(path,std::ios::binary);
    assert(file.read(reinterpret_cast<char*>(bytes.data()),size)); file.close();
    const auto valid=[&](const std::vector<u8>& data) {
        return DriverPipelineCache::Compatible(data,instance.GetVendorID(),instance.GetDeviceID(),
                    instance.GetDriverVersion(),instance.GetPipelineCacheUUID());
    };
    assert(valid(bytes));
    assert(!DriverPipelineCache::Compatible(bytes,instance.GetVendorID()+1,instance.GetDeviceID(),
                    instance.GetDriverVersion(),instance.GetPipelineCacheUUID()));
    assert(!DriverPipelineCache::Compatible(bytes,instance.GetVendorID(),instance.GetDeviceID(),
                    instance.GetDriverVersion()+1,instance.GetPipelineCacheUUID()));
    { DriverPipelineCache restored(instance,path); assert(restored.LoadedBytes()==size); }
    bytes.back()^=1; assert(!valid(bytes));
    { std::ofstream corrupt(path,std::ios::binary|std::ios::trunc);
      corrupt.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
    { DriverPipelineCache fallback(instance,path); assert(fallback.LoadedBytes()==0);
      fallback.Dirty(); assert(fallback.Save()); }
    { DriverPipelineCache restored(instance,path); assert(restored.LoadedBytes()>=56); }
    std::puts("Real Vulkan driver cache persistence/device-driver validation/corruption fallback PASS");
}

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
    const auto key = [&](SDL_Keycode code) {
        SDL_Event e{}; e.type=SDL_EVENT_KEY_DOWN; e.key.windowID=42; e.key.key=code;
        input.Event(e,42,true);
    };
    const auto button = [&](bool down, Uint8 which) {
        SDL_Event e{}; e.type=down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID=42; e.button.button=which; input.Event(e,42,true);
    };
    const auto move = [&](float dx, float dy) {
        SDL_Event e{}; e.type=SDL_EVENT_MOUSE_MOTION; e.motion.windowID=42;
        e.motion.xrel=dx; e.motion.yrel=dy; input.Event(e,42,true);
    };
    key(SDLK_F8); assert(input.TouchActive() && input.Captured() && !input.Active());
    event={}; event.type=SDL_EVENT_KEY_DOWN; event.key.windowID=42; event.key.key=SDLK_F8;
    event.key.repeat=true; input.Event(event,42,true); assert(input.TouchActive());
    // Entire swipe occurs before the guest polls. Contact origin, endpoint and release survive.
    button(true,SDL_BUTTON_LEFT); move(100,-50); move(50,-25); button(false,SDL_BUTTON_LEFT);
    input.Read(&sample); const auto first_id=sample.touch_id;
    assert(sample.touch_active && sample.touch_down && !sample.touch_click && !sample.left && !sample.active);
    assert(sample.touch_x==960 && sample.touch_y==471);
    input.Read(&sample); assert(sample.touch_down && sample.touch_x==1260 && sample.touch_y==321 && sample.touch_id==first_id);
    input.Read(&sample); assert(!sample.touch_down && !sample.touch_click);
    input.Read(&sample); assert(!sample.touch_down);
    // A click has a distinct contact identifier, and is independent of touching the surface.
    button(true,SDL_BUTTON_RIGHT); button(false,SDL_BUTTON_RIGHT);
    input.Read(&sample); assert(sample.touch_down && sample.touch_click && sample.touch_id!=first_id);
    input.Read(&sample); assert(!sample.touch_down && !sample.touch_click);
    for (const auto delta : std::array<std::array<float,2>,4>{{{200,0},{-200,0},{0,100},{0,-100}}}) {
        key(SDLK_F7); button(true,SDL_BUTTON_LEFT); move(delta[0],delta[1]);
        input.Read(&sample); const auto id=sample.touch_id;
        input.Read(&sample); assert(sample.touch_down && sample.touch_id==id);
        assert(sample.touch_x==static_cast<unsigned>(960+delta[0]*2));
        assert(sample.touch_y==static_cast<unsigned>(471+delta[1]*2));
        button(false,SDL_BUTTON_LEFT); input.Read(&sample); assert(!sample.touch_down);
    }
    key(SDLK_F7); button(true,SDL_BUTTON_LEFT); move(1e6f,-1e6f); move(NAN,INFINITY);
    input.Read(&sample); input.Read(&sample); assert(sample.touch_x==1919 && sample.touch_y==0);
    // Coalescing thousands of mouse events keeps the gesture bounded and avoids long input lag.
    for (unsigned i=0;i<10000;++i) move(-.01f,.01f);
    button(false,SDL_BUTTON_LEFT); input.Read(&sample); assert(sample.touch_down);
    input.Read(&sample); assert(!sample.touch_down);
    key(SDLK_F6); assert(input.Active() && !input.TouchActive());
    key(SDLK_F8); assert(input.TouchActive() && !input.Active());
    button(true,SDL_BUTTON_LEFT); input.Release(); input.Read(&sample);
    assert(!sample.touch_active && !sample.touch_down && !sample.touch_click && !input.Captured());
    key(SDLK_F8); button(true,SDL_BUTTON_RIGHT); input.Event(event,42,false); input.Read(&sample);
    assert(!sample.touch_active && !sample.touch_down && !sample.touch_click);
    key(SDLK_F8); button(true,SDL_BUTTON_LEFT); key(SDLK_ESCAPE); input.Read(&sample);
    assert(!sample.touch_active && !sample.touch_down);
    key(SDLK_F8); event={}; event.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID=99; event.button.button=SDL_BUTTON_LEFT; input.Event(event,42,true);
    input.Read(&sample); assert(!sample.touch_down);
    key(SDLK_F8); assert(!input.TouchActive() && !input.Captured());
    std::puts("Mouse motion: toggle, repeat suppression, window isolation, deltas, buttons, focus/menu release PASS");
    std::puts("Mouse touchpad: taps, four-way swipe, short gesture edges, click separation, IDs, bounds, mode isolation PASS");
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
    // Discarded geometry draws must not starve the following compute constants.
    for(u32 i=0;i<200;++i) {
        FrameCapture::Buffer(0x123,0,0x400000,constants,sizeof(constants));
        FrameCapture::Draw(0x123,0x456,3,1);
    }
    FrameCapture::Buffer(0x789,0,0x900000,constants,16);
    FrameCapture::Dispatch(0x789,4,2,1);
    // GPU particle frames contain hundreds of alternating emitters. Preserve the
    // late transparent consumer rather than stopping at the old 256-entry cap.
    for(u32 i=0;i<300;++i) FrameCapture::Dispatch(0x1000+i,1,1,1);
    scene.resources.layers=16;scene.resources.levels=2;
    FrameCapture::BeginPass(&scene_ptr,1,nullptr);
    FrameCapture::Draw(0xf1,0xf2,0,0);
    FrameCapture::CaptureDrawState(0xf3,0xf1,0xf2,"indirect counts=GPU_owned blend=1 src=1 dst=1");
    FrameCapture::BeginPass(&display_ptr,1,nullptr);
    assert(!FrameCapture::Active());
    std::filesystem::path report;
    for(const auto& entry:std::filesystem::directory_iterator(directory))
        if(entry.path().extension()==".txt") report=entry.path();
    assert(!report.empty());
    std::ifstream input(report); const std::string text((std::istreambuf_iterator<char>(input)),{});
    assert(text.find("DEBUG_FRAME_BEGIN")!=std::string::npos);
    assert(text.find("truncated=1")!=std::string::npos);
    assert(text.find("PASS draws 202 (indices 675)")!=std::string::npos);
    assert(text.find("COMPUTE dispatches 1")!=std::string::npos);
    assert(text.find("buffer stage 0000000000000789 slot 0 at 0x900000 size 16")!=std::string::npos);
    assert(text.find("samples 0x100000")!=std::string::npos);
    assert(text.find("DEBUG_FRAME_END passes=304")!=std::string::npos);
    assert(text.find("DRAW_STATE key=0xf3 vs=00000000000000f1 ps=00000000000000f2 indirect")!=std::string::npos);
    assert(text.find("layers=16 levels=2")!=std::string::npos);
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
    const auto history_storage=std::make_unique<BbFrameMetrics::History>();
    auto& history=*history_storage;
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
static void image_read_memo(bool dirty=false) {
    if(dirty)_putenv_s("BB_IMAGE_DIRTY_RANGES","1");
    using namespace Vulkan;
    Instance instance(0,false); Scheduler scheduler(instance); Runtime runtime(instance,scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    VideoCore::ImageInfo info; info.type=AmdGpu::ImageType::Color2DArray;
    info.pixel_format=vk::Format::eR8G8B8A8Unorm; info.size={32,32,1};
    info.resources={6,64}; info.num_bits=32;
    VideoCore::Image cached(instance,runtime,views,info), reference(instance,runtime,views,info);
    using Layout=vk::ImageLayout; using Access=vk::AccessFlagBits2; using Stage=vk::PipelineStageFlagBits2;
    const VideoCore::SubresourceRange range{{1,0},{5,64}};
    VideoCore::Image::Barriers actual,expected;
    const auto check=[&](Layout layout,vk::AccessFlags2 access,vk::PipelineStageFlags2 stage,
                         std::optional<VideoCore::SubresourceRange> subset) {
        actual.clear(); expected.clear();
        cached.GetBarriers(actual,layout,access,stage,subset);
        reference.GetBarriers(expected,layout,access,stage,subset,false);
        const auto expanded=[&](const auto& barriers) {
            std::vector<vk::ImageMemoryBarrier2> result;
            for(auto barrier:barriers) {
                const auto range=barrier.subresourceRange;barrier.image=nullptr;
                if(barrier.srcStageMask&vk::PipelineStageFlagBits2::eAllCommands)
                    barrier.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands;
                const u32 levels=range.levelCount==VK_REMAINING_MIP_LEVELS?info.resources.levels-range.baseMipLevel:range.levelCount;
                const u32 layers=range.layerCount==VK_REMAINING_ARRAY_LAYERS?info.resources.layers-range.baseArrayLayer:range.layerCount;
                for(u32 m=0;m<levels;++m)for(u32 l=0;l<layers;++l) {
                    barrier.subresourceRange.baseMipLevel=range.baseMipLevel+m;
                    barrier.subresourceRange.baseArrayLayer=range.baseArrayLayer+l;
                    barrier.subresourceRange.levelCount=1;barrier.subresourceRange.layerCount=1;
                    result.push_back(barrier);
                }
            }
            std::ranges::sort(result,{},[](const auto& b){return b.subresourceRange.baseMipLevel*64+b.subresourceRange.baseArrayLayer;});
            return result;
        };
        if(dirty) assert(expanded(actual)==expanded(expected));
        else {
            assert(actual.size()==expected.size());
            for(size_t i=0;i<actual.size();++i) {
                auto a=actual[i],b=expected[i];a.image=b.image;assert(a==b);
            }
        }
    };
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,range);
    if(dirty) {
        // Homogeneous reads avoid allocation; a few changed layers use the dirty list,
        // and more than 64 changed subresources must return to the complete scan.
        for(u32 l=0;l<64;++l) {
            check(Layout::eGeneral,Access::eShaderWrite,Stage::eAllCommands,VideoCore::SubresourceRange{{1,l},{1,1}});
            check(Layout::eGeneral,Access::eShaderWrite,Stage::eAllCommands,VideoCore::SubresourceRange{{2,l},{1,1}});
        }
        check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,{});
        check(Layout::eGeneral,Access::eShaderWrite,Stage::eAllCommands,VideoCore::SubresourceRange{{3,4},{1,1}});
        check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,{});
    }
    assert(!actual.empty());
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,range);
    assert(actual.empty());
    check(Layout::eGeneral,Access::eShaderWrite,Stage::eComputeShader,range);
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,range);
    assert(!actual.empty()); // First read after write must still synchronize.
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eFragmentShader,range);
    check(Layout::eTransferDstOptimal,Access::eTransferWrite,Stage::eTransfer,{});
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,range);
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,VideoCore::SubresourceRange{{0,0},{1,1}});
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,{});
    check(Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,range);
    const auto bench=[&](bool memo) {
        const auto begin=std::chrono::steady_clock::now();
        for(unsigned i=0;i<10000;++i) {
            cached.GetBarriers(actual,Layout::eShaderReadOnlyOptimal,Access::eShaderRead,
                               Stage::eAllCommands,range,memo);
        }
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    };
    const double baseline=bench(false), optimized=bench(true);
    std::printf("Image partial-read barrier equivalence PASS; 10000 repeated reads: baseline %.3f ms, memo %.3f ms\n",baseline,optimized);
}
static void descriptor_pack_cpu() {
    std::array<vk::DescriptorBufferInfo,2> buffers{{{{},16,32},{{},64,128}}};
    std::array<vk::DescriptorImageInfo,2> images{};
    images[0].imageLayout=vk::ImageLayout::eGeneral;
    images[1].imageLayout=vk::ImageLayout::eShaderReadOnlyOptimal;
    std::array<vk::BufferView,2> views{};
    std::array<vk::WriteDescriptorSet,3> writes{{
        {.dstBinding=3,.descriptorCount=2,.pBufferInfo=buffers.data()},
        {.dstBinding=7,.descriptorCount=2,.pImageInfo=images.data()},
        {.dstBinding=11,.descriptorCount=2,.pTexelBufferView=views.data()}}};
    const auto bytes=Vulkan::DescriptorPackingSize(writes);assert(bytes);
    std::vector<u64> storage((*bytes+7)/8);
    const auto packed=Vulkan::PackDescriptorWrites(writes,{reinterpret_cast<u8*>(storage.data()),*bytes});
    assert(packed.size()==3 && packed[0].pBufferInfo!=buffers.data());
    buffers[0].offset=999;images[0].imageLayout=vk::ImageLayout::eUndefined;
    writes[0].dstBinding=999;
    assert(packed[0].dstBinding==3 && packed[0].pBufferInfo[0].offset==16);
    assert(packed[0].pBufferInfo[1].range==128 && packed[1].pImageInfo[0].imageLayout==vk::ImageLayout::eGeneral);
    assert(packed[2].pTexelBufferView!=views.data());
    assert(Vulkan::PackDescriptorWrites(writes,{reinterpret_cast<u8*>(storage.data()),*bytes-1}).empty());
    writes[0].pNext=&buffers;assert(!Vulkan::DescriptorPackingSize(writes));
    std::puts("Descriptor owned snapshot: arrays, all info types, source mutation and unsupported extension PASS");
}
static void fault_decode_gpu(u32 chunk,bool packed=false,bool indirect=false) {
    using namespace Vulkan;
    if(packed) _putenv_s("BB_REDUNDANT_BINDS","1");
    Instance instance(0,false);Scheduler scheduler(instance,packed);const auto device=instance.GetDevice();
    constexpr u32 WordCount=2048,InputBytes=WordCount*4;
    VideoCore::Buffer input(instance,0,InputBytes,VideoCore::MemoryType::HostUncached);
    VideoCore::Buffer output(instance,0,64,VideoCore::MemoryType::HostCached);
    VideoCore::Buffer blank(instance,0,InputBytes,VideoCore::MemoryType::HostUncached);
    Runtime runtime(instance,scheduler);
    VideoCore::Buffer args(instance,0,12,VideoCore::MemoryType::DeviceLocal);
    VideoCore::Buffer args_source(instance,0,12,VideoCore::MemoryType::HostUncached);
    std::memset(blank.mapped_data.data(),0,InputBytes);blank.Flush(0,InputBytes);
    const std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
        {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute},
        {.binding=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute}}};
    auto set=Check(device.createDescriptorSetLayoutUnique({.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=2,.pBindings=bindings.data()}));
    const auto set_handle=*set;
    auto layout=Check(device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&set_handle}));
    const auto module=CompileSPV(FAULT_BUFFER_PROCESS_COMP,device);
    const std::array<u32,3> constants{14,8,chunk};
    const std::array<vk::SpecializationMapEntry,3> entries{{{0,0,4},{1,4,4},{2,8,4}}};
    const vk::SpecializationInfo spec{.mapEntryCount=3,.pMapEntries=entries.data(),.dataSize=12,.pData=constants.data()};
    auto pipeline=Check(device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,
        .module=module,.pName="main",.pSpecializationInfo=&spec},.layout=*layout}));
    vk::DescriptorBufferInfo ib{input.Handle(),0,InputBytes},ob{output.Handle(),0,64};
    const std::array<vk::WriteDescriptorSet,2> writes{{
        {.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&ib},
        {.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&ob}}};
    std::array<bool,WordCount*32> seen{};unsigned decoded=0;
    auto* words=reinterpret_cast<u32*>(input.mapped_data.data());
    std::memset(words,0,InputBytes);
    for(unsigned i=0;i<64;++i) words[i*32]=1;
    words[WordCount-1]|=1u<<31;input.Flush(0,InputBytes);
    for(unsigned pass=0;pass<(indirect ? 24u : 12u);++pass) {
        std::memset(output.mapped_data.data(),0,64);output.Flush(0,64);
        const vk::MemoryBarrier2 before{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands|vk::PipelineStageFlagBits2::eHost,
            .srcAccessMask=vk::AccessFlagBits2::eShaderWrite|vk::AccessFlagBits2::eHostWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eComputeShader,.dstAccessMask=vk::AccessFlagBits2::eShaderRead|vk::AccessFlagBits2::eShaderWrite};
        scheduler.Record([before](vk::CommandBuffer c) {
            c.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&before});
        });
        ib.buffer=input.Handle();
        if(indirect) {
            auto* values=reinterpret_cast<u32*>(args_source.mapped_data.data());
            values[0]=(pass%2==0) ? (WordCount+64*chunk-1)/(64*chunk) : 0;
            values[1]=values[2]=1;args_source.Flush(0,12);
            if(pass) assert(runtime.IsBufferAccessed(&args,0,12,true));
            const vk::BufferCopy copy{0,0,12};
            runtime.CopyBuffer(&args_source,&args,std::span{&copy,1});
            assert(runtime.IsBufferAccessed(&args,0,12));
            runtime.FlushBarriers();
            runtime.TrackIndirectRead(&args,0,12);
            assert(runtime.IsBufferAccessed(&args,0,12,true));
            auto cmd=scheduler.CommandBuffer();
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes);
            cmd.dispatchIndirect(args.Handle(),0);
        } else if(packed) {
            const auto bytes=Vulkan::DescriptorPackingSize(writes);assert(bytes);
            const auto owned=Vulkan::PackDescriptorWrites(writes,
                scheduler.RecordBytes(*bytes,alignof(vk::WriteDescriptorSet)));
            scheduler.Record([owner=&scheduler,handle=*pipeline,layout=*layout,owned,chunk](vk::CommandBuffer c) {
                owner->BindPipeline(c,vk::PipelineBindPoint::eCompute,handle);
                owner->PushDescriptors(c,vk::PipelineBindPoint::eCompute,layout,0,owned);
                owner->BindPipeline(c,vk::PipelineBindPoint::eCompute,handle);
                owner->PushDescriptors(c,vk::PipelineBindPoint::eCompute,layout,0,owned);
                c.dispatch((WordCount+64*chunk-1)/(64*chunk),1,1);
            });
            // A stale pointer would bind a valid but empty input, failing the
            // decoded+pending invariant without invalid GPU handles or buffers.
            ib.buffer=blank.Handle();
        } else {
            const auto cmd=scheduler.CommandBuffer();
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes);cmd.dispatch((WordCount+64*chunk-1)/(64*chunk),1,1);
        }
        const vk::MemoryBarrier2 after{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask=vk::AccessFlagBits2::eShaderWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        scheduler.Record([after](vk::CommandBuffer c) {
            c.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&after});
        });
        scheduler.Finish();output.Invalidate(0,64);input.Invalidate(0,InputBytes);
        const auto* values=reinterpret_cast<const u64*>(output.mapped_data.data());
        const u32 count=VideoCore::StoredFaultCount(values[0],8);assert(count<=7);
        for(u32 i=1;i<=count;++i) {
            const u64 page=values[i]>>14;assert(page<WordCount*32 && !seen[page]);seen[page]=true;++decoded;
        }
        unsigned pending=0;for(unsigned i=0;i<WordCount;++i) pending+=std::popcount(words[i]);
        assert(decoded+pending==65); // No overflow loss or duplicated fault address.
        if(decoded==65) assert(pending==0);
    }
    assert(decoded==65 && seen[WordCount*32-1]);
    assert(VideoCore::StoredFaultCount(~0ull,1024)==1023);
    device.destroyShaderModule(module);
    std::puts("Real GPU fault decoder: clean bitmap, bounded output, overflow retry and high-address fault PASS");
}
static void graphics_binding_memo() {
    Vulkan::GraphicsBindingMemo memo;
    unsigned emitted=0;
    // Interleaved pipelines and independent command buffers must retain the
    // last real graphics binding, not merely the last submitted guest draw.
    for (const auto [cmd,pipe] : std::array<std::pair<u64,u64>,8>{
        {{1,10},{1,10},{1,20},{1,10},{1,10},{2,10},{2,10},{1,10}}})
        emitted += memo.NeedsBind(cmd,pipe);
    assert(emitted==5);
    memo.Invalidate(); // Raw helper used the same command buffer.
    assert(memo.NeedsBind(1,10));
    assert(!memo.NeedsBind(1,10));
    memo.Invalidate(); // Pool reset/reuses the same VkCommandBuffer handle.
    assert(memo.NeedsBind(1,10));
    assert(memo.NeedsBind(0,0) && memo.NeedsBind(0,0));
    std::puts("Graphics bind memo: pipeline changes, raw helpers and command-buffer reuse PASS");
}
static void upload_protection_cpu() {
    constexpr VAddr Base=0x2000000000ull;
    constexpr u64 Page=4096;
    auto* memory=static_cast<u8*>(VirtualAlloc(reinterpret_cast<void*>(Base),4*Page,
        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));assert(memory);
    std::atomic<DWORD> protection_thread{};
    const DWORD producer=GetCurrentThreadId();
    VideoCore::PageManager pages{[&](VAddr address,u64 bytes,u32 perms) {
        DWORD old;const DWORD mode=(perms&2)?PAGE_READWRITE:(perms&1)?PAGE_READONLY:PAGE_NOACCESS;
        assert(VirtualProtect(reinterpret_cast<void*>(address),bytes,mode,&old));
        protection_thread=GetCurrentThreadId();
    }};
    const auto perms=[&](u64 offset) {MEMORY_BASIC_INFORMATION q{};
        assert(VirtualQuery(memory+offset,&q,sizeof(q)));return q.Protect;};
    VideoCore::UploadWorker worker(pages);
    VideoCore::RegionBits two;two.SetRange(0,2);
    assert(pages.BeginDeferredProtection());
    pages.UpdatePageWatchersForRegion<true,false>(Base,two);
    assert(perms(0)==PAGE_READWRITE); // Not protected while the region transaction is open.
    memory[0]=0x22;memory[Page]=0x33;
    auto batch=pages.EndDeferredProtection();assert(!batch.empty());
    std::array<u8,2> copied{};
    worker.Queue(std::move(batch),[&]{copied={memory[0],memory[Page]};});worker.Wait();
    assert(copied[0]==0x22&&copied[1]==0x33);
    assert(perms(0)==PAGE_READONLY&&perms(Page)==PAGE_READONLY);
    assert(protection_thread.load()!=producer);
    // Stronger GPU read protection and adjacent readonly pages must stay distinct.
    VideoCore::RegionBits first;first.Set(0);
    assert(pages.BeginDeferredProtection());
    pages.UpdatePageWatchersForRegion<true,true>(Base,first);
    worker.Queue(pages.EndDeferredProtection());worker.Wait();
    assert(perms(0)==PAGE_NOACCESS&&perms(Page)==PAGE_READONLY);
    pages.UpdatePageWatchersForRegion<false,true>(Base,first);
    pages.UpdatePageWatchersForRegion<false,false>(Base,two);
    assert(perms(0)==PAGE_READWRITE);
    // A queued older protect must not undo a newer guest write/unprotect.
    assert(pages.BeginDeferredProtection());
    pages.UpdatePageWatchersForRegion<true,false>(Base,first);
    auto stale=pages.EndDeferredProtection();
    pages.UpdatePageWatchersForRegion<false,false>(Base,first);
    worker.Queue(std::move(stale));worker.Wait();assert(perms(0)==PAGE_READWRITE);
    std::atomic<unsigned> copied_jobs{};
    { VideoCore::UploadWorker draining(pages);
      for(unsigned i=0;i<128;++i) draining.Queue({},[&]{++copied_jobs;}); }
    assert(copied_jobs==128);
    assert(VirtualFree(memory,0,MEM_RELEASE));
    std::puts("Upload protection: native permissions, copy order, stale batches and shutdown drain PASS");
}
static void upload_submit_gpu() {
    Vulkan::Instance instance(0,false);Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance,scheduler);
    VideoCore::PageManager pages{[](VAddr,u64,u32){std::abort();}};
    VideoCore::UploadWorker worker(pages);
    VideoCore::Buffer source(instance,0,4096,VideoCore::MemoryType::HostCached);
    VideoCore::Buffer result(instance,0,4096,VideoCore::MemoryType::HostCached);
    std::memset(source.mapped_data.data(),0,4096);source.Flush(0,4096);
    scheduler.SetUploadWaiter([&]{worker.Wait();},[&]{return worker.Fence();});
    worker.Queue({},[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        *reinterpret_cast<u32*>(source.mapped_data.data())=0x98765432;source.Flush(0,4096);
    });
    const vk::BufferCopy copy{0,0,4096};runtime.CopyBuffer(&source,&result,std::span{&copy,1});
    scheduler.Finish();result.Invalidate(0,4096);
    assert(*reinterpret_cast<const u32*>(result.mapped_data.data())==0x98765432);
    std::atomic<bool> copied{},signaled{};
    worker.Queue({},[&] {std::this_thread::sleep_for(std::chrono::milliseconds(30));copied=true;});
    scheduler.SignalAfterHostCopies([&] {assert(copied.load());signaled=true;});
    scheduler.WaitDeferredSignals();assert(signaled.load());
    scheduler.SetUploadWaiter({});
    std::puts("Real Vulkan submission waits for deferred uploads before reading staging data PASS");
}
static void cpu_buffer_sync_memo() {
    _putenv_s("BB_BUFFER_SYNC_MEMO", "1");
    _putenv_s("BB_HOT_PAGES", "0");
    VideoCore::PageManager pages{nullptr};
    auto tracker = std::make_unique<VideoCore::MemoryTracker>(pages);
    constexpr VAddr Base = 8 * VideoCore::TRACKER_HIGHER_PAGE_SIZE;
    constexpr u64 Region = VideoCore::TRACKER_HIGHER_PAGE_SIZE;
    std::array<u64,2> versions{};
    assert(!tracker->GetCpuVersions(Base, 65536, versions));
    assert(tracker->IsRegionCpuModified(Base, 65536)); // Creates production tracker.
    assert(tracker->GetCpuVersions(Base, 65536, versions) && versions[1] == 0);
    char first{}, second{};
    const auto* arena = reinterpret_cast<const VideoCore::Buffer*>(&first);
    const auto* replacement = reinterpret_cast<const VideoCore::Buffer*>(&second);
    VideoCore::CpuBufferMemo memo{Base,65536,arena,versions};
    assert(memo.Matches(Base,65536,versions,arena,arena));
    assert(!memo.Matches(Base+1,65536,versions,arena,arena));
    assert(!memo.Matches(Base,65535,versions,arena,arena));
    assert(!memo.Matches(Base,65536,versions,replacement,replacement));
    assert(!memo.Matches(Base,65536,versions,arena,replacement));
    assert(!memo.Matches(Base,65536,versions,nullptr,nullptr));
    // Pages start writable/dirty: no guest protection or fake runtime is used.
    // Even repeated writes to an already dirty range must invalidate a token.
    tracker->MarkRegionAsCpuModified(Base+4096,4096);
    std::array<u64,2> after{};
    assert(tracker->GetCpuVersions(Base,65536,after) && after != versions);
    assert(!memo.Matches(Base,65536,after,arena,arena));
    tracker->MarkRegionAsCpuModified(Base+4096,4096);
    assert(tracker->GetCpuVersions(Base,65536,versions) && versions != after);
    assert(!tracker->GetCpuVersions(Base+Region-4096,8192,after));
    tracker->IsRegionCpuModified(Base+Region,4096);
    assert(tracker->GetCpuVersions(Base+Region-4096,8192,after));
    const auto crossing = after;
    tracker->MarkRegionAsCpuModified(Base+Region,4096);
    assert(tracker->GetCpuVersions(Base+Region-4096,8192,after));
    assert(after[0] == crossing[0] && after[1] != crossing[1]);
    assert(!tracker->GetCpuVersions(Base,2*Region+1,after));
    assert(!tracker->GetCpuVersions(Base,0,after));
    assert(!tracker->GetCpuVersions(~u64{0}-10,32,after));
    assert(!tracker->GetCpuVersions((u64{1}<<40)-1,2,after));
    std::puts("Readonly buffer sync memo: guest writes, crossing regions, arena migration and bounds PASS");
}
static void cpu_word_summary() {
    VideoCore::RegionBits bits;
    VideoCore::CpuWordSummary clean(false);
    assert(!clean.MightBeDirty(0,VideoCore::NUM_PAGES_PER_REGION));
    VideoCore::CpuWordSummary summary;
    bits.Fill(); summary.Refresh(bits,0,1024);
    u32 random=12345;
    const auto next=[&] {random=random*1664525u+1013904223u;return random;};
    for(unsigned i=0;i<10000;++i) {
        const size_t a=next()%1024,b=std::min<size_t>(1024,a+1+next()%256);
        if(next()&1) {summary.Mark(a,b);bits.SetRange(a,b);}
        else bits.UnsetRange(a,b);
        summary.Refresh(bits,a,b);
        for(unsigned j=0;j<8;++j) {
            const size_t lo=next()%1024,hi=std::min<size_t>(1024,lo+1+next()%1024);
            bool reference=false;for(size_t at=lo;at<hi;++at) reference|=bits.Get(at);
            assert((summary.MightBeDirty(lo,hi)&&bits.AnyInRange(lo,hi))==reference);
        }
    }
    bits.Clear();summary.Refresh(bits,0,1024);assert(!summary.MightBeDirty(0,1024));
    summary.Mark(63,65);bits.SetRange(63,65);summary.Refresh(bits,63,65);
    assert(summary.MightBeDirty(63,65)); assert(!summary.MightBeDirty(128,1024));
    bits.Unset(63);summary.Refresh(bits,63,64);assert(bits.Get(64));
    assert(!summary.MightBeDirty(0,64));assert(summary.MightBeDirty(64,65));
    std::puts("CPU dirty-word summary: mixed writes, uploads and word boundaries PASS");
}
static void cpu_backing_hash() {
    std::vector<u8> backing(3*65536);
    for(size_t i=0;i<backing.size();++i) backing[i]=u8(i*13+(i>>8));
    void* guest=VirtualAlloc(nullptr,backing.size(),MEM_COMMIT|MEM_RESERVE,PAGE_NOACCESS);
    assert(guest);
    const VAddr base=reinterpret_cast<VAddr>(guest);
    VideoCore::CpuBackingHasher hasher;
    const auto copy=[&](VAddr source,u8* output,u64 bytes) {
        assert(source>=base && source-base+bytes<=backing.size());
        std::memcpy(output,backing.data()+source-base,bytes);
    };
    for(const size_t size:{size_t{0},size_t{1},size_t{65535},size_t{65536},size_t{65537},backing.size()-7}) {
        assert(hasher.Hash(base+7,size,copy)==XXH3_64bits(backing.data()+7,size));
    }
    const u64 before=hasher.Hash(base,backing.size(),copy);
    backing[65536]^=0x5a;
    assert(hasher.Hash(base,backing.size(),copy)!=before);
    assert(hasher.Hash(base,backing.size(),copy)==XXH3_64bits(backing.data(),backing.size()));
    MEMORY_BASIC_INFORMATION info{};
    assert(VirtualQuery(guest,&info,sizeof(info)) && info.Protect==PAGE_NOACCESS);
    assert(VirtualFree(guest,0,MEM_RELEASE));
    std::puts("CPU backing checksum: protected guest view, chunk boundaries and changed bytes PASS");
}
int main(int argc,char** argv) {
    // A failed automated assertion must be reported to CTest, not wait in a
    // Windows dialog. These settings affect only this test process.
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(_WRITE_ABORT_MSG,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    if(argc==2 && !std::strcmp(argv[1],"--image-dirty-ranges-gpu")) {image_read_memo(true);return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--readback-writer-history-cpu")) {readback_writer_history_cpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--readback-queue-gpu")) {readback_queue_gpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--recorded-bindings-cpu")) {recorded_bindings_cpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--shader-memos-cpu")) {shader_memos_cpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--upload-protection-cpu")) {upload_protection_cpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--upload-submit-gpu")) {upload_submit_gpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--graphics-binding-memo")) {graphics_binding_memo();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--cpu-buffer-sync-memo")) {cpu_buffer_sync_memo();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--lru-gc")) {lru_gc_cpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--texture-gc")) {VideoCore::TextureCacheTestAccess::Run();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--gpu-profile-async")) {
        gpu_profile_async();return 0;
    }
    if(argc==4 && !std::strcmp(argv[1],"--gcn-disassemble")) {
        gcn_disassemble(argv[2],argv[3]);return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--cpu-backing-hash")) {cpu_backing_hash();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--descriptor-pack")) {descriptor_pack_cpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--descriptor-pack-gpu")) {fault_decode_gpu(1,true);return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--indirect-buffer-gpu")) {fault_decode_gpu(1,false,true);return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--readback-prefetch-gpu")) {readback_prefetch_gpu();return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--particle-shader-cpu")) { particle_shader_cpu(); return 0; }
    if(argc==2 && !std::strcmp(argv[1],"--particle-shader-gpu")) { particle_shader_gpu(); return 0; }
    if(argc==2 && !std::strcmp(argv[1],"--readback-hint")) {
        VideoCore::ReadbackPrefetchGate gate;
        assert(gate.Ready(0,1000000));
        gate.Queued(0);
        assert(!gate.Ready(999999,1000000) && gate.Ready(1000000,1000000));
        assert(gate.Ready(1,0)); // Disabled gate preserves per-dispatch prefetch.
        assert(gate.Ready(1000001,1000000)); // No work queued: do not postpone readiness.
        gate.Queued(1000001);
        assert(!gate.Ready(1500000,1000000));
        VideoCore::ReadbackHint hint;hint.page=0x1000;assert(!hint.Valid());
        hint.Captured(3);assert(hint.Valid() && hint.Matches(0x1100,16));
        hint.Write(0x3000,4096);assert(hint.Valid());
        hint.Write(0x1ffc,4);assert(!hint.Valid());hint.Captured(4);assert(hint.Valid());
        assert(!hint.Matches(0x1ffc,8));assert(!hint.Matches(0x1000,4097));
        hint.Consumed();assert(!hint.Valid());
        hint.Captured(5);hint.Forget();assert(!hint.Valid() && !hint.Matches(0x1100,4));
        assert(hint.tick==5); // Storage remains pinned even when guest memory is unmapped.
        hint.page=0x1000;hint.Captured(6);
        const auto detached_tick=hint.Detach();
        assert(detached_tick==6 && !hint.Valid() && hint.tick==0 && hint.page==0x1000);
        hint.Captured(7);assert(hint.Valid()); // New buffer may be captured independently.
        struct Slot {VideoCore::ReadbackHint hint;u64 last_use{};};
        std::array<Slot,5> windows{};
        windows[0].hint.page=0x7100;windows[1].hint.page=0x7800;
        windows[2].hint.page=0x8500;windows[3].hint.page=0x9000;
        windows[4].hint.page=0x4500;
        const auto merged=VideoCore::CoalescedReadbackWindows(0x3000,0x4000,0x2000,0x8800,0x1000,windows);
        assert(merged.size()==2 && merged[0]==std::pair(VAddr{0x3000},VAddr{0x5000}) &&
               merged[1]==std::pair(VAddr{0x7000},VAddr{0x8800}));
        std::array<Slot,3> slots{};
        for(size_t i=0;i<slots.size();++i) {
            slots[i].hint.page=0x1000*(i+1);slots[i].last_use=i+1;slots[i].hint.Captured(i+1);
        }
        auto free=[](u64 tick){return tick<=2;};
        assert(VideoCore::SelectReadbackVictim(slots,free)==&slots[0]);
        slots[0].last_use=4; // A cache hit protects the frequently used page.
        assert(VideoCore::SelectReadbackVictim(slots,free)==&slots[1]);
        slots[0].hint.Forget();
        assert(VideoCore::SelectReadbackVictim(slots,free)==&slots[0]);
        assert(!VideoCore::SelectReadbackVictim(slots,[](u64){return false;}));
        std::puts("Readback snapshot generation / unrelated writes / page bounds / ownership invalidation PASS");return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--fault-decode-gpu")) {fault_decode_gpu(1);fault_decode_gpu(32);return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--cpu-word-summary")) {cpu_word_summary();return 0;}
    if (argc==3 && !std::strcmp(argv[1],"--runtime-benchmark-control")) runtime_benchmark_control(argv[2]);
    else if (argc==2 && !std::strcmp(argv[1],"--flat-data-visibility")) flat_visibility();
    else if (argc==2 && !std::strcmp(argv[1],"--flat-data-memo")) flat_memo();
    else if (argc==2 && !std::strcmp(argv[1],"--texture-set-distribution")) texture_set_distribution();
    else if (argc==3 && !std::strcmp(argv[1],"--snapshot-pixels")) snapshot_pixels(argv[2]);
    else if (argc==3 && !std::strcmp(argv[1],"--snapshot-worker")) snapshot_worker(argv[2]);
    else if (argc==2 && !std::strcmp(argv[1],"--image-read-memo")) image_read_memo();
    else if (argc==2 && !std::strcmp(argv[1],"--registry-changes")) registry_changes();
    else if (argc==3 && !std::strcmp(argv[1],"--runtime-cache")) runtime_cache(argv[2]);
    else if (argc==2 && !std::strcmp(argv[1],"--cache-guards")) cache_guards();
    else if (argc==3 && !std::strcmp(argv[1],"--shader-bundle")) shader_bundle(argv[2]);
    else if (argc==3 && !std::strcmp(argv[1],"--driver-cache")) driver_cache(argv[2]);
    else if (argc==2 && !std::strcmp(argv[1],"--irq")) irq_controller();
    else if (argc==2 && !std::strcmp(argv[1],"--stencil")) stencil_reference();
    else if (argc==2 && !std::strcmp(argv[1],"--mouse-motion")) mouse_motion_input();
    else if (argc==3 && !std::strcmp(argv[1],"--shaders")) shaders(argv[2]);
    else if (argc==3 && !std::strcmp(argv[1],"--diagnostics")) diagnostic_ring(argv[2]);
    else if (argc==3 && !std::strcmp(argv[1],"--frame-capture")) frame_capture(argv[2]);
    else textures();
}
