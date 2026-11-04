// fullauto: runs the recompiled Full Auto executable.
//
//   fullauto <game.iso>
//   fullauto <default.xex> <game.iso | extracted game folder>
//
// With the Vulkan presenter built in, the process main thread owns the game window (SDL events,
// presentation) and the guest main thread runs on its own host thread. FA_HEADLESS=1 skips the
// window. FA_FULLSCREEN=1 starts fullscreen (F11 or Alt+Enter toggles), FA_VSYNC=0 turns vsync
// off, FA_VK_VALIDATION=1 loads the Vulkan validation layer, FA_PRESENT picks what is shown
// (see fa/gpu/frame_backend.hpp).
#include "fa/rt/hle_hooks.hpp"
#include "fa/rt/crash.hpp"
#include "fa/rt/loader.hpp"
#include "fa/rt/memory.hpp"
#include "fa/rt/timebase.hpp"
#include "fa/xdvdfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <chrono>
#include <fstream>
#include <iterator>

#ifdef FA_WITH_PRESENTER
#include "fa/gpu/presenter.hpp"
#include "fa/hle/services.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/video.hpp"
#include <SDL.h>
#include <pthread.h>
#ifdef FA_WITH_RENDERER
#include "fa/gpu/renderer.hpp"
#endif
#endif

using namespace fa::rt;

