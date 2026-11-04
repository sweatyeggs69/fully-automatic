// Runtime test for the HLE imports: XamInput*, XAudio* render driver (with a
// fake guest callback driven by the audio thread) and the XMA context exports.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "fa/hle/services.hpp"
#include "fa/rt/hle_hooks.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/loader.hpp"
#include "fa/rt/memory.hpp"

using namespace fa::rt;

PPC_EXTERN_FUNC(XamInputGetState);
PPC_EXTERN_FUNC(XamInputGetCapabilities);
PPC_EXTERN_FUNC(XamInputSetState);
PPC_EXTERN_FUNC(XamInputGetKeystroke);
PPC_EXTERN_FUNC(XAudioGetSpeakerConfig);
PPC_EXTERN_FUNC(XAudioRegisterRenderDriverClient);
PPC_EXTERN_FUNC(XAudioUnregisterRenderDriverClient);
PPC_EXTERN_FUNC(XAudioSubmitRenderDriverFrame);
PPC_EXTERN_FUNC(XMACreateContext);
PPC_EXTERN_FUNC(XMAReleaseContext);
PPC_EXTERN_FUNC(XMAInitializeContext);
PPC_EXTERN_FUNC(XMASetInputBuffer0);
PPC_EXTERN_FUNC(XMASetInputBuffer0Valid);
PPC_EXTERN_FUNC(XMAIsInputBuffer0Valid);
PPC_EXTERN_FUNC(XMASetInputBufferReadOffset);
PPC_EXTERN_FUNC(XMAGetInputBufferReadOffset);
PPC_EXTERN_FUNC(XMAGetOutputBufferWriteOffset);
PPC_EXTERN_FUNC(XMASetLoopData);
PPC_EXTERN_FUNC(XMAEnableContext);
PPC_EXTERN_FUNC(XMADisableContext);
PPC_EXTERN_FUNC(XMsgStartIORequest);
PPC_EXTERN_FUNC(XMsgStartIORequestEx);
PPC_EXTERN_FUNC(XamGetSystemVersion);
PPC_EXTERN_FUNC(NtCreateEvent);
PPC_EXTERN_FUNC(XamContentCreateEnumerator);
PPC_EXTERN_FUNC(XamShowDeviceSelectorUI);
PPC_EXTERN_FUNC(XamContentCreateDeviceEnumerator);
PPC_EXTERN_FUNC(XamEnumerate);
PPC_EXTERN_FUNC(XamContentGetDeviceData);
PPC_EXTERN_FUNC(XamContentGetDeviceName);
PPC_EXTERN_FUNC(XamContentGetDeviceState);
PPC_EXTERN_FUNC(XamContentCreate);
PPC_EXTERN_FUNC(XamContentClose);
PPC_EXTERN_FUNC(XamContentDelete);
PPC_EXTERN_FUNC(_vscwprintf);
PPC_EXTERN_FUNC(vswprintf);

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

template <class... A>
static uint32_t K(PPCContext& ctx, PPCFunc* f, A... a) {
    uint32_t v[] = {uint32_t(a)..., 0};
    PPCRegister* r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (size_t i = 0; i < sizeof...(a); ++i) r[i]->u64 = v[i];
    f(ctx, g_base);
    return ctx.r3.u32;
}

// Scratch block shared with the audio callback: [0] driver, [4] frames, [8] samples ptr.
static uint32_t g_scratch = 0;

// The game's render callback: r3 points at the wrapped callback argument.
static void audio_callback(PPCContext& ctx, uint8_t*) {
    uint32_t arg_ptr = ctx.r3.u32;
    if (ld32(arg_ptr) != 0xC0FFEE) return;
    K(ctx, XAudioSubmitRenderDriverFrame, ld32(g_scratch), ld32(g_scratch + 8));
    st32(g_scratch + 4, ld32(g_scratch + 4) + 1);
}

