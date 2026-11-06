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
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "fa/gpu/command_processor.hpp"
#include "fa/gpu/xenos.hpp"
#include "fa/rt/import.hpp"

namespace fa::rt { uint64_t gpu_predicated_skips(); }

PPC_FUNC_IMPL(__imp__sub_82315500);  // generated as extern "C"
PPC_FUNC_IMPL(__imp__sub_823152C0);
PPC_FUNC_IMPL(__imp__sub_823162F8);

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

// ---- state parity -------------------------------------------------------------------------------
// The D3D device keeps a shadow of the GPU register file (notes/d3d-map.md, 0x82148508). Measured layout:
//   device+0x780  vertex shader float constants, 256 vec4 = registers 0x4000..0x43FF
//   device+0x1780 pixel shader float constants,  256 vec4 = registers 0x4400..0x47FF
//   device+0x480  fetch constants (textures and vertex streams), 32 x 6 dwords = registers 0x4800..0x48BF
// After the game's draw function has flushed dirty state, the shadow must equal what the command
// processor's register file holds when it executes that draw. The tap hashes the shadow per draw
// (game threads); the CP hashes its registers per draw; the CP's draw counts as matched when its hash
// is in the set of recent game hashes.
constexpr uint32_t kOffVs = 0x780, kOffPs = 0x1780, kOffTex = 0x480;
constexpr uint32_t kVsDwords = 1024, kPsDwords = 1024, kTexDwords = 192;
constexpr uint32_t kRegVs = 0x4000, kRegPs = 0x4400, kRegTex = 0x4800;
// Per-object constants are written straight into the command stream: 0x823162F8(device, ?, firstVec4,
// vec4Count) reserves ring space, writes the SET_CONSTANT header and returns the payload pointer; callers
// (821B9208, 821B8FA8) then fill it with 821B8510. Those vec4s bypass the device shadow, so they are left out
// of the constants hashes below (and counted here).
std::atomic<uint64_t> g_touched[8];  // bit per ALU vec4 (0..511)
std::mutex g_set_mu;
std::unordered_map<uint64_t, uint64_t> g_sets;  // (first << 32 | count) -> calls

inline bool touched(uint32_t vec4) { return g_touched[vec4 >> 6].load(std::memory_order_relaxed) >> (vec4 & 63) & 1; }

struct HashSets { std::unordered_set<uint64_t> vs, ps, tex; };
std::mutex g_par_mu;
HashSets g_gen[3];
int g_gen_idx = 0;
uint64_t g_par_total = 0, g_par_vs = 0, g_par_ps = 0, g_par_tex = 0, g_par_all = 0;

inline uint64_t fnv(uint64_t h, uint32_t v) { return (h ^ v) * 1099511628211ull; }

void game_hash(const uint8_t* base, uint32_t dev, uint64_t out[3]) {
    const uint32_t offs[3] = {kOffVs, kOffPs, kOffTex}, cnt[3] = {kVsDwords, kPsDwords, kTexDwords};
    for (int k = 0; k < 3; k++) {
        uint64_t h = 1469598103934665603ull;
        const uint32_t* p = reinterpret_cast<const uint32_t*>(base + dev + offs[k]);
        for (uint32_t i = 0; i < cnt[k]; i++) {
            if (k < 2 && touched(k * 256 + (i >> 2))) continue;
            h = fnv(h, __builtin_bswap32(p[i]));
        }
        out[k] = h;
    }
}

void cp_parity(const fa::gpu::RegisterFile& regs) {
    const uint32_t rb[3] = {kRegVs, kRegPs, kRegTex}, cnt[3] = {kVsDwords, kPsDwords, kTexDwords};
    uint64_t h[3];
    for (int k = 0; k < 3; k++) {
        uint64_t x = 1469598103934665603ull;
        for (uint32_t i = 0; i < cnt[k]; i++) {
            if (k < 2 && touched(k * 256 + (i >> 2))) continue;
            x = fnv(x, regs[rb[k] + i]);
        }
        h[k] = x;
    }
    std::lock_guard<std::mutex> lk(g_par_mu);
    bool ok[3];
    for (int k = 0; k < 3; k++) {
        ok[k] = false;
        for (const HashSets& g : g_gen) {
            const auto& set = k == 0 ? g.vs : k == 1 ? g.ps : g.tex;
            if (set.count(h[k])) { ok[k] = true; break; }
        }
    }
    g_par_total++;
    g_par_vs += ok[0];
    g_par_ps += ok[1];
    g_par_tex += ok[2];
    g_par_all += ok[0] && ok[1] && ok[2];
}

