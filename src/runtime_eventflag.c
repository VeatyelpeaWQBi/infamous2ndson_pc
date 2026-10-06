/* PS4 event flags: 64-bit opaque handles and bit predicates on Win32 conditions.
 * ABI/flags follow shadPS4 kernel/threads/event_flag.cpp. Priority queues use FIFO
 * because this runtime does not implement a guest-priority scheduler. */
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    uint64_t bits,result; uint32_t mode; int done; int32_t error;
    HostCond event; struct Waiter *next;
} Waiter;
typedef struct Event {
    uint64_t id,bits; unsigned active; int multi,deleted;
    Waiter *first; struct Event *next;
} Event;
static HostMutex lock=HOST_MUTEX_INIT;
static Event *registry;
static uint64_t next_id=1;
static Event *find(uint64_t id) { for (Event *e=registry;e;e=e->next) if (e->id==id) return e; return NULL; }
static int valid_mode(uint32_t mode) {
    return (mode&15)==1 || (mode&15)==2 ? !(mode&~UINT32_C(0x33)) && (mode&0x30)!=0x30 : 0;
}
static int met(Event *e,Waiter *w) { return (w->mode&15)==1 ? (e->bits&w->bits)==w->bits : (e->bits&w->bits)!=0; }
static void satisfy(Event *e,Waiter *w) {
    w->result=e->bits; w->done=1;
    if (w->mode&0x10) e->bits=0;
    else if (w->mode&0x20) e->bits&=~w->bits;
}
static ABI int32_t create(uint64_t *out,const char *name,uint32_t attr,uint64_t bits,const void *param) {
    if (!out || !name || param || (attr&~0x33u) || (attr&15)>2 || (attr&0x30)==0x30) return ERR(22);
    if (strlen(name)>=32) return ERR(63);
    Event *e=calloc(1,sizeof(*e)); if (!e) return ERR(12);
    host_lock(&lock);
    if (!next_id) { host_unlock(&lock); free(e); return ERR(28); }
    e->id=next_id++; e->bits=bits; e->multi=(attr&0x20)!=0;
    e->next=registry; registry=e; *out=e->id;
    host_unlock(&lock); return 0;
}
static int32_t wait_bits(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *out,uint32_t *timeout,int block) {
    if (!bits || !valid_mode(mode)) return ERR(22);
    host_lock(&lock); Event *e=find(id);
    if (!e) { host_unlock(&lock); return ERR(3); }
    if (!e->multi && e->active) { host_unlock(&lock); return ERR(1); }
    Waiter w={.bits=bits,.mode=mode};
    if (met(e,&w)) { satisfy(e,&w); if (out) *out=w.result; host_unlock(&lock); return 0; }
    if (!block || (timeout && !*timeout)) {
        if (out) *out=e->bits;
        host_unlock(&lock); return block ? ERR(60) : ERR(16);
    }
    host_cond_init(&w.event);
    Waiter **tail=&e->first; while (*tail) tail=&(*tail)->next; *tail=&w; ++e->active;
    const uint64_t deadline=timeout ? host_monotonic_ns()+(uint64_t)*timeout*1000 : 0;
    while (!w.done) {
        if (!timeout) host_cond_wait(&w.event,&lock);
        else {
            uint64_t now=host_monotonic_ns();
            int result=now<deadline ? host_cond_timedwait(&w.event,&lock,deadline-now) : ETIMEDOUT;
            if (result==ETIMEDOUT && !w.done) {
                Waiter **p=&e->first; while (*p!=&w) p=&(*p)->next; *p=w.next;
                w.error=ERR(60); w.result=e->bits; w.done=1;
            } else if (result && result!=ETIMEDOUT && !w.done) {
                Waiter **p=&e->first; while (*p!=&w) p=&(*p)->next; *p=w.next;
                w.error=ERR(5); w.result=e->bits; w.done=1;
            }
        }
    }
    if (out) *out=w.result;
    if (timeout) { uint64_t now=host_monotonic_ns(); *timeout=w.error || now>=deadline ? 0 : (uint32_t)((deadline-now)/1000); }
    if (!--e->active && e->deleted) free(e);
    host_unlock(&lock); return w.error;
}
static ABI int32_t wait_event(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *out,uint32_t *timeout) {
    return wait_bits(id,bits,mode,out,timeout,1);
}
static ABI int32_t poll(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *out) { return wait_bits(id,bits,mode,out,NULL,0); }
static ABI int32_t set(uint64_t id,uint64_t bits) {
    host_lock(&lock); Event *e=find(id);
    if (!e) { host_unlock(&lock); return ERR(3); }
    e->bits|=bits;
    Waiter **p=&e->first;
    while (*p) {
        Waiter *w=*p;
        if (!met(e,w)) { p=&w->next; continue; }
        satisfy(e,w); *p=w->next; host_cond_signal(&w->event);
    }
    host_unlock(&lock); return 0;
}
static ABI int32_t clear(uint64_t id,uint64_t mask) {
    host_lock(&lock); Event *e=find(id); if (e) e->bits&=mask;
    int32_t result=e ? 0 : ERR(3);
    host_unlock(&lock); return result;
}
static int wake_all(Event *e,int32_t error) {
    int count=0;
    while (e->first) {
        Waiter *w=e->first; e->first=w->next; w->result=e->bits; w->error=error; w->done=1;
        host_cond_signal(&w->event); ++count;
    }
    return count;
}
static ABI int32_t cancel(uint64_t id,uint64_t pattern,int32_t *waiters) {
    host_lock(&lock); Event *e=find(id);
    if (!e) { host_unlock(&lock); return ERR(3); }
    e->bits=pattern; int n=wake_all(e,ERR(85)); if (waiters) *waiters=n;
    host_unlock(&lock); return 0;
}
static ABI int32_t destroy(uint64_t id) {
    host_lock(&lock); Event **p=&registry; while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { host_unlock(&lock); return ERR(3); }
    Event *e=*p; *p=e->next; e->deleted=1; wake_all(e,ERR(13));
    if (!e->active) free(e);
    host_unlock(&lock); return 0;
}
unsigned runtime_eventflag_waiters(uint64_t id) {
    host_lock(&lock); Event *e=find(id); unsigned n=0;
    for (Waiter *w=e ? e->first : NULL;w;w=w->next) ++n;
    host_unlock(&lock); return n;
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",create},{"sceKernelDeleteEventFlag",destroy},
    {"sceKernelSetEventFlag",set},{"sceKernelClearEventFlag",clear},
    {"sceKernelWaitEventFlag",wait_event},{"sceKernelPollEventFlag",poll},
    {"sceKernelCancelEventFlag",cancel},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