static void guest_main(PPCContext& ctx, uint8_t*) {
    uint32_t s = g_mem.malloc(0x800);
    g_scratch = s;

    // ---- input: player 1 always exists (keyboard), player 2 does not.
    uint32_t st = s + 0x100;
    CHECK(K(ctx, XamInputGetState, 0, 1, st) == 0);
    CHECK(K(ctx, XamInputGetState, 0xFF, 0, st) == 0);
    CHECK(K(ctx, XamInputGetState, 1, 1, st) == 0x48F);
    CHECK(K(ctx, XamInputGetCapabilities, 0, 1, st) == 0);
    CHECK(ld8(st) == 1 && ld16(st + 16) == 0xFFFF);
    st32(st, 0);
    CHECK(K(ctx, XamInputSetState, 0, 0, st) == 0);
    CHECK(K(ctx, XamInputGetKeystroke, 0, 0, st) == 0x10D2);

    // ---- render driver
    CHECK(K(ctx, XAudioGetSpeakerConfig, st) == 0 && ld32(st) == 0x00010001);
    uint32_t frame = g_mem.malloc(256 * 6 * 4);
    st32(s + 8, frame);
    uint32_t cb = s + 0x200;
    st32(cb, 0x82000300);
    st32(cb + 4, 0xC0FFEE);
    CHECK(K(ctx, XAudioRegisterRenderDriverClient, cb, s) == 0);
    CHECK((ld32(s) & 0xFFFF0000) == 0x41550000);
    // The silent clock asks for a frame every 5.3 ms (after an initial burst).
    for (int i = 0; i < 400 && ld32(s + 4) < 30; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(ld32(s + 4) >= 30);
    CHECK(K(ctx, XAudioUnregisterRenderDriverClient, ld32(s)) == 0);
    uint32_t after = ld32(s + 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(ld32(s + 4) == after);

    // ---- XMA context exports
    uint32_t xc = s + 0x300;
    CHECK(K(ctx, XMACreateContext, xc) == 0);
    uint32_t c = ld32(xc);
    CHECK(c >= 0xA0000000);
    uint32_t in = g_mem.alloc(Heap::Physical, 0x10000, 0x10000);
    uint32_t out = g_mem.alloc(Heap::Physical, 0x2000, 0x1000);
    uint32_t init = s + 0x340;  // XMA_CONTEXT_INIT
    for (int i = 0; i < 56; i += 4) st32(init + i, 0);
    st32(init + 0, in);
    st32(init + 4, 4);
    st32(init + 20, out);
    st32(init + 24, 8);
    st32(init + 36, 1);
    st32(init + 40, 3);
    CHECK(K(ctx, XMAInitializeContext, c, init) == 0);
    CHECK(K(ctx, XMAIsInputBuffer0Valid, c) == 0);  // initialize clears the valid bits
    CHECK(K(ctx, XMASetInputBuffer0, c, in, 2) == 0);
    CHECK(K(ctx, XMASetInputBuffer0Valid, c) == 0);
    CHECK(K(ctx, XMAIsInputBuffer0Valid, c) == 1);
    CHECK(K(ctx, XMASetInputBufferReadOffset, c, 32) == 0);
    CHECK(K(ctx, XMAGetInputBufferReadOffset, c) == 32);
    uint32_t loop = s + 0x380;
    st32(loop, 64);
    st32(loop + 4, 4096);
    st8(loop + 8, 255);
    CHECK(K(ctx, XMASetLoopData, c, loop) == 0);
    CHECK(K(ctx, XMAEnableContext, c) == 0);
    CHECK(K(ctx, XMADisableContext, c, 1) == 0);
    CHECK(K(ctx, XMAReleaseContext, c) == 0);

    // ---- XMsg: synchronous music player query, then an overlapped one that signals an event.
    CHECK(K(ctx, XamGetSystemVersion) == 0);

    // ---- content: enumerator reports a buffer size and handle; device selector picks device 1.
    {
        uint32_t cb = s + 0x300, h = s + 0x304;
        CHECK(K(ctx, XamContentCreateEnumerator, 0, 0, 1, 0, 8, cb, h) == 0);
        CHECK(ld32(cb) == 8 * 0x134 && ld32(h) != 0);
        uint32_t dev = s + 0x308, ov = s + 0x310;
        std::memset(host(ov), 0, 0x1C);
        CHECK(K(ctx, XamShowDeviceSelectorUI, 0, 1, 0x200, 0, dev, ov) == 997);
        CHECK(ld32(dev) == 1 && ld32(ov) == 0);
        CHECK(K(ctx, XamShowDeviceSelectorUI, 0, 1, 0x200, 0, dev, 0) == 0);
    }

    // ---- device list, device info, and a save created, listed, reopened and deleted.
    {
        uint32_t cb = s + 0x300, h = s + 0x304, buf = s + 0x400, cnt = s + 0x30C;
        CHECK(K(ctx, XamContentCreateDeviceEnumerator, 1, 0, 4, cb, h) == 0);
        CHECK(ld32(cb) == 4 * 0x50);
        CHECK(K(ctx, XamEnumerate, ld32(h), 0, buf, 0, cnt, 0) == 0);
        CHECK(ld32(cnt) == 1 && ld32(buf) == 1 && ld32(buf + 4) == 1 && ld16(buf + 24) == 'H');
        CHECK(K(ctx, XamEnumerate, ld32(h), 0, buf, 0, cnt, 0) == 18);
        CHECK(K(ctx, XamContentGetDeviceData, 1, buf) == 0 && ld32(buf) == 1);
        CHECK(K(ctx, XamContentGetDeviceData, 7, buf) == 0x48F);
        CHECK(K(ctx, XamContentGetDeviceName, 1, buf, 32) == 0 && ld16(buf) == 'H' && ld16(buf + 20) == 0);
        CHECK(K(ctx, XamContentGetDeviceState, 1, 0) == 0);

        uint32_t data = s + 0x180, root = s + 0x2A0, disp = s + 0x2B0;
        std::memset(host(data), 0, 0x134);
        st32(data, 1); st32(data + 4, 1);
        st16(data + 8, 'S'); st16(data + 10, 'v');
        std::memcpy(host(data + 0x108), "slot0", 6);
        std::memcpy(host(root), "save", 5);
        CHECK(K(ctx, XamContentCreate, 0, root, data, 3, disp, 0, 0) == 2);  // OPEN_EXISTING, none yet
        CHECK(K(ctx, XamContentCreate, 0, root, data, 1, disp, 0, 0) == 0 && ld32(disp) == 1);
        CHECK(fa::hle::services().vfs.is_mounted("save:"));
        CHECK(K(ctx, XamContentClose, root, 0) == 0);
        CHECK(!fa::hle::services().vfs.is_mounted("save:"));
        CHECK(K(ctx, XamContentCreate, 0, root, data, 1, disp, 0, 0) == 183);  // CREATE_NEW exists
        CHECK(K(ctx, XamContentCreate, 0, root, data, 3, disp, 0, 0) == 0 && ld32(disp) == 2);
        K(ctx, XamContentClose, root, 0);
        CHECK(K(ctx, XamContentCreateEnumerator, 0, 0, 1, 0, 4, cb, h) == 0);
        CHECK(K(ctx, XamEnumerate, ld32(h), 0, buf, 0, cnt, 0) == 0 && ld32(cnt) == 1);
        CHECK(ld32(buf + 4) == 1 && ld16(buf + 8) == 'S' && std::memcmp(host(buf + 0x108), "slot0", 6) == 0);
        CHECK(K(ctx, XamContentDelete, 0, data, 0) == 0);
        CHECK(K(ctx, XamContentCreate, 0, root, data, 3, disp, 0, 0) == 2);
    }

    // ---- wide printf: %d %s (wide) %hs (narrow) %x %5.2f %I64d, args in 8-byte slots.
    {
        uint32_t fmt = s + 0x340, ws = s + 0x3A0, ns = s + 0x3C0, va = s + 0x2C0, out = s + 0x200;
        const char* f = "n=%d s=%s h=%hs x=%04X f=%5.2f q=%I64d%%";
        uint32_t o = fmt; for (const char* c = f; *c; ++c, o += 2) st16(o, uint8_t(*c));
        st16(o, 0);
        st16(ws, 'W'); st16(ws + 2, 'i'); st16(ws + 4, 0);
        std::memcpy(host(ns), "ok", 3);
        double d = 3.14159; uint64_t db; std::memcpy(&db, &d, 8);
        st64(va, uint64_t(int64_t(-42))); st64(va + 8, ws); st64(va + 16, ns);
        st64(va + 24, 0xAB); st64(va + 32, db); st64(va + 40, 1ull << 40);
        const char* want = "n=-42 s=Wi h=ok x=00AB f= 3.14 q=1099511627776%";
        uint32_t n = K(ctx, _vscwprintf, fmt, va);
        CHECK(n == std::strlen(want));
        CHECK(K(ctx, vswprintf, out, fmt, va) == std::strlen(want));
        bool same = true;
        for (size_t i = 0; i <= std::strlen(want); ++i) same &= ld16(out + i * 2) == uint8_t(want[i]);
        CHECK(same);
    }
    uint32_t args = s + 0x200, state = s + 0x220, ov = s + 0x240;
    st32(args, 2);
    st32(args + 4, state);
    st32(state, 0xFFFFFFFF);
    CHECK(K(ctx, XMsgStartIORequest, 0xFA, 0x00070009, 0, args, 8) == 0);  // XMPGetStatus
    CHECK(ld32(state) == 0);
    CHECK(K(ctx, NtCreateEvent, s + 0x260, 0, 0, 0) == 0);
    uint32_t ev = ld32(s + 0x260);
    for (int i = 0; i < 7; ++i) st32(ov + i * 4, 0);
    st32(ov + 0x0C, ev);
    CHECK(K(ctx, XMsgStartIORequestEx, 0xFB, 0x000B0006, ov, args, 8, 0) == 997);  // XUserSetContext
    CHECK(ld32(ov) == 0);
    {
        auto e = g_kernel.get_handle(ev);
        CHECK(e && static_cast<KEvent&>(*e).set);
    }
    CHECK(K(ctx, XMsgStartIORequest, 0x42, 1, 0, 0, 0) == 0x80070057);  // unknown app
    ctx.r3.u64 = g_fail;
}

PPCFuncMapping PPCFuncMappings[] = {
    {0x82000200, guest_main},
    {0x82000300, audio_callback},
    {0, nullptr},
};

int main() {
    CHECK(g_mem.init());
    g_mem.commit_fixed(0x7FEA0000, 0x10000);
    std::vector<uint8_t> image(0x10000, 0);
    CHECK(map_image(image.data(), 0x82000000, 0x10000));

    // A tiny "extracted game folder" and a private data dir.
    auto dir = std::filesystem::temp_directory_path() / ("fa-hle-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir / "game" / "Media");
    std::ofstream(dir / "game" / "Media" / "hello.txt") << "hello";
    setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
    fa::hle::ServicesConfig cfg;
    cfg.game_path = (dir / "game").string();
    cfg.data_dir = (dir / "data").string();
    cfg.audio_output = false;
    std::string err;
    CHECK(fa::hle::init_services(cfg, make_runtime_hooks(), err));
    fa::hle::FileInfo fi;
    CHECK(fa::hle::services().vfs.stat("game:\\MEDIA\\hello.txt", fi) == 0 && fi.size == 5);

    uint32_t rc = g_kernel.run_main_thread(0x82000200, 0x40000);
    CHECK(rc == 0);
    fa::hle::shutdown_services();
    std::filesystem::remove_all(dir);
    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    std::fflush(stdout);
    std::_Exit(g_fail ? 1 : 0);
}
