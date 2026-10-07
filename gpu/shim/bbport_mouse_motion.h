// Mouse events are produced by the window thread; guest pad reads drain deltas.
#pragma once
#include <algorithm>
#include <cmath>
#include <mutex>
#include <SDL3/SDL.h>
#include "../bbgpu.h"

class BbMouseMotionInput {
public:
    bool Active() { std::scoped_lock lock{mutex}; return state.active != 0; }
    void Release() {
        std::scoped_lock lock{mutex};
        state.active = state.left = 0;
        state.dx = state.dy = 0;
    }
    void Read(BbMouseMotion* out) {
        std::scoped_lock lock{mutex};
        *out = state;
        state.dx = state.dy = 0;
    }
    // allowed means this window is focused and neither IME nor overlay owns input.
    void Event(const SDL_Event& event, SDL_WindowID window, bool allowed) {
        std::scoped_lock lock{mutex};
        if (!allowed) {
            state.active = state.left = 0;
            state.dx = state.dy = 0;
            return;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.windowID == window && !event.key.repeat) {
            if (event.key.key == SDLK_F6) {
                state.active = !state.active;
                state.left = 0;
                state.dx = state.dy = 0;
                ++state.reset;
            } else if (event.key.key == SDLK_ESCAPE) {
                state.active = state.left = 0;
                state.dx = state.dy = 0;
            } else if (state.active && event.key.key == SDLK_F7) {
                state.dx = state.dy = 0;
                ++state.reset;
            }
        }
        if (!state.active) return;
        if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.windowID == window &&
            std::isfinite(event.motion.xrel) && std::isfinite(event.motion.yrel)) {
            state.dx = std::clamp(state.dx + event.motion.xrel, -4096.f, 4096.f);
            state.dy = std::clamp(state.dy + event.motion.yrel, -4096.f, 4096.f);
        } else if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                   event.button.windowID == window && event.button.button == SDL_BUTTON_LEFT) {
            state.left = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        }
    }
private:
    std::mutex mutex;
    BbMouseMotion state{};
};
