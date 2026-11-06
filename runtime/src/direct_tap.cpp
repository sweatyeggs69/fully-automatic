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
#include <cstring>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "fa/gpu/command_processor.hpp"
#include "fa/gpu/xenos.hpp"
#include "fa/rt/import.hpp"

namespace fa::rt { uint64_t gpu_predicated_skips(); }

PPC_FUNC_IMPL(__imp__sub_82315500);  // generated as extern "C"
PPC_FUNC_IMPL(__imp__sub_823152C0);
PPC_FUNC_IMPL(__imp__sub_82147BF8);
PPC_FUNC_IMPL(__imp__sub_82147370);
PPC_FUNC_IMPL(__imp__sub_823162F8);

namespace {
// FA_TAP=1 turns on the observe-only taps and parity report. FA_DIRECT is a bit mask of hooked families
// that replace the PM4 source: bit 0 (1) = draw state constants (vertex and pixel float constants and fetch
// constants come from the game's device shadow instead of the register file the PM4 writes built).
// FA_DIRECT implies the snapshot hooks but not the report.
const unsigned g_direct = std::getenv("FA_DIRECT") ? unsigned(std::strtoul(std::getenv("FA_DIRECT"), nullptr, 0)) : 0u;
bool g_tap_report = std::getenv("FA_TAP") != nullptr;
bool g_on = g_tap_report || g_direct != 0;
int g_feed_printed = 0;
uint64_t g_feed_draws = 0, g_feed_missing = 0, g_feed_changed = 0, g_feed_dwords = 0;
std::atomic<uint64_t> g_tap_calls{0}, g_tap_indices{0}, g_cp_draws{0}, g_cp_indices{0};
std::atomic<uint64_t> g_tap_last_calls{0};
std::atomic<uint64_t> g_tap2_calls{0}, g_cp_auto_draws{0};
// Shader bind tap (82147BF8: r4 flag, r5 shader object, microcode pointer at +0x28, byte size at +0x258).
std::mutex g_sh_mu;
std::atomic<uint64_t> g_sh_link_calls{0};
int g_sh_samples = 0;
std::atomic<uint64_t> g_dsh_null[2]{}, g_dsh_bad[2]{};
std::unordered_set<uint32_t> g_dsh_objs[2], g_dsh_seen[2];
std::unordered_set<uint64_t> g_dsh_code[2];
int g_dsh_samples[2] = {0, 0};
int g_dsh_hs = 0;
std::unordered_set<uint32_t> g_dsh_hseen;
std::atomic<uint64_t> g_sh_calls[2]{};
std::unordered_set<uint32_t> g_sh_objs;
std::unordered_set<uint64_t> g_sh_code;
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
// Exact parity by packet address: the tap records a snapshot of the shadow under the physical address of the
// DRAW_INDX packet the game just wrote (the last 5 dwords before the device's write pointer); the CP looks it
// up by DrawCall::packetAddr, which is also correct for tile replays and indirect buffers.
// Render state register windows in the device shadow (from the dirty-state flush, notes/d3d-map.md): shadow offset,
// first register, dword count. Run 95: with these sizes every register matched the CP except 0x2208 (RB_MODECONTROL,
// also written straight into the stream by 82147D18 and resolves), 0x2011 and 0x2388 (rare); those keep their PM4 value.
struct StWin { uint32_t off, reg, n; };
constexpr StWin kStWins[] = {{0x2CC0, 0x2000, 18}, {0x2D0C, 0x2100, 21}, {0x2D60, 0x2180, 5}, {0x2D74, 0x2200, 12},
                             {0x2DA4, 0x2280, 21}, {0x2DF8, 0x2300, 38}, {0x2E90, 0x2380, 9}};
constexpr uint32_t kStBase = 0x2CC0, kStDwords = 160;  // covers all windows
// Stage A draw record: besides the shadow, the hook keeps the draw call arguments, the index buffer and a mirror of
// the per-object constant blocks the game writes straight into the stream (823162F8 + 821B8510), which are not in the
// shadow. The mirror holds the latest value of every ALU vec4 any block wrote (the GPU keeps registers until rewritten).
struct DrawArgs { uint32_t prim, start, count, ibFlags, ibBase; };
struct Snap { uint32_t vs[kVsDwords], ps[kPsDwords], tex[kTexDwords], st[kStDwords], mir[1024]; DrawArgs a; };  // mir: VS vec4s only (all blocks seen start at VS vec4 64)
struct PendingBlock { uint32_t first, count, payload; };
std::vector<PendingBlock> g_pending;   // under g_set_mu
uint32_t g_mirror[2048];               // under g_set_mu
uint64_t g_mirror_valid[8];            // vec4 bits ever written by a block (under g_set_mu)
DrawArgs g_cur_args{};                 // set by the draw hook for snapshot_draw (game draw thread)
uint64_t g_rec_cmp = 0, g_rec_bad_prim = 0, g_rec_bad_count = 0, g_rec_bad_addr = 0, g_mir_cmp = 0, g_mir_diff = 0;
int g_rec_printed = 0, g_mir_printed = 0;
uint64_t g_mir_vec[256];
uint64_t g_st_mis[0x400], g_st_cmp = 0;
int g_st_printed = 0;
std::mutex g_snap_mu;
uint64_t g_rec_k[25];  // recording-variant lookups by backwards offset in dwords (under g_snap_mu)
std::unordered_map<uint32_t, Snap> g_snaps[3];
int g_snap_gen = 0;
uint64_t g_ex_found = 0, g_ex_equal = 0, g_ex_missing = 0, g_ex_diff_vs = 0, g_ex_diff_ps = 0, g_ex_diff_tex = 0;
int g_ex_printed = 0;
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

void snapshot_draw(const uint8_t* base, uint32_t dev) {
    const uint32_t wptr = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + dev));
    // The predicated-tiling recording variant emits a 17 dword predicated sequence ending at wptr. Its exact
    // packet address for the CP is not known yet, so it is stored under the end pointer with bit 31 set and
    // looked up with a small backwards offset search that also fills a histogram.
    const bool rec = (base[dev + 10432] & 4) != 0;
    const uint32_t pkt = rec ? ((wptr & 0x1FFFFFFF) | 0x80000000u) : ((wptr - 16) & 0x1FFFFFFF);
    Snap s;
    const uint32_t* v = reinterpret_cast<const uint32_t*>(base + dev + kOffVs);
    const uint32_t* p = reinterpret_cast<const uint32_t*>(base + dev + kOffPs);
    const uint32_t* t = reinterpret_cast<const uint32_t*>(base + dev + kOffTex);
    for (uint32_t i = 0; i < kVsDwords; i++) s.vs[i] = __builtin_bswap32(v[i]);
    for (uint32_t i = 0; i < kPsDwords; i++) s.ps[i] = __builtin_bswap32(p[i]);
    for (uint32_t i = 0; i < kTexDwords; i++) s.tex[i] = __builtin_bswap32(t[i]);
    const uint32_t* q = reinterpret_cast<const uint32_t*>(base + dev + kStBase);
    for (uint32_t i = 0; i < kStDwords; i++) s.st[i] = __builtin_bswap32(q[i]);
    s.a = g_cur_args;
    {
        std::lock_guard<std::mutex> lk(g_set_mu);
        std::memcpy(s.mir, g_mirror, sizeof s.mir);
    }
    std::lock_guard<std::mutex> lk(g_snap_mu);
    g_snaps[g_snap_gen][pkt] = s;
}

