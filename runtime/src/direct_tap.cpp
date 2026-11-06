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
#include <unordered_map>

#include "fa/gpu/command_processor.hpp"
#include "fa/gpu/xenos.hpp"
#include "fa/rt/import.hpp"

namespace fa::rt { uint64_t gpu_predicated_skips(); }

PPC_FUNC_IMPL(__imp__sub_82315500);  // generated as extern "C"
PPC_FUNC_IMPL(__imp__sub_823152C0);

namespace {
bool g_on = std::getenv("FA_TAP") != nullptr;
std::atomic<uint64_t> g_tap_calls{0}, g_tap_indices{0}, g_cp_draws{0}, g_cp_indices{0};
std::atomic<uint64_t> g_tap_last_calls{0};
std::atomic<uint64_t> g_tap2_calls{0}, g_cp_auto_draws{0};
std::atomic<int> g_swaps_a{0};
int g_swaps = 0;
// Draw identity repeats within one game frame (CP thread only). If the command processor sees every
// draw about three times, the game is replaying its command buffer once per screen tile.
struct Seen { uint32_t n; uint64_t sig; };
std::unordered_map<uint64_t, Seen> g_frame_draws;
uint64_t g_rep_same_sig = 0, g_rep_diff_sig = 0;
int g_samples = 0;
int g_epoch = 0;
uint64_t g_hist[6] = {};   // [1..4] draws seen that many times in a frame, [5] five or more
uint64_t g_frames_hist = 0;

void on_cp_draw(const fa::gpu::DrawCall& dc, const fa::gpu::RegisterFile& regs) {
    if (!dc.indexed) { g_cp_auto_draws.fetch_add(1, std::memory_order_relaxed); return; }
    const int e = g_swaps_a.load(std::memory_order_relaxed);
    if (e != g_epoch) {
        for (auto& [k, c] : g_frame_draws) g_hist[c.n > 5 ? 5 : c.n]++;
        g_frame_draws.clear();
        g_epoch = e;
        g_frames_hist++;
    }
    uint64_t key = dc.indexGuestAddr * 0x9E3779B97F4A7C15ull ^ (uint64_t(dc.indexCount) << 8) ^ dc.vsHash * 31 ^ dc.psHash * 131 ^ dc.primType;
    // What differs between repeats of the same draw: the tile window (offset/scissor) and render target setup.
    const uint32_t w[7] = {regs[fa::gpu::xe::PA_SC_WINDOW_OFFSET], regs[fa::gpu::xe::PA_SC_WINDOW_SCISSOR_TL],
                           regs[fa::gpu::xe::PA_SC_WINDOW_SCISSOR_BR], regs[fa::gpu::xe::PA_SC_SCREEN_SCISSOR_TL],
                           regs[fa::gpu::xe::PA_SC_SCREEN_SCISSOR_BR], regs[fa::gpu::xe::RB_COLOR_INFO],
                           regs[fa::gpu::xe::RB_SURFACE_INFO]};
    uint64_t sig = 1469598103934665603ull;
    for (uint32_t v : w) sig = (sig ^ v) * 1099511628211ull;
    auto it = g_frame_draws.find(key);
    if (it == g_frame_draws.end()) {
        g_frame_draws[key] = {1, sig};
    } else {
        it->second.n++;
        if (it->second.sig == sig) g_rep_same_sig++;
        else {
            g_rep_diff_sig++;
            if (g_samples < 8 && g_swaps_a.load() > 1500) {
                g_samples++;
                std::fprintf(stderr, "[tap] repeat with different state: window offset %08x scissor %08x-%08x screen %08x-%08x color %08x surface %08x\n",
                             w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
            }
        }
    }
    g_cp_draws.fetch_add(1, std::memory_order_relaxed);
    g_cp_indices.fetch_add(dc.indexCount, std::memory_order_relaxed);
}
}  // namespace

PPC_FUNC(sub_823152C0) {
    if (g_on) g_tap2_calls.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_823152C0(ctx, base);
}

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
    g_swaps_a.store(++g_swaps, std::memory_order_relaxed);
    if (g_swaps % 300) return;
    const uint64_t tc = g_tap_calls.load(), ti = g_tap_indices.load(), cd = g_cp_draws.load(), ci = g_cp_indices.load();
    std::fprintf(stderr, "[tap] swap %d: game DrawIndexed calls %llu indices %llu | CP indexed draws %llu indices %llu | index diff %lld\n",
                 g_swaps, (unsigned long long)tc, (unsigned long long)ti, (unsigned long long)cd, (unsigned long long)ci,
                 (long long)ti - (long long)ci);
    std::fprintf(stderr, "[tap]   game non-indexed (823152C0) calls %llu | CP auto-index draws %llu | repeats of an identical CP draw within a frame (cumulative, 1x 2x 3x 4x 5x+): %llu %llu %llu %llu %llu over %llu frames\n",
                 (unsigned long long)g_tap2_calls.load(), (unsigned long long)g_cp_auto_draws.load(), (unsigned long long)g_hist[1],
                 (unsigned long long)g_hist[2], (unsigned long long)g_hist[3], (unsigned long long)g_hist[4],
                 (unsigned long long)g_hist[5], (unsigned long long)g_frames_hist);
    std::fprintf(stderr, "[tap]   repeats with identical window/target state %llu, with different state %llu | CP predicatedSkips %llu\n",
                 (unsigned long long)g_rep_same_sig, (unsigned long long)g_rep_diff_sig,
                 (unsigned long long)fa::rt::gpu_predicated_skips());
}
}  // namespace fa::rt
