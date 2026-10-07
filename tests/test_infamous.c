/* Runtime adaptation contracts without executing any game instructions. */
#include "windows_test.h"
#include "../src/runtime.h"
#include "../src/runtime_identity.h"
#include <stdio.h>
#include <stdatomic.h>
#define GET(type,name) ((type)runtime_resolve(name,0))
#define K(n) n "#libkernel:1#libkernel:257#F"
#define P(n) n "#libScePlayGo:1#libScePlayGo:256#F"
#define TRACE "NWtTN10cJzE#libSceLibcInternalExt:1#libSceLibcInternal:257#F"
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
    test_setenv("BB_LANGUAGE","10");
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
    typedef int32_t (ABI *GetMask)(uint32_t,uint64_t *);
    GetMask get_mask=GET(GetMask,P("3OMbYZBaa50"));
    assert(get_mask); uint64_t mask=0;
    assert(get_mask(handle,&mask)==0 && mask==(UINT64_C(1)<<53));
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
typedef struct {
    uint64_t size;
    uint32_t flag,get_segment_info;
    uint64_t *mask,*states;
} TraceInfo;
typedef void (ABI *GetTrace)(TraceInfo *);
static void heap_trace(void) {
    GetTrace get=GET(GetTrace,TRACE); assert(get);
    assert(!runtime_resolve("NWtTN10cJzE#libSceLibcInternalExt:2#libSceLibcInternal:257#F",0));
    assert(!runtime_resolve("NWtTN10cJzE#libSceLibcInternalExt:1#libSceLibcInternal:258#F",0));
    assert(!runtime_resolve("NWtTN10cJzE#libSceLibcInternalExt:1#libSceLibcInternal:257#D",1));
    struct { uint64_t before; TraceInfo info; uint64_t after; } guarded={
        .before=UINT64_C(0x1122334455667788),
        .info={32,0x12345678,0xffffffff,NULL,NULL},
        .after=UINT64_C(0x8877665544332211)};
    get(&guarded.info);
    assert(guarded.info.size==32 && guarded.info.flag==0x12345678 && !guarded.info.get_segment_info);
    assert(guarded.info.mask && guarded.info.states);
    assert(!((uintptr_t)guarded.info.mask&7) && !((uintptr_t)guarded.info.states&7));
    assert(!*guarded.info.mask);
    for (size_t i=0;i<64;++i) assert(!guarded.info.states[i]);
    /* Model the native libc retaining and populating both tables. */
    *guarded.info.mask=UINT64_C(1)<<63; guarded.info.states[63]=0x123456;
    struct { TraceInfo info; uint64_t extension; } newer={.info={40,7,1,NULL,NULL},.extension=0xfedcba};
    get(&newer.info);
    assert(newer.info.size==40 && newer.info.flag==7 && !newer.info.get_segment_info);
    assert(newer.info.mask==guarded.info.mask && newer.info.states==guarded.info.states);
    assert(*newer.info.mask==(UINT64_C(1)<<63) && newer.info.states[63]==0x123456);
    assert(newer.extension==0xfedcba);
    assert(guarded.before==UINT64_C(0x1122334455667788) && guarded.after==UINT64_C(0x8877665544332211));
    puts("PASS: heap trace ABI, writable persistent 64-entry state and output bounds");
}
static void kernel_clocks(void) {
    typedef uint64_t (ABI *Clock)(void);
    Clock frequency=GET(Clock,K("1j3S3n-tTW4"));
    Clock tsc=GET(Clock,K("-2IRUCO--PM"));
    assert(frequency && tsc);
    const uint64_t hz=frequency(); assert(hz>1000000 && hz==frequency());
    const uint64_t start=host_monotonic_ns(),before=tsc();
    host_sleep_ns(20000000);
    const uint64_t after=tsc(), elapsed=host_monotonic_ns()-start;
    assert(after>before && elapsed>0);
    const double measured=(double)(after-before)*1000000000.0/(double)elapsed;
    assert(measured>(double)hz*0.8 && measured<(double)hz*1.2);
    assert(!runtime_resolve("1j3S3n-tTW4#libkernel:2#libkernel:257#F",0));
    assert(!runtime_resolve("1j3S3n-tTW4#libScePad:1#libScePad:257#F",0));
    typedef int32_t (ABI *Mode)(void);
    Mode mode=GET(Mode,K("WslcK1FQcGI")); assert(mode && mode()==0);
    puts("PASS: audited kernel TSC frequency and counter match elapsed host time");
}
static void console_locale(void) {
    typedef int32_t (ABI *GetInt)(int32_t,int32_t *);
    GetInt get=GET(GetInt,"fZo48un7LK4#libSceSystemService:1#libSceSystemService:257#F"); assert(get);
    test_setenv("BB_LANGUAGE","10"); test_setenv("BB_TIMEZONE_MINUTES","480"); test_setenv("BB_ENTER_BUTTON","0");
    struct { uint32_t before; int32_t value; uint32_t after; } output={0x12345678,-1,0x87654321};
    assert(get(1,&output.value)==0 && output.value==10);
    assert(get(4,&output.value)==0 && output.value==480);
    assert(get(1000,&output.value)==0 && output.value==0);
    assert(output.before==0x12345678 && output.after==0x87654321);
    assert((uint32_t)get(1,NULL)==0x80a10003);
    test_setenv("BB_TIMEZONE_MINUTES","480junk"); output.value=123;
    assert((uint32_t)get(4,&output.value)==0x80a10003 && output.value==123);
    test_setenv("BB_TIMEZONE_MINUTES","841");
    assert((uint32_t)get(4,&output.value)==0x80a10003 && output.value==123);
    test_setenv("BB_ENTER_BUTTON","2");
    assert((uint32_t)get(1000,&output.value)==0x80a10003 && output.value==123);
    test_unsetenv("BB_ENTER_BUTTON"); assert(get(1000,&output.value)==0 && output.value==1);
    test_unsetenv("BB_LANGUAGE"); test_unsetenv("BB_TIMEZONE_MINUTES");
    puts("PASS: Traditional Chinese / Hong Kong time zone / circle confirm ABI and bounds");
}
static void system_name(void) {
    typedef int32_t (ABI *GetString)(int32_t,char *,uint64_t);
    GetString get=GET(GetString,"SsC-m-S9JTA#libSceSystemService:1#libSceSystemService:257#F");
    assert(get);
    assert(_putenv_s("BB_SYSTEM_NAME","Second Son PC")==0);
    struct { unsigned char before; char value[65]; unsigned char after; } b;
    memset(&b,0x55,sizeof(b));
    assert(get(6,b.value,sizeof(b.value))==0 && !strcmp(b.value,"Second Son PC"));
    assert(b.before==0x55 && b.after==0x55 && b.value[14]==0x55);
    memset(b.value,0x55,sizeof(b.value));
    assert((uint32_t)get(6,b.value,13)==0x80a10003 && b.value[0]==0x55);
    assert((uint32_t)get(6,NULL,65)==0x80a10003);
    assert((uint32_t)get(1,b.value,65)==0x80a10003 && b.value[0]==0x55);
    assert(_putenv_s("BB_SYSTEM_NAME","")==0);
    typedef int32_t (ABI *SafeArea)(float *);
    SafeArea area=GET(SafeArea,"1n37q1Bvc5Y#libSceSystemService:1#libSceSystemService:257#F"); assert(area);
    struct { uint32_t before; float ratio; unsigned char reserved[128]; uint32_t after; } safe;
    memset(&safe,0x55,sizeof(safe));
    assert(area(&safe.ratio)==0 && safe.ratio==1.0f);
    assert(safe.before==0x55555555 && safe.after==0x55555555);
    for (size_t i=0;i<sizeof(safe.reserved);++i) assert(safe.reserved[i]==0x55);
    assert((uint32_t)area(NULL)==0x80a10003);
    puts("PASS: system name string, terminator, buffer bounds and invalid parameter rejection");
}
static void discmap_query(void) {
    typedef int32_t (ABI *Query)(const char *,int64_t,int64_t,int32_t *,int32_t *,int32_t *);
    Query query=GET(Query,"fJgP+wqifno#libSceDiscMap:1#libSceDiscMap:257#F"); assert(query);
    struct { int32_t before,flags,first,second,after; } out={1,2,3,4,5};
    assert(query("/app0/art/fixture.psarc",0,512,&out.flags,&out.first,&out.second)==0);
    assert(out.before==1 && !out.flags && !out.first && !out.second && out.after==5);
    out.flags=2; out.first=3; out.second=4;
    assert((uint32_t)query(NULL,0,512,&out.flags,&out.first,&out.second)==0x81100001);
    assert((uint32_t)query("/app0/art/fixture.psarc",-1,512,&out.flags,&out.first,&out.second)==0x81100001);
    assert((uint32_t)query("/app0/art/fixture.psarc",INT64_MAX,1,&out.flags,&out.first,&out.second)==0x81100001);
    assert((uint32_t)query("/app0/art/fixture.psarc",0,512,&out.flags,&out.first,NULL)==0x81100001);
    assert(out.flags==2 && out.first==3 && out.second==4);
    puts("PASS: installed-package disc placement query output bounds and invalid argument rejection");
}
static void trophy_state(void) {
    typedef int32_t (ABI *State)(int32_t,int32_t,uint32_t *,uint32_t *);
    State get=GET(State,"LHuSmO3SLd8#libSceNpTrophy:1#libSceNpTrophy:257#F"); assert(get);
    uint32_t flags[4]={1,2,3,4},count=123;
    assert((uint32_t)get(1,1,flags,&count)==0x8055160f);
    assert(flags[0]==1 && flags[1]==2 && flags[2]==3 && flags[3]==4 && count==123);
    assert((uint32_t)get(1,1,NULL,&count)==0x80551604);
    puts("PASS: unavailable trophy metadata returns NOT_REGISTERED without fabricated state");
}
static void offline_network(void) {
    typedef int32_t (ABI *Init)(void);
    typedef int32_t (ABI *State)(int32_t *);
    typedef int32_t (ABI *Info)(int32_t,void *);
    Init init=GET(Init,"gky0+oaNM4k#libSceNetCtl:1#libSceNetCtl:257#F");
    Init term=GET(Init,"Z4wwCFiBELQ#libSceNetCtl:1#libSceNetCtl:257#F");
    State state=GET(State,"uBPlr0lbuiI#libSceNetCtl:1#libSceNetCtl:257#F");
    Info info=GET(Info,"obuxdTiwkF8#libSceNetCtl:1#libSceNetCtl:257#F");
    assert(init && term && state && info && init()==0);
    int32_t value=123; assert(state(&value)==0 && value==0);
    uint32_t address=123; assert((uint32_t)info(14,&address)==0x80412108 && address==123);
    assert((uint32_t)state(NULL)==0x80412107 && term()==0);
    puts("PASS: NetCtl bootstraps a disconnected console without fabricated online state");
}
static void thread_priority_attribute(void) {
    typedef int32_t (ABI *Init)(void **);
    typedef int32_t (ABI *Param)(void **,int32_t *);
    Init init=GET(Init,K("nsYoNRywwNg")),destroy=GET(Init,K("62KCwEMmzcM"));
    Param set=GET(Param,K("DzES9hQF4f4")),get=GET(Param,K("FXPWHNk8Of0"));
    assert(init && destroy && set && get);
    void *attr=NULL; int32_t priority=0;
    assert(init(&attr)==0 && get(&attr,&priority)==0 && priority==700);
    priority=733; assert(set(&attr,&priority)==0); priority=0;
    assert(get(&attr,&priority)==0 && priority==733);
    assert((uint32_t)get(&attr,NULL)==0x80020016 && destroy(&attr)==0);
    assert((uint32_t)get(&attr,&priority)==0x80020016);
    puts("PASS: canonical thread scheduling attribute reads match recorded priority");
}
static void http_timeouts(void) {
    typedef int32_t (ABI *Create)(void);
    typedef int32_t (ABI *Set)(int32_t,uint32_t);
    typedef int32_t (ABI *Delete)(int32_t);
    Create create=GET(Create,"0gYjPTR-6cY#libSceHttp:1#libSceHttp:257#F");
    Delete destroy=GET(Delete,"4I8vEpuEhZ8#libSceHttp:1#libSceHttp:257#F");
    Delete request=GET(Delete,"1e2BNwI-XzE#libSceHttp:1#libSceHttp:257#F");
    Set recv=GET(Set,"yigr4V0-HTM#libSceHttp:1#libSceHttp:257#F");
    Set send=GET(Set,"xegFfZKBVlw#libSceHttp:1#libSceHttp:257#F");
    Set connect=GET(Set,"0S9tTH0uqTU#libSceHttp:1#libSceHttp:257#F");
    assert(create && destroy && request && recv && send && connect);
    assert((uint32_t)recv(-1,1000)==0x80431100);
    const int32_t object=create(); assert(object>0);
    assert(recv(object,1000)==0 && send(object,2000)==0 && connect(object,3000)==0);
    assert((uint32_t)request(object)==0x80431063); /* Still offline: no transfer success. */
    assert(destroy(object)==0 && (uint32_t)recv(object,1000)==0x80431100);
    assert((uint32_t)destroy(object)==0x80431100);
    typedef int32_t (ABI *Load)(int32_t,int32_t,const void **,const void *,const void *);
    Load load=GET(Load,"DK+GoXCNT04#libSceHttp:1#libSceHttp:257#F"); assert(load);
    assert((uint32_t)load(1,0,NULL,NULL,NULL)==0x80431075);
    assert((uint32_t)load(1,1,NULL,NULL,NULL)==0x804311fe);
    assert((uint32_t)load(-1,0,NULL,NULL,NULL)==0x80431100);
    puts("PASS: HTTP timeout object lifecycle and explicit offline transfer failure");
}
static ABI void offline_state_callback(int32_t user,int32_t state,void *argument) {
    (void)user; (void)state; (void)argument; assert(0 && "no fabricated sign-in callback");
}
static void np_subscriptions(void) {
    typedef int32_t (ABI *Register)(void *,void *);
    typedef int32_t (ABI *Account)(int32_t,uint64_t *);
    Register reg=GET(Register,"qQJfO8HAiaY#libSceNpManager:1#libSceNpManager:257#F");
    Account account=GET(Account,"rbknaUjpqWo#libSceNpManager:1#libSceNpManager:257#F");
    assert(reg && account && (uint32_t)reg(NULL,NULL)==0x80550003);
    int32_t id=reg((void *)offline_state_callback,(void *)1); assert(id>0);
    assert((uint32_t)reg((void *)offline_state_callback,NULL)==0x80550008);
    uint64_t value=123; assert((uint32_t)account(1,&value)==0x80550006 && !value);
    assert((uint32_t)account(1,NULL)==0x80550003);
    puts("PASS: NP A subscription identity and explicit signed-out account state");
}
int main(int argc,char **argv) {
    runtime_start(1);
    if (argc>1 && !strcmp(argv[1],"--identities")) identities();
    else if (argc>1 && !strcmp(argv[1],"--eventflags")) eventflags();
    else if (argc>2 && !strcmp(argv[1],"--playgo")) playgo(argv[2]);
    else if (argc>2 && !strcmp(argv[1],"--bad-playgo")) bad_playgo(argv[2]);
    else if (argc>1 && !strcmp(argv[1],"--heap-trace")) heap_trace();
    else if (argc>1 && !strcmp(argv[1],"--kernel-clocks")) kernel_clocks();
    else if (argc>1 && !strcmp(argv[1],"--system-name")) system_name();
    else if (argc>1 && !strcmp(argv[1],"--console-locale")) console_locale();
    else if (argc>1 && !strcmp(argv[1],"--discmap")) discmap_query();
    else if (argc>1 && !strcmp(argv[1],"--trophy-state")) trophy_state();
    else if (argc>1 && !strcmp(argv[1],"--offline-network")) offline_network();
    else if (argc>1 && !strcmp(argv[1],"--thread-priority")) thread_priority_attribute();
    else if (argc>1 && !strcmp(argv[1],"--http-timeouts")) http_timeouts();
    else if (argc>1 && !strcmp(argv[1],"--np-subscriptions")) np_subscriptions();
    else if (argc>1 && !strcmp(argv[1],"--bad-heap-trace")) {
        TraceInfo info={.size=31}; GET(GetTrace,TRACE)(&info); return 3;
    }
    else if (argc>1 && !strcmp(argv[1],"--null-heap-trace")) { GET(GetTrace,TRACE)(NULL); return 3; }
    else return 2;
    return 0;
}
