/* Exercise actual guest file operations on a linked overlay and writable saves. */
#include "../src/runtime_file.c"
#include <assert.h>
#include "windows_test.h"
static int32_t guest_errno;
static unsigned char *protected_buffer;
static size_t protected_size;
static unsigned write_faults;
static LONG CALLBACK write_fault_handler(PEXCEPTION_POINTERS exception) {
    if (exception->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters<2 ||
        exception->ExceptionRecord->ExceptionInformation[0]!=1) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t address=exception->ExceptionRecord->ExceptionInformation[1];
    if (address<(uintptr_t)protected_buffer || address-(uintptr_t)protected_buffer>=protected_size)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    if (!VirtualProtect((void *)(address&~(uintptr_t)4095),4096,PAGE_READWRITE,&old))
        return EXCEPTION_CONTINUE_SEARCH;
    ++write_faults; return EXCEPTION_CONTINUE_EXECUTION;
}
static void protected_file_reads(void) {
    int fd=(int)do_open("/app0/asset.dcx",0,0); assert(fd>=3);
    protected_size=2*1024*1024+4096;
    protected_buffer=VirtualAlloc(NULL,protected_size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(protected_buffer); memset(protected_buffer,0xcc,protected_size);
    PVOID handler=AddVectoredExceptionHandler(1,write_fault_handler); assert(handler);
    DWORD old; assert(VirtualProtect(protected_buffer,protected_size,PAGE_READONLY,&old));
    write_faults=0;
    assert(do_read(fd,protected_buffer,protected_size)==6);
    assert(!memcmp(protected_buffer,"modded",6));
    assert(write_faults==1); /* Only the page containing bytes actually returned is written. */
    assert(protected_buffer[6]==0xcc && protected_buffer[protected_size-1]==0xcc);
    assert(do_lseek(fd,2,0)==2);
    assert(VirtualProtect(protected_buffer,protected_size,PAGE_READONLY,&old));
    write_faults=0;
    assert(do_pread(fd,protected_buffer,3,1)==3 && !memcmp(protected_buffer,"odd",3));
    assert(write_faults==1 && do_lseek(fd,0,1)==2);
    assert(do_pread(fd,protected_buffer,16,6)==0);
    assert(do_pread(fd,protected_buffer,3,-1)==-EINVAL);
    assert(do_pread(fd,protected_buffer,0,-1)==-EINVAL);
    assert(do_read(fd,protected_buffer,0)==0);
    assert(RemoveVectoredExceptionHandler(handler));
    assert(VirtualFree(protected_buffer,0,MEM_RELEASE)); protected_buffer=NULL;
    assert(!do_close(fd));
    puts("Protected guest read/pread: correct bytes, partial EOF, untouched pages, position preserved PASS");
}
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    char root[MAX_PATH];
    test_temp_directory(root,sizeof(root));
    char game[512],user[512],source[512],link[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(source,sizeof(source),"%s/mod.dcx",root);
    snprintf(link,sizeof(link),"%s/game/asset.dcx",root);
    assert(!mkdir(game,0755));
    FILE *f=fopen(source,"w"); assert(f); assert(fputs("modded",f)>=0); assert(!fclose(f));
    /* NTFS hard links need neither administrator rights nor Developer Mode. */
    assert(CreateHardLinkA(link,source,NULL));
    runtime_file_configure(game,user);
    for (int i=0;i<3;++i) {
        const char *path=i==0 ? "/app0/asset.dcx" : i==1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd=(int)do_open(path,0,0); assert(fd>=3);
        char content[8]={0}; assert(do_read(fd,content,6)==6 && !strcmp(content,"modded"));
        GuestStat info; assert(!do_stat(path,&info) && info.size==6);
        assert(!do_close(fd));
        assert(do_open(path,2,0)==-EROFS);
        assert(do_open(path,0x400,0)==-EROFS);
        assert(do_truncate(path,0)==-EROFS);
        assert(path_op(path,2,0)==-EROFS);
        assert(do_rename(path,"/data/moved")==-EROFS);
    }
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,"asset.dcx")) { assert(entries[p+6]==8); found=1; }
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/test-save",0x202,0644); assert(save>=3);
    protected_file_reads();
    assert(do_write(save,"save",4)==4 && !do_close(save));
    assert(!path_op("/data/test-save",2,0));
    assert(!unlink(link) && !unlink(source) && !rmdir(game));
    const char *dirs[]={"temp0","download0","data"};
    for (int i=0;i<3;++i) { char p[1024]; snprintf(p,sizeof(p),"%s/%s",user,dirs[i]); assert(!rmdir(p)); }
    assert(!rmdir(user) && !rmdir(root));
    puts("Guest mod files: reads, stat, merged listing, readonly assets and writable saves PASS");
}