static bool read_game(const std::string& path, std::vector<uint8_t>& xex) {
    if (path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".iso") == 0) {
        fa::XdvdfsImage iso;
        fa::IsoEntry e;
        if (!iso.open(path) || !iso.find("default.xex", e) || !iso.read(e, xex)) {
            std::fprintf(stderr, "cannot read default.xex from %s: %s\n", path.c_str(), iso.error().c_str());
            return false;
        }
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    xex.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: fullauto <game.iso> | fullauto <default.xex> <game.iso | folder>\n"); return 2; }
    install_crash_handler();
    install_profiler();
    install_memory_guard();
#ifdef FA_BUILD_COMMIT
    std::fprintf(stderr, "[build] commit %s ppc %s\n", FA_BUILD_COMMIT, FA_BUILD_PPC);
#endif
    std::vector<uint8_t> xex;
    std::fprintf(stderr, "[boot] reading game\n");
    if (!read_game(argv[1], xex)) return 1;
    std::fprintf(stderr, "[boot] reserving guest memory\n");
    if (!g_mem.init()) return 1;
    install_write_watch();
    // XMA/APU register page: plain memory the audio layer polls.
    g_mem.commit_fixed(0x7FEA0000, 0x10000);
    // GPU register page: plain memory for now (reads as zero) until the GPU layer takes it over.
    g_mem.commit_fixed(0x7FC80000, 0x80000);
    // D1MODE_VIEWPORT_SIZE (reg 0x1961): 1280x720. D3D divides by the height.
    st32(0x7FC80000 + 0x1961 * 4, (1280u << 16) | 720u);

    // File system (game:, d:, hdd:, cache:), controllers, audio output and XMA.
    // FA_NO_AUDIO=1 runs the audio clock silently (headless/CI).
    fa::hle::ServicesConfig hle_cfg;
    hle_cfg.game_path = argv[1];
    if (argc > 2) hle_cfg.game_path = argv[2];  // fullauto default.xex <game.iso | extracted folder>
    else if (!(hle_cfg.game_path.size() > 4 &&
               strcasecmp(hle_cfg.game_path.c_str() + hle_cfg.game_path.size() - 4, ".iso") == 0)) {
        // A bare default.xex: serve game:\ from the folder it sits in.
        auto slash = hle_cfg.game_path.find_last_of('/');
        hle_cfg.game_path = slash == std::string::npos ? "." : hle_cfg.game_path.substr(0, slash);
    }
    hle_cfg.audio_output = !std::getenv("FA_NO_AUDIO");
    std::fprintf(stderr, "[boot] starting file, input, audio services\n");
    std::string hle_err;
    if (!fa::hle::init_services(hle_cfg, make_runtime_hooks(), hle_err)) {
        std::fprintf(stderr, "cannot mount game files from %s: %s\n", hle_cfg.game_path.c_str(), hle_err.c_str());
        return 1;
    }

    std::fprintf(stderr, "[boot] loading XEX\n");
    LoadedImage img;
    if (!load_xex(xex, img)) return 1;
    g_kernel.set_tls_info(img.tls);
    // mftb must tick at 50 MHz, not at the host TSC rate (FA_TIMEBASE=off to compare).
    install_guest_timebase();
#ifdef FA_WITH_PRESENTER
    if (!std::getenv("FA_HEADLESS")) {
        fa::gpu::Presenter window;
        fa::gpu::Presenter::Options opt;
        opt.title = "Full Auto";
        opt.fullscreen = std::getenv("FA_FULLSCREEN") != nullptr;
        opt.vsync = !(std::getenv("FA_VSYNC") && !std::strcmp(std::getenv("FA_VSYNC"), "0"));
        opt.validation = std::getenv("FA_VK_VALIDATION") != nullptr;
        std::string err;
        std::fprintf(stderr, "[boot] opening the game window\n");
        if (window.Init(opt, err)) {
            auto& backend = gpu_frame_backend();
            backend.SetEnabled(true);
            backend.SetAsync(true);  // decode frames off the GPU thread
#ifdef FA_WITH_RENDERER
            // Real draws: the Vulkan renderer resolves into guest memory and forwards every call
            // to the frame backend, which shows the front buffer as before. FA_RENDERER=0 = off.
            static fa::gpu::VulkanRenderer* renderer = nullptr;
            if (!(std::getenv("FA_RENDERER") && !std::strcmp(std::getenv("FA_RENDERER"), "0"))) {
                auto* r = new fa::gpu::VulkanRenderer(gpu_memory(), &backend);
                fa::gpu::VulkanRenderer::Options ropt;
                ropt.validation = opt.validation;
                if (const char* d = std::getenv("FA_DUMP_SHADERS")) ropt.dumpShaders = d;
                if (const char* t = std::getenv("FA_RENDER_TRACE")) ropt.traceDraws = std::atoi(t);
                if (const char* d = std::getenv("FA_VK_DEVICE")) ropt.deviceIndex = std::atoi(d);
                std::string rerr;
                if (r->Init(ropt, rerr)) {
                    set_gpu_backend(r);
                    renderer = r;
                    std::fprintf(stderr, "[boot] renderer on %s\n", r->DeviceName().c_str());
                } else {
                    std::fprintf(stderr, "[boot] renderer unavailable (%s): showing the front buffer only\n", rerr.c_str());
                    delete r;
                }
            }
#endif
            // The guest main thread gets a large host stack (it used to run on the process main
            // thread, whose stack grows on demand).
            static uint32_t entry, stack;
            entry = img.entry;
            stack = img.stack_size;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 64u << 20);
            pthread_t tid;
            int prc = pthread_create(&tid, &attr, [](void*) -> void* {
                std::fprintf(stderr, "entering 0x%08X (stack 0x%X)\n", entry, stack);
                uint32_t rc = g_kernel.run_main_thread(entry, stack);
                std::fprintf(stderr, "guest main returned 0x%08X\n", rc);
                std::fflush(stderr);
                std::_Exit(int(rc & 0xFF));
            }, nullptr);
            pthread_attr_destroy(&attr);
            if (prc != 0) { std::fprintf(stderr, "fatal: cannot start the guest main thread\n"); return 1; }
            pthread_detach(tid);
            auto& input = fa::hle::services().input;
            auto last = std::chrono::steady_clock::now();
            uint64_t lastSwaps = 0;
            std::string source = "waiting for the first swap";
            fa::gpu::RunWindowLoop(window, backend.Mailbox(),
                [&](const SDL_Event& e) { input.handle_event(e); },
                [&] {
                    auto now = std::chrono::steady_clock::now();
                    double secs = std::chrono::duration<double>(now - last).count();
                    uint64_t swaps = backend.GetStats().swaps.load();
                    char b[256];
                    int n = std::snprintf(b, sizeof b, "Full Auto - %.0f swaps/s - %u draws/frame - %llu resolves", (swaps - lastSwaps) / secs,
                                  backend.GetStats().drawsLastFrame.load(), (unsigned long long)backend.GetStats().resolves.load());
#ifdef FA_WITH_RENDERER
                    if (renderer && n > 0 && n < int(sizeof b)) {
                        const auto& rs = renderer->GetStats();
                        std::snprintf(b + n, sizeof b - n, " - rendered %llu, skipped %llu, shader errors %llu",
                                      (unsigned long long)rs.draws.load(), (unsigned long long)rs.drawsSkipped.load(),
                                      (unsigned long long)rs.shaderErrors.load());
                    }
#else
                    (void)n;
#endif
                    last = now;
                    lastSwaps = swaps;
                    return std::string(b);
                });
            std::fprintf(stderr, "[window] closed\n");
            std::fflush(stderr);
            std::_Exit(0);
        }
        std::fprintf(stderr, "[window] %s; running headless\n", err.c_str());
    }
#endif
    std::fprintf(stderr, "entering 0x%08X (stack 0x%X)\n", img.entry, img.stack_size);
    uint32_t rc = g_kernel.run_main_thread(img.entry, img.stack_size);
    std::fprintf(stderr, "guest main returned 0x%08X\n", rc);
    std::fflush(stderr);
    std::_Exit(int(rc & 0xFF));
}
