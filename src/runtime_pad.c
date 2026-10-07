/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Keyboard layout (when no gamepad is connected):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad, Backspace right touchpad,
 *   IJKL d-pad (I up, K down, J left, L right).
 * F6 mouse motion, F8 touchpad, F7 center, Esc release; motion left R2,
 * touchpad left drag / right click (window thread). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include "pad_motion.h"

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static HostMutex lock=HOST_MUTEX_INIT;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;
static uint8_t lightbar[3];
static PadMotion mouse_pose;
static int motion_enabled=1, motion_deadband=1;
static uint32_t mouse_reset;
static int diagnostic_motion=-1, diagnostic_center;
static float diagnostic_dx,diagnostic_dy;

static uint64_t now_us(void) { return host_monotonic_ns()/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}
static void touch_click(PadData *d, int right) {
    d->buttons|=BTN_TOUCHPAD;
    d->touch_count=1;
    d->touches[0]=(PadTouch){.x=right ? 1440 : 480,.y=471,.id=0};
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    if (bbgpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    if (g) {
        static const struct { SDL_GamepadButton sdl; uint32_t ps; } map[]={
            {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
            {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
            {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
        };
        for (size_t i=0;i<sizeof(map)/sizeof(*map);++i) if (SDL_GetGamepadButton(g,map[i].sdl)) d->buttons|=map[i].ps;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        // Back/Select on pads without a touch surface is a left-side click.
        if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
        if (k && k[SDL_SCANCODE_TAB]) touch_click(d,0);
        if (k && k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
        return;
    }
    if (!k) return;
    static const struct { SDL_Scancode key; uint32_t ps; } keys[]={
        {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_E,BTN_SQUARE},
        {SDL_SCANCODE_Q,BTN_TRIANGLE}, {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
        {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2}, {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
        {SDL_SCANCODE_RETURN,BTN_OPTIONS},
        {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN}, {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
    };
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) if (k[keys[i].key]) d->buttons|=keys[i].ps;
    if (k[SDL_SCANCODE_TAB]) touch_click(d,0);
    if (k[SDL_SCANCODE_BACKSPACE]) touch_click(d,1);
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    d->left_x=(uint8_t)(128-(k[SDL_SCANCODE_A] ? 128 : 0)+(k[SDL_SCANCODE_D] ? 127 : 0));
    d->left_y=(uint8_t)(128-(k[SDL_SCANCODE_W] ? 128 : 0)+(k[SDL_SCANCODE_S] ? 127 : 0));
    d->right_x=(uint8_t)(128-(k[SDL_SCANCODE_LEFT] ? 128 : 0)+(k[SDL_SCANCODE_RIGHT] ? 127 : 0));
    d->right_y=(uint8_t)(128-(k[SDL_SCANCODE_UP] ? 128 : 0)+(k[SDL_SCANCODE_DOWN] ? 127 : 0));
}

/* BB_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad defaults to a left-side click; lx= ly= rx= ry= (0..255) override
 * the sticks. motion=1/0 selects diagnostic mouse motion; mouse_dx=/mouse_dy=
 * are consumed once per update; motion_center recenters. Empty releases all. */
static struct { uint32_t buttons; int stick[4]; int touch_side; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a BB_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("BB_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second times: the size tells writes within the same second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime.tv_sec=st.st_mtime; mtime.tv_nsec=(long)st.st_size;
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const struct { const char *name; uint32_t ps; } names[]={
        {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
        {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
        {"options",BTN_OPTIONS}, {"touchpad",BTN_TOUCHPAD},
        {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    };
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    injected.touch_side=-1;
    diagnostic_motion=-1;
    diagnostic_dx=diagnostic_dy=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"motion=1")) diagnostic_motion=1;
        if (!strcmp(token,"motion=0")) diagnostic_motion=0;
        if (!strcmp(token,"motion_center")) diagnostic_center=1;
        if (!strncmp(token,"mouse_dx=",9)) diagnostic_dx=strtof(token+9,NULL);
        if (!strncmp(token,"mouse_dy=",9)) diagnostic_dy=strtof(token+9,NULL);
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* BB_PAD_REPLAY */
        if (!strcmp(token,"touchpad_left") || !strcmp(token,"touchpad_right")) {
            injected.buttons|=BTN_TOUCHPAD;
            injected.touch_side=!strcmp(token,"touchpad_right");
        }
        for (size_t i=0;i<sizeof(names)/sizeof(*names);++i) if (!strcmp(token,names[i].name)) injected.buttons|=names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
static void sample_mouse_motion(PadData *d) {
    BbMouseMotion input={0}; bbgpu_mouse_motion_read(&input);
    static int configured,was_active;
    static float sensitivity=0.003f;
    if (!configured) {
        configured=1;
        const char *value=getenv("BB_MOUSE_MOTION_SENSITIVITY");
        if (value) {
            const float requested=strtof(value,NULL);
            if (isfinite(requested) && requested>=0.0001f && requested<=0.02f) sensitivity=requested;
        }
    }
    if (diagnostic_motion>=0) input.active=diagnostic_motion;
    input.dx+=diagnostic_dx; input.dy+=diagnostic_dy;
    diagnostic_dx=diagnostic_dy=0;
    const int center=input.reset!=mouse_reset || diagnostic_center;
    mouse_reset=input.reset; diagnostic_center=0;
    motion_step(&mouse_pose,now_us(),input.active,center,input.dx,input.dy,sensitivity,
                d->orientation,d->acceleration,d->angular_velocity);
    if (!motion_enabled) {
        memset(d->orientation,0,sizeof(d->orientation)); d->orientation[3]=1;
        memset(d->acceleration,0,sizeof(d->acceleration));
        memset(d->angular_velocity,0,sizeof(d->angular_velocity));
    } else if (motion_deadband) {
        for (int i=0;i<3;++i) if (fabsf(d->angular_velocity[i])<0.001f) d->angular_velocity[i]=0;
    }
    if (input.active && input.left) { d->buttons|=BTN_R2; d->r2=255; }
    if (input.touch_active && input.touch_down) {
        d->touch_count=1;
        d->touches[0]=(PadTouch){.x=input.touch_x>1919 ? 1919 : input.touch_x,
            .y=input.touch_y>942 ? 942 : input.touch_y,.id=(uint8_t)(input.touch_id&127)};
        memset(&d->touches[1],0,sizeof(d->touches[1]));
        if (input.touch_click) d->buttons|=BTN_TOUCHPAD;
    }
    runtime_debug_touch(input.touch_active,input.touch_down,input.touch_click,input.touch_id,input.touch_x,input.touch_y);
    if (was_active!=(int)input.active) {
        printf("Runtime: mouse motion %s (F6 toggle, F7 center, Esc release, left click R2)\n",
               input.active ? "enabled" : "disabled");
        was_active=input.active;
    }
    runtime_debug_motion(input.active && motion_enabled,d->orientation,d->acceleration,d->angular_velocity);
}
/* BB_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; BB_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in BB_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2 [qx qy qz qw ax ay az wx wy wz], at most 20 Hz except button
 * edges. Eight-column legacy recordings remain valid. */
typedef struct {
    uint32_t ms, buttons; uint8_t axes[4], l2, r2;
    float orientation[4],acceleration[3],angular_velocity[3];
    int has_motion;
} PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("BB_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={.ms=(uint32_t)((now_us()-record_start)/1000),.buttons=d->buttons,
                 .axes={d->left_x,d->left_y,d->right_x,d->right_y},.l2=d->l2,.r2=d->r2,.has_motion=1};
    memcpy(s.orientation,d->orientation,sizeof(s.orientation));
    memcpy(s.acceleration,d->acceleration,sizeof(s.acceleration));
    memcpy(s.angular_velocity,d->angular_velocity,sizeof(s.angular_velocity));
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2 && s.ms-record_last.ms<50) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g %.7g\n",
        s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],s.axes[3],s.l2,s.r2,
        s.orientation[0],s.orientation[1],s.orientation[2],s.orientation[3],
        s.acceleration[0],s.acceleration[1],s.acceleration[2],
        s.angular_velocity[0],s.angular_velocity[1],s.angular_velocity[2]);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("BB_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0; char line[512];
            while (f && fgets(line,sizeof(line),f)) {
                memset(&s,0,sizeof(s));
                const int fields=sscanf(line,"%u %u %u %u %u %u %u %u %f %f %f %f %f %f %f %f %f %f",
                    &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],
                    &s.orientation[0],&s.orientation[1],&s.orientation[2],&s.orientation[3],
                    &s.acceleration[0],&s.acceleration[1],&s.acceleration[2],
                    &s.angular_velocity[0],&s.angular_velocity[1],&s.angular_velocity[2]);
                if (fields!=8 && fields!=18) continue;
                s.ms=v[0]; s.buttons=v[1]; s.l2=(uint8_t)v[6]; s.r2=(uint8_t)v[7];
                for (int i=0;i<4;++i) s.axes[i]=(uint8_t)v[i+2];
                s.has_motion=fields==18;
                int valid=1;
                for (int i=0;i<4;++i) if (!isfinite(s.orientation[i])) valid=0;
                for (int i=0;i<3;++i) if (!isfinite(s.acceleration[i]) || !isfinite(s.angular_velocity[i])) valid=0;
                if (!valid || (replay_count && s.ms<replay[replay_count-1].ms)) continue;
                if (replay_count==cap) {
                    size_t next=cap ? cap*2 : 1024;
                    if (next>1200000) break;
                    PadSample *grown=realloc(replay,next*sizeof(*replay));
                    if (!grown) break;
                    replay=grown; cap=next;
                }
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
    if (s->has_motion) {
        memcpy(d->orientation,s->orientation,sizeof(d->orientation));
        memcpy(d->acceleration,s->acceleration,sizeof(d->acceleration));
        memcpy(d->angular_velocity,s->angular_velocity,sizeof(d->angular_velocity));
    }
}
static void sample(PadData *d) {
    sample_host(d);
    if (bbgpu_overlay_captures_input()) {
        BbMouseMotion discarded; bbgpu_mouse_motion_read(&discarded);
        memset(&mouse_pose,0,sizeof(mouse_pose));
        return;
    }
    read_inject();
    sample_mouse_motion(d);
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.touch_side>=0) touch_click(d,injected.touch_side);
    else if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) touch_click(d,0);
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    record_sample(d);
    runtime_debug_input(d->buttons,d->left_x,d->left_y,d->right_x,d->right_y,d->l2,d->r2,d->touch_count);
}

static ABI int32_t pad_init(void) { host_lock(&lock); initialized=1; host_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    host_lock(&lock);
    int already=opened; opened=1;
    host_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    host_lock(&lock);
    sample(data); ++reads;
    host_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    host_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    host_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }
static ABI int32_t pad_motion_state(int32_t handle,uint8_t enabled) {
    if (pad_ok_handle(handle)) return ERR_INVALID_HANDLE;
    if (enabled>1) return ERR_INVALID_ARG;
    host_lock(&lock); motion_enabled=enabled; host_unlock(&lock);
    printf("Runtime: pad motion sensors %s\n",enabled ? "enabled" : "disabled");
    return 0;
}
static ABI int32_t pad_motion_reset(int32_t handle) {
    if (pad_ok_handle(handle)) return ERR_INVALID_HANDLE;
    host_lock(&lock); motion_reset_orientation(&mouse_pose); host_unlock(&lock);
    return 0;
}
static ABI int32_t pad_motion_deadband(int32_t handle,uint8_t enabled) {
    if (pad_ok_handle(handle)) return ERR_INVALID_HANDLE;
    if (enabled>1) return ERR_INVALID_ARG;
    host_lock(&lock); motion_deadband=enabled; host_unlock(&lock);
    return 0;
}
/* OrbisPadLightBarParam: RGB bytes plus one reserved byte. A keyboard pad
 * retains the logical colour; physical LEDs are used only when supported. */
static ABI int32_t pad_lightbar(int32_t handle,const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    host_lock(&lock);
    memcpy(lightbar,param,3);
    SDL_Gamepad *g=current_gamepad();
    if (g && SDL_GetBooleanProperty(SDL_GetGamepadProperties(g),SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN,false))
        SDL_SetGamepadLED(g,param[0],param[1],param[2]);
    host_unlock(&lock); return 0;
}

static const RuntimeExport exports[]={
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadSetLightBar",pad_lightbar},
    {"scePadResetOrientation",pad_motion_reset},
    {"scePadSetAngularVelocityDeadbandState",pad_motion_deadband}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_motion_state},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
