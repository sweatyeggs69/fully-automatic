// Video driver (Vd*) glue. The game talks to the GPU through the ring buffer and a few
// kernel calls. For now this records the state it sets up, answers display queries for
// 1280x720 and delivers a 60 Hz vsync interrupt to the registered graphics callback. The
// command processor (src/gpu) is attached to the recorded ring buffer later.
#include "fa/rt/import.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace fa::rt;

namespace fa::rt {
struct VdState {
    std::atomic<uint32_t> callback{0}, callback_ctx{0};
    uint32_t ring_base = 0, ring_size_log2 = 0;
    uint32_t rptr_writeback = 0, rptr_block_log2 = 0;
    uint32_t gpu_id_addr = 0;
};
VdState g_vd;
}  // namespace fa::rt

namespace {
std::atomic<bool> g_vsync_started{false};

void vsync_thread() {
    attach_guest_thread("vsync");
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        uint32_t cb = g_vd.callback.load();
        if (cb) invoke_guest(cb, 0, g_vd.callback_ctx.load());  // source 0 = vsync
    }
}
}  // namespace

FA_IMPORT(VdInitializeEngines) { ret32(ctx, 0); }

FA_IMPORT(VdSetGraphicsInterruptCallback) {
    g_vd.callback = arg32(ctx, 0);
    g_vd.callback_ctx = arg32(ctx, 1);
    if (g_vd.callback && !g_vsync_started.exchange(true)) std::thread(vsync_thread).detach();
}

FA_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { g_vd.gpu_id_addr = arg32(ctx, 0); }

FA_IMPORT(VdInitializeRingBuffer) {
    g_vd.ring_base = arg32(ctx, 0);
    g_vd.ring_size_log2 = arg32(ctx, 1);
}

FA_IMPORT(VdEnableRingBufferRPtrWriteBack) {
    g_vd.rptr_writeback = arg32(ctx, 0);
    g_vd.rptr_block_log2 = arg32(ctx, 1);
}

FA_IMPORT(VdQueryVideoMode) {
    uint32_t p = arg32(ctx, 0);
    if (!p) return;
    for (uint32_t i = 0; i < 0x30; i += 4) st32(p + i, 0);
    st32(p + 0x00, 1280);   // display width
    st32(p + 0x04, 720);    // display height
    st32(p + 0x08, 0);      // interlaced
    st32(p + 0x0C, 1);      // widescreen
    st32(p + 0x10, 1);      // hi def
    float hz = 60.0f;
    uint32_t bits;
    std::memcpy(&bits, &hz, 4);
    st32(p + 0x14, bits);   // refresh rate
    st32(p + 0x18, 1);      // video standard (NTSC-M)
}

FA_IMPORT(VdQueryVideoFlags) { ret32(ctx, 0x00000001); }
FA_IMPORT(VdCallGraphicsNotificationRoutines) { }
FA_IMPORT(VdIsHSIOTrainingSucceeded) { ret32(ctx, 1); }
FA_IMPORT(VdGetGraphicsAsicID) { ret32(ctx, 0x11); }
FA_IMPORT(VdEnableDisableClockGating) { }
FA_IMPORT(VdShutdownEngines) { }
FA_IMPORT(VdRetrainEDRAM) { ret32(ctx, 0); }
FA_IMPORT(VdRetrainEDRAMWorker) { ret32(ctx, 0); }
FA_IMPORT(VdPersistDisplay) { ret32(ctx, 0); }
FA_IMPORT(KiApcNormalRoutineNop) { }

FA_IMPORT(VdGetCurrentDisplayGamma) {
    // (type_ptr, gamma_ptr)
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), 2);
    float g = 2.22222233f;
    uint32_t bits;
    std::memcpy(&bits, &g, 4);
    if (arg32(ctx, 1)) st32(arg32(ctx, 1), bits);
}

FA_IMPORT(VdGetCurrentDisplayInformation) {
    uint32_t p = arg32(ctx, 0);
    if (!p) return;
    for (uint32_t i = 0; i < 0x58; i += 4) st32(p + i, 0);
    st16(p + 0x00, 1280);   // front buffer width
    st16(p + 0x02, 720);    // front buffer height
}

FA_IMPORT(VdSetDisplayMode) { ret32(ctx, 0); }
FA_IMPORT(VdInitializeScalerCommandBuffer) { ret32(ctx, 0); }
FA_IMPORT(VdSwap) { ret32(ctx, 0); }
