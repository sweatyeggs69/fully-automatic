#pragma once
// FrameBackend: a CPU-only backend that turns each swap into an RGBA8 picture for the
// window to show. There is no rasterizer yet, so it has two sources:
//   frontbuffer - the swapped front buffer, read from guest memory through the swap's fetch
//                 constant (untiled, endian swapped). Real once draws and resolves exist.
//   texture     - debug view: the largest 2D texture any draw of the frame had bound. This is
//                 what shows splash screens and movie frames before shaders are translated.
// FA_PRESENT=frontbuffer | texture | auto (default: front buffer when it holds anything,
// else the texture view).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fa/gpu/backend.hpp"
#include "fa/gpu/guest_memory.hpp"
#include "fa/gpu/texture.hpp"

namespace fa::gpu {

struct Frame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;  // R, G, B, A bytes, width * 4 per row
    uint64_t seq = 0;           // swap number
    std::string source;         // "frontbuffer", "texture k_DXT1 640x480 @1A2B3000", ...
};

// Latest-frame mailbox between the GPU thread (producer) and the window thread (consumer).
class FrameMailbox {
public:
    void Publish(Frame&& f) {
        std::lock_guard lk(mu_);
        frame_ = std::move(f);
        pending_.store(true, std::memory_order_release);
    }
    // Moves the newest unseen frame into `out` (out's old buffer is recycled).
    bool Take(Frame& out) {
        std::lock_guard lk(mu_);
        if (!pending_.load(std::memory_order_acquire)) return false;
        std::swap(out, frame_);
        pending_.store(false, std::memory_order_release);
        return true;
    }
    // True while the consumer has not yet picked up the last published frame.
    bool Pending() const { return pending_.load(std::memory_order_acquire); }

private:
    std::mutex mu_;
    Frame frame_;
    std::atomic<bool> pending_{false};
};

class FrameBackend : public Backend {
public:
    enum class Mode { Auto, FrontBuffer, Texture };
    struct Stats {
        std::atomic<uint64_t> swaps{0}, draws{0}, resolves{0}, framesDecoded{0};
        std::atomic<uint32_t> drawsLastFrame{0};
    };

    explicit FrameBackend(GuestMemory& mem);
    ~FrameBackend() override;

    // Nothing is decoded until a window wants frames.
    void SetEnabled(bool on) { enabled_.store(on); }
    // Decode swapped frames on a worker thread instead of the GPU thread (the game window turns
    // this on). Swap then only copies the guest bytes; a frame that arrives while the worker is
    // still busy is dropped, as it would be while the window has not shown the last one.
    void SetAsync(bool on);
    FrameMailbox& Mailbox() { return mailbox_; }
    const Stats& GetStats() const { return stats_; }

    void Draw(const DrawCall& draw, const RegisterFile& regs) override;
    void Resolve(const RegisterFile& regs) override;
    void Swap(uint32_t frontBufferGuestAddr, uint32_t width, uint32_t height, const RegisterFile& regs) override;

    static const char* FormatName(uint32_t format);

private:
    // One swap's sources, copied out of guest memory so decoding can happen on another thread.
    struct Job {
        uint64_t seq = 0;
        uint32_t width = 0, height = 0;
        bool haveFb = false, haveTex = false, dump = false;
        TextureFetch fb, tex;
        std::vector<uint8_t> fbBytes, texBytes;
    };
    bool CopyFetch(const TextureFetch& t, std::vector<uint8_t>& out);
    void DecodeJob(Job& job);
    void WorkerLoop();

    GuestMemory& mem_;
    FrameMailbox mailbox_;
    std::atomic<bool> enabled_{false};
    Mode mode_ = Mode::Auto;
    Stats stats_;
    uint32_t drawsThisFrame_ = 0;
    bool haveBest_ = false;
    TextureFetch best_;  // largest texture seen in this frame's draws
    std::vector<uint8_t> scratch_;
    // Async decode.
    std::thread worker_;
    std::mutex jobMu_;
    std::condition_variable jobCv_;
    Job job_, spare_;
    bool jobReady_ = false, workerBusy_ = false, stop_ = false;
};

}  // namespace fa::gpu
