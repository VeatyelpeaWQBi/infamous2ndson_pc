/* Windows output mailbox. A slow device driver must not run on the guest thread. */
#ifndef BB_PAD_FEEDBACK_H
#define BB_PAD_FEEDBACK_H
#include "host_sync.h"

typedef void (*PadFeedbackSend)(void *, uint32_t, uint16_t, uint16_t);
typedef struct {
    HostMutex mutex;
    HANDLE wake, stop;
    HostThread thread;
    PadFeedbackSend send;
    void *context;
    HostMutex *input_gate;
    uint32_t device;
    uint16_t low, high;
    uint64_t serial, completed, deadline, last_output;
    volatile LONG busy;
    int started;
} PadFeedback;

static void *pad_feedback_worker(void *argument) {
    PadFeedback *f=argument;
    SetThreadDescription(GetCurrentThread(),L"bb:PadFeedback");
    HANDLE events[]={f->stop,f->wake};
    uint32_t active_device=0;
    for (;;) {
        const DWORD result=WaitForMultipleObjects(2,events,FALSE,20);
        if (result==WAIT_OBJECT_0 || result==WAIT_FAILED) break;
        host_lock(&f->mutex);
        uint64_t serial=f->serial;
        const int pending=serial!=f->completed;
        const int expired=f->deadline && GetTickCount64()>=f->deadline;
        if (!pending && !expired) { host_unlock(&f->mutex); continue; }
        const uint32_t device=pending ? f->device : active_device;
        const uint16_t low=pending ? f->low : 0, high=pending ? f->high : 0;
        if (!pending && expired) {
            f->low=f->high=0;
            serial=++f->serial; // A later repeated nonzero request can restart vibration.
        }
        f->deadline=0;
        InterlockedExchange(&f->busy,1);
        host_unlock(&f->mutex);
        /* Drain an input read that started before busy became visible; device IO
         * then runs without either mutex, including automatic timeout stops. */
        if (f->input_gate) { host_lock(f->input_gate); host_unlock(f->input_gate); }
        if (active_device && active_device!=device) f->send(f->context,active_device,0,0);
        if (device) f->send(f->context,device,low,high);
        active_device=device;
        host_lock(&f->mutex);
        f->completed=serial;
        f->last_output=GetTickCount64();
        if (f->completed==f->serial) {
            if (low || high) f->deadline=GetTickCount64()+1000;
        }
        InterlockedExchange(&f->busy,0);
        host_unlock(&f->mutex);
    }
    if (active_device) f->send(f->context,active_device,0,0);
    InterlockedExchange(&f->busy,0);
    return NULL;
}
static int pad_feedback_start(PadFeedback *f,PadFeedbackSend send,void *context) {
    if (f->started) return 1;
    host_mutex_init(&f->mutex); f->send=send; f->context=context;
    f->wake=CreateEventW(NULL,FALSE,FALSE,NULL); f->stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!f->wake || !f->stop || host_thread_start(&f->thread,0,pad_feedback_worker,f)) {
        if (f->wake) CloseHandle(f->wake);
        if (f->stop) CloseHandle(f->stop);
        f->wake=f->stop=NULL; return 0;
    }
    f->started=1; return 1;
}
static void pad_feedback_submit(PadFeedback *f,uint32_t device,uint16_t low,uint16_t high) {
    if (!f->started) return;
    host_lock(&f->mutex);
    if (f->serial && f->device==device && f->low==low && f->high==high &&
        (f->serial!=f->completed || (!low && !high) || GetTickCount64()-f->last_output<500)) {
        host_unlock(&f->mutex); return;
    }
    f->device=device; f->low=low; f->high=high; ++f->serial;
    /* A queued request does not hold SDL's lock. Only the worker's actual IO
     * may suppress host sampling, otherwise a vibration-before-read game loop
     * returns the same cached neutral input forever. */
    host_unlock(&f->mutex);
    SetEvent(f->wake);
}
static int pad_feedback_busy(PadFeedback *f) { return InterlockedCompareExchange(&f->busy,0,0)!=0; }
/* Called during host shutdown, never while holding the input lock. */
static void pad_feedback_stop(PadFeedback *f) {
    if (!f->started) return;
    SetEvent(f->stop); host_thread_join(f->thread);
    CloseHandle(f->wake); CloseHandle(f->stop); f->started=0;
}
#endif
