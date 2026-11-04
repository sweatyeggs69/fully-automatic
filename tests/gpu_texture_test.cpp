// Texture helpers: tiling is a bijection inside the surface, fetch constants parse, and the
// decoders produce the expected colours. Plus FrameBackend end to end on fake guest memory.
#include "fa/gpu/frame_backend.hpp"
#include "fa/gpu/pm4.hpp"
#include "fa/gpu/texture.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace fa::gpu;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void TestTilingBijection() {
    const uint32_t bpbs[] = {1, 2, 4, 8, 16};
    for (uint32_t bpb : bpbs) {
        for (auto [w, h] : {std::pair<uint32_t, uint32_t>{64, 64}, {1280, 736}, {96, 32}}) {
            // Odd macro tile counts only come up for 4+ byte blocks (small formats pad further).
            if (w == 96 && bpb < 4) continue;
            std::vector<uint8_t> seen(size_t(w) * h, 0);
            bool ok = true;
            for (uint32_t y = 0; y < h && ok; y++)
                for (uint32_t x = 0; x < w && ok; x++) {
                    uint32_t off = TiledBlockOffset(x, y, w, bpb);
                    if (off % bpb || off / bpb >= seen.size() || seen[off / bpb]) {
                        std::fprintf(stderr, "tiling bpb %u %ux%u: block (%u,%u) -> %u bad\n", bpb, w, h, x, y, off);
                        ok = false;
                    } else seen[off / bpb] = 1;
                }
            CHECK(ok);
        }
    }
    // Linear inside the first 32x2 row pair isn't expected, but block (0,0) is always at 0.
    CHECK(TiledBlockOffset(0, 0, 32, 4) == 0);
}

static void TestFetchParse() {
    uint32_t d[6] = {};
    d[0] = 2u | (40u << 22) | (1u << 31);           // texture, pitch 40*32 = 1280, tiled
    d[1] = kFmt8_8_8_8 | (kEndian8in32 << 6) | 0x1A2B3000;
    d[2] = (1279u) | (719u << 13);
    d[3] = 0x688u << 1;
    d[5] = 1u << 9;
    TextureFetch t = ParseTextureFetch(d);
    CHECK(t.IsTexture());
    CHECK(t.pitch == 1280);
    CHECK(t.tiled);
    CHECK(t.format == kFmt8_8_8_8);
    CHECK(t.endian == kEndian8in32);
    CHECK(t.base == 0x1A2B3000);
    CHECK(t.width == 1280 && t.height == 720);
    CHECK(t.dimension == 1);
    CHECK(t.swizzle == 0x688);
}

static void TestDecodeLinear8888() {
    TextureFetch t;
    t.type = 2; t.format = kFmt8_8_8_8; t.endian = kEndian8in32; t.width = 2; t.height = 1; t.pitch = 32;
    std::vector<uint8_t> src(TextureByteSize(t), 0);
    CHECK(src.size() == 256);
    // Big-endian dwords as the CPU stores them: 0xAABBGGRR -> bytes AA BB GG RR.
    const uint8_t px0[4] = {0xFF, 0x30, 0x20, 0x10}, px1[4] = {0x80, 0x03, 0x02, 0x01};
    std::memcpy(&src[0], px0, 4); std::memcpy(&src[4], px1, 4);
    std::vector<uint8_t> out;
    CHECK(DecodeTexture(t, src.data(), out));
    CHECK(out.size() == 8);
    CHECK(out[0] == 0x10 && out[1] == 0x20 && out[2] == 0x30 && out[3] == 0xFF);
    CHECK(out[4] == 0x01 && out[5] == 0x02 && out[6] == 0x03 && out[7] == 0x80);
    // Swizzle ZYXW swaps red and blue.
    t.swizzle = 2 | (1 << 3) | (0 << 6) | (3 << 9);
    CHECK(DecodeTexture(t, src.data(), out));
    CHECK(out[0] == 0x30 && out[2] == 0x10);
}

