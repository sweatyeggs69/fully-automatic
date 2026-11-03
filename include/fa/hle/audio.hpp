#pragma once
// Audio for the recompiled game.
//
// On the Xbox 360 the game's own (statically linked, so recompiled) XAudio /
// XACT code does all mixing in software. It talks to two pieces of the system
// that we provide here:
//
//  * The audio render driver (XAudioRegisterRenderDriverClient and friends).
//    The game registers a callback; whenever the output queue has room the
//    system calls it, and the callback submits one frame: 256 samples x 6
//    channels (FL FR C LFE BL BR), stored channel after channel, as big-endian
//    floats. AudioSystem queues those frames to an SDL2 audio device.
//
//  * The XMA hardware decoder. Compressed XMA audio (wave banks, music) is
//    decoded by "contexts": 64-byte structs in guest memory pointing at input
//    packets and an output ring buffer. XmaDecoder emulates it with FFmpeg's
//    WMA Pro decoder (XMA frames are WMA Pro frames in a different packet
//    wrapper), so only the stock system FFmpeg is needed.
//
// This layer is host-side: guest access goes through GuestHooks, which the
// runtime fills in (runtime/src/audio/xaudio_imports.cpp).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fa::hle {

struct GuestHooks {
    uint8_t* membase = nullptr;                                    // guest address 0
    std::function<uint32_t(uint32_t size, uint32_t align)> alloc;  // zeroed guest memory
    std::function<void(uint32_t addr)> free;
    std::function<void(const char* name)> attach_thread;           // make a host thread guest-capable
    std::function<void(uint32_t func, uint32_t arg)> invoke;       // run a guest function, r3 = arg
    std::function<uint8_t*(uint32_t phys)> phys_to_host;           // XMA buffer pointers are physical

    uint8_t* host(uint32_t guest) const { return membase + guest; }
    uint8_t* phys(uint32_t p) const { return phys_to_host ? phys_to_host(p) : membase + 0xA0000000u + (p & 0x1FFFFFFFu); }
};

// ------------------------------------------------------------ render driver

class AudioSystem {
public:
    static constexpr int kMaxClients = 8;
    static constexpr int kChannels = 6;
    static constexpr int kFrameSamples = 256;          // per channel
    static constexpr int kSampleRate = 48000;
    static constexpr uint32_t kDriverMagic = 0x41550000;  // 'AU' | index

    AudioSystem();
    ~AudioSystem();

    // Opens the SDL2 audio device. If that fails (or disable_output is set),
    // runs a silent clock instead so the game's audio callbacks still fire at
    // the right rate.
    bool init(const GuestHooks& hooks, bool disable_output = false);
    void shutdown();

    // XAudioRegisterRenderDriverClient: callback_ptr points at {callback, arg}
    // in guest memory. Returns 0 and writes the driver handle, or an error.
    uint32_t register_client(uint32_t callback, uint32_t callback_arg, uint32_t& driver_out);
    uint32_t unregister_client(uint32_t driver);
    // XAudioSubmitRenderDriverFrame: samples_guest points at 1536 BE floats.
    uint32_t submit_frame(uint32_t driver, uint32_t samples_guest);
    // Same, from host memory (BE floats). Exposed for tests and tools.
    void submit_frame_host(int client, const uint8_t* be_samples);

    void set_volume(float v) { volume_.store(v); }
    bool has_device() const { return device_ != 0; }

    // 5.1 -> stereo downmix of one frame (planar host floats in, interleaved
    // stereo out). Exposed for tests.
    static void downmix_frame(const float* planar6, float* stereo_out);

    static constexpr int kMaxQueuedFrames = 8;    // ~43 ms

private:
    struct Client {
        bool in_use = false;
        uint32_t callback = 0;
        uint32_t wrapped_arg = 0;
        std::thread thread;
        std::atomic<bool> running{false};
        // Free slots in the output queue: the client thread waits for one,
        // then calls the guest callback, which submits a frame.
        std::mutex mu;
        std::condition_variable cv;
        int credits = 0;
    };
    void client_main(int index);
    void clock_main();
    static void sdl_callback(void* user, uint8_t* stream, int len);
    void consume(float* out, int frames);
    void give_credit();

    GuestHooks hooks_;
    Client clients_[kMaxClients];
    std::mutex clients_mu_;

    // Interleaved stereo frames waiting for the device.
    std::mutex queue_mu_;
    std::vector<float> queue_;     // ring of kMaxQueuedFrames * 2 frames
    size_t q_read_ = 0, q_size_ = 0;
    uint32_t device_ = 0;
    int period_pos_ = 0;           // device samples into the current 256-sample period
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> clock_running_{false};
    std::thread clock_;
    bool initialised_ = false;
};

// ------------------------------------------------------------ XMA decoder

