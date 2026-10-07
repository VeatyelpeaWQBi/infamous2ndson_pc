/* Second Son's native libc initialization support.
 * ABI reference: shadPS4 core/libraries/libc_internal/libc_internal_memory.cpp
 * https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/libc_internal/libc_internal_memory.cpp
 * Audited libc.prx caller +0x5b8e1 reads offsets 12, 16 and 24 after the call.
 * No tracing producer exists in this runtime: the guest owns the shared tables.
 */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint64_t size;
    uint32_t flag, get_segment_info;
    uint64_t *mspace_atomic_id_mask, *mstate_table;
} HeapTraceInfo;
_Static_assert(sizeof(HeapTraceInfo)==32, "PS4 heap trace ABI size");
_Static_assert(offsetof(HeapTraceInfo,get_segment_info)==12, "PS4 heap trace flag offset");
_Static_assert(offsetof(HeapTraceInfo,mspace_atomic_id_mask)==16, "PS4 heap trace mask offset");
_Static_assert(offsetof(HeapTraceInfo,mstate_table)==24, "PS4 heap trace table offset");

/* Aligned, writable, process-lifetime backing. Never clear on a repeated query:
 * native libc retains these addresses and may update their contents itself. */
static _Alignas(8) uint64_t heap_trace_mask;
static _Alignas(8) uint64_t heap_trace_states[64];

static ABI void heap_get_trace_info(HeapTraceInfo *info) {
    if (!info || info->size<sizeof(*info)) {
        fputs("STOP: invalid sceLibcHeapGetTraceInfo structure\n",stderr);
        exit(21);
    }
    info->get_segment_info=0;
    info->mspace_atomic_id_mask=&heap_trace_mask;
    info->mstate_table=heap_trace_states;
}

uintptr_t runtime_libc_internal_resolve(const char *name) {
    static const RuntimeExport exports[]={
        {"sceLibcHeapGetTraceInfo",(GuestCallback)heap_get_trace_info},
    };
    return runtime_lookup(exports,sizeof(exports)/sizeof(*exports),name);
}