static void TestDecodeDxt1() {
    TextureFetch t;
    t.type = 2; t.format = kFmtDXT1; t.endian = kEndian8in16; t.width = 4; t.height = 4; t.pitch = 32; t.tiled = true;
    std::vector<uint8_t> src(TextureByteSize(t), 0);
    // One block, c0 = pure red (0xF800), c1 = 0, all indices 0. Stored 8in16 swapped.
    const uint8_t blk[8] = {0xF8, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    std::memcpy(&src[TiledBlockOffset(0, 0, 32, 8)], blk, 8);
    std::vector<uint8_t> out;
    CHECK(DecodeTexture(t, src.data(), out));
    CHECK(out.size() == 64);
    bool allRed = true;
    for (int i = 0; i < 16; i++) allRed &= out[i * 4] == 255 && out[i * 4 + 1] == 0 && out[i * 4 + 2] == 0 && out[i * 4 + 3] == 255;
    CHECK(allRed);
}

// Byte-addressed fake guest memory (raw big-endian bytes like the real runtime).
struct FakeMem : GuestMemory {
    std::map<uint32_t, std::vector<uint8_t>> regions;
    uint8_t* Find(uint32_t a, uint32_t size) const {
        for (auto& [base, v] : regions)
            if (a >= base && uint64_t(a) + size <= base + v.size()) return const_cast<uint8_t*>(v.data()) + (a - base);
        return nullptr;
    }
    uint32_t Read32(uint32_t a) const override {
        const uint8_t* p = Find(a, 4);
        return p ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3] : 0;
    }
    void Write32(uint32_t a, uint32_t v) override {
        if (uint8_t* p = Find(a, 4)) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = uint8_t(v); }
    }
    const uint8_t* HostPtr(uint32_t a, uint32_t size) const override { return Find(a, size); }
};

static void TestFrameBackend() {
    FakeMem mem;
    const uint32_t fbAddr = 0x00A00000, w = 64, h = 32;
    TextureFetch fb;
    fb.type = 2; fb.format = kFmt8_8_8_8; fb.endian = kEndian8in32; fb.width = w; fb.height = h; fb.tiled = true;
    mem.regions[fbAddr].assign(TextureByteSize(fb), 0);
    FrameBackend be(mem);
    be.SetEnabled(true);
    RegisterFile regs;
    // Front buffer fetch constant as VdSwap loads it.
    regs[pm4::kFetchConstBase + 0] = 2u | (2u << 22) | (1u << 31);
    regs[pm4::kFetchConstBase + 1] = kFmt8_8_8_8 | (kEndian8in32 << 6) | fbAddr;
    regs[pm4::kFetchConstBase + 2] = (w - 1) | ((h - 1) << 13);
    regs[pm4::kFetchConstBase + 3] = 0x688u << 1;

    // Empty front buffer and no draws: a black frame.
    be.Swap(fbAddr, w, h, regs);
    Frame f;
    CHECK(be.Mailbox().Take(f));
    CHECK(f.width == w && f.height == h);
    CHECK(f.source.rfind("black", 0) == 0);

    // Paint pixel (5, 3) green (big-endian dword 0xFF00FF00 -> R=0 G=FF B=0 A=FF).
    uint8_t* p = mem.regions[fbAddr].data() + TiledBlockOffset(5, 3, 64, 4);
    p[0] = 0xFF; p[1] = 0x00; p[2] = 0xFF; p[3] = 0x00;
    be.Swap(fbAddr, w, h, regs);
    CHECK(be.Mailbox().Take(f));
    CHECK(f.source == "frontbuffer");
    const uint8_t* q = &f.rgba[(3 * w + 5) * 4];
    CHECK(q[0] == 0 && q[1] == 0xFF && q[2] == 0 && q[3] == 0xFF);
    CHECK(!be.Mailbox().Take(f));

    // Clear the front buffer, bind a texture in slot 3 and draw: the texture view kicks in.
    std::memset(mem.regions[fbAddr].data(), 0, mem.regions[fbAddr].size());
    const uint32_t texAddr = 0x01000000;
    TextureFetch tex;
    tex.type = 2; tex.format = kFmt8; tex.width = 128; tex.height = 64; tex.pitch = 128;
    mem.regions[texAddr].assign(TextureByteSize(tex), 0x7F);
    uint32_t s = pm4::kFetchConstBase + 3 * 6;
    regs[s + 0] = 2u | (4u << 22);
    regs[s + 1] = kFmt8 | texAddr;
    regs[s + 2] = 127u | (63u << 13);
    regs[s + 5] = 1u << 9;
    be.Draw(DrawCall{}, regs);
    be.Swap(fbAddr, w, h, regs);
    CHECK(be.Mailbox().Take(f));
    CHECK(f.source == "texture k_8 128x64");
    CHECK(f.width == 128 && f.height == 64 && f.rgba[0] == 0x7F && f.rgba[3] == 255);
}

int main() {
    TestTilingBijection();
    TestFetchParse();
    TestDecodeLinear8888();
    TestDecodeDxt1();
    TestFrameBackend();
    if (g_fail) { std::fprintf(stderr, "%d failures\n", g_fail); return 1; }
    std::printf("gpu_texture_test: ok\n");
    return 0;
}
