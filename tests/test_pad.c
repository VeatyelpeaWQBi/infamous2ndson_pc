#define _GNU_SOURCE
#include <assert.h>
#include "windows_test.h"
#include "../src/runtime_pad.c"
#include "../gpu/shim/bbport_debug_hotkey.h"

static int capture;
static BbMouseMotion mouse;
static int recording_toggle_count;
static HANDLE feedback_entered,feedback_release;
static volatile LONG feedback_test_armed,feedback_calls,feedback_last_low,feedback_last_high;
static bool SDLCALL slow_feedback(void *userdata,Uint16 low,Uint16 high) {
    (void)userdata;
    InterlockedIncrement(&feedback_calls);
    InterlockedExchange(&feedback_last_low,low); InterlockedExchange(&feedback_last_high,high);
    if (InterlockedExchange(&feedback_test_armed,0)) {
        SetEvent(feedback_entered);
        assert(WaitForSingleObject(feedback_release,2000)==WAIT_OBJECT_0);
    }
    return true;
}
static int test_recording_toggle(void) { return ++recording_toggle_count & 1; }
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_mouse_motion_read(BbMouseMotion *state) { *state=mouse; mouse.dx=mouse.dy=0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    SDL_Delay(25);
}
static void inject_at(const char *path,const char *tokens,uint64_t stamp) {
    inject(path,tokens);
    HANDLE file=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            NULL,OPEN_EXISTING,0,NULL);
    assert(file!=INVALID_HANDLE_VALUE);
    FILETIME time={(DWORD)stamp,(DWORD)(stamp>>32)};
    assert(SetFileTime(file,NULL,NULL,&time));CloseHandle(file);
    SDL_Delay(25);
}
static void test_motion_math(void) {
    PadMotion m={0}; float q[4],a[3],w[3]; uint64_t now=1000000;
    motion_step(&m,now,1,0,0,0,0.003f,q,a,w);
    assert(fabsf(a[0]-1)<0.0001f && fabsf(a[1])<0.0001f);
    assert(fabsf(q[2]-0.70710678f)<0.0001f);
    motion_reset_orientation(&m);
    motion_step(&m,now+=16667,1,0,0,0,0.003f,q,a,w);
    assert(fabsf(q[3]-1)<0.0001f && fabsf(q[2])<0.0001f && fabsf(a[0]-1)<0.0001f);
    float peak=0;
    for (int i=0;i<120;++i) {
        motion_step(&m,now+=16667,1,0,0,i%2 ? 100 : -100,0.003f,q,a,w);
        peak=fmaxf(peak,fabsf(a[0]-1));
        float norm=0;
        for (int j=0;j<4;++j) { assert(isfinite(q[j])); norm+=q[j]*q[j]; }
        assert(fabsf(norm-1)<0.0001f);
        for (int j=0;j<3;++j) assert(isfinite(a[j]) && isfinite(w[j]) && fabsf(a[j])<=6.67f);
    }
    assert(peak>0.8f); // repeated mouse reversals produce detectable shake
    for (int i=0;i<120;++i) motion_step(&m,now+=16667,1,0,0,0,0.003f,q,a,w);
    assert(fabsf(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]-1)<0.001f);
    assert(fabsf(w[0])+fabsf(w[1])+fabsf(w[2])<0.001f);
    motion_step(&m,now+=16667,1,1,NAN,INFINITY,0.003f,q,a,w);
    assert(fabsf(a[0]-1)<0.0001f && isfinite(q[3]));
    motion_step(&m,now+=16667,0,0,100,100,0.003f,q,a,w);
    assert(q[3]==1 && a[0]==0 && w[0]==0);
    // The same mouse distance delivered at 60/120 Hz settles at the same pose.
    float final[2][4];
    for (int rate=0;rate<2;++rate) {
        memset(&m,0,sizeof(m)); now=1000000;
        for (int i=0;i<(60<<rate);++i)
            motion_step(&m,now+=(uint64_t)(1000000/(60<<rate)),1,0,2.f/(1<<rate),0,0.003f,q,a,w);
        for (int i=0;i<120;++i) motion_step(&m,now+=16667,1,0,0,0,0.003f,q,a,w);
        memcpy(final[rate],q,sizeof(q));
    }
    for (int i=0;i<4;++i) assert(fabsf(final[0][i]-final[1][i])<0.0001f);
}

