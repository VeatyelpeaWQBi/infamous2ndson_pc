/* Short, explicit profiling of one named thread in a verified game process.
 * Never changes priorities, affinity, registers or memory. Suspend to copy CONTEXT;
 * resume before logging. This is a diagnostic, not a benchmark measurement. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static uint64_t ticks(FILETIME t) { return ((uint64_t)t.dwHighDateTime<<32)|t.dwLowDateTime; }
static int error(const char *operation) {
    fprintf(stderr,"CPU_PROFILE_ERROR operation=%s win32=%lu\n",operation,GetLastError());
    return 1;
}
static uint64_t process_created(HANDLE process) {
    FILETIME c,e,k,u;
    return GetProcessTimes(process,&c,&e,&k,&u) ? ticks(c) : 0;
}
static HANDLE gpu_thread(DWORD pid,DWORD *id) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE) return NULL;
    THREADENTRY32 item={.dwSize=sizeof(item)};
    HANDLE found=NULL;
    for(BOOL more=Thread32First(snapshot,&item);more;more=Thread32Next(snapshot,&item)) {
        if(item.th32OwnerProcessID!=pid) continue;
        HANDLE thread=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,item.th32ThreadID);
        if(!thread) continue;
        PWSTR name=NULL;
        if(SUCCEEDED(GetThreadDescription(thread,&name)) && name &&
           !wcscmp(name,L"shadPS4:GpuCommandProcessor")) {
            *id=item.th32ThreadID;
            if(name) { LocalFree(name);name=NULL; }
            CloseHandle(thread);
            found=OpenThread(THREAD_QUERY_LIMITED_INFORMATION|THREAD_GET_CONTEXT|
                             THREAD_SUSPEND_RESUME,FALSE,*id);
            DWORD code=GetLastError();CloseHandle(snapshot);SetLastError(code);return found;
        }
        if(name) LocalFree(name);
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return found;
}
static int sample(DWORD pid,DWORD duration,const wchar_t *expected,uint64_t created) {
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process) return error("OpenProcess");
    WCHAR actual[32768];DWORD length=32768;
    if(!QueryFullProcessImageNameW(process,0,actual,&length)) {CloseHandle(process);return error("process image");}
    if(!created || _wcsicmp(actual,expected) || process_created(process)!=created) {
        fprintf(stderr,"CPU_PROFILE_ERROR process identity mismatch\n");CloseHandle(process);return 2;
    }
    HANDLE modules=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid);
    if(modules==INVALID_HANDLE_VALUE) {CloseHandle(process);return error("module snapshot");}
    struct Module { char name[1024];uint64_t base;DWORD size; } loaded[160];
    unsigned loaded_count=0;
    MODULEENTRY32W module={.dwSize=sizeof(module)};uint64_t base=0;
    for(BOOL more=Module32FirstW(modules,&module);more;more=Module32NextW(modules,&module)) {
        if(!_wcsicmp(module.szExePath,actual)) base=(uint64_t)(uintptr_t)module.modBaseAddr;
        if(loaded_count<160) {
            struct Module *entry=&loaded[loaded_count++];
            entry->base=(uint64_t)(uintptr_t)module.modBaseAddr;entry->size=module.modBaseSize;
            if(!WideCharToMultiByte(CP_UTF8,0,module.szModule,-1,entry->name,sizeof(entry->name),NULL,NULL))
                strcpy(entry->name,"unknown");
        }
    }
    CloseHandle(modules);
    DWORD tid=0;HANDLE thread=gpu_thread(pid,&tid);
    if(!thread || !base) {if(thread) CloseHandle(thread);CloseHandle(process);return error("named GPU thread");}
    FILETIME c,e,k,u;
    if(!GetThreadTimes(thread,&c,&e,&k,&u)) {CloseHandle(thread);CloseHandle(process);return error("thread time");}
    uint64_t previous=ticks(k)+ticks(u),start=GetTickCount64();unsigned samples=0;
    printf("# pid=%lu tid=%lu image_base=0x%llx created=%llu diagnostic=1\n",pid,tid,
           (unsigned long long)base,(unsigned long long)created);
    for(unsigned i=0;i<loaded_count;++i) printf("# module_base=0x%llx bytes=%lu name=%s\n",
        (unsigned long long)loaded[i].base,loaded[i].size,loaded[i].name);
    puts("tick_ms,cpu_delta_100ns,rip");
    while(GetTickCount64()-start<duration) {
        Sleep(5);
        DWORD exit_code=0;
        if(!GetExitCodeProcess(process,&exit_code) || exit_code!=STILL_ACTIVE) break;
        if(!GetThreadTimes(thread,&c,&e,&k,&u)) break;
        const uint64_t cpu=ticks(k)+ticks(u);
        CONTEXT context={.ContextFlags=CONTEXT_CONTROL};
        if(SuspendThread(thread)==(DWORD)-1) {CloseHandle(thread);CloseHandle(process);return error("SuspendThread");}
        BOOL ok=GetThreadContext(thread,&context);DWORD context_error=GetLastError();
        DWORD resumed=ResumeThread(thread); /* Always restore our suspension, even on failure. */
        if(resumed==(DWORD)-1) {CloseHandle(thread);CloseHandle(process);return error("ResumeThread");}
        if(!ok) {SetLastError(context_error);CloseHandle(thread);CloseHandle(process);return error("GetThreadContext");}
        printf("%llu,%llu,0x%llx\n",(unsigned long long)GetTickCount64(),
               (unsigned long long)(cpu>=previous ? cpu-previous : 0),(unsigned long long)context.Rip);
        previous=cpu;++samples;
    }
    CloseHandle(thread);CloseHandle(process);
    fflush(stdout);
    return samples ? 0 : 3;
}
static volatile LONG stop_worker,worker_ready;
static DWORD WINAPI worker(void *unused) {
    (void)unused;SetThreadDescription(GetCurrentThread(),L"shadPS4:GpuCommandProcessor");
    InterlockedExchange(&worker_ready,1);
    volatile uint64_t value=1;
    while(!InterlockedCompareExchange(&stop_worker,0,0)) value=value*6364136223846793005ULL+1;
    return (DWORD)(value&0);
}
int wmain(int argc,wchar_t **argv) {
    if(argc==2 && !wcscmp(argv[1],L"--self-test")) {
        HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
        if(!thread) return error("self-test thread");
        while(!InterlockedCompareExchange(&worker_ready,0,0)) Sleep(1);
        WCHAR path[32768];GetModuleFileNameW(NULL,path,32768);
        int result=sample(GetCurrentProcessId(),150,path,process_created(GetCurrentProcess()));
        InterlockedExchange(&stop_worker,1);
        if(WaitForSingleObject(thread,2000)!=WAIT_OBJECT_0) result=4;
        CloseHandle(thread);return result;
    }
    if(argc!=5) {fputs("Usage: cpu-profile PID DURATION_MS VERIFIED_EXE CREATED_FILETIME\n",stderr);return 2;}
    unsigned long pid=wcstoul(argv[1],NULL,10),duration=wcstoul(argv[2],NULL,10);
    uint64_t created=wcstoull(argv[4],NULL,10);
    if(!pid || duration<100 || duration>10000) return 2;
    return sample((DWORD)pid,(DWORD)duration,argv[3],created);
}
