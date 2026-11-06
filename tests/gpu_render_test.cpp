// End-to-end renderer test: PM4 stream -> CommandProcessor -> VulkanRenderer (offscreen EDRAM
// targets) -> resolve into guest memory, then decode the resolved surface and check pixels.
// Runs on any Vulkan 1.3 device (lavapipe works). Exits 0 with "skipped" when there is none.
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    // Big-endian storage in a shared mapping, like the runtime's physical memory, so the kernel
    // write tracking can watch it.
    struct Bytes {
        size_t n = 16 << 20;
        uint8_t* p = [this] {
            int fd = memfd_create("render-test", 0);
            void* m = fd >= 0 && ftruncate(fd, n) == 0 ? mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
            if (fd >= 0) close(fd);
            if (m == MAP_FAILED) { std::printf("gpu_render_test: guest memory mapping failed\n"); std::exit(1); }
            return static_cast<uint8_t*>(m);
        }();
        size_t size() const { return n; }
        uint8_t& operator[](size_t i) { return p[i]; }
        const uint8_t& operator[](size_t i) const { return p[i]; }
    } bytes;
    uint32_t PhysicalSize() const override { return uint32_t(bytes.size()); }
    int PhysicalViews(PhysicalView* out, int max) const override {
        if (max < 1) return 0;
        out[0] = {bytes.p, 0, uint32_t(bytes.size())};
        return 1;
    }
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
    setenv("FA_WW", "2", 0);  // write tracking without pinned memory (no udmabuf here)
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

    setenv("FA_TILE_ONCE", "0", 1);  // the main command processor replays every tile
    CommandProcessor cp(mem, r);
    unsetenv("FA_TILE_ONCE");
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

    // Kernel write tracking: next frame an unchanged texture is not hashed again, and a texture the
    // guest wrote is uploaded again.
    {
        RegisterFile none;
        Stream q{mem, ring, ringBytes / 4, s.w};
        auto texDraw = [&] {
            q.shader(0, xasm::ColorVS());
            q.shader(1, xasm::TexturePS(1));
            q.reg(xe::PA_CL_VTE_CNTL, 0);
            q.vfetch0(texVb, 21);
            q.draw(xe::kPrimRectangleList, 3);
            cp.SetWritePtr(q.w % (ringBytes / 4));
            CHECK(cp.Pump() == CommandProcessor::Status::Idle);
            r.Flush();
        };
        const uint64_t up0 = st.textureUploads, clean0 = st.textureWwClean;
        r.Swap(0, 0, 0, none);
        texDraw();
        CHECK(st.textureUploads == up0);
        const bool on = st.writeWatch != 0;
        if (on) CHECK(st.textureWwClean == clean0 + 1);
        mem.Write32(tex, 0xFF00FF00u);
        r.Swap(0, 0, 0, none);
        texDraw();
        CHECK(st.textureUploads == up0 + 1);
        if (on) CHECK(st.textureWwClean == clean0 + 1);
        std::printf("  write watch %s: unchanged texture %s, written texture uploaded again %s\n", on ? "on" : "off",
                    st.textureWwClean > clean0 ? "skipped" : "hashed", st.textureUploads == up0 + 1 ? "yes" : "NO");
    }

    // Fences and the swap's writebacks (FA_FRAMES=2 runs this test with frames in flight too): a fence
    // the stream writes reaches guest memory once the GPU is done, and a resolve flushed by the swap
    // lands in guest memory by the next full flush.
    {
        RegisterFile none;
        const uint32_t fenceAddr = 0x280000, dest3 = 0x2C0000;
        mem.Write32(fenceAddr, 0);
        Stream q{mem, ring, ringBytes / 4, s.w};
        q.shader(0, xasm::ColorVS());
        q.shader(1, xasm::ColorPS());
        q.reg(xe::PA_CL_VTE_CNTL, 0);
        q.vfetch0(rectVb, 6);
        q.reg(xe::RB_COPY_CONTROL, (1u << 8) | (3u << 20));  // clear to blue
        q.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
        q.draw(xe::kPrimRectangleList, 3);
        q.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);
        q.vfetch0(vb, 21);
        q.draw(xe::kPrimTriangleList, 3);
        q.vfetch0(rectVb, 6);
        q.reg(xe::RB_COPY_CONTROL, 0);
        q.reg(xe::RB_COPY_DEST_BASE, dest3);
        q.reg(xe::RB_COPY_DEST_PITCH, (64u << 16) | 128);
        q.reg(xe::RB_COPY_DEST_INFO, (kFmt8_8_8_8 << 7) | kEndian8in32);
        q.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
        q.draw(xe::kPrimRectangleList, 3);
        q.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);
        q.put(T3(pm4::kEventWriteShd, 3));
        q.put(0);
        q.put(fenceAddr);
        q.put(0x1234);
        cp.SetWritePtr(q.w % (ringBytes / 4));
        CHECK(cp.Pump() == CommandProcessor::Status::Idle);
        int polls = 0;
        while (mem.Read32(fenceAddr) != 0x1234 && polls < 2000) {
            usleep(1000);  // like the runtime's GPU thread, which polls every 0.5 ms
            cp.Pump();     // idle: finished GPU work applies its writes
            polls++;
        }
        CHECK(mem.Read32(fenceAddr) == 0x1234);
        r.Swap(0, 0, 0, none);
        r.Flush();
        Pixel(mem, dest3, 128, 64, 4, 4, p);
        CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255);
        Pixel(mem, dest3, 128, 64, 60, 60, p);
        CHECK(p[0] == 0 && p[1] == 0 && p[2] == 255 && p[3] == 255);
        std::printf("  fence written after %d idle polls, swap writeback (60,60) = %02x %02x %02x %02x\n", polls, p[0], p[1], p[2], p[3]);
    }

    // An interrupt after a fence: the handler sees the fence in memory (with frames in flight the
    // fence is deferred, so the command processor drains the backend before the handler runs).
    {
        const uint32_t fenceAddr = 0x284000;
        mem.Write32(fenceAddr, 0);
        uint32_t seen = 0;
        int calls = 0;
        cp.SetInterruptHandler([&](uint32_t) { seen = mem.Read32(fenceAddr); calls++; });
        Stream q{mem, ring, ringBytes / 4, cp.ReadPtr()};
        q.shader(0, xasm::ColorVS());
        q.shader(1, xasm::ColorPS());
        q.vfetch0(vb, 21);
        q.draw(xe::kPrimTriangleList, 3);
        q.put(T3(pm4::kEventWriteShd, 3));
        q.put(0);
        q.put(fenceAddr);
        q.put(0x5678);
        q.put(T3(pm4::kInterrupt, 1));
        q.put(1);
        cp.SetWritePtr(q.w % (ringBytes / 4));
        CHECK(cp.Pump() == CommandProcessor::Status::Idle);
        CHECK(calls == 1 && seen == 0x5678);
        std::printf("  interrupt handler saw fence %08x\n", seen);
        cp.SetInterruptHandler(nullptr);
    }

    // Tile replay: a frame drawn in two 32-row tiles (predicated tiling, window offset per tile),
    // each resolved to its band of a 128x64 destination. With tile once (the default) the second frame
    // draws the first tile at full height and skips the second; the destination must match.
    {
        RegisterFile none;
        const uint32_t blueVb = 0x105000, tileRect = 0x106000, destT[2] = {0x340000, 0x360000};
        const float blue[3][7] = {{0, 0, 0, 0, 0, 1, 1}, {128, 0, 0, 0, 0, 1, 1}, {0, 64, 0, 0, 0, 1, 1}};
        for (int v = 0; v < 3; v++)
            for (int c = 0; c < 7; c++) mem.Write32(blueVb + (v * 7 + c) * 4, F(blue[v][c]));
        for (int t = 0; t < 2; t++) {
            const float rc[6] = {0, float(32 * t), 128, float(32 * t), 128, float(32 * t + 32)};
            for (int i = 0; i < 6; i++) mem.Write32(tileRect + t * 64 + i * 4, F(rc[i]));
        }
        auto tiledFrame = [&](Stream& q, uint32_t dest) {
            q.shader(0, xasm::ColorVS());
            q.shader(1, xasm::ColorPS());
            q.reg(xe::PA_CL_VTE_CNTL, 0);
            q.reg(xe::PA_SU_SC_MODE_CNTL, 1u << 16);  // window offset applies to vertices and resolves
            // Full state: the second command processor starts from zeroed registers.
            q.alu(256 * 4, {1, 1, 1, 1});
            q.reg(xe::RB_SURFACE_INFO, 128);
            q.reg(xe::RB_COLOR_INFO, 0);
            q.reg(xe::RB_DEPTH_INFO, 0x200);
            q.reg(xe::RB_COLOR_MASK, 0xF);
            q.reg(xe::RB_DEPTHCONTROL, 0);
            q.reg(xe::RB_BLENDCONTROL0, 0x00010001);
            q.reg(xe::PA_SU_VTX_CNTL, 1);
            for (uint32_t t = 0; t < 2; t++) {
                q.put(T3(pm4::kSetBinSelect, 2));
                q.put(1u << (2 * t));  // high word: tile t, first pass
                q.put(0);
                q.reg(xe::PA_SC_WINDOW_OFFSET, ((0u - 32u * t) & 0x7FFF) << 16);
                q.reg(xe::PA_SC_WINDOW_SCISSOR_TL, (32u * t) << 16);
                q.reg(xe::PA_SC_WINDOW_SCISSOR_BR, ((32u * t + 32) << 16) | 128);
                q.vfetch0(blueVb, 21);
                q.draw(xe::kPrimRectangleList, 3);
                q.vfetch0(vb, 21);
                q.draw(xe::kPrimTriangleList, 3);
                q.vfetch0(tileRect + t * 64, 6);
                q.reg(xe::RB_COPY_CONTROL, 0);
                q.reg(xe::RB_COPY_DEST_BASE, dest + t * 32 * 128 * 4);
                q.reg(xe::RB_COPY_DEST_PITCH, ((64u - 32u * t) << 16) | 128);
                q.reg(xe::RB_COPY_DEST_INFO, (kFmt8_8_8_8 << 7) | kEndian8in32);
                q.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
                q.draw(xe::kPrimRectangleList, 3);
                q.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);
            }
            q.put(T3(pm4::kSetBinSelect, 2));
            q.put(0xFFFFFFFFu);
            q.put(0xFFFFFFFFu);
            q.reg(xe::PA_SC_WINDOW_OFFSET, 0);
            q.reg(xe::PA_SU_SC_MODE_CNTL, 0);
            q.reg(xe::PA_SC_WINDOW_SCISSOR_TL, 0x80000000u);
            q.reg(xe::PA_SC_WINDOW_SCISSOR_BR, (64u << 16) | 128);
            q.put(T3(pm4::kXeSwap, 4));
            q.put(0x50415753);
            q.put(0);
            q.put(128);
            q.put(64);
        };
        auto check = [&](const char* what, uint32_t dest) {
            uint8_t px[4];
            const uint32_t pts[4][2] = {{4, 4}, {40, 4}, {4, 40}, {40, 40}};
            const bool red[4] = {true, true, true, false};
            for (int i = 0; i < 4; i++) {
                Pixel(mem, dest, 128, 64, pts[i][0], pts[i][1], px);
                const bool ok = red[i] ? (px[0] == 255 && px[1] == 0 && px[2] == 0) : (px[0] == 0 && px[1] == 0 && px[2] == 255);
                if (!ok) std::printf("FAIL %s (%u,%u) = %02x %02x %02x %02x\n", what, pts[i][0], pts[i][1], px[0], px[1], px[2], px[3]);
                CHECK(ok);
            }
        };
        // Normal path.
        Stream q{mem, ring, ringBytes / 4, cp.ReadPtr()};
        tiledFrame(q, destT[0]);
        cp.SetWritePtr(q.w % (ringBytes / 4));
        CHECK(cp.Pump() == CommandProcessor::Status::Idle);
        r.Flush();
        check("tiles", destT[0]);
        // Tile once: the first frame learns the tile height, the second skips the second tile.
        CommandProcessor cpT(mem, r);  // default: tile once
        const uint32_t ring2 = 0x380000;
        cpT.SetRingBuffer(ring2, ringBytes);
        Stream q2{mem, ring2, ringBytes / 4};
        tiledFrame(q2, destT[1]);
        cpT.SetWritePtr(q2.w % (ringBytes / 4));
        CHECK(cpT.Pump() == CommandProcessor::Status::Idle);
        r.Flush();
        CHECK(cpT.GetStats().tileSkips == 0);
        for (uint32_t i = 0; i < 64 * 128; i++) mem.Write32(destT[1] + i * 4, 0);
        // Clear EDRAM to green, so rows the second frame does not draw show up.
        q2.vfetch0(rectVb, 6);
        q2.reg(xe::RB_COLOR_CLEAR, 0xFF00FF00u);
        q2.reg(xe::RB_COLOR_CLEAR_LO, 0xFF00FF00u);
        q2.reg(xe::RB_COPY_CONTROL, (1u << 8) | (3u << 20));
        q2.reg(xe::RB_MODECONTROL, xe::kEdramCopy);
        q2.draw(xe::kPrimRectangleList, 3);
        q2.reg(xe::RB_MODECONTROL, xe::kEdramColorDepth);
        tiledFrame(q2, destT[1]);
        cpT.SetWritePtr(q2.w % (ringBytes / 4));
        CHECK(cpT.Pump() == CommandProcessor::Status::Idle);
        r.Flush();
        CHECK(cpT.GetStats().tileSkips == 3);  // two draws and a resolve of tile 1, second frame
        CHECK(st.tileResolvesExtended >= 1);
        check("tile once", destT[1]);
        std::printf("  tile once: %llu draws/resolves skipped, %llu resolves extended\n",
                    (unsigned long long)cpT.GetStats().tileSkips, (unsigned long long)st.tileResolvesExtended.load());
        (void)none;
    }

    // Direct draw records: the same red triangle as the first section, and a green one that only
    // changes the vertex fetch constant, drawn from hand-made records with the shaders read from
    // guest memory; then a resolve built from the records' register state.
    {
        const uint32_t vsAddr = 0x108000, psAddr = 0x10A000, greenVb = 0x107000, destD = 0x3A0000;
        const std::vector<uint32_t> vsU = xasm::ColorVS(), psU = xasm::ColorPS();
        for (size_t i = 0; i < vsU.size(); i++) mem.Write32(vsAddr + uint32_t(i) * 4, vsU[i]);
        for (size_t i = 0; i < psU.size(); i++) mem.Write32(psAddr + uint32_t(i) * 4, psU[i]);
        const float green[3][7] = {{64, 0, 0, 0, 1, 0, 1}, {128, 0, 0, 0, 1, 0, 1}, {64, 64, 0, 0, 1, 0, 1}};
        for (int v = 0; v < 3; v++)
            for (int c = 0; c < 7; c++) mem.Write32(greenVb + (v * 7 + c) * 4, F(green[v][c]));
        std::vector<uint8_t> payload;
        auto run = [&](uint32_t first, std::initializer_list<uint32_t> v) {
            DirectRegRun h{first, uint32_t(v.size())};
            const uint8_t* hp = reinterpret_cast<const uint8_t*>(&h);
            payload.insert(payload.end(), hp, hp + sizeof h);
            for (uint32_t x : v) { const uint8_t* xp = reinterpret_cast<const uint8_t*>(&x); payload.insert(payload.end(), xp, xp + 4); }
        };
        auto record = [&](uint32_t vbAddr) {
            DirectDraw d;
            d.primType = xe::kPrimTriangleList;
            d.indexCount = 3;
            d.vsObj = 0x40001000; d.vsUcode = vsAddr; d.vsBytes = uint32_t(vsU.size() * 4);
            d.psObj = 0x40002000; d.psUcode = psAddr; d.psBytes = uint32_t(psU.size() * 4);
            run(pm4::kFetchConstBase, {vbAddr | 3, (21u << 2) | kEndian8in32});
            d.payloadBytes = uint32_t(payload.size());
            return d;
        };
        // First record of a frame: all the state it needs.
        run(xe::RB_SURFACE_INFO, {128, 0, 0x200});  // surface, colour, depth info
        run(xe::RB_COLOR_MASK, {0xF});
        run(xe::RB_DEPTHCONTROL, {0, 0x00010001});   // depth control, blend control 0
        run(xe::PA_SU_SC_MODE_CNTL, {0, 0});         // SC mode, VTE (positions in pixels)
        run(xe::PA_SC_WINDOW_OFFSET, {0, 0x80000000u, (64u << 16) | 128});
        run(xe::PA_SU_VTX_CNTL, {1});
        run(pm4::kAluConstBase + 256 * 4, {F(1), F(1), F(1), F(1)});  // PS c0
        const uint64_t draws0 = st.draws, reads0 = st.directShaderReads;
        DirectDraw d1 = record(vb);
        // Clear to blue: copy and clear registers for the resolve.
        RegisterFile rr;
        rr[pm4::kFetchConstBase] = rectVb | 3;
        rr[pm4::kFetchConstBase + 1] = (6u << 2) | kEndian8in32;
        rr[xe::RB_COLOR_CLEAR] = rr[xe::RB_COLOR_CLEAR_LO] = 0xFFFF0000u;
        rr[xe::RB_COPY_CONTROL] = (1u << 8) | (3u << 20);
        rr[xe::RB_MODECONTROL] = xe::kEdramCopy;
        CHECK(r.DrawDirect(d1, payload.data()));  // applies the state (this triangle is cleared away)
        // Clear with the resolve built from that state, then draw the red triangle again (applying
        // the same payload twice changes nothing).
        RegisterFile rc = r.DirectRegisters();
        for (uint32_t i : {uint32_t(xe::RB_COLOR_CLEAR), uint32_t(xe::RB_COLOR_CLEAR_LO), uint32_t(xe::RB_COPY_CONTROL), uint32_t(xe::RB_MODECONTROL),
                           pm4::kFetchConstBase, pm4::kFetchConstBase + 1})
            rc[i] = rr[i];
        r.Resolve(rc);
        CHECK(r.DrawDirect(d1, payload.data()));
        // Second record: only the vertex fetch constant changes.
        payload.clear();
        DirectDraw d2 = record(greenVb);
        CHECK(r.DrawDirect(d2, payload.data()));
        CHECK(r.DirectRegisters()[xe::RB_COLOR_MASK] == 0xF);  // state from the first record kept
        // Resolve the frame.
        rc[xe::RB_COPY_CONTROL] = 0;
        rc[xe::RB_COPY_DEST_BASE] = destD;
        rc[xe::RB_COPY_DEST_PITCH] = (64u << 16) | 128;
        rc[xe::RB_COPY_DEST_INFO] = (kFmt8_8_8_8 << 7) | kEndian8in32;
        r.Resolve(rc);
        r.Flush();
        CHECK(st.draws == draws0 + 3);
        CHECK(st.directShaderReads == reads0 + 2);  // each shader read once, then reused
        uint8_t px[4];
        const uint32_t pts[3][2] = {{4, 4}, {100, 4}, {100, 60}};
        const uint8_t want[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        for (int i = 0; i < 3; i++) {
            Pixel(mem, destD, 128, 64, pts[i][0], pts[i][1], px);
            const bool ok = px[0] == want[i][0] && px[1] == want[i][1] && px[2] == want[i][2];
            if (!ok) std::printf("FAIL direct (%u,%u) = %02x %02x %02x %02x\n", pts[i][0], pts[i][1], px[0], px[1], px[2], px[3]);
            CHECK(ok);
        }
        std::printf("  direct records: %llu draws, %llu register dwords, %llu shader reads\n", (unsigned long long)st.directDraws.load(),
                    (unsigned long long)st.directRegs.load(), (unsigned long long)st.directShaderReads.load());
    }

    if (failures) { std::printf("gpu_render_test: %d failure(s)\n", failures); return 1; }
    std::printf("gpu_render_test: ok\n");
    return 0;
}
