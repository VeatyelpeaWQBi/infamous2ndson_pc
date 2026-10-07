/* Window-thread diagnostics: do not depend on guest pad reads or keyboard polling. */
#pragma once
#include <SDL3/SDL.h>

static inline int bb_debug_hotkey(const SDL_Event *event, SDL_WindowID window,
                                  void (*mark)(void)) {
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.windowID != window ||
        event->key.scancode != SDL_SCANCODE_F10 || event->key.repeat)
        return 0;
    mark();
    return 1;
}

static inline int bb_performance_hotkey(const SDL_Event *event, SDL_WindowID window,
                                        int (*toggle)(void), int *active) {
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.windowID != window ||
        event->key.scancode != SDL_SCANCODE_F11 || event->key.repeat ||
        (event->key.mod & SDL_KMOD_SHIFT))
        return 0;
    *active = toggle();
    return 1;
}
