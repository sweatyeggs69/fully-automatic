#include "fa/gpu/frame_backend.hpp"

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

bool FrameBackend::DecodeFetch(const TextureFetch& t, Frame& out) {
    uint32_t size = TextureByteSize(t);
    const uint8_t* src = mem_.HostPtr(t.base, size);
    if (!src) return false;
    if (!DecodeTexture(t, src, out.rgba)) return false;
    out.width = t.width;
    out.height = t.height;
    return true;
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
    stats_.drawsLastFrame.store(drawsThisFrame_, std::memory_order_relaxed);
    drawsThisFrame_ = 0;
    bool haveTex = haveBest_;
    TextureFetch tex = best_;
    haveBest_ = false;

    // Skip the decode while the window still has not shown the previous frame.
    if (!enabled_.load(std::memory_order_relaxed) || mailbox_.Pending()) return;

    Frame f;
    f.seq = seq;
    bool fbOk = false;
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
        fbOk = DecodeFetch(fb, f);
        // FA_RENDER_DUMP=<dir>: the decoded front buffer of every 30th presented swap (first 20).
        static const char* dumpDir = std::getenv("FA_RENDER_DUMP");
        static int dumped = 0;
        static uint64_t lastDump = 0;
        if (fbOk && dumpDir && *dumpDir && dumped < 20 && (dumped == 0 || seq >= lastDump + 30)) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/frontbuffer_swap%05llu_%08x.png", dumpDir, (unsigned long long)seq, fb.base);
            std::vector<uint8_t> opaque = f.rgba;
            for (size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
            WritePng(path, f.width, f.height, opaque.data());
            dumped++;
            lastDump = seq;
        }
        if (fbOk) {
            f.source = "frontbuffer";
            if (mode_ == Mode::Auto && !HasContent(f.rgba)) fbOk = false;
        }
    }
    if (!fbOk && mode_ != Mode::FrontBuffer && haveTex && DecodeFetch(tex, f)) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "texture %s %ux%u", FormatName(tex.format), tex.width, tex.height);
        f.source = buf;
        fbOk = true;
    }
    if (!fbOk) {
        // Nothing to show yet: a black 1280x720 frame keeps the window alive.
        f.width = width ? width : 1280;
        f.height = height ? height : 720;
        f.rgba.assign(size_t(f.width) * f.height * 4, 0);
        f.source = "black (no front buffer content or textures yet)";
    }
    stats_.framesDecoded.fetch_add(1, std::memory_order_relaxed);
    mailbox_.Publish(std::move(f));
}

}  // namespace fa::gpu
