// SPDX-License-Identifier: GPL-2.0-or-later
// AYOUB1080p 153a5aa second-queue copies; same family, bidirectional timeline ordering.
#pragma once
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
namespace Vulkan {
class ReadbackQueue {
    const Instance& instance;Scheduler& graphics;
    Semaphore completion;CommandPool commands;
    u64 last{};
public:
    ReadbackQueue(const Instance& device,Scheduler& scheduler):instance{device},graphics{scheduler},
        completion{device},commands{device,&completion} {}
    ~ReadbackQueue() {
        if(last)completion.Wait(last);
        // Graphics submissions can still reference this semaphore even after the copy finished.
        graphics.Finish();graphics.SetReadbackDependency({},0);
    }
    Semaphore& SemaphoreState() {return completion;}
    u64 Copy(vk::Buffer source,vk::Buffer destination,std::span<const vk::BufferCopy> copies,u64 producer) {
        const auto cmd=commands.Commit();
        Check(cmd.begin(vk::CommandBufferBeginInfo{.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
        cmd.copyBuffer(source,destination,vk::ArrayProxy<const vk::BufferCopy>{u32(copies.size()),copies.data()});
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&host});
        Check(cmd.end());
        const auto wait=graphics.GetWorkSemaphore()->Handle(),signal=completion.Handle();
        const u64 tick=completion.NextTick();const vk::PipelineStageFlags stage=vk::PipelineStageFlagBits::eAllCommands;
        const vk::TimelineSemaphoreSubmitInfo timeline{.waitSemaphoreValueCount=producer?1u:0u,
            .pWaitSemaphoreValues=&producer,.signalSemaphoreValueCount=1,.pSignalSemaphoreValues=&tick};
        const vk::SubmitInfo submit{.pNext=&timeline,.waitSemaphoreCount=producer?1u:0u,
            .pWaitSemaphores=&wait,.pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&cmd,
            .signalSemaphoreCount=1,.pSignalSemaphores=&signal};
        Check(instance.GetReadbackQueue().submit(submit,{}));
        last=tick;graphics.SetReadbackDependency(signal,tick);completion.Refresh();return tick;
    }
};
} // namespace Vulkan
