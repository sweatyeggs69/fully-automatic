#include "fa/gpu/frame_backend.hpp"

#include "fa/gpu/gpu_profile.hpp"
#include "fa/gpu/png.hpp"
#include "fa/gpu/pm4.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fa::gpu {

static int EnvInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

FrameBackend::FrameBackend(GuestMemory& mem) : mem_(mem) {
    if (const char* m = std::getenv("FA_PRESENT")) {
        if (!std::strcmp(m, "frontbuffer")) mode_ = Mode::FrontBuffer;
        else if (!std::strcmp(m, "texture")) mode_ = Mode::Texture;
    }
}

const char* FrameBackend::FormatName(uint32_t f) {
    switch (f) {
        case kFmt8: return "k_8";
        case kFmt5_6_5: return "k_5_6_5";
        case kFmt8_8_8_8: return "k_8_8_8_8";
        case kFmt2_10_10_10: return "k_2_10_10_10";
        case kFmt8_8: return "k_8_8";
        case kFmt8_8_8_8_A: return "k_8_8_8_8_A";
        case kFmtDXT1: return "k_DXT1";
        case kFmtDXT2_3: return "k_DXT2_3";
        case kFmtDXT4_5: return "k_DXT4_5";
        case kFmt8_8_8_8_AS_16: return "k_8_8_8_8_AS_16_16_16_16";
        case kFmtDXT1_AS_16: return "k_DXT1_AS_16_16_16_16";
        case kFmtDXT2_3_AS_16: return "k_DXT2_3_AS_16_16_16_16";
        case kFmtDXT4_5_AS_16: return "k_DXT4_5_AS_16_16_16_16";
        case kFmt2_10_10_10_AS_16: return "k_2_10_10_10_AS_16_16_16_16";
        default: return "unsupported";
    }
}

void FrameBackend::Draw(const DrawCall&, const RegisterFile& regs) {
    stats_.draws.fetch_add(1, std::memory_order_relaxed);
    drawsThisFrame_++;
    if (!enabled_.load(std::memory_order_relaxed) || mode_ == Mode::FrontBuffer) return;
    // Without shader parsing we do not know which fetch slots the pixel shader samples, so
    // look at every texture fetch constant and keep the largest 2D one.
    for (uint32_t slot = 0; slot < 32; slot++) {
        uint32_t d[6];
        for (int k = 0; k < 6; k++) d[k] = regs[pm4::kFetchConstBase + slot * 6 + k];
        TextureFetch t = ParseTextureFetch(d);
        if (!t.IsTexture() || !t.base || t.dimension != 1 || t.width < 64 || t.height < 64) continue;
        if (!GetFormatInfo(t.format).supported) continue;
        if (!haveBest_ || uint64_t(t.width) * t.height >= uint64_t(best_.width) * best_.height) {
            best_ = t;
            haveBest_ = true;
        }
    }
}

void FrameBackend::Resolve(const RegisterFile&) { stats_.resolves.fetch_add(1, std::memory_order_relaxed); }

bool FrameBackend::CopyFetch(const TextureFetch& t, std::vector<uint8_t>& out) {
    if (!t.width || !t.height) return false;
    uint32_t size = TextureByteSize(t);
    const uint8_t* src = mem_.HostPtr(t.base, size);
    if (!src) return false;
    out.assign(src, src + size);
    return true;
}

FrameBackend::~FrameBackend() { SetAsync(false); }

void FrameBackend::SetAsync(bool on) {
    if (on == worker_.joinable()) return;
    if (on) {
        stop_ = false;
        worker_ = std::thread([this] { WorkerLoop(); });
    } else {
        {
            std::lock_guard lk(jobMu_);
            stop_ = true;
        }
        jobCv_.notify_all();
        worker_.join();
    }
}

void FrameBackend::WorkerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lk(jobMu_);
            jobCv_.wait(lk, [&] { return jobReady_ || stop_; });
            if (stop_) return;
            std::swap(job, job_);
            jobReady_ = false;
            workerBusy_ = true;
        }
        DecodeJob(job);
        std::lock_guard lk(jobMu_);
        workerBusy_ = false;
        spare_ = std::move(job);  // keeps the byte buffers' capacity for the next swap
    }
}

static bool HasContent(const std::vector<uint8_t>& rgba) {
    // Sample a grid of pixels: any non-black colour counts.
    size_t px = rgba.size() / 4;
    size_t step = px > 4096 ? px / 4096 : 1;
    for (size_t i = 0; i < px; i += step)
        if (rgba[i * 4] | rgba[i * 4 + 1] | rgba[i * 4 + 2]) return true;
    return false;
}

