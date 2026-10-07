// Mouse events are produced by the window thread; guest pad reads drain deltas.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <SDL3/SDL.h>
#include "../bbgpu.h"

class BbMouseMotionInput {
public:
    bool Active() { std::scoped_lock lock{mutex}; return state.active != 0; }
    bool TouchActive() { std::scoped_lock lock{mutex}; return state.touch_active != 0; }
    bool Captured() { std::scoped_lock lock{mutex}; return state.active || state.touch_active; }
    void Release() {
        std::scoped_lock lock{mutex};
        ReleaseLocked();
    }
    void Read(BbMouseMotion* out) {
        std::scoped_lock lock{mutex};
        *out = state;
        if (touch_count) {
            const auto& touch = touches[touch_head];
            out->touch_down = touch.down;
            out->touch_click = touch.click;
            out->touch_x = touch.x;
            out->touch_y = touch.y;
            out->touch_id = touch.id;
            touch_head = (touch_head + 1) % touches.size();
            --touch_count;
        }
        state.dx = state.dy = 0;
    }
    // allowed means this window is focused and neither IME nor overlay owns input.
    void Event(const SDL_Event& event, SDL_WindowID window, bool allowed) {
        std::scoped_lock lock{mutex};
        if (!allowed) {
            ReleaseLocked();
            return;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.windowID == window && !event.key.repeat) {
            if (event.key.key == SDLK_F6) {
                const bool enabled = !state.active;
                ReleaseLocked();
                state.active = enabled;
                ++state.reset;
            } else if (event.key.key == SDLK_F8) {
                const bool enabled = !state.touch_active;
                ReleaseLocked();
                state.touch_active = enabled;
                touch_x = 960.f;
                touch_y = 471.f;
                state.touch_x = 960;
                state.touch_y = 471;
                ++state.reset;
            } else if (event.key.key == SDLK_ESCAPE) {
                ReleaseLocked();
            } else if (state.touch_active && event.key.key == SDLK_F7) {
                const bool enabled = state.touch_active;
                ReleaseLocked();
                state.touch_active = enabled;
                touch_x = 960.f; touch_y = 471.f;
                state.touch_x = 960; state.touch_y = 471;
            } else if (state.active && event.key.key == SDLK_F7) {
                state.dx = state.dy = 0;
                ++state.reset;
            }
        }
        if (state.touch_active) {
            if (event.type == SDL_EVENT_MOUSE_MOTION && event.motion.windowID == window &&
                std::isfinite(event.motion.xrel) && std::isfinite(event.motion.yrel)) {
                touch_x = std::clamp(touch_x + event.motion.xrel * 2.f, 0.f, 1919.f);
                touch_y = std::clamp(touch_y + event.motion.yrel * 2.f, 0.f, 942.f);
                state.touch_x = static_cast<uint16_t>(std::lround(touch_x));
                state.touch_y = static_cast<uint16_t>(std::lround(touch_y));
                if (state.touch_down) QueueTouch(true);
            } else if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
                       event.button.windowID == window &&
                       (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)) {
                const bool was_down = state.touch_down;
                const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                if (event.button.button == SDL_BUTTON_LEFT) touch_left = down;
                else touch_right = down;
                state.touch_down = touch_left || touch_right;
                state.touch_click = touch_right;
                if (!was_down && state.touch_down) state.touch_id = next_touch_id++ & 127;
                QueueTouch(false);
            }
            return;
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
    struct TouchSample {
        uint32_t down, click;
        uint16_t x, y;
        uint8_t id;
        bool movement;
    };
    // Coalesce consecutive motion events, but preserve contact/press/release edges and the
    // endpoint of a swipe completed before the guest next samples its controller.
    void QueueTouch(bool movement) {
        const TouchSample sample{state.touch_down, state.touch_click, state.touch_x,
                                 state.touch_y, state.touch_id, movement};
        if (movement && touch_count) {
            auto& last = touches[(touch_head + touch_count - 1) % touches.size()];
            if (last.movement && last.id == sample.id && last.down == sample.down && last.click == sample.click) {
                last = sample;
                return;
            }
        }
        if (touch_count == touches.size()) {
            // Bounded overflow: drop oldest queued samples; the newest release must survive.
            touch_head = (touch_head + 1) % touches.size();
            --touch_count;
        }
        touches[(touch_head + touch_count) % touches.size()] = sample;
        ++touch_count;
    }
    void ReleaseLocked() {
        state.active = state.left = state.touch_active = state.touch_down = state.touch_click = 0;
        state.dx = state.dy = 0;
        touch_left = touch_right = false;
        touch_head = touch_count = 0;
    }
    std::mutex mutex;
    BbMouseMotion state{};
    std::array<TouchSample, 32> touches{};
    size_t touch_head{}, touch_count{};
    float touch_x = 960.f, touch_y = 471.f;
    bool touch_left{}, touch_right{};
    uint8_t next_touch_id{};
};