// Finds the snapshot of the game draw that wrote the packet at pkt (caller holds g_snap_mu).
const Snap* lookup_snap_locked(uint32_t pkt) {
    const Snap* sn = nullptr;
    for (int k = 0; k < 3 && !sn; k++) {
        auto it = g_snaps[(g_snap_gen + 3 - k) % 3].find(pkt);
        if (it != g_snaps[(g_snap_gen + 3 - k) % 3].end()) sn = &it->second;
    }
    if (!sn) {
        for (uint32_t k = 1; k <= 24 && !sn; k++) {
            const uint32_t key = ((pkt + 4 * k) & 0x1FFFFFFF) | 0x80000000u;
            for (int g = 0; g < 3 && !sn; g++) {
                auto it = g_snaps[(g_snap_gen + 3 - g) % 3].find(key);
                if (it != g_snaps[(g_snap_gen + 3 - g) % 3].end()) { sn = &it->second; g_rec_k[k]++; }
            }
        }
    }
    return sn;
}

void exact_compare(uint32_t pkt, const fa::gpu::RegisterFile& regs, const fa::gpu::DrawCall* dc = nullptr) {
    std::lock_guard<std::mutex> lk(g_snap_mu);
    const Snap* sn = lookup_snap_locked(pkt);
    if (!sn) { g_ex_missing++; return; }
    g_ex_found++;
    int dv = 0, dp = 0, dt = 0;
    int printed = 0;
    auto cmp = [&](const char* name, const uint32_t* game, uint32_t rb, uint32_t n, bool alu, uint32_t vecBase, int& cnt) {
        for (uint32_t i = 0; i < n; i++) {
            if (alu && touched(vecBase + (i >> 2))) continue;
            if (game[i] != regs[rb + i]) {
                cnt++;
                if (g_ex_printed < 24 && printed < 4 && g_snap_gen >= 0) {
                    std::fprintf(stderr, "[tap] exact mismatch %s reg %04x: game %08x cp %08x (packet %08x)\n", name, rb + i, game[i], regs[rb + i], pkt);
                    printed++;
                }
            }
        }
    };
    cmp("vs", sn->vs, kRegVs, kVsDwords, true, 0, dv);
    cmp("ps", sn->ps, kRegPs, kPsDwords, true, 256, dp);
    cmp("fetch", sn->tex, kRegTex, kTexDwords, false, 0, dt);
    // Stage A record checks: per-object constant mirror on touched vec4s, and the draw arguments.
    {
        uint64_t valid[8];
        {
            std::lock_guard<std::mutex> lk2(g_set_mu);
            std::memcpy(valid, g_mirror_valid, sizeof valid);
        }
        for (uint32_t v = 0; v < 256; v++) {
            if (!(valid[v >> 6] >> (v & 63) & 1)) continue;
            g_mir_cmp++;
            bool same = true;
            for (uint32_t j = 0; j < 4; j++) same &= sn->mir[v * 4 + j] == regs[0x4000 + v * 4 + j];
            if (!same) {
                g_mir_diff++;
                g_mir_vec[v]++;
                if (g_mir_printed < 12 && g_mir_cmp > 20000000) {
                    g_mir_printed++;
                    std::fprintf(stderr, "[tap] mirror vec4 %u: game %08x %08x %08x %08x cp %08x %08x %08x %08x (packet %08x)\n", v,
                                 sn->mir[v * 4], sn->mir[v * 4 + 1], sn->mir[v * 4 + 2], sn->mir[v * 4 + 3],
                                 regs[0x4000 + v * 4], regs[0x4001 + v * 4], regs[0x4002 + v * 4], regs[0x4003 + v * 4], pkt);
                }
            }
        }
    }
    if (dc) {
        g_rec_cmp++;
        const DrawArgs& a = sn->a;
        const uint32_t isz = (a.ibFlags & 0x80000000u) ? 4 : 2;  // guess: flag bit 0 (MSB) of the index buffer object = 32-bit
        const uint32_t addr = (a.ibBase + a.start * isz) & 0x1FFFFFFF;
        const bool bp = (dc->primType & 0x3F) != (a.prim & 0x3F), bc = a.count <= 65535 && dc->indexCount != a.count,
                   ba = dc->indexGuestAddr != addr && dc->indexGuestAddr != addr + 0x1000 && dc->indexGuestAddr + 0x1000 != addr;
        g_rec_bad_prim += bp; g_rec_bad_count += bc; g_rec_bad_addr += ba;
        if ((bp || bc || ba) && g_rec_printed < 12) {
            g_rec_printed++;
            std::fprintf(stderr, "[tap] record args: game prim %u start %u count %u ib flags %08x base %08x -> addr %08x | cp prim %u count %u addr %08x idx32 %d (packet %08x)\n",
                         a.prim, a.start, a.count, a.ibFlags, a.ibBase, addr, dc->primType, dc->indexCount, dc->indexGuestAddr, int(dc->index32), pkt);
        }
    }
    for (const StWin& w : kStWins)
        for (uint32_t j = 0; j < w.n; j++) {
            const uint32_t g = sn->st[(w.off - kStBase) / 4 + j], c = regs[w.reg + j];
            g_st_cmp++;
            if (g != c) {
                g_st_mis[w.reg + j - 0x2000]++;
                if (g_st_printed < 24) {
                    g_st_printed++;
                    std::fprintf(stderr, "[tap] state reg %04x: game %08x cp %08x (packet %08x)\n", w.reg + j, g, c, pkt);
                }
            }
        }
    if (printed) g_ex_printed++;
    g_ex_diff_vs += dv != 0;
    g_ex_diff_ps += dp != 0;
    g_ex_diff_tex += dt != 0;
    if (!dv && !dp && !dt) g_ex_equal++;
}


