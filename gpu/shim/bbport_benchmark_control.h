// Only opt-in benchmark children accept these commands. No process enumeration or UI automation.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sstream>
#include <charconv>
#include "common/types.h"
#include <windows.h>
namespace BbBenchmark {
struct Command { u64 id{}; std::string action; };
inline bool Parse(const std::string& text,Command& command) {
    std::istringstream input(text); std::string extra,id;
    if(!(input>>id>>command.action) || (input>>extra)) return false;
    const auto parsed=std::from_chars(id.data(),id.data()+id.size(),command.id);
    if(parsed.ec!=std::errc{} || parsed.ptr!=id.data()+id.size() || !command.id) return false;
    return command.action=="record-start" || command.action=="record-stop" || command.action=="snapshot" || command.action=="quit";
}
class Control {
    std::string path; u64 last_id{},last_poll{};
public:
    Control() { if(const char* value=std::getenv("BB_BENCH_CONTROL")) path=value; }
    template<class Record,class Snapshot> bool Poll(Record record,Snapshot snapshot) {
        const auto tick=GetTickCount64();
        if(path.empty() || tick-last_poll<20) return false;
        last_poll=tick;
        // Atomic command replacement must remain possible while Windows reads the
        // previous file. The CRT ifstream does not share FILE_SHARE_DELETE.
        const HANDLE input=CreateFileA(path.c_str(),GENERIC_READ,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,nullptr);
        if(input==INVALID_HANDLE_VALUE) return false;
        char bytes[128];DWORD count{};
        const bool read=ReadFile(input,bytes,sizeof(bytes),&count,nullptr);
        CloseHandle(input);
        if(!read || count==sizeof(bytes)) return false;
        const std::string text(bytes,count);Command command;
        if(!Parse(text,command) || command.id<=last_id) return false;
        last_id=command.id;
        int state=-1;
        if(command.action=="record-start") state=record(true);
        if(command.action=="record-stop") state=record(false);
        if(command.action=="snapshot") snapshot();
        std::ofstream output(path+".ack.next");
        output<<"{\"id\":"<<command.id<<",\"action\":\""<<command.action<<"\",\"tick_ms\":"<<GetTickCount64()<<",\"recording\":"<<state<<"}\n";
        output.close();
        MoveFileExA((path+".ack.next").c_str(),(path+".ack").c_str(),MOVEFILE_REPLACE_EXISTING);
        std::fprintf(stderr,"BENCH_CONTROL id=%llu action=%s tick_ms=%llu\n",command.id,command.action.c_str(),GetTickCount64());
        return command.action=="quit";
    }
};
}
