/* Bounded local test-session input timeline. No diagnostic files without opt-in. */
#include "runtime.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static INIT_ONCE debug_once=INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION debug_lock;
static char input_path[32768],previous_path[32768];
static FILE *input_file;
static uint64_t input_bytes,last_sample;
static uint32_t last_buttons=UINT32_MAX;
static int enabled,configured;
enum { INPUT_LIMIT=4*1024*1024 };
static BOOL CALLBACK debug_initialize(PINIT_ONCE once,PVOID param,PVOID *context) {
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&debug_lock);
    const char *directory=getenv("BB_DEBUG_DIR");
    if (directory && *directory && strlen(directory)<sizeof(input_path)-32) {
        configured=1;
        snprintf(input_path,sizeof(input_path),"%s/input.txt",directory);
        snprintf(previous_path,sizeof(previous_path),"%s/input.previous.txt",directory);
        input_file=fopen(input_path,"w"); enabled=input_file!=NULL;
        if (!enabled) fprintf(stderr,"DEBUG: input recording unavailable: %s\n",input_path);
    }
    return TRUE;
}
static int debug_ready(void) {
    InitOnceExecuteOnce(&debug_once,debug_initialize,NULL,NULL);
    return configured;
}
static void input_write(const char *line) {
    const size_t length=strlen(line);
    if (input_bytes+length>INPUT_LIMIT) {
        fclose(input_file);
        /* UTF-8 paths: use CRT rename's narrow-path convention consistently
         * with the rest of the runtime. Both files are within this session. */
        remove(previous_path);
        if (rename(input_path,previous_path)!=0) {
            enabled=0; input_file=NULL;
            fprintf(stderr,"DEBUG: input rotation failed; recording stopped\n"); return;
        }
        input_file=fopen(input_path,"w"); input_bytes=0;
        if (!input_file) { enabled=0; return; }
    }
    if (fwrite(line,1,length,input_file)!=length) {
        enabled=0; fclose(input_file); input_file=NULL;
        fprintf(stderr,"DEBUG: input write failed; recording stopped\n"); return;
    }
    input_bytes+=length; fflush(input_file);
}
void runtime_debug_input(uint32_t buttons,uint8_t lx,uint8_t ly,uint8_t rx,uint8_t ry,
                         uint8_t l2,uint8_t r2,uint8_t touches) {
    if (!debug_ready()) return;
    const uint64_t tick=GetTickCount64();
    EnterCriticalSection(&debug_lock);
    /* At most 20 Hz for analog motion; button edges always survive. */
    if (enabled && (buttons!=last_buttons || tick-last_sample>=50)) {
        char line[192];
        snprintf(line,sizeof(line),"%llu %lu %u %u %u %u %u %u %u %u\n",
            (unsigned long long)tick,GetCurrentThreadId(),buttons,lx,ly,rx,ry,l2,r2,touches);
        input_write(line); last_sample=tick; last_buttons=buttons;
    }
    LeaveCriticalSection(&debug_lock);
}
void runtime_debug_mark(void) {
    if (!debug_ready()) {
        fprintf(stderr,"DEBUG: F10 received, but no diagnostic session is configured\n");
        return;
    }
    const uint64_t tick=GetTickCount64();
    EnterCriticalSection(&debug_lock);
    if (enabled) {
        char line[96]; snprintf(line,sizeof(line),"# MARK %llu F10\n",(unsigned long long)tick);
        input_write(line);
    }
    LeaveCriticalSection(&debug_lock);
    fprintf(stderr,"DEBUG_MARK tick_ms=%llu: F10 received on window event thread\n",(unsigned long long)tick);
    /* Keep performance marks out of the expensive frame analyzer and thread
     * suspension path. Explicit debug-control snapshot retains deep capture. */
    const char *deep=getenv("BB_F10_DEEP");
    if (!deep || deep[0]!='1') {
        fprintf(stderr,"DEBUG: F10 performance mark saved tick_ms=%llu; see frames.csv\n",(unsigned long long)tick);
        fflush(stderr);
        return;
    }
    char capture[32768]; const char *directory=getenv("BB_DEBUG_DIR");
    if (directory && strlen(directory)<sizeof(capture)-32) {
        snprintf(capture,sizeof(capture),"%s/capture-next",directory);
        FILE *request=fopen(capture,"w"); if (request) fclose(request);
        snprintf(capture,sizeof(capture),"%s/render-frame-request",directory);
        request=fopen(capture,"w"); if (request) fclose(request);
    }
    wchar_t name[64]; swprintf(name,64,L"Local\\bbport-dump-%lu",GetCurrentProcessId());
    HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);
    if (event) {
        if (SetEvent(event)) fprintf(stderr,"DEBUG: F10 thread snapshot signaled tick_ms=%llu\n",(unsigned long long)tick);
        else fprintf(stderr,"DEBUG: F10 SetEvent failed, Win32 error %lu\n",GetLastError());
        CloseHandle(event);
    }
    else fprintf(stderr,"DEBUG: thread snapshot request failed, Win32 error %lu\n",GetLastError());
    fflush(stderr);
}
void runtime_debug_motion(int active,const float q[4],const float a[3],const float w[3]) {
    if (!debug_ready()) return;
    static uint64_t last_motion;
    static int was_active;
    const uint64_t tick=GetTickCount64();
    EnterCriticalSection(&debug_lock);
    if (enabled && (active!=was_active || (active && tick-last_motion>=50))) {
        char line[384];
        snprintf(line,sizeof(line),"# MOTION %llu %d q=%.6g,%.6g,%.6g,%.6g a=%.6g,%.6g,%.6g w=%.6g,%.6g,%.6g\n",
            (unsigned long long)tick,active,q[0],q[1],q[2],q[3],a[0],a[1],a[2],w[0],w[1],w[2]);
        input_write(line); last_motion=tick; was_active=active;
    }
    LeaveCriticalSection(&debug_lock);
}
