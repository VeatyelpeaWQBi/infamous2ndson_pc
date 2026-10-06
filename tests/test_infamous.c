/* Runtime adaptation contracts without executing any game instructions. */
#include "windows_test.h"
#include "../src/runtime.h"
#include "../src/runtime_identity.h"
#include <stdio.h>
#include <stdatomic.h>
#define GET(type,name) ((type)runtime_resolve(name,0))
#define K(n) n "#libkernel:1#libkernel:257#F"
#define P(n) n "#libScePlayGo:1#libScePlayGo:256#F"
void runtime_restart(void) { abort(); } // no guest restart is allowed in a unit test
static void identities(void) {
    uintptr_t sem=runtime_resolve("188x57JYp0g#p#J",0);
    assert(sem && sem==runtime_resolve(K("188x57JYp0g"),0));
    assert(!runtime_resolve("188x57JYp0g#libScePosix:1#libkernel:258#F",0));
    assert(!runtime_resolve("188x57JYp0g#libkernel:2#libkernel:257#F",0));
    assert(!runtime_resolve("188x57JYp0g#libScePad:1#libScePad:257#F",0));
    assert(!runtime_resolve("188x57JYp0g#libkernel:1#libkernel:257#D",0));
    assert(!runtime_resolve(K("188x57JYp0g"),1));
    assert(runtime_resolve("f7uOxY9mM1U#libkernel:1#libkernel:257#D",1));
    assert(!runtime_resolve("f7uOxY9mM1U#libkernel:1#libkernel:257#F",0));
    const char **prog=(const char **)runtime_resolve("djxxOmW6-aw#libkernel:1#libkernel:257#D",1);
    assert(prog && !strcmp(*prog,"eboot.bin"));
    RuntimeImportIdentity value;
    const char *invalid[]={"188x57JYp0g#libkernel:-1#libkernel:257#F",
        "188x57JYp0g#libkernel:4294967297#libkernel:257#F",
        "188x57JYp0g#libkernel:1#libkernel:65536#F",
        "188x57JYp0g#libkernel:1#libkernel:257#Fjunk",
        "188x57JYp0g#libkernel:1#libkernel:257#X","188x57JYp0g#b#T"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) assert(!runtime_identity_parse(invalid[i],&value));
    puts("PASS: scoped identity, version, namespace and data/function isolation");
}
typedef int32_t (ABI *FlagCreate)(uint64_t *,const char *,uint32_t,uint64_t,const void *);
typedef int32_t (ABI *EventOp)(uint64_t);
typedef int32_t (ABI *FlagSet)(uint64_t,uint64_t);
typedef int32_t (ABI *FlagWait)(uint64_t,uint64_t,uint32_t,uint64_t *,uint32_t *);
typedef int32_t (ABI *FlagPoll)(uint64_t,uint64_t,uint32_t,uint64_t *);
typedef int32_t (ABI *FlagCancel)(uint64_t,uint64_t,int32_t *);
static FlagWait wait_event;
typedef struct { uint64_t id,result; int32_t error; } EventFixture;
static void *worker(void *arg) {
    EventFixture *f=arg; uint32_t timeout=1000000;
    f->error=wait_event(f->id,3,1,&f->result,&timeout); return NULL;
}
static void await_waiter(uint64_t id) {
    uint64_t end=host_monotonic_ns()+1000000000;
    while (!runtime_eventflag_waiters(id) && host_monotonic_ns()<end) host_sleep_ns(1000000);
    assert(runtime_eventflag_waiters(id)==1);
}
static void eventflags(void) {
    FlagCreate create=GET(FlagCreate,K("BpFoboUJoZU"));
    EventOp destroy=GET(EventOp,K("8mql9OcQnd4"));
    FlagSet set=GET(FlagSet,K("IOnSvHzqu6A")),clear=GET(FlagSet,K("7uhBFWRAS60"));
    FlagPoll poll=GET(FlagPoll,K("9lvj5DjHZiA"));
    FlagCancel cancel=GET(FlagCancel,K("PZku4ZrXJqg"));
    wait_event=GET(FlagWait,K("JTvBflhYazQ"));
    assert(create && destroy && set && clear && poll && cancel && wait_event);
    uint64_t id=0,result=0;
    assert(create(&id,"streaming",0x21,5,NULL)==0 && id);
    assert(poll(id,1,1|0x20,&result)==0 && result==5);
    assert((uint32_t)poll(id,1,1,&result)==0x80020010 && result==4);
    assert(set(id,3)==0 && clear(id,3)==0);
    assert(poll(id,2,2|0x10,&result)==0 && result==3);
    uint32_t timeout=1000;
    assert((uint32_t)wait_event(id,1,1,&result,&timeout)==0x8002003c && timeout==0 && result==0);
    assert((uint32_t)poll(id,0,1,&result)==0x80020016);
    assert((uint32_t)poll(id,1,0x31,&result)==0x80020016);
    EventFixture f={.id=id}; HostThread thread;
    assert(host_thread_start(&thread,0,worker,&f)==0); await_waiter(id);
    assert(set(id,3)==0 && host_thread_join(thread)==0 && f.error==0 && f.result==3);
    assert(clear(id,0)==0);
    f=(EventFixture){.id=id}; assert(host_thread_start(&thread,0,worker,&f)==0); await_waiter(id);
    int32_t n=0; assert(cancel(id,4,&n)==0 && n==1 && host_thread_join(thread)==0);
    assert((uint32_t)f.error==0x80020055 && f.result==4);
    assert(clear(id,0)==0);
    f=(EventFixture){.id=id}; assert(host_thread_start(&thread,0,worker,&f)==0); await_waiter(id);
    assert(destroy(id)==0 && host_thread_join(thread)==0 && (uint32_t)f.error==0x8002000d);
    assert((uint32_t)destroy(id)==0x80020003 && (uint32_t)set(id,1)==0x80020003);
    assert((uint32_t)create(&id,"bad",0x30,0,NULL)==0x80020016);
    puts("PASS: event flag bits, timeout, cancellation and delete with blocked waiter");
}
typedef struct { const void *buffer; uint32_t size,reserved; } Init;
typedef struct { uint64_t completed,total; } Progress;
typedef struct { uint16_t chunk; int8_t locus,reserved; } Todo;
static void playgo(const char *game) {
    runtime_file_configure(game,game);
    typedef int32_t (ABI *Initialize)(const Init *);
    typedef int32_t (ABI *Open)(uint32_t *,const void *);
    typedef int32_t (ABI *Close)(uint32_t);
    typedef int32_t (ABI *Term)(void);
    typedef int32_t (ABI *Chunks)(uint32_t,uint16_t *,uint32_t,uint32_t *);
    typedef int32_t (ABI *GetProgress)(uint32_t,const uint16_t *,uint32_t,Progress *);
    typedef int32_t (ABI *GetTodo)(uint32_t,Todo *,uint32_t,uint32_t *);
    typedef int32_t (ABI *SetTodo)(uint32_t,const Todo *,uint32_t);
    Initialize init=GET(Initialize,P("ts6GlZOKRrE")); Open open=GET(Open,P("M1Gma1ocrGE"));
    Close close=GET(Close,P("Uco1I0dlDi8")); Term term=GET(Term,P("MPe0EeBGM-E"));
    Chunks chunks=GET(Chunks,P("73fF1MFU8hA")); GetProgress progress=GET(GetProgress,P("-RJWNMK3fC8"));
    GetTodo todo=GET(GetTodo,P("Nn7zKwnA5q0")); SetTodo set=GET(SetTodo,P("gUPGiOQ1tmQ"));
    assert(init && open && close && term && chunks && progress && todo && set);
    uint32_t handle=0; assert((uint32_t)open(&handle,NULL)==0x80b20005);
    Init params={(void *)1,0x200000,0};
    assert((uint32_t)init(NULL)==0x80b2000a); assert(init(&params)==0);
    assert((uint32_t)init(&params)==0x80b20006 && open(&handle,NULL)==0);
    uint16_t ids[72]={0}; ids[71]=0xffff; uint32_t n=0;
    assert(chunks(handle,ids,71,&n)==0 && n==71 && ids[70]==70 && ids[71]==0xffff);
    Progress p={0}; assert(progress(handle,ids,71,&p)==0 && p.total==2556 && p.completed==p.total);
    uint16_t bad[2]={0,71}; p=(Progress){123,456};
    assert((uint32_t)progress(handle,bad,2,&p)==0x80b2000c && p.total==456 && p.completed==123);
    Todo tasks[72]; memset(tasks,0x55,sizeof(tasks));
    assert(todo(handle,tasks,71,&n)==0 && n==0 && tasks[0].chunk==0x5555);
    Todo one={70,3,0}; assert(set(handle,&one,1)==0); one.chunk=71;
    assert((uint32_t)set(handle,&one,1)==0x80b2000c);
    one.chunk=0; one.locus=1; assert((uint32_t)set(handle,&one,1)==0x80b20010);
    assert(close(handle)==0 && (uint32_t)chunks(handle,NULL,0,&n)==0x80b20009);
    uint32_t next=0; assert(open(&next,NULL)==0 && next!=handle);
    assert(term()==0 && (uint32_t)close(next)==0x80b20005);
    puts("PASS: PlayGo 71 chunks, progress, todo, malformed IDs and handle lifetime");
}
static void bad_playgo(const char *game) {
    runtime_file_configure(game,game);
    typedef int32_t (ABI *Initialize)(const Init *);
    typedef int32_t (ABI *Open)(uint32_t *,const void *);
    Init params={(void *)1,0x200000,0}; uint32_t id=123;
    assert((uint32_t)GET(Initialize,P("ts6GlZOKRrE"))(&params)==0x80b20002);
    assert((uint32_t)GET(Open,P("M1Gma1ocrGE"))(&id,NULL)==0x80b20005 && id==123);
    puts("PASS: malformed PlayGo metadata cannot become installed");
}
int main(int argc,char **argv) {
    runtime_start(1);
    if (argc>1 && !strcmp(argv[1],"--identities")) identities();
    else if (argc>1 && !strcmp(argv[1],"--eventflags")) eventflags();
    else if (argc>2 && !strcmp(argv[1],"--playgo")) playgo(argv[2]);
    else if (argc>2 && !strcmp(argv[1],"--bad-playgo")) bad_playgo(argv[2]);
    else return 2;
    return 0;
}