static void test_debug_hotkey(void) {
    assert(getenv("BB_DEBUG_DIR"));
    test_setenv("SDL_VIDEODRIVER","dummy");
    assert(SDL_Init(SDL_INIT_VIDEO));
    SDL_Window *window=SDL_CreateWindow("F10 test",64,64,SDL_WINDOW_HIDDEN);
    assert(window);
    const SDL_WindowID id=SDL_GetWindowID(window);
    wchar_t name[64]; swprintf(name,64,L"Local\\bbport-dump-%lu",GetCurrentProcessId());
    HANDLE request=CreateEventW(NULL,FALSE,FALSE,name); assert(request);
    SDL_Event event={0};
    while (SDL_PollEvent(&event)) {} /* discard window creation events */
    for (int press=0;press<3;++press) {
        event=(SDL_Event){0}; event.type=SDL_EVENT_KEY_DOWN;
        event.key.windowID=id; event.key.scancode=SDL_SCANCODE_F10;
        assert(SDL_PushEvent(&event));
        event.key.repeat=true; assert(SDL_PushEvent(&event));
        event.key.repeat=false; event.type=SDL_EVENT_KEY_UP; assert(SDL_PushEvent(&event));
        /* The entire short press has finished before processing any event.
         * No pad_init/read calls: a cutscene may never poll the controller. */
        int handled=0;
        while (SDL_PollEvent(&event)) handled+=bb_debug_hotkey(&event,id,runtime_debug_mark);
        assert(handled==1);
        const char *deep=getenv("BB_F10_DEEP");
        assert(WaitForSingleObject(request,0)==(deep && deep[0]=='1' ? WAIT_OBJECT_0 : WAIT_TIMEOUT));
        assert(WaitForSingleObject(request,0)==WAIT_TIMEOUT);
    }
    event=(SDL_Event){0}; event.type=SDL_EVENT_KEY_DOWN;
    event.key.windowID=id+1; event.key.scancode=SDL_SCANCODE_F10;
    assert(!bb_debug_hotkey(&event,id,runtime_debug_mark));
    event.key.windowID=id; event.key.scancode=SDL_SCANCODE_F9;
    assert(!bb_debug_hotkey(&event,id,runtime_debug_mark));
    assert(WaitForSingleObject(request,0)==WAIT_TIMEOUT);
    recording_toggle_count=0;
    event=(SDL_Event){0}; event.type=SDL_EVENT_KEY_DOWN;
    event.key.windowID=id; event.key.scancode=SDL_SCANCODE_F11;
    int active=0; assert(bb_performance_hotkey(&event,id,test_recording_toggle,&active));
    assert(active==1 && recording_toggle_count==1);
    event.key.repeat=true; assert(!bb_performance_hotkey(&event,id,test_recording_toggle,&active));
    event.key.repeat=false; event.key.windowID=id+1; assert(!bb_performance_hotkey(&event,id,test_recording_toggle,&active));
    event.key.windowID=id; event.key.mod=SDL_KMOD_SHIFT;
    assert(!bb_performance_hotkey(&event,id,test_recording_toggle,&active));
    event.key.mod=0;
    event.key.windowID=id; assert(bb_performance_hotkey(&event,id,test_recording_toggle,&active));
    assert(active==0 && recording_toggle_count==2);
    CloseHandle(request); SDL_DestroyWindow(window); SDL_Quit();
    puts("PASS: F10 short presses without pad reads, repeat/window isolation, real diagnostic writes and snapshot event");
}

