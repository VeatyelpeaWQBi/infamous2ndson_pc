/* Offline, fully installed PlayGo with actual package chunk sizes. ABI and
 * chunk metadata layout follow shadPS4 playgo_types.h / playgo_chunk.h (GPLv2+).
 * No downloads or writes to /app0; malformed metadata cannot become "installed". */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PG(n) ((int32_t)(UINT32_C(0x80B20000)|(n)))
typedef struct { const void *buffer; uint32_t size,reserved; } Init;
typedef struct { uint16_t chunk; int8_t locus,reserved; } Todo;
typedef struct { uint64_t completed,total; } Progress;
static HostMutex lock=HOST_MUTEX_INIT;
static uint64_t chunk_sizes[1000], language_mask;
static uint32_t chunk_count, handle, next_handle=1;
static int initialized, speed=1;
static uint16_t u16(const unsigned char *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t u32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t u64(const unsigned char *p) { uint64_t v; memcpy(&v,p,8); return v; }
static int region(uint32_t offset,uint32_t length,uint64_t size) { return offset<=size && length<=size-offset; }
static int parse_chunks(const unsigned char *data,uint64_t size) {
    if (size<256 || u32(data)!=0x6f676c70 || u32(data+16)!=size) return 0;
    uint32_t count=u16(data+10), mcount=u16(data+12);
    uint32_t attrs=u32(data+192), attr_size=u32(data+196);
    uint32_t ids=u32(data+200), id_size=u32(data+204);
    uint32_t mattrs=u32(data+216), mattr_size=u32(data+220);
    if (!count || count>1000 || mcount>8000 || attr_size<count*32 || mattr_size<mcount*16 ||
        !region(attrs,attr_size,size) || !region(ids,id_size,size) || !region(mattrs,mattr_size,size)) return 0;
    uint64_t values[1000]={0};
    for (uint32_t i=0;i<count;++i) {
        const unsigned char *attr=data+attrs+i*32;
        uint32_t n=u16(attr+14), offset=u32(attr+24);
        if (!region(offset,n*2,id_size)) return 0;
        for (uint32_t j=0;j<n;++j) {
            uint32_t id=u16(data+ids+offset+j*2);
            if (id>=mcount) return 0;
            uint64_t bytes=u64(data+mattrs+id*16+8)&UINT64_C(0xffffffffffff);
            if (values[i]>UINT64_MAX-bytes) return 0;
            values[i]+=bytes;
        }
    }
    memcpy(chunk_sizes,values,sizeof(values)); chunk_count=count;
    return 1;
}
static ABI int32_t initialize(const Init *param) {
    if (!param || !param->buffer) return PG(10);
    if (param->size<0x200000) return PG(11);
    if (param->reserved) return PG(4);
    host_lock(&lock);
    if (initialized) { host_unlock(&lock); return PG(6); }
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    int valid=0;
    if (fd>=0) {
        int64_t size=runtime_file_lseek(fd,0,2);
        if (size>=256 && size<=16*1024*1024 && runtime_file_lseek(fd,0,0)==0) {
            unsigned char *data=malloc((size_t)size);
            if (data) {
                if (runtime_file_read(fd,data,size)==size) valid=parse_chunks(data,size);
                free(data);
            }
        }
        runtime_file_close(fd);
        if (!valid) { host_unlock(&lock); return PG(2); }
    } else chunk_count=0;
    const char *env=getenv("BB_LANGUAGE"); int lang=env ? atoi(env) : 1;
    language_mask=lang>=0 && lang<48 ? UINT64_C(1)<<(63-lang) : 0;
    initialized=1; speed=1;
    printf("Runtime: PlayGo %u chunks from package metadata, fully installed offline\n",chunk_count);
    host_unlock(&lock); return 0;
}
static int32_t check(uint32_t id) { return !initialized ? PG(5) : !handle || id!=handle ? PG(9) : 0; }
static ABI int32_t open_package(uint32_t *out,const void *param) {
    if (!out) return PG(10);
    if (param) return PG(4);
    host_lock(&lock);
    int32_t r=!initialized ? PG(5) : !chunk_count ? PG(14) : handle ? PG(7) : 0;
    if (!r) { handle=next_handle++; if (!next_handle) ++next_handle; *out=handle; }
    host_unlock(&lock); return r;
}
static ABI int32_t close_package(uint32_t id) {
    host_lock(&lock); int32_t r=check(id); if (!r) handle=0; host_unlock(&lock); return r;
}
static ABI int32_t terminate(void) {
    host_lock(&lock); int32_t r=initialized ? 0 : PG(5);
    initialized=0; handle=0; chunk_count=0; host_unlock(&lock); return r;
}
static int32_t chunks_valid(const uint16_t *ids,uint32_t count) {
    if (!ids) return PG(10);
    if (!count) return PG(11);
    for (uint32_t i=0;i<count;++i) if (ids[i]>=chunk_count) return PG(12);
    return 0;
}
static ABI int32_t chunk_ids(uint32_t id,uint16_t *ids,uint32_t count,uint32_t *out) {
    host_lock(&lock); int32_t r=check(id);
    if (!r && !out) r=PG(10);
    if (!r && ids && !count) r=PG(11);
    if (!r) {
        uint32_t n=ids && count<chunk_count ? count : chunk_count;
        if (ids) for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
        *out=n;
    }
    host_unlock(&lock); return r;
}
static ABI int32_t locus(uint32_t id,const uint16_t *ids,uint32_t count,int8_t *out) {
    host_lock(&lock); int32_t r=check(id);
    if (!r) r=out ? chunks_valid(ids,count) : PG(10);
    if (!r) memset(out,3,count);
    host_unlock(&lock); return r;
}
static ABI int32_t progress(uint32_t id,const uint16_t *ids,uint32_t count,Progress *out) {
    host_lock(&lock); int32_t r=check(id);
    if (!r) r=out ? chunks_valid(ids,count) : PG(10);
    uint64_t total=0;
    if (!r) for (uint32_t i=0;i<count;++i) {
        if (total>UINT64_MAX-chunk_sizes[ids[i]]) { r=PG(4); break; }
        total+=chunk_sizes[ids[i]];
    }
    if (!r) *out=(Progress){total,total};
    host_unlock(&lock); return r;
}
static ABI int32_t eta(uint32_t id,const uint16_t *ids,uint32_t count,int64_t *out) {
    host_lock(&lock); int32_t r=check(id);
    if (!r) r=out ? chunks_valid(ids,count) : PG(10);
    if (!r) *out=0;
    host_unlock(&lock); return r;
}
static ABI int32_t todo(uint32_t id,Todo *list,uint32_t count,uint32_t *out) {
    host_lock(&lock); int32_t r=check(id);
    if (!r && (!list || !out)) r=PG(10);
    if (!r && !count) r=PG(11);
    if (!r) *out=0; // every chunk is already local; don't touch unused slots
    host_unlock(&lock); return r;
}
static ABI int32_t set_todo(uint32_t id,const Todo *list,uint32_t count) {
    host_lock(&lock); int32_t r=check(id);
    if (!r && !list) r=PG(10);
    if (!r && !count) r=PG(11);
    if (!r) for (uint32_t i=0;i<count;++i) {
        if (list[i].chunk>=chunk_count) { r=PG(12); break; }
        if (list[i].locus!=0 && list[i].locus!=2 && list[i].locus!=3) { r=PG(16); break; }
        if (list[i].reserved) { r=PG(4); break; }
    }
    host_unlock(&lock); return r;
}
static ABI int32_t set_language(uint32_t id,uint64_t mask) {
    host_lock(&lock); int32_t r=check(id); if (!r) language_mask=mask; host_unlock(&lock); return r;
}
static ABI int32_t get_language(uint32_t id,uint64_t *mask) {
    host_lock(&lock); int32_t r=check(id); if (!r) { if (mask) *mask=language_mask; else r=PG(10); }
    host_unlock(&lock); return r;
}
static ABI int32_t set_speed(uint32_t id,int32_t value) {
    host_lock(&lock); int32_t r=check(id); if (!r) { if (value<0 || value>2) r=PG(13); else speed=value; }
    host_unlock(&lock); return r;
}
static const RuntimeExport exports[]={
    {"scePlayGoInitialize",initialize},{"scePlayGoOpen",open_package},{"scePlayGoClose",close_package},
    {"scePlayGoTerminate",terminate},{"scePlayGoGetChunkId",chunk_ids},{"scePlayGoGetLocus",locus},
    {"scePlayGoGetProgress",progress},{"scePlayGoGetEta",eta},{"scePlayGoGetToDoList",todo},
    {"scePlayGoSetToDoList",set_todo},{"scePlayGoSetLanguageMask",set_language},
    {"scePlayGoGetLanguageMask",get_language},{"scePlayGoSetInstallSpeed",set_speed},
};
uintptr_t runtime_playgo_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
