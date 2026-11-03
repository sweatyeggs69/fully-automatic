#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <SDL.h>

#include "fa/hle/audio.hpp"

namespace fa::hle {

namespace {
constexpr int kFrameFloats = AudioSystem::kFrameSamples * 2;  // one stereo frame
constexpr int kRingFrames = AudioSystem::kMaxQueuedFrames * 2;

float be_float(const uint8_t* p) {
    uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void store_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
}  // namespace

// Per-client ring of stereo frames; lives next to the client array.
struct ClientRing {
    std::vector<float> data = std::vector<float>(size_t(kRingFrames) * kFrameFloats);
    size_t read = 0;   // in floats
    size_t size = 0;   // in floats
};
static ClientRing g_rings[AudioSystem::kMaxClients];

AudioSystem::AudioSystem() = default;
AudioSystem::~AudioSystem() { shutdown(); }

void AudioSystem::downmix_frame(const float* in, float* out) {
    constexpr float c = 0.70710678f;
    const float* fl = in;
    const float* fr = in + kFrameSamples;
    const float* fc = in + 2 * kFrameSamples;
    const float* lfe = in + 3 * kFrameSamples;
    const float* bl = in + 4 * kFrameSamples;
    const float* br = in + 5 * kFrameSamples;
    for (int i = 0; i < kFrameSamples; ++i) {
        float common = c * fc[i] + 0.5f * lfe[i];
        out[2 * i] = std::clamp(fl[i] + common + c * bl[i], -1.0f, 1.0f);
        out[2 * i + 1] = std::clamp(fr[i] + common + c * br[i], -1.0f, 1.0f);
    }
}

bool AudioSystem::init(const GuestHooks& hooks, bool disable_output) {
    hooks_ = hooks;
    initialised_ = true;
    if (!disable_output && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want{}, have{};
        want.freq = kSampleRate;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = kFrameSamples * 2;
        want.callback = &AudioSystem::sdl_callback;
        want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (device_) {
            SDL_PauseAudioDevice(device_, 0);
            std::fprintf(stderr, "[audio] output: %s, %d Hz\n",
                         SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", have.freq);
            return true;
        }
        std::fprintf(stderr, "[audio] cannot open audio device: %s\n", SDL_GetError());
    }
    // No device: keep the game's audio clock running in silence.
    clock_running_ = true;
    clock_ = std::thread([this] { clock_main(); });
    return false;
}

void AudioSystem::shutdown() {
    if (!initialised_) return;
    for (int i = 0; i < kMaxClients; ++i)
        if (clients_[i].in_use) unregister_client(kDriverMagic | uint32_t(i));
    if (device_) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (clock_running_.exchange(false) && clock_.joinable()) clock_.join();
    initialised_ = false;
}

uint32_t AudioSystem::register_client(uint32_t callback, uint32_t callback_arg, uint32_t& driver_out) {
    std::lock_guard lk(clients_mu_);
    for (int i = 0; i < kMaxClients; ++i) {
        Client& c = clients_[i];
        if (c.in_use) continue;
        c.in_use = true;
        c.callback = callback;
        // The callback receives a pointer to a copy of its argument.
        c.wrapped_arg = hooks_.alloc ? hooks_.alloc(4, 4) : 0;
        if (c.wrapped_arg) store_be32(hooks_.host(c.wrapped_arg), callback_arg);
        {
            std::lock_guard ql(queue_mu_);
            g_rings[i].read = g_rings[i].size = 0;
        }
        {
            std::lock_guard cl(c.mu);
            c.credits = kMaxQueuedFrames;
        }
        c.running = true;
        c.thread = std::thread([this, i] { client_main(i); });
        driver_out = kDriverMagic | uint32_t(i);
        return 0;
    }
    return 0x8007000E;  // E_OUTOFMEMORY
}

uint32_t AudioSystem::unregister_client(uint32_t driver) {
    int i = int(driver & 0xFFFF);
    if ((driver & 0xFFFF0000) != kDriverMagic || i >= kMaxClients) return 0x80070057;  // E_INVALIDARG
    Client& c = clients_[i];
    std::thread t;
    {
        std::lock_guard lk(clients_mu_);
        if (!c.in_use) return 0x80070057;
        c.running = false;
        c.cv.notify_all();
        t = std::move(c.thread);
    }
    // Unregister may be called from inside the callback itself.
    if (t.joinable()) {
        if (t.get_id() == std::this_thread::get_id()) t.detach();
        else t.join();
    }
    std::lock_guard lk(clients_mu_);
    if (c.wrapped_arg && hooks_.free) hooks_.free(c.wrapped_arg);
    c.wrapped_arg = 0;
    c.callback = 0;
    c.in_use = false;
    return 0;
}

void AudioSystem::client_main(int index) {
    Client& c = clients_[index];
    if (hooks_.attach_thread) hooks_.attach_thread("XAudio render client");
    while (c.running) {
        {
            std::unique_lock lk(c.mu);
            c.cv.wait(lk, [&] { return c.credits > 0 || !c.running; });
            if (!c.running) break;
            --c.credits;
        }
        if (hooks_.invoke && c.callback) hooks_.invoke(c.callback, c.wrapped_arg);
    }
}

void AudioSystem::give_credit() {
    for (auto& c : clients_) {
        if (!c.in_use) continue;
        {
            std::lock_guard lk(c.mu);
            if (c.credits < kMaxQueuedFrames) ++c.credits;
        }
        c.cv.notify_one();
    }
}

uint32_t AudioSystem::submit_frame(uint32_t driver, uint32_t samples_guest) {
    int i = int(driver & 0xFFFF);
    if ((driver & 0xFFFF0000) != kDriverMagic || i >= kMaxClients || !clients_[i].in_use) return 0x80070057;
    submit_frame_host(i, hooks_.host(samples_guest));
    return 0;
}

void AudioSystem::submit_frame_host(int client, const uint8_t* be) {
    float planar[kChannels * kFrameSamples];
    for (int i = 0; i < kChannels * kFrameSamples; ++i) planar[i] = be_float(be + 4 * i);
    float stereo[kFrameFloats];
    downmix_frame(planar, stereo);
    std::lock_guard lk(queue_mu_);
    ClientRing& r = g_rings[client];
    const size_t cap = r.data.size();
    if (r.size + kFrameFloats > cap) {  // overflow: drop the oldest frame
        r.read = (r.read + kFrameFloats) % cap;
        r.size -= kFrameFloats;
    }
    size_t w = (r.read + r.size) % cap;
    for (int k = 0; k < kFrameFloats; ++k) r.data[(w + size_t(k)) % cap] = stereo[k];
    r.size += kFrameFloats;
}

void AudioSystem::consume(float* out, int frames) {
    // frames = stereo sample pairs requested by the device.
    std::memset(out, 0, sizeof(float) * size_t(frames) * 2);
    float vol = volume_.load();
    {
        std::lock_guard lk(queue_mu_);
        for (int ci = 0; ci < kMaxClients; ++ci) {
            if (!clients_[ci].in_use) continue;
            ClientRing& r = g_rings[ci];
            const size_t cap = r.data.size();
            size_t n = std::min<size_t>(r.size, size_t(frames) * 2);
            for (size_t k = 0; k < n; ++k) out[k] += r.data[(r.read + k) % cap] * vol;
            r.read = (r.read + n) % cap;
            r.size -= n;
        }
    }
    // One credit (one callback) per 256 samples of device time.
    period_pos_ += frames;
    while (period_pos_ >= kFrameSamples) {
        period_pos_ -= kFrameSamples;
        give_credit();
    }
}

void AudioSystem::sdl_callback(void* user, uint8_t* stream, int len) {
    auto* self = static_cast<AudioSystem*>(user);
    self->consume(reinterpret_cast<float*>(stream), len / int(sizeof(float) * 2));
}

void AudioSystem::clock_main() {
    using clk = std::chrono::steady_clock;
    std::vector<float> sink(size_t(kFrameSamples) * 2);
    auto period = std::chrono::nanoseconds(1000000000ll * kFrameSamples / kSampleRate);
    auto next = clk::now();
    while (clock_running_) {
        next += period;
        consume(sink.data(), kFrameSamples);
        std::this_thread::sleep_until(next);
    }
}

}  // namespace fa::hle
