#include "fa/hle/services.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace fa::hle {

namespace {
// FA_AUDIO_STATS=<seconds> (or FA_PROFILE=<seconds>): one [audio] line per
// interval with the render driver and XMA decoder rates, so a log shows
// whether the game is feeding sound and whether the device is starving.
void audio_stats_reporter(int secs) {
    Services& s = services();
    auto prev = s.audio.take_stats();
    auto xprev = s.xma.stats();
    auto t_prev = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - t_prev).count();
        t_prev = now;
        auto a = s.audio.take_stats();
        auto x = s.xma.stats();
        auto db = [](double v) { return v > 0 ? 20.0 * std::log10(v) : -120.0; };
        uint64_t dev = a.device_samples - prev.device_samples;
        double under = dev ? 100.0 * double(a.underrun_samples - prev.underrun_samples) / double(dev) : 0.0;
        double rms = a.level_samples ? std::sqrt(a.sum_sq / double(a.level_samples)) : 0.0;
        std::fprintf(stderr,
                     "[audio] callbacks %.0f/s, frames %.0f/s (%.0f/s needed), silent %llu, dropped %llu, "
                     "underrun %.1f%%, peak %.1f dBFS, rms %.1f dBFS | xma: %d contexts, kicks %.0f/s, "
                     "frames %.0f/s, bad %llu, errors %llu, passes %.0f/s, worker loops %.0f/s | "
                     "device: clipped %llu, max callback gap %.1f ms, min queued %d samples\n",
                     double(a.callbacks - prev.callbacks) / dt, double(a.frames - prev.frames) / dt,
                     double(AudioSystem::kSampleRate) / AudioSystem::kFrameSamples,
                     (unsigned long long)(a.silent_frames - prev.silent_frames),
                     (unsigned long long)(a.dropped - prev.dropped), under, db(a.peak), db(rms), x.allocated,
                     double(x.kicks - xprev.kicks) / dt, double(x.frames - xprev.frames) / dt,
                     (unsigned long long)(x.bad_frames - xprev.bad_frames),
                     (unsigned long long)(x.decode_errors - xprev.decode_errors), double(x.passes - xprev.passes) / dt,
                     double(x.loops - xprev.loops) / dt, (unsigned long long)(a.clipped_samples - prev.clipped_samples),
                     a.max_gap_ms, a.min_queued == (1 << 30) ? -1 : a.min_queued);
        std::fflush(stderr);
        prev = a;
        xprev = x;
    }
}
}  // namespace

Services& services() {
    static Services s;
    return s;
}

bool init_services(const ServicesConfig& in, const GuestHooks& hooks, std::string& err) {
    Services& s = services();
    s.config = in;
    if (s.config.data_dir.empty()) s.config.data_dir = default_data_dir();
    if (s.config.config_dir.empty()) s.config.config_dir = default_config_dir();

    if (!s.vfs.mount_defaults(s.config.game_path, s.config.data_dir, err)) return false;
    if (!s.input.init(s.config.config_dir))
        std::fprintf(stderr, "[hle] controllers unavailable, keyboard only\n");
    // Until a window thread takes over pumping (set_self_pump(false) + pump()
    // each frame), XamInputGetState refreshes the state itself.
    s.input.set_self_pump(true);
    s.audio.init(hooks, !s.config.audio_output);
    if (!s.xma.init(hooks)) std::fprintf(stderr, "[hle] XMA decoder could not allocate its context array\n");
    if (const char* dump = std::getenv("FA_AUDIO_DUMP")) s.audio.start_dump(dump);
    const char* st = std::getenv("FA_AUDIO_STATS");
    if (!st) st = std::getenv("FA_PROFILE");
    if (st) {
        int secs = std::atoi(st);
        std::thread(audio_stats_reporter, secs > 0 ? secs : 5).detach();
    }
    return true;
}

void shutdown_services() {
    Services& s = services();
    s.audio.shutdown();
    s.xma.shutdown();
    s.input.shutdown();
}

}  // namespace fa::hle