int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--debug-hotkey")) { test_debug_hotkey(); return 0; }
    test_motion_math();
    char path[MAX_PATH];
    test_temp_file(path,sizeof(path));
    test_setenv("BB_PAD_FILE",path);
    test_setenv("SDL_VIDEODRIVER","dummy");
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    uint8_t colour[4]={20,70,130,0};
    assert(pad_lightbar(1,colour)==0 && memcmp(lightbar,colour,3)==0);
    assert(pad_lightbar(1,NULL)==ERR_INVALID_ARG && pad_lightbar(2,colour)==ERR_INVALID_HANDLE);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);
    test_setenv("BB_BENCHMARK_INPUT","1");
    inject(path,"rx=180");
    HANDLE input_locked=CreateFileA(path,GENERIC_READ,0,NULL,OPEN_EXISTING,0,NULL);
    assert(input_locked!=INVALID_HANDLE_VALUE);
    assert(pad_read_state(1,&data)==0 && data.right_x==128);
    CloseHandle(input_locked);
    SDL_Delay(25);
    assert(pad_read_state(1,&data)==0 && data.right_x==180);
    FILETIME input_time;GetSystemTimeAsFileTime(&input_time);
    uint64_t input_stamp=(((uint64_t)input_time.dwHighDateTime<<32)|input_time.dwLowDateTime);
    input_stamp=(input_stamp/10000000-2)*10000000+1000000;
    inject_at(path,"rx=180",input_stamp);
    assert(pad_read_state(1,&data)==0 && data.right_x==180);
    inject_at(path,"rx=181",input_stamp+1000000); // Same size and whole second.
    assert(pad_read_state(1,&data)==0 && data.right_x==181);
    test_setenv("BB_BENCHMARK_INPUT","");
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.right_x==128);
    mouse=(BbMouseMotion){.active=1,.left=1,.reset=1};
    assert(pad_motion_state(1,2)==ERR_INVALID_ARG && pad_motion_state(2,1)==ERR_INVALID_HANDLE);
    assert(pad_read_state(1,&data)==0 && (data.buttons&BTN_R2) && data.r2==255);
    assert(fabsf(data.acceleration[0]-1)<0.0001f);
    assert(pad_motion_reset(1)==0 && pad_read_state(1,&data)==0);
    assert(fabsf(data.orientation[3]-1)<0.0001f && fabsf(data.acceleration[0]-1)<0.0001f);
    assert(pad_motion_state(1,0)==0 && pad_read_state(1,&data)==0);
    assert(data.acceleration[0]==0 && data.orientation[3]==1);
    assert(pad_motion_state(1,1)==0);
    mouse.active=mouse.left=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.r2==0);
    mouse=(BbMouseMotion){.touch_active=1,.touch_down=1,.touch_x=1234,.touch_y=567,.touch_id=7};
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.buttons==0 && data.r2==0);
    assert(data.touches[0].x==1234 && data.touches[0].y==567 && data.touches[0].id==7);
    assert(pad_read_state(1,&data)==0 && data.touches[0].id==7);
    mouse.touch_click=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.buttons==BTN_TOUCHPAD && data.r2==0);
    mouse.touch_x=65535; mouse.touch_y=65535; mouse.touch_id=255;
    assert(pad_read_state(1,&data)==0 && data.touches[0].x==1919 && data.touches[0].y==942 && data.touches[0].id==127);
    mouse.touch_down=mouse.touch_click=0;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    mouse=(BbMouseMotion){0};
    char replay_path[MAX_PATH]; test_temp_file(replay_path,sizeof(replay_path));
    FILE *recording=fopen(replay_path,"w"); assert(recording);
    fputs("0 0 128 128 128 128 0 0\n"
          "1 512 128 128 128 128 0 255 0 0 0.70710678 0.70710678 1 0 0 1 2 3\n"
          "2 0 128 128 128 128 0 0 nan 0 0 1 0 0 0 0 0 0\n",recording);
    fclose(recording); test_setenv("BB_PAD_REPLAY",replay_path);
    inject(path,"replay");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    SDL_Delay(25);
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_R2 && data.r2==255);
    assert(data.acceleration[0]==1 && data.angular_velocity[2]==3 && fabsf(data.orientation[2]-0.70710678f)<0.0001f);
    assert(replay_count==2);
    replay_armed=0; unlink(replay_path); inject(path,"");

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    desc.Rumble=slow_feedback;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    mouse=(BbMouseMotion){.touch_active=1,.touch_down=1,.touch_x=1000,.touch_y=400,.touch_id=9};
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].id==9 && data.touches[0].x==1000);
    assert(data.buttons==BTN_CROSS && data.r2==0);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    mouse=(BbMouseMotion){0};
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    /* Queuing repeated output before each input read must not freeze input.
     * Pause consumption with an owned mailbox to make the pending state
     * deterministic, rather than relying on worker scheduling. */
    PadFeedback queued={0};host_mutex_init(&queued.mutex);
    queued.started=1;queued.wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    assert(queued.wake);
    pad_feedback_submit(&queued,id,0,0);
    assert(!pad_feedback_busy(&queued) && queued.serial==1);
    for(unsigned i=0;i<200;++i) pad_feedback_submit(&queued,id,0,0);
    assert(queued.serial==1 && !pad_feedback_busy(&queued));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_SOUTH,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,24000));
    SDL_UpdateJoysticks();SDL_UpdateGamepads();
    PadFeedback *live=&feedback; // The live mailbox has no pending device IO here.
    assert(!pad_feedback_busy(live));
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_CROSS) && data.left_x>200);
    CloseHandle(queued.wake);
    /* The virtual backend blocks while SDL holds its joystick lock. Neither the
     * guest output API nor a concurrent input read may wait for that driver. */
    feedback_entered=CreateEventW(NULL,TRUE,FALSE,NULL);
    feedback_release=CreateEventW(NULL,TRUE,FALSE,NULL);
    assert(feedback_entered && feedback_release);
    InterlockedExchange(&feedback_test_armed,1);
    uint8_t motors[2]={120,80};
    const uint64_t queued_at=host_monotonic_ns();
    assert(pad_vibration(1,motors)==0);
    assert(host_monotonic_ns()-queued_at<50000000);
    assert(WaitForSingleObject(feedback_entered,2000)==WAIT_OBJECT_0);
    const uint64_t read_at=host_monotonic_ns();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    assert(host_monotonic_ns()-read_at<50000000 && pad_feedback_busy(&feedback));
    for (int i=0;i<200;++i) { motors[0]=(uint8_t)i; assert(pad_vibration(1,motors)==0); }
    motors[0]=motors[1]=0; assert(pad_vibration(1,motors)==0);
    SetEvent(feedback_release);
    const uint64_t stop_deadline=GetTickCount64()+2000;
    while (pad_feedback_busy(&feedback) && GetTickCount64()<stop_deadline) SDL_Delay(1);
    assert(!pad_feedback_busy(&feedback));
    assert(feedback_last_low==0 && feedback_last_high==0 && feedback_calls<=3);
    assert(pad_vibration(1,NULL)==ERR_INVALID_ARG && pad_vibration(2,motors)==ERR_INVALID_HANDLE);
    feedback_shutdown();
    CloseHandle(feedback_entered); CloseHandle(feedback_release);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI/motion/touch; blocked vibration backend, nonblocking input and latest stop coalescing");
}