// FA_DIRECT bit 0: the command processor takes the draw's constants from the game's device shadow. Per-object
// vec4s the game writes straight into the command stream (g_touched) are not in the shadow and keep their PM4 value.
void feed_cp_draw(const fa::gpu::DrawCall& dc, fa::gpu::RegisterFile& regs) {
    if (!(g_direct & 3) || !dc.indexed) return;
    std::lock_guard<std::mutex> lk(g_snap_mu);
    const Snap* sn = lookup_snap_locked(dc.packetAddr);
    g_feed_draws++;
    if (!sn) { g_feed_missing++; return; }
    if (g_direct & 1) {
        const uint32_t rb[2] = {kRegVs, kRegPs};
        const uint32_t* src[2] = {sn->vs, sn->ps};
        for (int k = 0; k < 2; k++) {
            for (uint32_t i = 0; i < 1024; i++) {
                if (touched(k * 256 + (i >> 2))) continue;
                uint32_t& dst = regs[rb[k] + i];
                if (dst != src[k][i]) {
                    if (g_feed_printed < 16) {
                        g_feed_printed++;
                        std::fprintf(stderr, "[direct] differs: %s const reg %04x pm4 %08x game %08x (packet %08x, vec4 %u)\n", k ? "ps" : "vs",
                                     rb[k] + i, dst, src[k][i], dc.packetAddr, k * 256 + (i >> 2));
                    }
                    dst = src[k][i];
                    regs.aluDirty |= 1u << ((k * 1024 + i) >> 6);
                    g_feed_changed++;
                }
            }
        }
        for (uint32_t i = 0; i < kTexDwords; i++) {
            uint32_t& dst = regs[kRegTex + i];
            if (dst != sn->tex[i]) {
                if (g_feed_printed < 16) {
                    g_feed_printed++;
                    std::fprintf(stderr, "[direct] differs: fetch reg %04x pm4 %08x game %08x (packet %08x)\n", kRegTex + i, dst, sn->tex[i], dc.packetAddr);
                }
                dst = sn->tex[i];
                g_feed_changed++;
            }
        }
    }
    g_feed_dwords += 2048 + kTexDwords;
    if (g_direct & 2) {
        // FA_DIRECT bit 1: render-state register windows from the shadow, except the ones the game also writes directly.
        for (const StWin& w : kStWins)
            for (uint32_t j = 0; j < w.n; j++) {
                const uint32_t reg = w.reg + j;
                if (reg == 0x2208 || reg == 0x2011 || reg == 0x2388) continue;
                uint32_t& dst = regs[reg];
                const uint32_t v = sn->st[(w.off - kStBase) / 4 + j];
                if (dst != v) {
                    if (g_feed_printed < 16) {
                        g_feed_printed++;
                        std::fprintf(stderr, "[direct] differs: state reg %04x pm4 %08x game %08x (packet %08x)\n", reg, dst, v, dc.packetAddr);
                    }
                    dst = v;
                    g_feed_changed++;
                }
            }
    }
}

