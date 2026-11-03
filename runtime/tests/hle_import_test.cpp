// Runtime test for the HLE imports: XamInput*, XAudio* render driver (with a
// fake guest callback driven by the audio thread) and the XMA context exports.
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

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
    uint32_t s = g_mem.malloc(0x400);
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
