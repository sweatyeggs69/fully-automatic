#pragma once
// GPU-thread timing (debug aid), on when FA_PROFILE is set (same switch as the runtime profiler).
//
// Sections time where the command processor thread spends its wall time. Every FA_PROFILE
// seconds (default 10) the swap path prints one line with swaps per second, how busy the GPU
// thread was, and milliseconds per frame for each section. Sections nest (texture work happens
// inside draws), so they do not add up to the busy time.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace fa::gpu::prof {

enum Section : int {
    kPump,         // command processor busy (everything below happens inside it)
    kDraw,         // renderer Draw, total
    kTexCheck,     // hashing texture bytes to see whether they changed
    kTexUpload,    // decoding and staging changed textures
    kShader,       // translating and compiling shaders
    kPipeline,     // creating pipelines
    kResolve,      // renderer Resolve, total
    kResolveWait,  // waiting for the GPU before a resolve can read the render target
    kResolveCpu,   // converting and tiling resolved pixels into guest memory
    kOtherWait,    // other GPU waits (swap, batch full)
    kFrameDecode,  // decoding the front buffer for the window
    kCount
};

inline const char* const kNames[kCount] = {"busy",         "draw",        "tex check",  "tex upload",
                                           "shader",       "pipeline",    "resolve",    "resolve wait",
                                           "resolve cpu",  "other wait",  "frame decode"};

inline const bool g_enabled = std::getenv("FA_PROFILE") != nullptr;
inline std::atomic<uint64_t> g_ns[kCount];
inline std::atomic<uint64_t> g_count[kCount];

inline uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Scope {
    Section s;
    uint64_t t0;
    explicit Scope(Section sec) : s(sec), t0(g_enabled ? NowNs() : 0) {}
    ~Scope() {
        if (!g_enabled) return;
        g_ns[s].fetch_add(NowNs() - t0, std::memory_order_relaxed);
        g_count[s].fetch_add(1, std::memory_order_relaxed);
    }
};

// Called once per swap on the GPU thread; prints and resets when the interval has passed.
inline void OnSwap() {
    if (!g_enabled) return;
    static const int secs = [] {
        int v = std::atoi(std::getenv("FA_PROFILE"));
        return v > 0 ? v : 10;
    }();
    static uint64_t start = NowNs();
    static uint64_t swaps = 0;
    swaps++;
    uint64_t now = NowNs();
    if (now - start < uint64_t(secs) * 1000000000ull) return;
    double dt = double(now - start) / 1e9;
    uint64_t ns[kCount], n[kCount];
    for (int i = 0; i < kCount; i++) {
        ns[i] = g_ns[i].exchange(0, std::memory_order_relaxed);
        n[i] = g_count[i].exchange(0, std::memory_order_relaxed);
    }
    char line[1024];
    int len = std::snprintf(line, sizeof line, "[gpuprof] %.1f s: %.1f swaps/s, gpu thread busy %.0f%% | ms per frame:", dt,
                            double(swaps) / dt, 100.0 * double(ns[kPump]) / 1e9 / dt);
    for (int i = 1; i < kCount && len > 0 && len < int(sizeof line); i++) {
        if (!n[i]) continue;
        len += std::snprintf(line + len, sizeof line - len, " %s %.2f (x%.1f)", kNames[i], double(ns[i]) / 1e6 / double(swaps),
                             double(n[i]) / double(swaps));
    }
    std::fprintf(stderr, "%s\n", line);
    start = now;
    swaps = 0;
}

}  // namespace fa::gpu::prof