// Guest XMA_CONTEXT_DATA, decoded from / encoded to its big-endian form.
struct XmaContextData {
    uint32_t input_buffer_0_packet_count = 0;  // 12 bits
    uint32_t loop_count = 0;                   // 8
    uint32_t input_buffer_0_valid = 0;         // 1
    uint32_t input_buffer_1_valid = 0;         // 1
    uint32_t output_buffer_block_count = 0;    // 5 (256-byte blocks)
    uint32_t output_buffer_write_offset = 0;   // 5
    uint32_t input_buffer_1_packet_count = 0;  // 12
    uint32_t loop_subframe_start = 0;          // 2
    uint32_t loop_subframe_end = 0;            // 3
    uint32_t loop_subframe_skip = 0;           // 3
    uint32_t subframe_decode_count = 0;        // 4
    uint32_t subframe_skip_count = 0;          // 3
    uint32_t sample_rate = 0;                  // 2: 24k, 32k, 44.1k, 48k
    uint32_t is_stereo = 0;                    // 1
    uint32_t unk_dword_1_c = 0;                // 1
    uint32_t output_buffer_valid = 0;          // 1
    uint32_t input_buffer_read_offset = 0;     // 26 (bits)
    uint32_t unk_dword_2 = 0;                  // 6
    uint32_t loop_start = 0;                   // 26 (bits)
    uint32_t unk_dword_3 = 0;                  // 6
    uint32_t loop_end = 0;                     // 26 (bits)
    uint32_t packet_metadata = 0;              // 5
    uint32_t current_buffer = 0;               // 1
    uint32_t input_buffer_0_ptr = 0;           // physical
    uint32_t input_buffer_1_ptr = 0;
    uint32_t output_buffer_ptr = 0;
    uint32_t work_buffer_ptr = 0;
    uint32_t output_buffer_read_offset = 0;    // 5
    uint32_t unk_dword_9 = 0;                  // 25
    uint32_t stop_when_done = 0;               // 1
    uint32_t interrupt_when_done = 0;          // 1
    uint32_t reserved[6] = {};

    void load(const uint8_t* guest64);
    void store(uint8_t* guest64) const;
};

class XmaDecoder {
public:
    static constexpr int kContextCount = 320;
    static constexpr uint32_t kContextSize = 64;
    static constexpr uint32_t kBytesPerPacket = 2048;
    static constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
    static constexpr uint32_t kSamplesPerFrame = 512;
    static constexpr uint32_t kBytesPerBlock = 256;
    // APU register page (plain guest memory, polled).
    static constexpr uint32_t kRegisterBase = 0x7FEA0000;
    static constexpr uint32_t kRegContextArray = 0x1800;
    static constexpr uint32_t kRegCurrentContext = 0x1818;
    static constexpr uint32_t kRegNextContext = 0x181C;
    static constexpr uint32_t kRegKick = 0x1940;
    static constexpr uint32_t kRegLock = 0x1A40;
    static constexpr uint32_t kRegClear = 0x1A80;

    XmaDecoder();
    ~XmaDecoder();

    // Allocates the context array in guest memory and starts the worker.
    bool init(const GuestHooks& hooks, bool start_thread = true);
    void shutdown();

    // Kernel exports.
    uint32_t create_context();                 // guest pointer, 0 if none left
    void release_context(uint32_t ctx_ptr);
    void enable_context(uint32_t ctx_ptr);     // XMAEnableContext (kick)
    bool disable_context(uint32_t ctx_ptr, bool wait);
    void clear_context(uint32_t ctx_ptr);      // used by XMAInitializeContext
    int context_index(uint32_t ctx_ptr) const;
    // Locked read / read-modify-write of a context's guest data, so kernel
    // exports never race the worker thread.
    bool read_context(uint32_t ctx_ptr, XmaContextData& out);
    bool modify_context(uint32_t ctx_ptr, const std::function<void(XmaContextData&)>& fn);
    uint32_t context_array() const { return array_ptr_; }

    // Register writes, for a runtime that traps MMIO. The worker also polls
    // the register page, so a plain-memory page works without this.
    void write_register(uint32_t offset, uint32_t value);
    uint32_t read_register(uint32_t offset);

    // One pass over all contexts (what the worker thread does). Returns true if
    // any context did work. Exposed for tests.
    bool work_once();

    // Decodes a whole XMA stream held in host memory (one stream, packets of
    // 2048 bytes) to interleaved 16-bit PCM. Used by the wave bank tool and
    // the tests; shares the frame parser and decoder with the hardware path.
    static bool decode_stream(const uint8_t* data, size_t size, int channels, int sample_rate,
                              std::vector<int16_t>& pcm_out, std::string& err);

    struct Context;

private:
    void poll_registers();
    void worker_main();

    GuestHooks hooks_;
    uint32_t array_ptr_ = 0;
    std::vector<std::unique_ptr<Context>> contexts_;
    std::mutex alloc_mu_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex wake_mu_;
    std::condition_variable wake_cv_;
    bool wake_ = false;
};

}  // namespace fa::hle
