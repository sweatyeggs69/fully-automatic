// Video driver (Vd*) glue. The game talks to the GPU through the ring buffer and a few
// kernel calls. For now this records the state it sets up, answers display queries for
// 1280x720 and delivers a 60 Hz vsync interrupt to the registered graphics callback. The
// command processor (src/gpu) runs the ring buffer into a FrameBackend, whose frames the
// window in main.cpp shows.
#include "fa/rt/import.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include "fa/gpu/command_processor.hpp"
#include "fa/gpu/frame_backend.hpp"
#include "fa/gpu/gpu_profile.hpp"
#include "fa/rt/video.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace fa::rt;

namespace fa::rt {
struct VdState {
    std::atomic<uint32_t> callback{0}, callback_ctx{0};
    uint32_t ring_base = 0, ring_size_log2 = 0, ring_size_bytes = 0;
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
        if (cb) {
            static uint64_t n = 0;
            if (n == 0 || n == 60 || n == 600)
                std::fprintf(stderr, "[vd] vsync callback #%llu cb=%08x ctx=%08x\n", (unsigned long long)n,
                             cb, g_vd.callback_ctx.load());
            n++;
            // The game's callback only does its vblank work when bit 0 of the display interrupt
            // status register (GPU register page + 0x6544) is set, as the hardware would at vblank.
            st32(0x7FC86544, 1);
            invoke_guest(cb, 0, g_vd.callback_ctx.load());  // source 0 = vsync
        }
    }
}
}  // namespace

FA_IMPORT(VdInitializeEngines) { ret32(ctx, 0); }

FA_IMPORT(VdSetGraphicsInterruptCallback) {
    g_vd.callback = arg32(ctx, 0);
    g_vd.callback_ctx = arg32(ctx, 1);
    std::fprintf(stderr, "[vd] VdSetGraphicsInterruptCallback cb=%08x ctx=%08x\n", g_vd.callback.load(),
                 g_vd.callback_ctx.load());
    if (g_vd.callback && !g_vsync_started.exchange(true)) std::thread(vsync_thread).detach();
}

FA_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { g_vd.gpu_id_addr = arg32(ctx, 0); }

// The kernel hands these around as physical addresses; physical p lives at 0xA0000000 + p.
static uint32_t phys_to_guest(uint32_t a) { return a < 0x20000000 ? a + 0xA0000000u : a; }

namespace {
// The GPU addresses memory physically: physical p lives at 0xA0000000 + p in the guest window,
// and 0xC0000000 / 0xE0000000 are aliases of the same memory. Accesses to memory the game never
// allocated are ignored (and reported once) instead of faulting the GPU thread.
struct RtGuestMemory : fa::gpu::GuestMemory {
    static uint32_t tr(uint32_t a) {
        if (a < 0x20000000) return a + 0xA0000000u;
        if (a >= 0xC0000000u) return 0xA0000000u | (a & 0x1FFFFFFFu);
        return a;
    }
    static bool mapped(uint32_t g, uint32_t orig) {
        if (g_mem.is_committed(g)) return true;
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 20)
            std::fprintf(stderr, "[gpu] access to unmapped memory %08x (guest %08x)\n", orig, g);
        return false;
    }
    uint32_t Read32(uint32_t a) const override {
        uint32_t g = tr(a);
        return mapped(g, a) ? ld32(g) : 0;
    }
    void Write32(uint32_t a, uint32_t v) override {
        uint32_t g = tr(a);
        if (mapped(g, a)) st32(g, v);
    }
    const uint8_t* HostPtr(uint32_t a, uint32_t size) const override {
        uint32_t g = tr(a);
        uint32_t st, sz;
        // The whole range must sit inside one allocation (textures, front buffer).
        if (!g_mem.find_allocation(g, st, sz) || uint64_t(g) + size > uint64_t(st) + sz) {
            static std::atomic<int> warned{0};
            if (warned.fetch_add(1) < 20)
                std::fprintf(stderr, "[gpu] %u bytes at %08x (guest %08x) not inside one allocation\n", size, a, g);
            return nullptr;
        }
        return host(g);
    }
    uint8_t* HostPtrWrite(uint32_t a, uint32_t size) override {
        return const_cast<uint8_t*>(HostPtr(a, size));  // resolves write textures / the front buffer
    }
};

constexpr uint32_t kCpRbRptr = 0x7FC80000 + 0x1C4 * 4;
constexpr uint32_t kCpRbWptr = 0x7FC80000 + 0x1C5 * 4;

RtGuestMemory g_gpu_mem;
fa::gpu::FrameBackend g_gpu_backend(g_gpu_mem);
fa::gpu::CommandProcessor* g_cp = nullptr;
std::atomic<bool> g_gpu_started{false};
std::atomic<fa::gpu::Backend*> g_backend_override{nullptr};

