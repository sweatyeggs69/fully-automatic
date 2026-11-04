#pragma once
// Window + Vulkan swapchain that shows RGBA8 frames (from FrameBackend). SDL2 owns the window
// (it already provides controllers, keyboard and audio). Must be created and driven from one
// thread, normally the process main thread, which also pumps SDL events.
//
// Frames are uploaded to an image and blitted (scaled, aspect kept, letterboxed) into the
// swapchain, so no shaders are needed for presentation.
#include <functional>
#include <memory>
#include <string>

#include "fa/gpu/frame_backend.hpp"

struct SDL_Window;
union SDL_Event;

namespace fa::gpu {

class Presenter {
public:
    struct Options {
        std::string title = "Full Auto";
        int width = 1280, height = 720;
        bool fullscreen = false;
        bool vsync = true;
        bool validation = false;  // VK_LAYER_KHRONOS_validation when installed
    };

    Presenter();
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    bool Init(const Options& opt, std::string& err);
    // Shows `f` (uploads it), or redraws the last frame when f is null.
    // Returns false only on an unrecoverable Vulkan error.
    bool Present(const Frame* f);
    // Call on SDL_WINDOWEVENT_SIZE_CHANGED and friends.
    void Resized();
    void ToggleFullscreen();
    void SetTitle(const std::string& title);
    SDL_Window* Window() const;
    std::string DeviceName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Window thread main loop: pumps SDL events (each one is passed to on_event as well), shows
// every new frame from `frames`, redraws at least 10 times a second, and refreshes the window
// title from title() once a second. Returns when the window is closed (or Vulkan fails).
void RunWindowLoop(Presenter& p, FrameMailbox& frames, const std::function<void(const SDL_Event&)>& on_event,
                   const std::function<std::string()>& title);

}  // namespace fa::gpu
