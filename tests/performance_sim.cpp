// SPDX-License-Identifier: GPL-2.0-or-later
// Independent Windows benchmark. Uses production helpers and Image::GetBarriers;
// synthetic access sequences are NOT a replay of F11 or a whole-game FPS estimate.
#include "vulkan_test.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "bbport_toggles.h"
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_set_key.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/registry_changes.h"
#include "video_core/renderer_vulkan/vk_flat_data_memo.h"
#include "video_core/buffer_cache/buffer.h"

namespace {
using Clock = std::chrono::steady_clock;
using Layout = vk::ImageLayout;
using Access = vk::AccessFlagBits2;
using Stage = vk::PipelineStageFlagBits2;
struct Config { u32 operations=200000, rounds=9; u64 seed=0x309; bool cpu_only=false, self_test=false; };
struct Measurement { double ms{}; u64 checksum{}, hits{}, checks{}; };
struct Result {
    std::string name, scope;
    std::vector<Measurement> baseline, candidate;
    bool equivalent=true;
};
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
u64 Next(u64& state) { state^=state<<13; state^=state>>7; state^=state<<17; return state; }
double Milliseconds(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
double Quantile(std::vector<double> values, double q) {
    std::sort(values.begin(),values.end());
    if(q==.5) return (values[(values.size()-1)/2]+values[values.size()/2])/2;
    return values[std::min(values.size()-1,size_t(std::ceil(q*values.size())-1))];
}
void PrintMeasurements(const std::vector<Measurement>& rows) {
    std::vector<double> times;
    for(const auto& row:rows) times.push_back(row.ms);
    const double median=Quantile(times,.5);
    std::vector<double> deviations;
    for(auto ms:times) deviations.push_back(std::abs(ms-median));
    std::cout<<"{\"median_ms\":"<<median<<",\"p95_round_ms\":"<<Quantile(times,.95)
             <<",\"min_ms\":"<<*std::min_element(times.begin(),times.end())
             <<",\"max_ms\":"<<*std::max_element(times.begin(),times.end())
             <<",\"mad_ratio\":"<<(median ? Quantile(deviations,.5)/median : 0)
             <<",\"checksum\":"<<rows.front().checksum<<",\"hits\":"<<rows.front().hits
             <<",\"subresource_checks\":"<<rows.front().checks<<",\"round_ms\":[";
    for(size_t i=0;i<times.size();++i) std::cout<<(i?",":"")<<times[i];
    std::cout<<"]}";
}
template<class Run> Result Measure(const Config& cfg, std::string name, std::string scope, Run run) {
    Result result{std::move(name),std::move(scope)};
    // Warm both modes; every timed round resets state outside the clock.
    run(false); run(true);
    for(u32 round=0;round<cfg.rounds;++round) {
        Measurement old_value,new_value;
        if(round%2) { new_value=run(true); old_value=run(false); }
        else { old_value=run(false); new_value=run(true); }
        Require(old_value.checksum==new_value.checksum,"Baseline/candidate semantic checksum differs");
        if(!result.baseline.empty()) {
            Require(old_value.checksum==result.baseline.front().checksum &&
                    new_value.hits==result.candidate.front().hits,"Workload is not deterministic");
        }
        result.baseline.push_back(old_value); result.candidate.push_back(new_value);
    }
    return result;
}

Result CacheCase(const Config& cfg, u32 working_set, const char* name) {
    struct Entry { u64 key{}, first{}, second{}; };
    // Heap storage: Windows test threads normally have only a 1 MiB stack.
    std::vector<Entry> slots(32768);
    std::vector<std::array<u64,2>> descriptors;
    u64 random=cfg.seed;
    for(u32 i=0;i<working_set;++i) descriptors.push_back({Next(random),Next(random)});
    return Measure(cfg,name,"production_hash_with_direct_mapped_cache_model",[&](bool candidate) {
        std::fill(slots.begin(),slots.end(),Entry{});
        Measurement value;
        const auto start=Clock::now();
        for(u32 i=0;i<cfg.operations;++i) {
            const auto& desc=descriptors[i%working_set];
            const u64 hash=Vulkan::TextureSetKey(desc,0x1234);
            const u64 key=candidate ? hash : hash|1;
            auto& slot=slots[key%slots.size()];
            const bool hit=slot.key==key && slot.first==desc[0] && slot.second==desc[1];
            value.hits+=hit;
            if(!hit) slot={key,desc[0],desc[1]};
            Require(slot.first==desc[0] && slot.second==desc[1],"Cache returned another descriptor");
            value.checksum^=slot.first+slot.second+u64(i)*0x9E3779B97F4A7C15ull;
        }
        value.ms=Milliseconds(start);
        return value;
    });
}

// Scope invalidation correctness: no artificial expensive miss work to manufacture speedups.
u64 CheckRegistry(u64 seed) {
    using namespace VideoCore;
    RegistryChanges journal;
    std::vector<RegistryRange> history;
    const std::array watched{RegistryRange{100,200},RegistryRange{400,500}};
    u64 random=seed, previous=0, checked=0;
    for(u32 i=0;i<5000;++i) {
        const auto begin=Next(random)%1024;
        const RegistryRange changed{begin,begin+Next(random)%64};
        journal.Record(changed); history.push_back(changed);
        if(i%83==0 || i%7==0) {
            bool expected=history.size()-previous<=RegistryChanges::Capacity;
            for(size_t at=previous;expected && at<history.size();++at)
                for(const auto& range:watched) if(history[at].Overlaps(range)) expected=false;
            u64 actual_generation=previous;
            Require(journal.Unchanged(actual_generation,watched)==expected,"Registry overlap/history validity differs");
            if(expected) Require(actual_generation==journal.Generation(),"Registry generation was not advanced");
            previous=journal.Generation(); ++checked;
        }
    }
    // Explicit expired history and future generation must conservatively miss.
    for(u32 i=0;i<65;++i) journal.Record({900,1000});
    Require(!journal.Unchanged(previous,watched),"Expired registry journal incorrectly reused");
    u64 future=journal.Generation()+1;
    Require(!journal.Unchanged(future,watched),"Future registry generation incorrectly reused");
    return checked+2;
}

struct Op { u8 image{}, kind{}, view{}; };
struct Request { Layout layout; vk::AccessFlags2 access; vk::PipelineStageFlags2 stage; std::optional<VideoCore::SubresourceRange> range; };
Request Decode(Op op) {
    const VideoCore::SubresourceRange wide{{1,0},{5,64}}, slice{{u32(op.view)%6,u32(op.view)*7%64},{1,1}};
    switch(op.kind) {
    case 1: return {Layout::eGeneral,Access::eShaderWrite,Stage::eComputeShader,wide};
    case 2: return {Layout::eTransferDstOptimal,Access::eTransferWrite,Stage::eTransfer,{}};
    case 3: return {Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,{}};
    case 4: return {Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eFragmentShader,slice};
    case 5: return {Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eComputeShader,wide};
    default: return {Layout::eShaderReadOnlyOptimal,Access::eShaderRead,Stage::eAllCommands,wide};
    }
}
using Images = std::vector<std::unique_ptr<VideoCore::Image>>;
void Reset(Images& images) {
    for(auto& image:images) {
        image->backing->state={}; image->backing->subresource_states.clear();
        image->backing->read_memo.valid=false;
    }
}
Measurement RunImages(Images& images, const std::vector<Op>& ops, bool candidate) {
    Reset(images);
    VideoCore::Image::Barriers barriers;
    // Reserve outside timing, including the worst full-resource/write transition.
    barriers.reserve(384);
    const auto before_hits=BbStats::image_read_memo_hits.load(), before_checks=BbStats::image_subresource_checks.load();
    Measurement result;
    const auto start=Clock::now();
    for(const auto op:ops) {
        const auto request=Decode(op); barriers.clear();
        images[op.image]->GetBarriers(barriers,request.layout,request.access,request.stage,request.range,candidate);
        result.checksum+=barriers.size();
        for(const auto& barrier:barriers) {
            // Device-specific image handles are intentionally excluded.
            result.checksum+=u64(barrier.srcAccessMask)+u64(barrier.dstAccessMask)+u64(barrier.oldLayout)+u64(barrier.newLayout)
                +barrier.subresourceRange.baseMipLevel+barrier.subresourceRange.baseArrayLayer;
        }
    }
    result.ms=Milliseconds(start);
    result.hits=BbStats::image_read_memo_hits.load()-before_hits;
    result.checks=BbStats::image_subresource_checks.load()-before_checks;
    return result;
}
void CheckImages(Images& old_images, Images& new_images, const std::vector<Op>& ops) {
    Reset(old_images); Reset(new_images);
    VideoCore::Image::Barriers old_barriers,new_barriers;
    for(const auto op:ops) {
        const auto request=Decode(op); old_barriers.clear(); new_barriers.clear();
        old_images[op.image]->GetBarriers(old_barriers,request.layout,request.access,request.stage,request.range,false);
        new_images[op.image]->GetBarriers(new_barriers,request.layout,request.access,request.stage,request.range,true);
        Require(old_barriers.size()==new_barriers.size(),"Barrier count differs");
        for(size_t i=0;i<old_barriers.size();++i) {
            auto a=old_barriers[i],b=new_barriers[i]; a.image=b.image;
            Require(a==b,"Barrier fields differ (layout/access/stage/subresource)");
        }
        const auto& a=*old_images[op.image]->backing;
        const auto& b=*new_images[op.image]->backing;
        Require(a.state.layout==b.state.layout && a.state.access_mask==b.state.access_mask &&
                a.state.pl_stage==b.state.pl_stage && a.subresource_states.size()==b.subresource_states.size(),"Final image state differs");
        for(size_t i=0;i<a.subresource_states.size();++i)
            Require(a.subresource_states[i].layout==b.subresource_states[i].layout &&
                    a.subresource_states[i].access_mask==b.subresource_states[i].access_mask &&
                    a.subresource_states[i].pl_stage==b.subresource_states[i].pl_stage,"Subresource state differs");
    }
}
std::vector<Op> ImageWorkload(const Config& cfg, unsigned pattern) {
    std::vector<Op> ops; ops.reserve(cfg.operations);
    u64 random=cfg.seed;
    for(u32 i=0;i<cfg.operations;++i) {
        Op op{u8((i/64)%8),0,0};
        if(pattern==1) { op.kind=4; op.view=u8(i%6); } // No consecutive matching view.
        if(pattern==2) {
            const auto value=Next(random)%100;
            op.image=u8((i/16)%8);
            op.kind=value<5 ? 1 : value<7 ? 2 : value<10 ? 4 : value<12 ? 5 : 0;
            op.view=u8(Next(random)%6);
        }
        if(pattern==3) op.kind=3; // Existing constant-time full-resource fast path.
        ops.push_back(op);
    }
    return ops;
}
Result FlatCase(const Config& cfg,const Vulkan::Instance& instance,Vulkan::Scheduler& scheduler,unsigned pattern) {
    std::vector<std::array<u32,128>> sources(16);
    for(u32 i=0;i<sources.size();++i) for(u32 j=0;j<128;++j) sources[i][j]=i*31+j;
    const char* names[]={"flat_upload_repeated","flat_upload_mixed","flat_upload_changing_control"};
    const auto run=[&](bool candidate,bool validate=false) {
        VideoCore::StreamBuffer stream(instance,scheduler,VideoCore::MemoryType::Stream,32ull<<20);
        auto memo=std::make_unique<Vulkan::FlatDataMemo>();
        Measurement result;
        const auto start=Clock::now();
        const u32 count=validate ? 4096 : cfg.operations;
        for(u32 i=0;i<count;++i) {
            const u32 group=(i/128)%16;
            auto& source=sources[group]; source[0]=group*31;
            if(pattern==2 || (pattern==1 && i%16==0)) source[0]=i+100000;
            const auto bytes=std::span<const u8>{reinterpret_cast<const u8*>(source.data()),sizeof(source)};
            const auto uploaded=memo->Upload(u64(group+1)*16,bytes,
                [&]{return Vulkan::FlatDataMemo::Epoch{scheduler.CurrentTick(),stream.WrapGeneration()};},
                [&]{return stream.Copy(source.data(),sizeof(source),instance.StorageMinAlignment());},candidate);
            if(validate) Require(std::memcmp(stream.mapped_data.data()+uploaded.offset,source.data(),sizeof(source))==0,"Flat upload returned stale/overwritten data");
            if(BbStats::enabled && uploaded.reused) {
                BbStats::flat_data_memo_hits.fetch_add(1,std::memory_order_relaxed);
                BbStats::flat_data_bytes_saved.fetch_add(sizeof(source),std::memory_order_relaxed);
            }
            result.hits+=uploaded.reused;
            result.checksum+=source[0]+source.back();
        }
        result.ms=Milliseconds(start);
        return result;
    };
    run(false,true); run(true,true);
    return Measure(cfg,names[pattern],"production_FlatDataMemo_StreamBuffer_Copy_cpu_time",[&](bool candidate){return run(candidate);});
}
}

int main(int argc,char** argv) {
    try {
        Config cfg;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if(arg=="--cpu-only") cfg.cpu_only=true;
            else if(arg=="--self-test") cfg.self_test=true;
            else if((arg=="--operations" || arg=="--rounds" || arg=="--seed") && i+1<argc) {
                const auto value=std::stoull(argv[++i],nullptr,0);
                if(arg=="--operations") { Require(value>=1000 && value<=2000000,"Operations must be 1000..2000000"); cfg.operations=u32(value); }
                else if(arg=="--rounds") { Require(value>=3 && value<=31,"Rounds must be 3..31"); cfg.rounds=u32(value); }
                else { Require(value!=0,"Seed cannot be zero"); cfg.seed=value; }
            } else throw std::runtime_error("Usage: performance-sim-test [--cpu-only] [--self-test] [--operations N] [--rounds N] [--seed N]");
        }
        if(cfg.self_test) { cfg.operations=4096; cfg.rounds=3; }
        Require(BbStats::enabled,"Set BB_PERF_STATS=1 so benchmark includes production recording overhead");
        const char* memo=std::getenv("BB_IMAGE_READ_MEMO");
        Require(!memo || memo[0]!='0',"BB_IMAGE_READ_MEMO=0 disables the candidate; use 1 for this benchmark");
        const u64 registry_checks=CheckRegistry(cfg.seed);
        std::vector<Result> results;
        results.push_back(CacheCase(cfg,512,"texture_cache_small_set"));
        results.push_back(CacheCase(cfg,50000,"texture_cache_pressure"));
        std::string gpu="not_used";
        if(!cfg.cpu_only) {
            Vulkan::Instance instance(0,false); gpu=instance.GetModelName();
            Vulkan::Scheduler scheduler(instance); Vulkan::Runtime runtime(instance,scheduler);
            Common::SlotVector<VideoCore::ImageView> views;
            VideoCore::ImageInfo info; info.type=AmdGpu::ImageType::Color2DArray;
            info.pixel_format=vk::Format::eR8G8B8A8Unorm; info.size={32,32,1}; info.resources={6,64}; info.num_bits=32;
            Images old_images,new_images;
            for(u32 i=0;i<8;++i) {
                old_images.push_back(std::make_unique<VideoCore::Image>(instance,runtime,views,info));
                new_images.push_back(std::make_unique<VideoCore::Image>(instance,runtime,views,info));
            }
            const char* names[]={"image_repeated_partial_read","image_view_switch_control","image_mixed_read_write","image_full_range_control"};
            for(unsigned pattern=0;pattern<4;++pattern) {
                auto ops=ImageWorkload(cfg,pattern);
                // Verify every operation's barriers and final states before measuring.
                CheckImages(old_images,new_images,ops);
                results.push_back(Measure(cfg,names[pattern],"production_Image_GetBarriers_cpu_time",[&](bool candidate) {
                    return RunImages(candidate?new_images:old_images,ops,candidate);
                }));
            }
            for(unsigned pattern=0;pattern<3;++pattern) results.push_back(FlatCase(cfg,instance,scheduler,pattern));
        }
        std::cout<<std::setprecision(10)<<"PERF_SIM_JSON {\"schema\":1,\"kind\":\"synthetic_component_benchmark\",\"gpu\":"<<std::quoted(gpu)
                 <<",\"operations\":"<<cfg.operations<<",\"rounds\":"<<cfg.rounds<<",\"seed\":"<<cfg.seed
                 <<",\"cpu_only\":"<<(cfg.cpu_only?"true":"false")<<",\"registry_checks\":"<<registry_checks<<",\"cases\":[";
        for(size_t i=0;i<results.size();++i) {
            const auto& result=results[i];
            std::cout<<(i?",":"")<<"{\"name\":"<<std::quoted(result.name)<<",\"scope\":"<<std::quoted(result.scope)
                     <<",\"equivalent\":true,\"baseline\":";
            PrintMeasurements(result.baseline); std::cout<<",\"candidate\":"; PrintMeasurements(result.candidate); std::cout<<"}";
        }
        std::cout<<"]}\n";
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"PERF_SIM_ERROR: %s\n",error.what()); return 1; }
}