void gpu_thread() {
    attach_guest_thread("gpu");
    g_cp->SetInterruptHandler([](uint32_t cpu_mask) {
        uint32_t cb = g_vd.callback.load();
        if (!cb) return;
        // The interrupt is raised on every CPU in the mask. D3D's handler acknowledges it by
        // clearing its own processor's bit in the scratch block the GPU then waits on.
        for (uint8_t cpu = 0; cpu < 6; cpu++) {
            if (!(cpu_mask & (1u << cpu))) continue;
            set_current_processor(cpu);
            invoke_guest(cb, 1, g_vd.callback_ctx.load());  // source 1 = command processor
        }
        set_current_processor(0);
    });
    uint32_t last_wptr = 0;
    const bool trace = std::getenv("FA_TRACE_GPU") != nullptr;
    for (;;) {
        uint32_t w = ld32(kCpRbWptr);
        g_cp->SetWritePtr(w);
        {
            fa::gpu::prof::Scope ps(fa::gpu::prof::kPump);
            g_cp->Pump();
        }
        st32(kCpRbRptr, g_cp->ReadPtr());
        if (w != last_wptr) {
            last_wptr = w;
            if (trace)
                std::fprintf(stderr, "[gpu] wptr=%u rptr=%u packets=%llu draws=%llu swaps=%llu unknown=%llu\n", w,
                             g_cp->ReadPtr(), (unsigned long long)g_cp->GetStats().packets,
                             (unsigned long long)g_cp->GetStats().draws, (unsigned long long)g_cp->GetStats().swaps,
                             (unsigned long long)g_cp->GetStats().unknown);
            static auto last_hist = std::chrono::steady_clock::now();
            if (trace && std::chrono::steady_clock::now() - last_hist > std::chrono::seconds(5)) {
                last_hist = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[gpu] unknown type-3 opcodes:");
                for (int op = 0; op < 128; op++)
                    if (g_cp->GetStats().unknownOps[op])
                        std::fprintf(stderr, " %02x:%llu", op, (unsigned long long)g_cp->GetStats().unknownOps[op]);
                std::fprintf(stderr, "\n");
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void start_gpu() {
    if (g_gpu_started.exchange(true)) return;
    fa::gpu::Backend* be = g_backend_override.load();
    g_cp = new fa::gpu::CommandProcessor(g_gpu_mem, be ? *be : static_cast<fa::gpu::Backend&>(g_gpu_backend));
    g_cp->SetRingBuffer(g_vd.ring_base, g_vd.ring_size_bytes);
    if (g_vd.rptr_writeback) g_cp->SetReadPtrWriteback(g_vd.rptr_writeback);
    std::thread(gpu_thread).detach();
}
}  // namespace

fa::gpu::FrameBackend& fa::rt::gpu_frame_backend() { return g_gpu_backend; }
fa::gpu::GuestMemory& fa::rt::gpu_memory() { return g_gpu_mem; }
void fa::rt::set_gpu_backend(fa::gpu::Backend* backend) {
    if (g_gpu_started.load()) std::fprintf(stderr, "[gpu] set_gpu_backend after the GPU started: ignored\n");
    g_backend_override.store(backend);
}

FA_IMPORT(VdInitializeRingBuffer) {
    // (physical base, log2 of the size in 8-byte units)
    g_vd.ring_base = phys_to_guest(arg32(ctx, 0));
    g_vd.ring_size_log2 = arg32(ctx, 1);
    g_vd.ring_size_bytes = 8u << g_vd.ring_size_log2;
    std::fprintf(stderr, "[gpu] ring buffer at %08x, %u bytes\n", g_vd.ring_base, g_vd.ring_size_bytes);
    start_gpu();
}

FA_IMPORT(VdEnableRingBufferRPtrWriteBack) {
    g_vd.rptr_writeback = phys_to_guest(arg32(ctx, 0));
    g_vd.rptr_block_log2 = arg32(ctx, 1);
    std::fprintf(stderr, "[gpu] rptr writeback at %08x (block log2 %u)\n", g_vd.rptr_writeback, g_vd.rptr_block_log2);
    if (g_cp) g_cp->SetReadPtrWriteback(g_vd.rptr_writeback);
}

FA_IMPORT(VdGetSystemCommandBuffer) {
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), 0xBEEF0000);
    if (arg32(ctx, 1)) st32(arg32(ctx, 1), 0xBEEF0001);
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
FA_IMPORT(VdSwap) {
    // (buffer, fetch constant, writeback, system buffer, system buffer id, frontbuffer address ptr,
    //  format ptr, color space ptr, width ptr, height ptr). The caller reserves 64 dwords of the
    // ring at `buffer` for the swap: load the frontbuffer fetch constant into fetch slot 0, emit
    // XE_SWAP, then pad with type-2 NOPs.
    uint32_t buf = arg32(ctx, 0), fetch = arg32(ctx, 1), fb_ptr = arg32(ctx, 5);
    uint32_t w_ptr = arg32(ctx, 8), h_ptr = arg32(ctx, 9);
    uint32_t i = 0;
    st32(buf + 4 * i++, (5u << 16) | 0x4800);  // type 0, 6 registers from SHADER_CONSTANT_FETCH_00_0
    for (uint32_t k = 0; k < 6; k++) st32(buf + 4 * i++, fetch ? ld32(fetch + 4 * k) : 0);
    st32(buf + 4 * i++, (3u << 30) | (3u << 16) | (0x64u << 8));  // XE_SWAP, 4 dwords
    st32(buf + 4 * i++, 0x53574150);                                // 'SWAP'
    st32(buf + 4 * i++, fb_ptr ? ld32(fb_ptr) & 0x1FFFFFFF : 0);
    st32(buf + 4 * i++, w_ptr ? ld32(w_ptr) : 1280);
    st32(buf + 4 * i++, h_ptr ? ld32(h_ptr) : 720);
    while (i < 64) st32(buf + 4 * i++, 0x80000000u);
    static int logged = 0;
    if (logged++ < 3)
        std::fprintf(stderr, "[vd] VdSwap buffer=%08x frontbuffer=%08x %ux%u\n", buf, fb_ptr ? ld32(fb_ptr) : 0,
                     w_ptr ? ld32(w_ptr) : 0, h_ptr ? ld32(h_ptr) : 0);
    ret32(ctx, 0);
}
