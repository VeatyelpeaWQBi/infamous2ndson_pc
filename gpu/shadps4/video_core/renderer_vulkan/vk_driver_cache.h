// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a bounded, device/driver-validated Vulkan compiler cache, saved off the command thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>
#include <xxhash.h>
#include <windows.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {
class DriverPipelineCache {
public:
    static constexpr size_t Limit=32*1024*1024;
    static bool Compatible(std::span<const u8> bytes,u32 vendor,u32 device,u32 driver,
                           const std::array<u8,VK_UUID_SIZE>& uuid) {
        if (bytes.size()<56 || bytes.size()>Limit || std::memcmp(bytes.data(),"BBVKPC01",8)) return false;
        const auto word=[&](size_t offset) { u32 value; std::memcpy(&value,bytes.data()+offset,4); return value; };
        // Our prefix adds driver version; Vulkan's standard header supplies vendor/device/UUID.
        u64 checksum; std::memcpy(&checksum,bytes.data()+16,8);
        return word(8)==driver && word(12)==2 && word(24)>=32 && word(24)<=bytes.size()-24 &&
               word(28)==VK_PIPELINE_CACHE_HEADER_VERSION_ONE && word(32)==vendor && word(36)==device &&
               std::memcmp(bytes.data()+40,uuid.data(),VK_UUID_SIZE)==0 &&
               XXH3_64bits(bytes.data()+24,bytes.size()-24)==checksum;
    }
    DriverPipelineCache(const Instance& instance_,std::filesystem::path path_)
        : instance{instance_},path{std::move(path_)} {
        std::vector<u8> initial;
        std::error_code ec;
        const auto size=path.empty() ? 0 : std::filesystem::file_size(path,ec);
        if (!ec && size>=56 && size<=Limit) {
            initial.resize(size);
            std::ifstream input(path,std::ios::binary);
            if (!input.read(reinterpret_cast<char*>(initial.data()),size) ||
                !Compatible(initial,instance.GetVendorID(),instance.GetDeviceID(),instance.GetDriverVersion(),
                            instance.GetPipelineCacheUUID())) initial.clear();
        }
        vk::PipelineCacheCreateInfo info{};
        if (!initial.empty()) { info.initialDataSize=initial.size()-24; info.pInitialData=initial.data()+24; }
        auto [result,created]=instance.GetDevice().createPipelineCacheUnique(info);
        if (result!=vk::Result::eSuccess && !initial.empty()) {
            auto [fallback_result,fallback_cache]=instance.GetDevice().createPipelineCacheUnique({});
            result=fallback_result; created=std::move(fallback_cache); initial.clear();
        }
        ASSERT_MSG(result==vk::Result::eSuccess,"Failed to create Vulkan driver cache");
        cache=std::move(created);
        loaded_bytes=initial.size();
    }
    ~DriverPipelineCache() {
        if (writer.joinable()) { writer.request_stop(); cv.notify_all(); writer.join(); }
        Save();
    }
    vk::PipelineCache Handle() const { return *cache; }
    std::unique_lock<std::mutex> Lock() { return std::unique_lock{mutex}; }
    void Dirty() { revision.fetch_add(1,std::memory_order_relaxed); }
    size_t LoadedBytes() const { return loaded_bytes; }
    void Start() {
        if (path.empty() || writer.joinable()) return;
        writer=std::jthread([this](std::stop_token stop) {
            Common::SetCurrentThreadName("bb:DriverCache");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            std::unique_lock lk{wait_mutex};
            while (!stop.stop_requested()) {
                cv.wait_for(lk,stop,std::chrono::seconds(5),[] { return false; });
                if (!stop.stop_requested()) { lk.unlock(); Save(); lk.lock(); }
            }
        });
    }
    bool Save() {
        std::scoped_lock saving{save_mutex};
        const auto current=revision.load(std::memory_order_acquire);
        if (path.empty() || current==saved_revision) return true;
        std::vector<u8> data;
        {
            auto guard=Lock(); size_t size=0;
            auto result=instance.GetDevice().getPipelineCacheData(*cache,&size,nullptr);
            if (result!=vk::Result::eSuccess || size<32 || size>Limit-24) return false;
            data.resize(size+24); std::memcpy(data.data(),"BBVKPC01",8);
            const u32 driver=instance.GetDriverVersion(),schema=2;
            std::memcpy(data.data()+8,&driver,4); std::memcpy(data.data()+12,&schema,4);
            result=instance.GetDevice().getPipelineCacheData(*cache,&size,data.data()+24);
            if (result!=vk::Result::eSuccess) return false;
            data.resize(size+24);
            const u64 hash=XXH3_64bits(data.data()+24,size); std::memcpy(data.data()+16,&hash,8);
        }
        std::error_code ec; std::filesystem::create_directories(path.parent_path(),ec);
        if (ec) return false;
        auto pending=path; pending += ".pending";
        {
            std::ofstream output(pending,std::ios::binary|std::ios::trunc);
            output.write(reinterpret_cast<const char*>(data.data()),data.size()); output.close();
            if (!output) return false;
        }
        if (!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
        saved_revision=current;
        return true;
    }
private:
    const Instance& instance;
    std::filesystem::path path;
    vk::UniquePipelineCache cache;
    std::mutex mutex,wait_mutex,save_mutex;
    std::condition_variable_any cv;
    std::atomic<u64> revision{};
    u64 saved_revision{};
    size_t loaded_bytes{};
    std::jthread writer;
};
} // namespace Vulkan
