// SPDX-License-Identifier: GPL-2.0-or-later
// AYOUB1080p 153a5aa upload-worker design, adapted to existing Windows backing copies.
#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <functional>
#include "video_core/page_manager.h"
#include "common/thread.h"
#include "bbport_toggles.h"
namespace VideoCore {
class UploadWorker {
    struct Job {PageManager::ProtectionBatch protections;std::function<void()> copy;u64 sequence;};
    PageManager& pages;
    std::mutex mutex;
    std::condition_variable available,finished;
    std::deque<Job> jobs;
    bool stopping{},flush_requested{};
    u64 submitted{},completed{};
    std::thread worker;
    void Run() {
        Common::SetCurrentThreadName("bb:UploadProtect");
        for(;;) {
            std::deque<Job> batch;
            {
                std::unique_lock lock{mutex};
                available.wait(lock,[&]{return stopping||!jobs.empty();});
                if(jobs.empty()&&stopping) return;
                // AYOUB batches small jobs to merge protection calls.
                if(jobs.size()<8&&!stopping)
                    available.wait_for(lock,std::chrono::milliseconds(1),[&]{return stopping||flush_requested||jobs.size()>=8;});
                flush_requested=false;
                batch.swap(jobs);finished.notify_all();
            }
            PageManager::ProtectionBatch runs;
            for(auto& job:batch) runs.insert(runs.end(),job.protections.begin(),job.protections.end());
            std::ranges::sort(runs,{},&PageManager::ProtectionRange::address);
            PageManager::ProtectionBatch merged;
            for(const auto& run:runs) {
                if(!merged.empty() && run.address<=merged.back().address+merged.back().size) {
                    auto& last=merged.back();last.size=std::max(last.address+last.size,run.address+run.size)-last.address;
                } else merged.push_back(run);
            }
            pages.RefreshDeferredProtection(merged);
            BbStats::upload_protect_ranges.fetch_add(merged.size(),std::memory_order_relaxed);
            for(auto& job:batch) if(job.copy) job.copy();
            {
                std::scoped_lock lock{mutex};completed=batch.back().sequence;
            }
            finished.notify_all();
        }
    }
public:
    explicit UploadWorker(PageManager& tracker):pages{tracker},worker{[this]{Run();}} {}
    ~UploadWorker() {
        {std::scoped_lock lock{mutex};stopping=true;}
        available.notify_all();worker.join(); // Drain, do not drop uploads at shutdown.
    }
    void Queue(PageManager::ProtectionBatch ranges,std::function<void()> copy={}) {
        std::unique_lock lock{mutex};
        finished.wait(lock,[&]{return jobs.size()<4096;});
        jobs.push_back({std::move(ranges),std::move(copy),++submitted});
        BbStats::upload_jobs.fetch_add(1,std::memory_order_relaxed);
        lock.unlock();available.notify_one();
    }
    std::function<void()> Fence() {
        std::scoped_lock lock{mutex};const u64 target=submitted;
        return [this,target]{Wait(target);};
    }
    void Wait(u64 target) {
        BbStats::WaitTimer timer{BbStats::upload_worker_wait_ns};
        std::unique_lock lock{mutex};
        flush_requested=true;
        available.notify_one();finished.wait(lock,[&]{return completed>=target;});
    }
    void Wait() {Fence()();}
};
} // namespace VideoCore
