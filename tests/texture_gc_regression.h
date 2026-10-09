// Regression exercises the production collector, not a copy of its budget logic.
#pragma once
#include "common/lru_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/texture_cache/texture_cache.h"
static void lru_gc_cpu() {
    Common::LeastRecentlyUsedCache<unsigned,u64> cache;
    for(unsigned i=0;i<64;++i) cache.Insert(i,1);
    unsigned visits=0,evicted=0;
    cache.ForEachItemBelow(2,[&](unsigned image) {
        ++visits;
        if(image<32) return false; // Old retained entries precede evictable ones.
        return ++evicted==3;
    });
    assert(visits==35 && evicted==3); // bool callbacks really stop at their limit.
    visits=0;cache.ForEachItemBelow(2,[&](unsigned){++visits;});assert(visits==64);
    visits=0;cache.ForEachItemBelow(0,[&](unsigned){++visits;});assert(!visits);
    std::puts("LRU bool early stop / void walk / age cutoff PASS");
}
namespace VideoCore {
struct TextureCacheTestAccess {
    static void Run() {
        Vulkan::Instance instance(0,false);Vulkan::Scheduler scheduler(instance);
        Vulkan::Runtime runtime(instance,scheduler);
        struct Fixture {
            PageManager tracker{nullptr};BufferCache buffers;TextureCache textures;
            Fixture(const Vulkan::Instance& i,Vulkan::Scheduler& s,Vulkan::Runtime& r)
                : buffers(i,s,r,nullptr,textures,tracker),textures(i,s,r,nullptr,buffers,tracker) {}
        } fixture(instance,scheduler,runtime);
        auto& cache=fixture.textures;
        // Metadata-only images: undefined host format allocates no image and
        // no guest page is touched or tracked. Use the real registry and LRU.
        for(unsigned i=0;i<34;++i) {
            ImageInfo info;info.guest_address=0x1000000000ull+i*4096;info.guest_size=4096;
            info.tile_mode=static_cast<AmdGpu::TileMode>(1);
            const auto id=cache.slot_images.insert(instance,runtime,cache.slot_image_views,info);
            if(i<32) cache.slot_images[id].flags=ImageFlagBits::GpuModified;
            cache.RegisterImage(id);
        }
        cache.gc_tick=200;cache.trigger_gc_memory=0;cache.pressure_gc_memory=0;
        cache.critical_gc_memory=~u64{0};
        // The production pressure report resets its interval eviction counters.
        // Keep that report outside this assertion; do not depend on machine uptime.
        cache.gc_report_time=std::chrono::steady_clock::now();
        cache.GarbageCollectImages();
        assert(cache.gc_evictions==2 && cache.gc_kept==32);
        scheduler.Finish();
        for(unsigned i=0;i<64;++i) scheduler.PopPendingOperations();
        std::puts("Production texture GC: 32 retained tiled images do not block two evictions PASS");
    }
};
}
