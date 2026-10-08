// bbport: SDL3 window for the Vulkan swapchain (X11, Wayland or Win32).
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_mouse_motion.h"
#include "bbport_debug_hotkey.h"
#include "bbport_diagnostics.h"
#include "bbport_benchmark_control.h"

extern "C" void runtime_debug_mark(void);
extern "C" int bbgpu_toggle_performance_recording(void);

static BbMouseMotionInput mouse_motion;
extern "C" void bbgpu_mouse_motion_read(BbMouseMotion* state) {
    mouse_motion.Read(state);
}

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           fullscreen ? fullscreen[0] == '1' : BbSettings::Get().fullscreen.load());
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
    UpdateTextTitle();
}

WindowSDL::~WindowSDL() {
    mouse_motion.Release();
    SDL_SetWindowRelativeMouseMode(window, false);
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
        : base_title + (mouse_touch
            ? " | Mouse touchpad ON: Left drag swipe, Right click press, F7 center, F8/Esc release"
            : mouse_relative
            ? " | Mouse motion ON: move/ shake, Left click spray, F7 center, F6/Esc release"
            : " | F6: mouse motion | F8: mouse touchpad");
    SDL_SetWindowTitle(window, title.c_str());
}

void WindowSDL::UpdateMouseMotion() {
    const bool allowed = !text_active && !BbOverlay::CapturesInput() &&
                         (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
    if (!allowed) mouse_motion.Release();
    const bool active = mouse_motion.Captured();
    const bool touch = mouse_motion.TouchActive();
    if (active == mouse_relative && touch == mouse_touch) return;
    if (active != mouse_relative && !SDL_SetWindowRelativeMouseMode(window, active)) {
        LOG_WARNING(Frontend, "Mouse motion capture failed: {}", SDL_GetError());
        mouse_motion.Release();
        SDL_SetWindowRelativeMouseMode(window, false);
        mouse_relative = false;
        mouse_touch = false;
    } else {
        mouse_relative = active;
        mouse_touch = touch;
    }
    LOG_INFO(Frontend, "Mouse motion {}", mouse_relative ? "enabled" : "released");
    UpdateTextTitle();
}

bool WindowSDL::PollEvents() {
    static BbBenchmark::Control benchmark;
    if(benchmark.Poll([](bool state){return BbDiagnostics::SetRecording(state);},[] {
        if(const char* dir=std::getenv("BB_DEBUG_DIR")) std::ofstream(std::string(dir)+"/capture-next").put('1');
    })) is_open=false;
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    UpdateMouseMotion();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Receive short presses even if the guest stops polling pads in a
        // cutscene. Handle before the overlay or text input can consume them.
        if (bb_debug_hotkey(&event, SDL_GetWindowID(window), runtime_debug_mark)) {
            BbOverlay::NotifyDebugMark();
            continue;
        }
        int recording_active=0;
        if (bb_performance_hotkey(&event, SDL_GetWindowID(window),
                                  bbgpu_toggle_performance_recording, &recording_active)) {
            BbOverlay::NotifyPerformanceRecording(recording_active != 0);
            continue;
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            UpdateMouseMotion();
            continue;
        }
        const bool allowed = !text_active && !BbOverlay::CapturesInput() &&
                             (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && event.window.windowID == SDL_GetWindowID(window)) {
            mouse_motion.Release();
        } else {
            mouse_motion.Event(event, SDL_GetWindowID(window), allowed);
        }
        UpdateMouseMotion();
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            // F11: borderless fullscreen at the desktop size, or back to the window.
            // Shift+F11 preserves the previous fullscreen shortcut; plain F11 controls
            // the bounded asynchronous performance recording.
            if (event.key.key == SDLK_F11 && !event.key.repeat &&
                (event.key.mod & SDL_KMOD_SHIFT)) {
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            }
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            mouse_motion.Release();
            UpdateMouseMotion();
            is_open = false;
            break;
        default:
            break;
        }
    }
    return is_open;
}

} // namespace Frontend
