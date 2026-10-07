// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: engine-side SPIR-V/meta/pipeline package. The original PS4 package stays read-only.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <vector>
#include <thread>
#include <condition_variable>
#include <xxhash.h>
#include <windows.h>
#ifdef MemoryBarrier
#undef MemoryBarrier
#endif
#include "common/types.h"

namespace Storage {
class ShaderBundle {
public:
    static constexpr size_t Limit=64*1024*1024;
    using Blobs=std::map<std::string,std::vector<u8>>;
    ShaderBundle(std::filesystem::path path_,u64 source_):path{std::move(path_)},source{source_} {}
    ~ShaderBundle() { Stop(); }
    static bool ValidName(const std::string& name) {
        return !name.empty() && name.size()<=128 && name.find("..") == std::string::npos &&
               name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.")==std::string::npos;
    }
    static std::vector<u8> Encode(u64 source,const Blobs& blobs) {
        if (blobs.size()>32768) return {};
        std::vector<u8> data;
        const auto add=[&](const auto& value) { const auto* p=reinterpret_cast<const u8*>(&value); data.insert(data.end(),p,p+sizeof(value)); };
        data.insert(data.end(),{'B','B','V','K','P','K','0','1'});
        add(source); add(u32(blobs.size())); add(u32(1));
        for (const auto& [name,blob]:blobs) {
            if (!ValidName(name) || blob.size()>Limit || data.size()+16+name.size()+blob.size()>Limit) return {};
            add(u32(name.size())); add(u32(blob.size())); add(XXH3_64bits(blob.data(),blob.size()));
            data.insert(data.end(),name.begin(),name.end()); data.insert(data.end(),blob.begin(),blob.end());
        }
        return data;
    }
    static bool Decode(std::span<const u8> data,u64 source,Blobs& output) {
        if (data.size()<24 || data.size()>Limit || std::memcmp(data.data(),"BBVKPK01",8)) return false;
        size_t at=8;
        const auto read=[&](auto& value) {
            if (sizeof(value)>data.size()-at) return false;
            std::memcpy(&value,data.data()+at,sizeof(value)); at+=sizeof(value); return true;
        };
        u64 stored; u32 count,version;
        if (!read(stored)||!read(count)||!read(version)||stored!=source||version!=1||count>32768) return false;
        Blobs decoded;
        for (u32 i=0;i<count;++i) {
            u32 namesize,size; u64 hash;
            if (!read(namesize)||!read(size)||!read(hash)||!namesize||namesize>128||namesize>data.size()-at) return false;
            std::string name(reinterpret_cast<const char*>(data.data()+at),namesize); at+=namesize;
            if (!ValidName(name) || size>data.size()-at) return false;
            std::vector<u8> blob(data.begin()+at,data.begin()+at+size); at+=size;
            if (XXH3_64bits(blob.data(),blob.size())!=hash || !decoded.emplace(name,std::move(blob)).second) return false;
        }
        if (at!=data.size()) return false;
        output=std::move(decoded); return true;
    }
    bool Load() {
        std::scoped_lock lock{mutex};
        std::error_code ec; const auto size=std::filesystem::file_size(path,ec);
        if (ec || size>Limit) return false;
        std::vector<u8> data(size); std::ifstream f(path,std::ios::binary);
        if (!f.read(reinterpret_cast<char*>(data.data()),size)) return false;
        if (!Decode(data,source,blobs)) return false;
        bytes=24;
        for (const auto& [name,blob]:blobs) bytes+=16+name.size()+blob.size();
        return true;
    }
    bool Get(const std::string& name,std::vector<u8>& data) {
        std::scoped_lock lock{mutex}; const auto it=blobs.find(name);
        if (it==blobs.end()) return false; data=it->second; return true;
    }
    bool Put(const std::string& name,std::span<const u8> data) {
        std::scoped_lock lock{mutex};
        if (!ValidName(name) || data.size()>Limit) return false;
        auto it=blobs.find(name);
        const size_t old=it==blobs.end() ? 0 : 16+name.size()+it->second.size();
        const size_t next=bytes-old+16+name.size()+data.size();
        if (next>Limit || (it==blobs.end() && blobs.size()>=32768)) return false;
        auto& value=blobs[name];
        if (old && value.size()==data.size() && std::equal(value.begin(),value.end(),data.begin())) return true;
        value.assign(data.begin(),data.end()); ++revision;
        bytes=next;
        return true;
    }
    void Clear() { std::scoped_lock lock{mutex}; blobs.clear(); bytes=24; ++revision; }
    Blobs Snapshot() { std::scoped_lock lock{mutex}; return blobs; }
    bool Save() {
        std::scoped_lock saving{save_mutex};
        Blobs copy; u64 captured;
        { std::scoped_lock lock{mutex}; captured=revision; if(captured==saved) return true; copy=blobs; }
        auto data=Encode(source,copy); if(data.empty()) return false;
        auto pending=path; pending += ".pending";
        { std::ofstream f(pending,std::ios::binary|std::ios::trunc);
          f.write(reinterpret_cast<const char*>(data.data()),data.size()); f.close(); if(!f) return false; }
        if(!MoveFileExW(pending.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
        { std::scoped_lock lock{mutex}; saved=captured; }
        return true;
    }
    void Start() {
        if(thread.joinable()) return;
        thread=std::jthread([this](std::stop_token stop) {
            SetThreadDescription(GetCurrentThread(),L"bb:ShaderBundle");
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            std::unique_lock lk{wait_mutex};
            while(!stop.stop_requested()) {
                cv.wait_for(lk,stop,std::chrono::seconds(5),[] { return false; });
                if(!stop.stop_requested()) { lk.unlock(); Save(); lk.lock(); }
            }
        });
    }
    void Stop() {
        if(thread.joinable()) { thread.request_stop(); cv.notify_all(); thread.join(); }
        Save();
    }
private:
    std::filesystem::path path;
    u64 source,revision{},saved{};
    size_t bytes{24};
    Blobs blobs;
    std::mutex mutex,wait_mutex,save_mutex;
    std::condition_variable_any cv;
    std::jthread thread;
};
} // namespace Storage
