// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded manual diagnostics. GPU waiting, pixel conversion and file I/O run on this worker.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <vk_mem_alloc.h>
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_common.h"
namespace Vulkan {
class FrameSnapshotWriter {
public:
    // Four marked frames survive follow-up traffic; total disk bound stays eight.
    struct Slots {
        u32 marks{},followups{};
        u32 Next(bool manual) { return manual ? marks++%4 : 4+followups++%4; }
    };
    struct Job {
        VkBuffer buffer{}; VmaAllocation allocation{}; void* pixels{};
        vk::Semaphore semaphore{}; u64 gpu_tick{},tick{};
        u32 width{},height{},source_format{}; bool bgra{},hdr{},srgb{},manual{true};
        float gamma{1}; std::filesystem::path directory;
    };
    FrameSnapshotWriter(vk::Device device_,VmaAllocator allocator_):device{device_},allocator{allocator_} {
        thread=std::jthread([this](std::stop_token stop) {
            Common::SetCurrentThreadName("bb:FrameSnapshot");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            for(;;) {
                Job job;
                {
                    std::unique_lock lock{mutex};
                    cv.wait(lock,[&]{ return stop.stop_requested() || !jobs.empty(); });
                    if(jobs.empty() && stop.stop_requested()) return;
                    job=std::move(jobs.front()); jobs.pop_front();
                }
                try {
                    const vk::SemaphoreWaitInfo wait{.semaphoreCount=1,.pSemaphores=&job.semaphore,.pValues=&job.gpu_tick};
                    if(device.waitSemaphores(wait,UINT64_MAX)==vk::Result::eSuccess) {
                        vmaInvalidateAllocation(allocator,job.allocation,0,VK_WHOLE_SIZE);
                        Save(job,slots.Next(job.manual));
                    }
                } catch(const std::exception& error) {
                    // Diagnostic failures must not terminate the game process.
                    std::fprintf(stderr,"FRAME_SNAPSHOT failed: %s\n",error.what());
                }
                vmaDestroyBuffer(allocator,job.buffer,job.allocation);
                {std::scoped_lock lock{mutex}; --pending;}
            }
        });
    }
    ~FrameSnapshotWriter() { thread.request_stop(); cv.notify_all(); thread.join(); }
    bool Full() { std::scoped_lock lock{mutex}; return pending>=2; }
    void Submit(Job job) { std::scoped_lock lock{mutex}; jobs.push_back(std::move(job)); ++pending; cv.notify_one(); }
    static bool Save(const Job& job,u32 slot) {
        if(!job.pixels || !job.width || !job.height || job.width>4096 || job.height>2160) return false;
        const double scale=std::min({1.0,1920.0/job.width,1080.0/job.height});
        const u32 width=std::max(1u,u32(job.width*scale)),height=std::max(1u,u32(job.height*scale));
        const u32 stride=(width*3+3)&~3u;
        std::vector<u8> bmp(54+stride*height,0);
        const auto word=[&](size_t at,u32 value) {std::memcpy(bmp.data()+at,&value,4);};
        bmp[0]='B';bmp[1]='M';word(2,u32(bmp.size()));word(10,54);word(14,40);
        word(18,width);word(22,0u-height);bmp[26]=1;bmp[28]=24;word(34,stride*height);
        const auto* bytes=static_cast<const u8*>(job.pixels); double sum=0;
        for(u32 y=0;y<height;++y) for(u32 x=0;x<width;++x) {
            const auto* src=bytes+(size_t(u64(y)*job.height/height)*job.width+u64(x)*job.width/width)*4;
            auto* dst=bmp.data()+54+size_t(y)*stride+x*3;
            dst[0]=src[job.bgra?0:2];dst[1]=src[1];dst[2]=src[job.bgra?2:0];
            sum+=0.2126*dst[2]+0.7152*dst[1]+0.0722*dst[0];
        }
        auto path=job.directory/("frame-snapshot-"+std::to_string(slot)+".bmp");
        std::ofstream output(path,std::ios::binary|std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bmp.data()),bmp.size());output.close();
        auto metadata=path;metadata.replace_extension("json");
        std::ofstream info(metadata,std::ios::trunc);
        info<<"{\"tick_ms\":"<<job.tick<<",\"source_width\":"<<job.width<<",\"source_height\":"<<job.height
            <<",\"width\":"<<width<<",\"height\":"<<height<<",\"pixel_format\":"<<job.source_format
            <<",\"gamma\":"<<job.gamma<<",\"hdr\":"<<(job.hdr?"true":"false")
            <<",\"manual\":"<<(job.manual?"true":"false")
            <<",\"srgb_input\":"<<(job.srgb?"true":"false")<<",\"mean_encoded_luma\":"<<sum/(width*height)<<"}\n";
        info.close();
        std::fprintf(stderr,"FRAME_SNAPSHOT tick_ms=%llu gamma=%.6g hdr=%d srgb=%d luma=%.3f saved=%d path=%s\n",
            static_cast<unsigned long long>(job.tick),job.gamma,job.hdr,job.srgb,sum/(width*height),bool(output)&&bool(info),path.string().c_str());
        return bool(output)&&bool(info);
    }
private:
    vk::Device device; VmaAllocator allocator;
    std::mutex mutex; std::condition_variable cv; std::deque<Job> jobs;
    u32 pending{}; Slots slots; std::jthread thread;
};
}