void on_cp_draw(const fa::gpu::DrawCall& dc, const fa::gpu::RegisterFile& regs) {
    cp_parity(regs);
    exact_compare(dc.packetAddr, regs, &dc);
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
    const uint32_t first_arg = ctx.r5.u32, count_arg = ctx.r6.u32;
    if (g_on) {
        const uint32_t first = first_arg, count = count_arg;
        for (uint32_t v = first; v < first + count && v < 512; v++) g_touched[v >> 6].fetch_or(1ull << (v & 63), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_set_mu);
        g_sets[(uint64_t(first) << 32) | count]++;
    }
    __imp__sub_823162F8(ctx, base);
    if (g_on) {
        // Returns the payload pointer the caller then fills (821B8510); read it at the next draw.
        std::lock_guard<std::mutex> lk(g_set_mu);
        g_pending.push_back({first_arg, count_arg, ctx.r3.u32});
        static int blk_samples = 0;
        if (blk_samples < 8) {
            blk_samples++;
            const uint32_t r3 = ctx.r3.u32;
            auto rd = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
            std::fprintf(stderr, "[tap] const block: first %u count %u returned %08x | [-8] %08x [-4] %08x [0] %08x [+4] %08x\n",
                         first_arg, count_arg, r3, rd(r3 - 8), rd(r3 - 4), rd(r3), rd(r3 + 4));
        }
    }
}

