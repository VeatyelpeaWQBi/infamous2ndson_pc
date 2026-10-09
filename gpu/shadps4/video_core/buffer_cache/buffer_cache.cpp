// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_readback_queue.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      page_manager{tracker},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    if(instance.GetReadbackQueue())readback_queue=std::make_unique<Vulkan::ReadbackQueue>(instance,scheduler);
    if(const auto* flag=std::getenv("BB_ASYNC_PROTECT");flag&&flag[0]=='1') {
        upload_worker=std::make_unique<UploadWorker>(tracker);
        scheduler.SetUploadWaiter([this]{upload_worker->Wait();},[this]{return upload_worker->Fence();});
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = ARENA_PAGE_SIZE,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = ARENA_PAGE_SIZE / block_size;
    blocks_per_arena_page_shift = ARENA_PAGE_BITS - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * NUM_ARENA_PAGES) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * NUM_ARENA_PAGES);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() {
    readback_queue.reset();
    if(upload_worker) {
        upload_worker->Wait();scheduler.SetUploadWaiter({});upload_worker.reset();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    BbCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    static const bool async_readback=[] {
        const char* value=std::getenv("BB_ASYNC_READBACK");return value && value[0]=='1';
    }();
    if(async_readback && !is_write && !assume_locks) {
        for(unsigned attempt=0;attempt<3;++attempt) {
            std::shared_ptr<PendingReadback> readback;
            {
                BbStats::Timer timer{BbStats::readback_begin_roundtrip_ns};
                liverpool->SendCommand<true>([&] {readback=BeginReadback(device_addr,size);});
            }
            if(!readback) return;
            // Use the semaphore directly: Scheduler::Wait can flush the active
            // command buffer and must not be called by a guest fault thread.
            {
                BbStats::WaitTimer timer{BbStats::buffer_readback_wait_ns,&BbStats::readback_guest_wait_ns};
                if(readback->separate_queue)readback_queue->SemaphoreState().Wait(readback->tick);
                else scheduler.GetWorkSemaphore()->Wait(readback->tick);
            }
            bool complete=false;
            {
                BbStats::Timer timer{BbStats::readback_complete_roundtrip_ns};
                liverpool->SendCommand<true>([&] {complete=CompleteReadback(readback);});
            }
            if(complete) return;
            BbStats::readback_retries.fetch_add(1,std::memory_order_relaxed);
        }
        // Repeated GPU writes need a serialized snapshot for forward progress.
        // CPU writes and GPU-internal calls always retain the existing path.
    }
    const auto original_ip=BbStats::fault_instruction;
    const auto write_bytes=BbStats::write_data_bytes;
    const auto flush_request = [this, device_addr, size, is_write,assume_locks,original_ip,write_bytes] {
        if(TryPrefetchedReadback(device_addr,size)) {
            if(is_write) memory_tracker->MarkRegionAsCpuModified(device_addr,size);
            return;
        }
        NoteReadbackPage(device_addr);
        static const bool trace=[] {const char* v=std::getenv("BB_READBACK_TRACE");return v && v[0]=='1';}();
        static auto last=std::chrono::steady_clock::time_point{};
        const auto now=std::chrono::steady_clock::now();
        if(trace && now-last>=std::chrono::seconds(1)) {
            last=now;u64 dirty_bytes=0;
            const auto page=Common::AlignDown(device_addr,u64{4096});
            gpu_modified_ranges.ForEachInRange(page,4096,[&](VAddr a,VAddr b){dirty_bytes+=b-a;});
            std::fprintf(stderr,"READBACK_ORIGIN address=%#llx request=%llu write=%d gpu_caller=%d ip=%#llx dirty_page_bytes=%llu write_data_bytes=%llu\n",
                device_addr,size,int(is_write),int(assume_locks),u64(original_ip),dirty_bytes,write_bytes);
            bool found=false;
            for(const auto& slot:readback_slots) if(slot.hint.page==page) {
                const auto& hint=slot.hint;found=true;
                std::fprintf(stderr,"READBACK_MISS page=%#llx tick=%llu epoch=%llu copied_epoch=%llu valid=%d cpu_dirty=%d image_overlap=%d\n",
                    page,hint.tick,hint.epoch,hint.copied_epoch,int(hint.Valid()),
                    int(IsRegionCpuModified(page,4096)),int(texture_cache.HasImageOverlap(page,4096)));
            }
            if(!found) std::fprintf(stderr,"READBACK_MISS page=%#llx pool_full=1\n",page);
        }
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        static const u64 WindowSize = [] {
            const char* value=std::getenv("BB_READBACK_WINDOW_KIB");
            const u64 kib=value ? std::strtoull(value,nullptr,10) : 512;
            return std::bit_ceil(std::clamp<u64>(kib,4,4096))*1024;
        }();
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

std::shared_ptr<BufferCache::PendingReadback> BufferCache::BeginReadback(VAddr address,u64 size) {
    if(!memory_tracker->IsRegionGpuModified(address,size)) return {};
    static const bool async_prefetch=[] {
        const char* value=std::getenv("BB_ASYNC_PREFETCH");return value && value[0]=='1';
    }();
    if(async_prefetch) {
        for(auto& slot:readback_slots) {
            auto& hint=slot.hint;
            if(!hint.Matches(address,size) || !hint.Valid() || !slot.buffer ||
               IsRegionCpuModified(hint.page,4096) || texture_cache.HasImageOverlap(hint.page,4096)) continue;
            const auto* arena=GetArena(hint.page>>block_shift,(hint.page+4095)>>block_shift);
            auto readback=std::make_shared<PendingReadback>();
            readback->arena_base=arena->cpu_addr;readback->request=address;readback->request_size=size;
            readback->version=ReadbackVersion{{{hint.page,hint.page+4096}}};
            // Publish GPU-owned bytes only, never overwrite the rest of a page
            // with arena contents that may not mirror its CPU-owned backing.
            gpu_modified_ranges.ForEachInRange(hint.page,4096,[&](VAddr begin,VAddr end) {
                readback->copies.push_back({begin-arena->cpu_addr,begin-hint.page,end-begin});
            });
            readback->tick=hint.Detach();readback->bytes=4096;
            readback->buffer=std::move(slot.buffer);
            slot.last_use=++readback_use_serial;
            pending_readback_versions.push_back(&readback->version);
            BbStats::readback_prefetch_hits.fetch_add(1,std::memory_order_relaxed);
            // The exact existing GPU copy is now owned by the fault request.
            // The guest waits for its tick; the GPU command thread keeps working.
            return readback;
        }
    }
    if(TryPrefetchedReadback(address,size)) return {};
    NoteReadbackPage(address);
    const auto* arena=GetArena(address>>block_shift,(address+size-1)>>block_shift);
    static const u64 window=[] {
        const char* value=std::getenv("BB_READBACK_WINDOW_KIB");
        return std::bit_ceil(std::clamp<u64>(value ? std::strtoull(value,nullptr,10) : 512,4,4096))*1024;
    }();
    const auto begin=std::max<VAddr>(Common::AlignDown(address,window),arena->cpu_addr);
    const auto end=std::min<VAddr>(std::max<VAddr>(begin+window,address+size),arena->cpu_addr+arena->size_bytes);
    auto readback=std::make_shared<PendingReadback>();readback->arena_base=arena->cpu_addr;
    readback->request=address;readback->request_size=size;
    static const bool coalesce=[] {
        const char* value=std::getenv("BB_READBACK_COALESCE");return value && value[0]=='1';
    }();
    readback->version.windows=coalesce
        ? CoalescedReadbackWindows(begin,end,arena->cpu_addr,arena->cpu_addr+arena->size_bytes,window,readback_slots)
        : std::vector<std::pair<VAddr,VAddr>>{{begin,end}};
    readback->version.Capture();
    for(const auto [start,finish]:readback->version.windows) {
        memory_tracker->ForEachDownloadRange<false>(start,finish-start,[&](VAddr dirty,u64 bytes) {
            gpu_modified_ranges.ForEachInRange(dirty,bytes,[&](VAddr first,VAddr last) {
                readback->copies.push_back({first-arena->cpu_addr,readback->bytes,last-first});
                readback->bytes=Common::AlignUp(readback->bytes+last-first,u64{64});
            });
        });
    }
    if(!readback->bytes) return {};
    // Each pending snapshot owns its host allocation until its exact GPU tick
    // completes. Ring staging may be reused while the guest waits.
    const auto spare=std::find_if(readback_pool.begin(),readback_pool.end(),[&](const auto& buffer) {
        return buffer->size_bytes>=readback->bytes;
    });
    if(spare!=readback_pool.end()) {
        readback_pool_bytes-=(*spare)->size_bytes;
        readback->buffer=std::move(*spare);readback_pool.erase(spare);
    } else readback->buffer=std::make_unique<Buffer>(instance,0,readback->bytes,MemoryType::HostCached);
    pending_readback_versions.push_back(&readback->version);
    u64 writer{};
    if(readback_queue)for(const auto& copy:readback->copies)
        writer=std::max(writer,runtime.LastBufferWriter(arena,copy.srcOffset,copy.size));
    if(readback_queue&&writer<scheduler.CurrentTick()&&pending_binds.empty()) {
        readback->tick=readback_queue->Copy(arena->Handle(),readback->buffer->Handle(),readback->copies,writer);
        readback->separate_queue=true;
        BbStats::readback_queue_copies.fetch_add(1,std::memory_order_relaxed);
    } else {
        if(readback_queue)BbStats::readback_queue_fallbacks.fetch_add(1,std::memory_order_relaxed);
        runtime.CopyBuffer(arena,readback->buffer.get(),readback->copies);
        readback->tick=scheduler.CurrentTick();scheduler.Flush();
    }
    BbStats::buffer_readback_calls.fetch_add(1,std::memory_order_relaxed);
    BbStats::buffer_readback_bytes.fetch_add(readback->bytes,std::memory_order_relaxed);
    return readback;
}

bool BufferCache::CompleteReadback(const std::shared_ptr<PendingReadback>& readback) {
    std::erase(pending_readback_versions,&readback->version);
    const auto recycle=[&] {
        if(readback_pool.size()<8 && readback_pool_bytes+readback->buffer->size_bytes<=64_MB) {
            readback_pool_bytes+=readback->buffer->size_bytes;
            readback_pool.push_back(std::move(readback->buffer));
        }
    };
    readback->buffer->Invalidate(0,readback->bytes);
    for(const auto& copy:readback->copies) {
        const auto address=readback->arena_base+copy.srcOffset;
        readback->version.ForEachCurrentRange(address,copy.size,[&](VAddr begin,VAddr end) {
            memory->TryWriteBacking(std::bit_cast<u8*>(begin),
                readback->buffer->mapped_data.data()+copy.dstOffset+begin-address,end-begin);
        });
    }
    for(const auto [start,finish]:readback->version.windows) {
      readback->version.ForEachCurrentRange(start,finish-start,[&](VAddr begin,VAddr end) {
        gpu_modified_ranges.Subtract(begin,end-begin);
        memory_tracker->UnmarkRegionAsGpuModified(begin,end-begin);
        NoteReadbackWrite(begin,end-begin); // Reject older overlapping snapshots.
      });
    }
    const bool complete=!memory_tracker->IsRegionGpuModified(readback->request,readback->request_size);
    recycle();return complete;
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    static const bool coalesce=[] {
        const char* value=std::getenv("BB_READBACK_COALESCE");return value && value[0]=='1';
    }();
    auto windows=std::vector<std::pair<VAddr,VAddr>>{{device_addr,device_addr+size}};
    if(coalesce) {
        const char* value=std::getenv("BB_READBACK_WINDOW_KIB");
        const u64 window=std::bit_ceil(std::clamp<u64>(value ? std::strtoull(value,nullptr,10) : 512,4,4096))*1024;
        windows=CoalescedReadbackWindows(device_addr,device_addr+size,arena_base,
                                         arena_base+arena->size_bytes,window,readback_slots);
    }
    for(const auto [begin,end]:windows) {
      memory_tracker->ForEachDownloadRange<false>(begin, end-begin, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
      });
    }
    if (total_size_bytes == 0) {
        return;
    }
    for(const auto [begin,end]:windows) NoteReadbackWrite(begin,end-begin);
    BbStats::buffer_readback_calls.fetch_add(1,std::memory_order_relaxed);
    BbStats::buffer_readback_bytes.fetch_add(total_size_bytes,std::memory_order_relaxed);
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    {BbStats::WaitTimer timer{BbStats::buffer_readback_wait_ns,&BbStats::readback_gpu_wait_ns};scheduler.Finish();}

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    for(const auto [begin,end]:windows) memory_tracker->UnmarkRegionAsGpuModified(begin,end-begin);
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size) &&
        !(is_texel_buffer && texture_cache.HasImageOverlap(device_addr,size))) {
        static const bool dedup=[] {const auto* f=std::getenv("BB_STREAM_DEDUP");return f&&f[0]=='1';}();
        StreamMemo* reuse=nullptr;const u64 tick=scheduler.CurrentTick();
        u64 epoch{},host{};
        if(dedup&&size) {
            epoch=CurrentUploadEpoch();host=CurrentHostWriteGen();
            const u64 key=device_addr^(u64{size}<<40);
            reuse=&stream_memos[(key*0x9E3779B97F4A7C15ULL)>>52];
            if(reuse->address==device_addr&&reuse->size==size&&reuse->tick==tick&&
               reuse->epoch==epoch&&reuse->host==host)return {&stream_buffer,reuse->offset};
        }
        static const bool clean_arena=[] {
            const char* value=std::getenv("BB_CLEAN_ARENA_READ");return value && value[0]=='1';
        }();
        // A larger earlier upload may already cover this small readonly range.
        // Reuse only fully resident, CPU-clean raw buffers; texel aliases retain
        // their normal image synchronization path. No content/version shortcut.
        if(clean_arena && size && !is_texel_buffer && !IsRegionCpuModified(device_addr,size)) {
            const auto* arena=address_space[device_addr>>ARENA_PAGE_BITS];
            const u64 end=device_addr+size;
            const u64 first=device_addr>>block_shift,last=(end-1)>>block_shift;
            if(arena && end>=device_addr && end<=arena->cpu_addr+arena->size_bytes &&
               resident_ranges.Contains(first,last+1)) {
                BbStats::clean_arena_read_hits.fetch_add(1,std::memory_order_relaxed);
                return {arena,arena->Offset(device_addr)};
            }
        }
        BbStats::BufferTimer<2> timer{BbStats::buffer_stream_ns};
        if (BbStats::buffer_profiling) BbStats::buffer_stream_calls.fetch_add(1,std::memory_order_relaxed);
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                if(reuse)*reuse={device_addr,size,tick,epoch,host,*offset};
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        if(reuse)*reuse={device_addr,size,tick,epoch,host,offset};
        return {&stream_buffer, offset};
    }
    BbStats::BufferTimer<3> timer{BbStats::buffer_arena_ns};
    if (BbStats::buffer_profiling) BbStats::buffer_arena_calls.fetch_add(1,std::memory_order_relaxed);
    CpuBufferMemo* memo = nullptr;
    std::array<u64, 2> versions{};
    if (!is_written && !is_texel_buffer && RegionManager::CpuVersionsEnabled() &&
        memory_tracker->GetCpuVersions(device_addr, size, versions)) {
        const u64 key = device_addr ^ (u64{size} << 40) ^ 0x5bd1e995ULL;
        memo = &cpu_buffer_memos[(key * 0x9E3779B97F4A7C15ULL) >> 52];
        if (memo->Matches(device_addr, size, versions,
                          address_space[device_addr >> ARENA_PAGE_BITS],
                          address_space[(device_addr + size - 1) >> ARENA_PAGE_BITS])) {
            BbStats::buffer_sync_memo_hits.fetch_add(1, std::memory_order_relaxed);
            return {memo->arena, memo->arena->Offset(device_addr)};
        }
        BbStats::buffer_sync_memo_misses.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        NoteReadbackWrite(device_addr,size);
        gpu_modified_ranges.Add(device_addr, size);
    } else if (memo) {
        // Capture BEFORE synchronization: writes during it invalidate reuse.
        *memo = {.address=device_addr, .size=size, .arena=arena, .versions=versions};
    }
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

bool BufferCache::ReadbackPrefetchEnabled() const {
    static const bool enabled=[] {const char* v=std::getenv("BB_READBACK_PREFETCH");return v && v[0]=='1';}();return enabled;
}
void BufferCache::NoteReadbackPage(VAddr address) {
    if(!ReadbackPrefetchEnabled()) return;
    const VAddr page=Common::AlignDown(address,u64{4096});
    for(auto& slot:readback_slots) if(slot.hint.page==page) {
        slot.last_use=++readback_use_serial;return;
    }
    static const bool adaptive=[] {
        const char* value=std::getenv("BB_READBACK_LRU");return !value || value[0]!='0';
    }();
    if(!adaptive) {
        for(auto& slot:readback_slots) if(!slot.hint.page) {
            slot.hint.page=page;slot.last_use=++readback_use_serial;return;
        }
        return;
    }
    auto* victim=SelectReadbackVictim(readback_slots,[&](u64 tick){return scheduler.IsFree(tick);});
    if(!victim) return; // All storage is still in flight: retain the normal readback path.
    victim->hint.Forget();
    victim->hint.page=page;
    victim->last_use=++readback_use_serial;
}
void BufferCache::NoteReadbackWrite(VAddr address,u64 size) {
    for(auto* version:pending_readback_versions) version->Write(address,size);
    if(!ReadbackPrefetchEnabled()) return;
    for(auto& slot:readback_slots) if(slot.hint.page) slot.hint.Write(address,size);
}
void BufferCache::InvalidateReadbackHints() {
    if(!ReadbackPrefetchEnabled()) return;
    for(auto& slot:readback_slots) if(slot.hint.page) slot.hint.Write(slot.hint.page,4096);
}
void BufferCache::PrefetchReadbacks(bool fence_boundary) {
    if(!ReadbackPrefetchEnabled()) return;
    // Keep demand-page learning and coalesced readbacks active when measuring
    // the cost of speculative copies. BB_READBACK_PREFETCH=0 disables both and
    // cannot isolate the extra GPU submissions made here.
    static const bool speculative=[] {
        const char* value=std::getenv("BB_READBACK_SPECULATIVE");
        return !value || value[0]!='0';
    }();
    if(!speculative) return;
    // Precise mode can expose many counters per emitter. A diagnostic option
    // batches learned snapshots at guest synchronization points instead of
    // repeatedly capturing versions invalidated by subsequent dispatches.
    static const bool at_fences=[] {
        const char* value=std::getenv("BB_READBACK_FENCE_BATCH");return value && value[0]=='1';
    }();
    if(fence_boundary!=at_fences) return;
    static const u64 interval_ns=[] {
        const char* value=std::getenv("BB_READBACK_PREFETCH_INTERVAL_US");
        return std::clamp<u64>(value ? std::strtoull(value,nullptr,10) : 0,0,10000)*1000;
    }();
    const u64 now_ns=interval_ns ? std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
    if(!fence_boundary && !readback_prefetch_gate.Ready(now_ns,interval_ns)) return;
    bool queued=false;
    for(auto& slot:readback_slots) {
        auto& hint=slot.hint;
        if(!hint.page || hint.Valid() || !gpu_modified_ranges.Intersects(hint.page,4096) || IsRegionCpuModified(hint.page,4096)) continue;
        if(texture_cache.HasImageOverlap(hint.page,4096)) continue;
        if(hint.tick && !scheduler.IsFree(hint.tick)) continue; // Never reuse in-flight snapshot storage.
        if(!slot.buffer) slot.buffer=std::make_unique<Buffer>(instance,0,4096,MemoryType::HostCached,"Hot CPU readback");
        const auto first=hint.page>>block_shift,last=(hint.page+4095)>>block_shift;
        const auto* arena=GetArena(first,last);
        const vk::BufferCopy copy{arena->Offset(hint.page),0,4096};
        runtime.CopyBuffer(arena,slot.buffer.get(),std::span{&copy,1});
        hint.Captured(scheduler.CurrentTick());
        queued=true;
        BbStats::readback_prefetches.fetch_add(1,std::memory_order_relaxed);
    }
    if(queued) {
        readback_prefetch_gate.Queued(now_ns);
        scheduler.Flush(); // Signal snapshots before unrelated later rendering can extend their wait.
    }
}
bool BufferCache::TryPrefetchedReadback(VAddr address,u64 size) {
    if(!ReadbackPrefetchEnabled()) return false;
    for(auto& slot:readback_slots) {
        auto& hint=slot.hint;
        if(!hint.Matches(address,size) || !hint.Valid() || !slot.buffer || IsRegionCpuModified(hint.page,4096)) continue;
        if(texture_cache.HasImageOverlap(hint.page,4096)) continue;
        {BbStats::WaitTimer timer{BbStats::buffer_readback_wait_ns,&BbStats::readback_gpu_wait_ns};scheduler.Wait(hint.tick);}
        slot.buffer->Invalidate(0,4096);
        bool copied=true;
        gpu_modified_ranges.ForEachInRange(hint.page,4096,[&](VAddr a,VAddr b) {
            copied &= memory->TryWriteBacking(reinterpret_cast<void*>(a),slot.buffer->mapped_data.data()+a-hint.page,b-a);
        });
        if(!copied) return false;
        gpu_modified_ranges.Subtract(hint.page,4096);
        memory_tracker->UnmarkRegionAsGpuModified(hint.page,4096);
        hint.Consumed();
        slot.last_use=++readback_use_serial;
        BbStats::readback_prefetch_hits.fetch_add(1,std::memory_order_relaxed);
        return true;
    }
    return false;
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (ARENA_PAGE_BITS - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << ARENA_PAGE_BITS, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << ARENA_PAGE_BITS);
    const u64 first_size = first_arena ? first_arena->size_bytes : ARENA_PAGE_SIZE;
    const u64 last_size = last_arena ? last_arena->size_bytes : ARENA_PAGE_SIZE;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> ARENA_PAGE_BITS;
    for (u32 page = 0; page < (first_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> ARENA_PAGE_BITS); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    BbStats::BufferTimer<4> timer{BbStats::buffer_residency_ns};
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    BbStats::Timer allocate_timer{BbStats::t_resident};

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    BbStats::BufferTimer<5> timer{BbStats::buffer_sync_ns};
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    static const bool late=[] {const auto* flag=std::getenv("BB_LATE_PROTECT");return flag&&flag[0]=='1';}();
    bool deferred=(upload_worker || (late&&!is_written)) && page_manager.BeginDeferredProtection();
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] {
            auto protections=deferred ? page_manager.EndDeferredProtection() : PageManager::ProtectionBatch{};
            deferred=false;
            // Close the CPU batch BEFORE staging allocation (which can submit
            // and recursively process faults). No borrowed TLS batch in a worker.
            src_buffer = UploadCopies(arena, copies, total_size_bytes,std::move(protections));
            if(upload_worker && is_written) deferred=page_manager.BeginDeferredProtection();
        });

    if(deferred) {
        auto protections=page_manager.EndDeferredProtection();
        // GPU read protection is queued only after its region locks are released;
        // submissions wait for it even when this binding uploaded no CPU data.
        if(upload_worker&&!protections.empty()) upload_worker->Queue(std::move(protections));
        else page_manager.RefreshDeferredProtection(protections);
    }

    if (src_buffer) {
        for(const auto& copy:copies) NoteReadbackWrite(arena->cpu_addr+copy.dstOffset,copy.size);
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes, PageManager::ProtectionBatch protections) {
    if (copies.empty()) {
        if(upload_worker&&!protections.empty()) upload_worker->Queue(std::move(protections));
        else page_manager.RefreshDeferredProtection(protections);
        return nullptr;
    }
    if(!upload_worker) page_manager.RefreshDeferredProtection(protections);
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    if(upload_worker) {
        struct HostCopy {VAddr source;u64 offset,size;};
        std::vector<HostCopy> work;work.reserve(copies.size());
        for(auto& copy:copies) {
            work.push_back({copy.dstOffset,copy.srcOffset,copy.size});
            copy.srcOffset+=staging.offset;copy.dstOffset-=arena->cpu_addr;
        }
        upload_worker->Queue(std::move(protections),[work=std::move(work),staging,memory=memory] {
            for(const auto& item:work)
                memory->CopySparseMemory(item.source,staging.mapped+item.offset,item.size);
            staging.Flush();
        });
        return staging.buffer;
    }
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    BbStats::BufferTimer<6> timer{BbStats::buffer_alias_ns};
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            NoteReadbackWrite(device_addr,size);
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    NoteReadbackWrite(device_addr,size);
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
