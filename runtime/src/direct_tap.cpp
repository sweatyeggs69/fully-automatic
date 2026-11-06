// Phase 1 of the direct graphics hooks (notes/direct-gpu-hooks-design.md): observe-only taps on
// the game's own D3D functions. Part of the fullauto executable only (it needs the recompiled
// code). Every tap calls the original function unchanged; FA_TAP=1 turns the counters on.
//
// Taps so far:
//   sub_82315500  DrawIndexedPrimitive(device, primType, ?, startIndex, indexCount): writes the
//                 DRAW_INDX packet (notes/d3d-map.md). Counted against the command processor's
//                 indexed draws; the totals must match (draws over 65535 indices are split, so
//                 compare index totals, not call counts).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "fa/gpu/command_processor.hpp"
#include "fa/rt/import.hpp"

PPC_FUNC_IMPL(__imp__sub_82315500);  // generated as extern "C"

namespace {
bool g_on = std::getenv("FA_TAP") != nullptr;
std::atomic<uint64_t> g_tap_calls{0}, g_tap_indices{0}, g_cp_draws{0}, g_cp_indices{0};
std::atomic<uint64_t> g_tap_last_calls{0};
int g_swaps = 0;

void on_cp_draw(const fa::gpu::DrawCall& dc) {
    if (!dc.indexed) return;
    g_cp_draws.fetch_add(1, std::memory_order_relaxed);
    g_cp_indices.fetch_add(dc.indexCount, std::memory_order_relaxed);
}
}  // namespace

PPC_FUNC(sub_82315500) {
    if (g_on) {
        // r3 device, r4 primType, r6 startIndex, r7 indexCount (reads of the registers the function itself uses).
        g_tap_calls.fetch_add(1, std::memory_order_relaxed);
        g_tap_indices.fetch_add(ctx.r7.u32, std::memory_order_relaxed);
    }
    __imp__sub_82315500(ctx, base);
}

namespace fa::rt {
// Called at every VdSwap.
void direct_tap_frame_end() {
    if (!g_on) return;
    if (!fa::gpu::g_draw_observer) fa::gpu::g_draw_observer = on_cp_draw;
    if (++g_swaps % 300) return;
    const uint64_t tc = g_tap_calls.load(), ti = g_tap_indices.load(), cd = g_cp_draws.load(), ci = g_cp_indices.load();
    std::fprintf(stderr, "[tap] swap %d: game DrawIndexed calls %llu indices %llu | CP indexed draws %llu indices %llu | index diff %lld\n",
                 g_swaps, (unsigned long long)tc, (unsigned long long)ti, (unsigned long long)cd, (unsigned long long)ci,
                 (long long)ti - (long long)ci);
}
}  // namespace fa::rt