void on_cp_draw(const fa::gpu::DrawCall& dc, const fa::gpu::RegisterFile& regs) {
    cp_parity(regs);
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

PPC_FUNC(sub_823162F8) {
    if (g_on) {
        const uint32_t first = ctx.r5.u32, count = ctx.r6.u32;
        for (uint32_t v = first; v < first + count && v < 512; v++) g_touched[v >> 6].fetch_or(1ull << (v & 63), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_set_mu);
        g_sets[(uint64_t(first) << 32) | count]++;
    }
    __imp__sub_823162F8(ctx, base);
}

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
    const uint32_t dev = ctx.r3.u32;
    __imp__sub_82315500(ctx, base);
    if (g_on) {
        uint64_t h[3];
        game_hash(base, dev, h);
        std::lock_guard<std::mutex> lk(g_par_mu);
        g_gen[g_gen_idx].vs.insert(h[0]);
        g_gen[g_gen_idx].ps.insert(h[1]);
        g_gen[g_gen_idx].tex.insert(h[2]);
    }
}

namespace fa::rt {
// Called at every VdSwap.
void direct_tap_frame_end() {
    if (!g_on) return;
    if (!fa::gpu::g_draw_observer) fa::gpu::g_draw_observer = on_cp_draw;
    g_swaps_a.store(++g_swaps, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_par_mu);
        g_gen_idx = (g_gen_idx + 1) % 3;
        g_gen[g_gen_idx] = HashSets{};
    }
    if (g_swaps % 300) return;
    const uint64_t tc = g_tap_calls.load(), ti = g_tap_indices.load(), cd = g_cp_draws.load(), ci = g_cp_indices.load();
    std::fprintf(stderr, "[tap] swap %d: game DrawIndexed calls %llu indices %llu | CP indexed draws %llu indices %llu | index diff %lld\n",
                 g_swaps, (unsigned long long)tc, (unsigned long long)ti, (unsigned long long)cd, (unsigned long long)ci,
                 (long long)ti - (long long)ci);
    std::fprintf(stderr, "[tap]   game non-indexed (823152C0) calls %llu | CP auto-index draws %llu | repeats of an identical CP draw within a frame (cumulative, 1x 2x 3x 4x 5x+): %llu %llu %llu %llu %llu over %llu frames\n",
                 (unsigned long long)g_tap2_calls.load(), (unsigned long long)g_cp_auto_draws.load(), (unsigned long long)g_hist[1],
                 (unsigned long long)g_hist[2], (unsigned long long)g_hist[3], (unsigned long long)g_hist[4],
                 (unsigned long long)g_hist[5], (unsigned long long)g_frames_hist);
    {
        std::lock_guard<std::mutex> lk(g_par_mu);
        std::fprintf(stderr, "[tap]   state parity: CP draws %llu | vs consts match %llu | ps consts match %llu | fetch consts match %llu | all three %llu\n",
                     (unsigned long long)g_par_total, (unsigned long long)g_par_vs, (unsigned long long)g_par_ps,
                     (unsigned long long)g_par_tex, (unsigned long long)g_par_all);
    }
    {
        std::lock_guard<std::mutex> lk(g_set_mu);
        std::fprintf(stderr, "[tap]   direct SET_CONSTANT blocks (first vec4:count x calls):");
        int shown = 0;
        for (auto& [k, n] : g_sets) {
            if (shown++ >= 12) break;
            std::fprintf(stderr, " %u:%u x%llu", unsigned(k >> 32), unsigned(k & 0xFFFFFFFF), (unsigned long long)n);
        }
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "[tap]   repeats with identical window/target state %llu, with different state %llu | CP predicatedSkips %llu\n",
                 (unsigned long long)g_rep_same_sig, (unsigned long long)g_rep_diff_sig,
                 (unsigned long long)fa::rt::gpu_predicated_skips());
}
}  // namespace fa::rt