void FrameBackend::Swap(uint32_t addr, uint32_t width, uint32_t height, const RegisterFile& regs) {
    uint64_t seq = stats_.swaps.fetch_add(1, std::memory_order_relaxed) + 1;
    prof::OnSwap();
    prof::Scope ps_(prof::kFrameDecode);
    stats_.drawsLastFrame.store(drawsThisFrame_, std::memory_order_relaxed);
    drawsThisFrame_ = 0;
    bool haveTex = haveBest_;
    TextureFetch tex = best_;
    haveBest_ = false;

    // Skip the decode while the window still has not shown the previous frame.
    // FA_RENDER_DUMP: the front buffer after every 30th frame (matches the renderer's dump frames).
    static const char* dumpDir = std::getenv("FA_RENDER_DUMP");
    static const uint64_t dumpCount = [] {
        const char* c = std::getenv("FA_RENDER_DUMP_COUNT");
        return c ? std::strtoull(c, nullptr, 10) : 12ull;
    }();
    const bool dumpThis = dumpDir && *dumpDir && (seq <= 6 || ((seq - 1) % 30 == 0 && seq <= 30 * dumpCount));
    if (!enabled_.load(std::memory_order_relaxed) || (mailbox_.Pending() && !dumpThis)) return;

    const bool async = worker_.joinable();
    Job local;
    Job* job = &local;
    if (async) {
        std::lock_guard lk(jobMu_);
        if ((workerBusy_ || jobReady_) && !dumpThis) return;  // still decoding the last frame
        local = std::move(spare_);  // reuse the byte buffers of an earlier job
    }
    job->seq = seq;
    job->width = width;
    job->height = height;
    job->dump = dumpThis;
    job->haveFb = job->haveTex = false;
    if (mode_ != Mode::Texture) {
        // VdSwap loaded the front buffer's fetch constant into slot 0 just before XE_SWAP.
        uint32_t d[6];
        for (int k = 0; k < 6; k++) d[k] = regs[pm4::kFetchConstBase + k];
        TextureFetch fb = ParseTextureFetch(d);
        if (!fb.IsTexture() || !GetFormatInfo(fb.format).supported) {
            fb = TextureFetch{};
            fb.type = 2;
            fb.format = kFmt8_8_8_8;
            fb.endian = kEndian8in32;
            fb.tiled = true;
        }
        fb.base = addr ? addr : fb.base;
        if (width && height) { fb.width = width; fb.height = height; }
        fb.tiled = EnvInt("FA_FB_TILED", fb.tiled) != 0;
        fb.endian = uint32_t(EnvInt("FA_FB_ENDIAN", int(fb.endian))) & 3;
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[present] front buffer %08x %ux%u %s endian %u %s pitch %u swizzle %03x\n", fb.base,
                         fb.width, fb.height, FormatName(fb.format), fb.endian, fb.tiled ? "tiled" : "linear", fb.pitch,
                         fb.swizzle);
        }
        job->fb = fb;
        job->haveFb = CopyFetch(fb, job->fbBytes);
    }
    // The texture view is only needed when the front buffer turns out empty; copying it is cheap
    // next to decoding, so it rides along.
    if (mode_ != Mode::FrontBuffer && haveTex) {
        job->tex = tex;
        job->haveTex = CopyFetch(tex, job->texBytes);
    }
    if (!async) {
        DecodeJob(*job);
        return;
    }
    {
        std::lock_guard lk(jobMu_);
        job_ = std::move(local);
        jobReady_ = true;
    }
    jobCv_.notify_one();
}

void FrameBackend::DecodeJob(Job& job) {
    static const char* dumpDir = std::getenv("FA_RENDER_DUMP");
    Frame f;
    f.seq = job.seq;
    bool fbOk = false;
    if (job.haveFb && DecodeTexture(job.fb, job.fbBytes.data(), f.rgba)) {
        fbOk = true;
        f.width = job.fb.width;
        f.height = job.fb.height;
        if (job.dump) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/f%04llu_frontbuffer_%08x.png", dumpDir, (unsigned long long)(job.seq - 1),
                          job.fb.base);
            std::vector<uint8_t> opaque = f.rgba;
            for (size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
            WritePng(path, f.width, f.height, opaque.data());
        }
        if (job.dump && mailbox_.Pending()) return;
        f.source = "frontbuffer";
        if (mode_ == Mode::Auto && !HasContent(f.rgba)) fbOk = false;
    }
    if (!fbOk && job.haveTex && DecodeTexture(job.tex, job.texBytes.data(), f.rgba)) {
        f.width = job.tex.width;
        f.height = job.tex.height;
        char buf[96];
        std::snprintf(buf, sizeof buf, "texture %s %ux%u", FormatName(job.tex.format), job.tex.width, job.tex.height);
        f.source = buf;
        fbOk = true;
    }
    if (!fbOk) {
        // Nothing to show yet: a black 1280x720 frame keeps the window alive.
        f.width = job.width ? job.width : 1280;
        f.height = job.height ? job.height : 720;
        f.rgba.assign(size_t(f.width) * f.height * 4, 0);
        f.source = "black (no front buffer content or textures yet)";
    }
    stats_.framesDecoded.fetch_add(1, std::memory_order_relaxed);
    mailbox_.Publish(std::move(f));
}

}  // namespace fa::gpu