// Shader pair link (82147370): r4 = shader object A (device+0x3294), r6 = shader object B (device+0x2C90).
// Called from 82147E08 whenever the bound shaders change; the common path never enters 82147BF8.
static void tap_shader_obj(const uint8_t* base, uint32_t obj, int which) {
    if (!obj) return;
    const uint32_t code = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x28));
    const uint32_t size = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x258));
    uint64_t h = 1469598103934665603ull ^ size;
    if (size && size < 65536)
        for (uint32_t i = 0; i < size; i++) h = (h ^ base[code + i]) * 1099511628211ull;
    std::lock_guard<std::mutex> lk(g_sh_mu);
    g_sh_objs.insert(obj);
    g_sh_code.insert(h);
    if (g_sh_samples < 6) {
        g_sh_samples++;
        std::fprintf(stderr, "[tap] shader link sample: %c obj %08x code %08x size %u hash %016llx\n", which ? 'B' : 'A', obj, code, size, (unsigned long long)h);
    }
}

// Draw-time shader identity: objects at device+0x3294 and device+0x3290 (see notes/d3d-map.md).
static void tap_draw_shaders(const uint8_t* base, uint32_t dev) {
    static const uint32_t kOffs[2] = {0x3294, 0x3290};
    for (int k = 0; k < 2; k++) {
        const uint32_t obj = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + dev + kOffs[k]));
        if (!obj) { g_dsh_null[k].fetch_add(1, std::memory_order_relaxed); continue; }
        if (k == 1) {
            // Slot 1 is the pixel shader object: microcode pointer at +0x0C, byte size at +0x3C (82148100).
            {
                const uint32_t pcode = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x0C));
                const uint32_t psize = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x3C));
                const bool pok = psize && psize < 65536 && (psize & 3) == 0 && pcode >= 0x10000;
                uint64_t ph = 1469598103934665603ull ^ psize;
                if (pok) for (uint32_t i = 0; i < psize; i++) ph = (ph ^ base[pcode + i]) * 1099511628211ull;
                else g_dsh_bad[1].fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(g_sh_mu);
                g_dsh_code[1].insert(ph);
                if (g_dsh_hs < 4 && g_dsh_hseen.insert(obj).second) {
                    g_dsh_hs++;
                    std::fprintf(stderr, "[tap] draw slot1 ps microcode: obj %08x code %08x size %u hash %016llx\n", obj, pcode, psize, (unsigned long long)ph);
                }
            }
            std::lock_guard<std::mutex> lk(g_sh_mu);
            g_dsh_objs[1].insert(obj);
            if (g_dsh_samples[1] < 4 && g_dsh_seen[1].insert(obj).second) {
                g_dsh_samples[1]++;
                std::fprintf(stderr, "[tap] draw slot1 raw obj %08x:", obj);
                for (uint32_t i = 0; i < 24; i++) std::fprintf(stderr, " %08x", __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + i * 4)));
                std::fprintf(stderr, "\n");
                // 82147E08 derives r23 = obj + [obj+0x48] + 0x34 for this slot; also dump the guest pointer at +0x0C.
                const uint32_t off48 = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x48));
                if (off48 < 0x10000) {
                    const uint32_t r23 = obj + off48 + 0x34;
                    std::fprintf(stderr, "[tap] draw slot1 r23 %08x:", r23);
                    for (uint32_t i = 0; i < 24; i++) std::fprintf(stderr, " %08x", __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + r23 + i * 4)));
                    std::fprintf(stderr, "\n");
                }
                const uint32_t p3 = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x0C));
                if (p3 >= 0x10000) {
                    std::fprintf(stderr, "[tap] draw slot1 ptr+0x0C %08x:", p3);
                    for (uint32_t i = 0; i < 24; i++) std::fprintf(stderr, " %08x", __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + p3 + i * 4)));
                    std::fprintf(stderr, "\n");
                }
            }
            continue;
        }
        const uint32_t code = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x28));
        const uint32_t size = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + 0x258));
        const bool ok = size && size < 65536 && (size & 3) == 0 && code >= 0x10000;
        uint64_t h = 1469598103934665603ull ^ size;
        if (ok) for (uint32_t i = 0; i < size; i++) h = (h ^ base[code + i]) * 1099511628211ull;
        else g_dsh_bad[k].fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_sh_mu);
        g_dsh_objs[k].insert(obj);
        g_dsh_code[k].insert(h);
        if (g_dsh_samples[k] < 4 && g_dsh_seen[k].insert(obj).second) {
            g_dsh_samples[k]++;
            std::fprintf(stderr, "[tap] draw shader sample: slot %d obj %08x code %08x size %u hash %016llx\n", k, obj, code, size, (unsigned long long)h);
        }
    }
}

