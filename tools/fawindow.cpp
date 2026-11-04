// fawindow: opens the game window and shows a moving test pattern through the same
// FrameMailbox -> Presenter path the game uses. Checks SDL2 + Vulkan without the game.
//
//   fawindow              run until the window is closed
//   fawindow --frames N   quit after N frames (used by CI / headless checks)
#include "fa/gpu/presenter.hpp"

#include <SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char** argv) {
    long limit = 0;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) limit = std::atol(argv[++i]);

    fa::gpu::Presenter p;
    fa::gpu::Presenter::Options o;
    o.title = "fawindow";
    o.validation = std::getenv("FA_VK_VALIDATION") != nullptr;
    std::string err;
    if (!p.Init(o, err)) { std::fprintf(stderr, "fawindow: %s\n", err.c_str()); return 1; }

    fa::gpu::FrameMailbox mb;
    std::atomic<bool> stop{false};
    std::atomic<long> made{0};
    // Producer thread at 60 Hz, like the game's GPU thread.
    std::thread producer([&] {
        auto next = std::chrono::steady_clock::now();
        for (uint64_t n = 1; !stop; n++) {
            fa::gpu::Frame f;
            f.width = 1280; f.height = 720; f.seq = n; f.source = "test pattern";
            f.rgba.resize(size_t(f.width) * f.height * 4);
            for (uint32_t y = 0; y < f.height; y++)
                for (uint32_t x = 0; x < f.width; x++) {
                    uint8_t* px = &f.rgba[(size_t(y) * f.width + x) * 4];
                    bool bar = ((x + n * 8) / 80) % 2;
                    px[0] = uint8_t(x * 255 / f.width);
                    px[1] = uint8_t(y * 255 / f.height);
                    px[2] = bar ? 200 : 40;
                    px[3] = 255;
                }
            mb.Publish(std::move(f));
            made = long(n);
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);
        }
    });
    long shown = 0;
    auto start = std::chrono::steady_clock::now();
    fa::gpu::RunWindowLoop(p, mb, [&](const SDL_Event&) {}, [&] {
        shown = made.load();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        char b[128];
        std::snprintf(b, sizeof b, "fawindow - %s - %.0f frames/s", p.DeviceName().c_str(), shown / secs);
        if (limit && shown >= limit) {
            SDL_Event q{};
            q.type = SDL_QUIT;
            SDL_PushEvent(&q);
        }
        return std::string(b);
    });
    stop = true;
    producer.join();
    std::fprintf(stderr, "fawindow: ok, %ld frames produced\n", made.load());
    return 0;
}
