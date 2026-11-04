// End-to-end renderer test: PM4 stream -> CommandProcessor -> VulkanRenderer (offscreen EDRAM
// targets) -> resolve into guest memory, then decode the resolved surface and check pixels.
// Runs on any Vulkan 1.3 device (lavapipe works). Exits 0 with "skipped" when there is none.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fa/gpu/command_processor.hpp"
#include "fa/gpu/pm4.hpp"
#include "fa/gpu/renderer.hpp"
#include "fa/gpu/texture.hpp"
#include "fa/gpu/xenos.hpp"
#include "xenos_asm.hpp"

using namespace fa::gpu;

struct FakeMem : GuestMemory {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(16 << 20);  // big-endian storage
    uint32_t Read32(uint32_t a) const override {
        return (uint32_t(bytes[a]) << 24) | (bytes[a + 1] << 16) | (bytes[a + 2] << 8) | bytes[a + 3];
    }
    void Write32(uint32_t a, uint32_t v) override {
        bytes[a] = v >> 24; bytes[a + 1] = v >> 16; bytes[a + 2] = v >> 8; bytes[a + 3] = v;
    }
    const uint8_t* HostPtr(uint32_t a, uint32_t n) const override { return a + uint64_t(n) <= bytes.size() ? &bytes[a] : nullptr; }
    uint8_t* HostPtrWrite(uint32_t a, uint32_t n) override { return a + uint64_t(n) <= bytes.size() ? &bytes[a] : nullptr; }
};

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static uint32_t T3(uint32_t op, uint32_t payloadDwords) { return (3u << 30) | ((payloadDwords - 1) << 16) | (op << 8); }
static uint32_t F(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

struct Stream {
    FakeMem& mem;
    uint32_t ring, words, w = 0;
    void put(uint32_t v) { mem.Write32(ring + (w++ % words) * 4, v); }
    void reg(uint32_t r, uint32_t v) { put(r); put(v); }  // type 0, one register
    void shader(uint32_t type, const std::vector<uint32_t>& u) {
        put(T3(pm4::kImLoadImmediate, uint32_t(u.size()) + 2));
        put(type);
        put(uint32_t(u.size()));
        for (uint32_t v : u) put(v);
    }
    void alu(uint32_t index, std::initializer_list<float> v) {
        put(T3(pm4::kSetConstant, uint32_t(v.size()) + 1));
        put(index);
        for (float f : v) put(F(f));
    }
    void vfetch0(uint32_t addr, uint32_t words_) {
        put(T3(pm4::kSetConstant, 3));
        put(1u << 16);  // fetch constants, index 0
        put(addr | 3);
        put((words_ << 2) | kEndian8in32);
    }
    void draw(uint32_t prim, uint32_t count) { put(T3(pm4::kDrawIndx2, 1)); put((count << 16) | (2u << 6) | prim); }
};

// Reads back pixel (x, y) of a resolved k_8_8_8_8 surface as RGBA8.
static void Pixel(const FakeMem& mem, uint32_t base, uint32_t pitch, uint32_t height, uint32_t x, uint32_t y, uint8_t out[4]) {
    TextureFetch t;
    t.type = 2;
    t.format = kFmt8_8_8_8;
    t.endian = kEndian8in32;
    t.base = base;
    t.pitch = pitch;
    t.width = pitch;
    t.height = height;
    t.tiled = true;
    std::vector<uint8_t> rgba;
    if (!DecodeTexture(t, mem.HostPtr(base, TextureByteSize(t)), rgba)) { std::memset(out, 0xEE, 4); return; }
    std::memcpy(out, &rgba[(size_t(y) * pitch + x) * 4], 4);
}

int main() {
    FakeMem mem;
    VulkanRenderer r(mem, nullptr);
    VulkanRenderer::Options opt;
    opt.validation = std::getenv("FA_VK_VALIDATION") != nullptr;
    if (const char* d = std::getenv("FA_DUMP_SHADERS")) opt.dumpShaders = d;
    std::string err;
    if (!r.Init(opt, err)) {
        std::printf("gpu_render_test: skipped (%s)\n", err.c_str());
        return 0;
    }
    std::printf("gpu_render_test: device %s\n", r.DeviceName().c_str());

    CommandProcessor cp(mem, r);
    const uint32_t ring = 0x1000, ringBytes = 0x10000;
    cp.SetRingBuffer(ring, ringBytes);
    Stream s{mem, ring, ringBytes / 4};

    // Vertex buffer: three vertices of (x, y, z, r, g, b, a) in pixels, a red triangle covering
    // the top-left half of a 64x64 square.
    const uint32_t vb = 0x100000;
    const float tri[3][7] = {{0, 0, 0, 1, 0, 0, 1}, {64, 0, 0, 1, 0, 0, 1}, {0, 64, 0, 1, 0, 0, 1}};
    for (int v = 0; v < 3; v++)
        for (int c = 0; c < 7; c++) mem.Write32(vb + (v * 7 + c) * 4, F(tri[v][c]));
    // Resolve rectangle: (x, y) pairs for a RECTANGLE_LIST covering 128x64.
    const uint32_t rectVb = 0x101000;
    const float rect[6] = {0, 0, 128, 0, 128, 64};
    for (int i = 0; i < 6; i++) mem.Write32(rectVb + i * 4, F(rect[i]));

    s.shader(0, xasm::ColorVS());
    s.shader(1, xasm::ColorPS());
    s.alu(256 * 4, {1, 1, 1, 1});  // PS c0
    s.reg(xe::RB_SURFACE_INFO, 128);                 // pitch 128, no MSAA
    s.reg(xe::RB_COLOR_INFO, 0);                     // EDRAM base 0, k_8_8_8_8
    s.reg(xe::RB_DEPTH_INFO, 0x200);                 // depth elsewhere (unused)
    s.reg(xe::RB_COLOR_MASK, 0xF);
    s.reg(xe::RB_DEPTHCONTROL, 0);
    s.reg(xe::RB_BLENDCONTROL0, 0x00010001);         // ONE, ZERO
    s.reg(xe::PA_CL_VTE_CNTL, 0);                    // positions already in pixels
    s.reg(xe::PA_SU_SC_MODE_CNTL, 0);
    s.reg(xe::PA_SC_WINDOW_SCISSOR_TL, 0x80000000u);
    s.reg(xe::PA_SC_WINDOW_SCISSOR_BR, (64u << 16) | 128);
    s.reg(xe::PA_SU_VTX_CNTL, 1);                    // pixel centres at integers (no D3D9 half offset)

    // Clear the colour target to blue with a clear-only resolve, then draw, then resolve.
    s.reg(xe::RB_COLOR_CLEAR, 0xFFFF0000u);          // opaque blue (A B G R, R in the low byte)
    s.reg(xe::RB_COLOR_CLEAR_LO, 0xFFFF0000u);
    s.vfetch0(rectVb, 6);
    s.reg(xe::RB_COPY_CONTROL, (1u << 8) | (3u << 20));  // clear colour, no copy
    s.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
    s.draw(xe::kPrimRectangleList, 3);
    s.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);

    s.vfetch0(vb, 21);
    s.draw(xe::kPrimTriangleList, 3);

    // A textured RECTANGLE_LIST over the right half: 2x2 point-sampled texture in fetch slot 1.
    const uint32_t texVb = 0x102000, tex = 0x300000;
    const float quad[3][7] = {{64, 0, 0, 0, 0, 0, 1}, {128, 0, 0, 1, 0, 0, 1}, {64, 64, 0, 0, 1, 0, 1}};
    for (int v = 0; v < 3; v++)
        for (int c = 0; c < 7; c++) mem.Write32(texVb + (v * 7 + c) * 4, F(quad[v][c]));
    const uint32_t texels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};  // A B G R in a dword
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) mem.Write32(tex + y * 256 + x * 4, texels[y * 2 + x]);  // linear rows are 256-byte aligned
    s.shader(1, xasm::TexturePS(1));
    s.put(T3(pm4::kSetConstant, 7));
    s.put((1u << 16) | 6);  // fetch constants, texture slot 1
    s.put(2 | (2u << 10) | (2u << 13) | (1u << 22));              // texture, clamp, pitch 32
    s.put(tex | (kEndian8in32 << 6) | kFmt8_8_8_8);
    s.put(1 | (1u << 13));                                         // 2x2
    s.put(0x688u << 1);                                            // XYZW, point filtering
    s.put(0);
    s.put(1u << 9);                                                // 2D
    s.vfetch0(texVb, 21);
    s.draw(xe::kPrimRectangleList, 3);

    const uint32_t dest = 0x200000;
    s.vfetch0(rectVb, 6);
    s.reg(xe::RB_COPY_CONTROL, 0);                   // copy colour 0, no clear
    s.reg(xe::RB_COPY_DEST_BASE, dest);
    s.reg(xe::RB_COPY_DEST_PITCH, (64u << 16) | 128);
    s.reg(xe::RB_COPY_DEST_INFO, (kFmt8_8_8_8 << 7) | kEndian8in32);
    s.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
    s.draw(xe::kPrimRectangleList, 3);
    s.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);

    cp.SetWritePtr(s.w);
    CHECK(cp.Pump() == CommandProcessor::Status::Idle);
    r.Flush();

    const auto& st = r.GetStats();
    std::printf("gpu_render_test: draws %llu skipped %llu resolves %llu clears %llu shaders %llu errors %llu\n",
                (unsigned long long)st.draws.load(), (unsigned long long)st.drawsSkipped.load(),
                (unsigned long long)st.resolves.load(), (unsigned long long)st.clears.load(),
                (unsigned long long)st.shadersCompiled.load(), (unsigned long long)st.shaderErrors.load());
    CHECK(st.draws == 2);
    CHECK(st.textureUploads == 1);
    CHECK(st.drawsSkipped == 0);
    CHECK(st.resolves == 2);
    CHECK(st.shaderErrors == 0);

    uint8_t p[4];
    Pixel(mem, dest, 128, 64, 4, 4, p);
    std::printf("  (4,4)    = %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
    CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255);  // inside the triangle: red
    Pixel(mem, dest, 128, 64, 60, 60, p);
    std::printf("  (60,60)  = %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);  // outside: clear colour

    // Textured quadrants match the CPU decode of the same texture.
    TextureFetch t;
    t.type = 2; t.format = kFmt8_8_8_8; t.endian = kEndian8in32; t.base = tex; t.pitch = 32; t.width = 2; t.height = 2;
    std::vector<uint8_t> ref;
    CHECK(DecodeTexture(t, mem.HostPtr(tex, TextureByteSize(t)), ref));
    const uint32_t probe[4][2] = {{72, 8}, {120, 8}, {72, 56}, {120, 56}};
    for (int i = 0; i < 4 && ref.size() >= 16; i++) {
        Pixel(mem, dest, 128, 64, probe[i][0], probe[i][1], p);
        const uint8_t* e = &ref[((i / 2) * 2 + (i % 2)) * 4];
        std::printf("  (%u,%u) = %02x %02x %02x %02x, texel %02x %02x %02x %02x\n", probe[i][0], probe[i][1], p[0], p[1],
                    p[2], p[3], e[0], e[1], e[2], e[3]);
        CHECK(!std::memcmp(p, e, 4));
    }
    CHECK(ref.size() >= 16 && ref[0] == 255 && ref[1] == 0 && ref[2] == 0);  // texel (0,0) is red

    // Perspective: clip-space positions with w = 2 through the viewport transform (VTE 0x43f, the
    // usual D3D setting), a green triangle over the top-left half again.
    {
        const uint32_t pvb = 0x104000, dest2 = 0x240000;
        const float ptri[3][8] = {{-2, 2, 0, 2, 0, 1, 0, 1}, {0, 2, 0, 2, 0, 1, 0, 1}, {-2, -2, 0, 2, 0, 1, 0, 1}};
        for (int v = 0; v < 3; v++)
            for (int c = 0; c < 8; c++) mem.Write32(pvb + (v * 8 + c) * 4, F(ptri[v][c]));
        Stream q{mem, ring, ringBytes / 4, s.w};
        q.vfetch0(rectVb, 6);
        q.reg(xe::PA_CL_VTE_CNTL, 0);
        q.reg(xe::RB_COPY_CONTROL, (1u << 8) | (3u << 20));  // clear to blue
        q.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
        q.draw(xe::kPrimRectangleList, 3);
        q.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);

        q.shader(0, xasm::ColorVS(true));
        q.shader(1, xasm::ColorPS());
        q.reg(xe::PA_CL_VTE_CNTL, 0x43f);
        q.reg(xe::PA_CL_VPORT_XSCALE, F(64));
        q.reg(xe::PA_CL_VPORT_XOFFSET, F(64));
        q.reg(xe::PA_CL_VPORT_YSCALE, F(-32));
        q.reg(xe::PA_CL_VPORT_YOFFSET, F(32));
        q.reg(xe::PA_CL_VPORT_ZSCALE, F(1));
        q.reg(xe::PA_CL_VPORT_ZOFFSET, F(0));
        q.vfetch0(pvb, 24);
        q.draw(xe::kPrimTriangleList, 3);

        q.reg(xe::PA_CL_VTE_CNTL, 0);
        q.vfetch0(rectVb, 6);
        q.reg(xe::RB_COPY_CONTROL, 0);
        q.reg(xe::RB_COPY_DEST_BASE, dest2);
        q.reg(xe::RB_COPY_DEST_PITCH, (64u << 16) | 128);
        q.reg(xe::RB_COPY_DEST_INFO, (kFmt8_8_8_8 << 7) | kEndian8in32);
        q.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
        q.draw(xe::kPrimRectangleList, 3);
        q.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);
        cp.SetWritePtr(q.w % (ringBytes / 4));
        CHECK(cp.Pump() == CommandProcessor::Status::Idle);
        r.Flush();
        Pixel(mem, dest2, 128, 64, 4, 4, p);
        std::printf("  perspective (4,4)   = %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
        CHECK(p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255);
        Pixel(mem, dest2, 128, 64, 60, 60, p);
        std::printf("  perspective (60,60) = %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
        CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);
    }

    if (failures) { std::printf("gpu_render_test: %d failure(s)\n", failures); return 1; }
    std::printf("gpu_render_test: ok\n");
    return 0;
}