PPC_FUNC(sub_82147BF8) {
    if (g_on) g_sh_calls[ctx.r4.u32 != 0].fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82147BF8(ctx, base);
}

PPC_FUNC(sub_82147370) {
    if (g_on) {
        g_sh_link_calls.fetch_add(1, std::memory_order_relaxed);
        tap_shader_obj(base, ctx.r4.u32, 0);
        tap_shader_obj(base, ctx.r6.u32, 1);
    }
    __imp__sub_82147370(ctx, base);
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
    const DrawArgs args0{ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, 0, 0};
    __imp__sub_82315500(ctx, base);
    if (g_on) {
        {
            std::lock_guard<std::mutex> lk(g_set_mu);
            for (const PendingBlock& b : g_pending) {
                for (uint32_t i = 0; i < b.count * 4 && b.first * 4 + i < 2048; i++)
                    g_mirror[b.first * 4 + i] = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + b.payload + i * 4));
                for (uint32_t v = b.first; v < b.first + b.count && v < 512; v++) g_mirror_valid[v >> 6] |= 1ull << (v & 63);
            }
            g_pending.clear();
        }
        g_cur_args = args0;
        const uint32_t ib = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + dev + 0x30F4));
        if (ib) {
            g_cur_args.ibFlags = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ib));
            g_cur_args.ibBase = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ib + 0xC));
        }
        snapshot_draw(base, dev);
        if (g_tap_report) {
            tap_draw_shaders(base, dev);
            uint64_t h[3];
            game_hash(base, dev, h);
            std::lock_guard<std::mutex> lk(g_par_mu);
            g_gen[g_gen_idx].vs.insert(h[0]);
            g_gen[g_gen_idx].ps.insert(h[1]);
            g_gen[g_gen_idx].tex.insert(h[2]);
        }
    }
}

namespace fa::rt {
// Called at every VdSwap.
void direct_tap_frame_end() {
    if (!g_on) return;
    if (g_tap_report && !fa::gpu::g_draw_observer) fa::gpu::g_draw_observer = on_cp_draw;
    if ((g_direct & 3) && !fa::gpu::g_draw_feeder) fa::gpu::g_draw_feeder = feed_cp_draw;
    g_swaps_a.store(++g_swaps, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_par_mu);
        g_gen_idx = (g_gen_idx + 1) % 3;
        g_gen[g_gen_idx] = HashSets{};
    }
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap_gen = (g_snap_gen + 1) % 3;
        g_snaps[g_snap_gen].clear();
    }
    if (g_direct && g_swaps % 300 == 0) {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        std::fprintf(stderr, "[direct] swap %d: FA_DIRECT=%u fed draws %llu, no snapshot %llu, dwords replaced where the PM4 value differed %llu\n",
                     g_swaps, g_direct, (unsigned long long)g_feed_draws, (unsigned long long)g_feed_missing, (unsigned long long)g_feed_changed);
    }
    if (!g_tap_report || g_swaps % 300) return;
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
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        std::fprintf(stderr, "[tap]   exact parity by packet address: CP draws with a game snapshot %llu, without %llu | identical state %llu | differ in vs %llu ps %llu fetch %llu\n",
                     (unsigned long long)g_ex_found, (unsigned long long)g_ex_missing, (unsigned long long)g_ex_equal,
                     (unsigned long long)g_ex_diff_vs, (unsigned long long)g_ex_diff_ps, (unsigned long long)g_ex_diff_tex);
    }
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        std::fprintf(stderr, "[tap]   stage A record: draws %llu, prim differs %llu, count differs %llu, index address differs %llu | per-object constant mirror: vec4 compared %llu, differ %llu\n",
                     (unsigned long long)g_rec_cmp, (unsigned long long)g_rec_bad_prim, (unsigned long long)g_rec_bad_count, (unsigned long long)g_rec_bad_addr,
                     (unsigned long long)g_mir_cmp, (unsigned long long)g_mir_diff);
        std::fprintf(stderr, "[tap]   mirror differences by vec4:");
        for (uint32_t v = 0; v < 256; v++) if (g_mir_vec[v]) std::fprintf(stderr, " %u:%llu", v, (unsigned long long)g_mir_vec[v]);
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[tap]   render-state windows: compared %llu dwords; registers that differ (reg:count of %llu draws):", (unsigned long long)g_st_cmp, (unsigned long long)g_ex_found);
        for (uint32_t i = 0; i < 0x400; i++) if (g_st_mis[i]) std::fprintf(stderr, " %04x:%llu", 0x2000 + i, (unsigned long long)g_st_mis[i]);
        std::fprintf(stderr, "\n");
    }
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        std::fprintf(stderr, "[tap]   recording-variant match offsets (dwords back from end, count):");
        for (uint32_t k = 1; k <= 24; k++) if (g_rec_k[k]) std::fprintf(stderr, " %u:%llu", k, (unsigned long long)g_rec_k[k]);
        std::fprintf(stderr, "\n");
    }
    {
        std::lock_guard<std::mutex> lk(g_sh_mu);
        std::fprintf(stderr, "[tap]   shader links %llu, 82147BF8 binds flag0 %llu flag1 %llu | distinct shader objects %zu, distinct microcode hashes %zu\n",
                     (unsigned long long)g_sh_link_calls.load(), (unsigned long long)g_sh_calls[0].load(), (unsigned long long)g_sh_calls[1].load(), g_sh_objs.size(), g_sh_code.size());
    }
    {
        std::lock_guard<std::mutex> lk(g_sh_mu);
        for (int k = 0; k < 2; k++)
            std::fprintf(stderr, "[tap]   draw shader slot %d (device+0x%x): null %llu, implausible size %llu, distinct objects %zu, distinct microcode hashes %zu\n",
                         k, k ? 0x3290 : 0x3294, (unsigned long long)g_dsh_null[k].load(), (unsigned long long)g_dsh_bad[k].load(),
                         g_dsh_objs[k].size(), g_dsh_code[k].size());
    }
    std::fprintf(stderr, "[tap]   repeats with identical window/target state %llu, with different state %llu | CP predicatedSkips %llu\n",
                 (unsigned long long)g_rep_same_sig, (unsigned long long)g_rep_diff_sig,
                 (unsigned long long)fa::rt::gpu_predicated_skips());
}
}  // namespace fa::rt
