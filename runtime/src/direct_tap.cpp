// Phase 1 of the direct graphics hooks (notes/direct-gpu-hooks-design.md): observe-only taps on
// the game's own D3D functions. Part of the fullauto executable only (it needs the recompiled
// code). Every tap calls the original function unchanged; FA_TAP=1 turns the counters on.
//
// Taps so far:
//   sub_82315500  DrawIndexedPrimitive(device, primType, ?, startIndex, indexCount): writes the
//                 DRAW_INDX packet (notes/d3d-map.md). Counted against the command processor's
//                 indexed draws; the totals must match (draws over 65535 indices are split, so
//                 compare index totals, not call counts).
#include <algorithm>
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
#include "fa/gpu/pm4.hpp"
#include "fa/gpu/xenos.hpp"
#include "fa/rt/import.hpp"
#include "fa/rt/memory.hpp"

namespace fa::rt { uint64_t gpu_predicated_skips(); }

PPC_FUNC_IMPL(__imp__sub_82315500);
PPC_FUNC_IMPL(__imp__sub_82148508);  // generated as extern "C"
PPC_FUNC_IMPL(__imp__sub_823152C0);
PPC_FUNC_IMPL(__imp__sub_82147BF8);
PPC_FUNC_IMPL(__imp__sub_82147370);
PPC_FUNC_IMPL(__imp__sub_823162F8);
PPC_FUNC_IMPL(__imp__sub_82147160);
PPC_FUNC_IMPL(__imp__sub_82142490);

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
struct Snap { uint32_t vs[kVsDwords], ps[kPsDwords], tex[kTexDwords], st[kStDwords], mir[1024]; uint64_t own[4]; uint32_t bl[40]; uint32_t vsObj, vsUcode, vsBytes, psObj, psUcode, psBytes, declObj; DrawArgs a; };  // bl: bool+loop constants  // mir: VS vec4s only (all blocks seen start at VS vec4 64)
struct PendingBlock { uint32_t first, count, payload; };
std::vector<PendingBlock> g_pending;   // under g_set_mu
uint32_t g_mirror[2048];               // under g_set_mu
uint64_t g_mirror_valid[8];            // vec4 bits ever written by a block (under g_set_mu)
DrawArgs g_cur_args{};
uint64_t g_cur_own[4];                 // vec4s written by blocks issued since the previous draw (draw thread)
uint64_t g_bl_cmp = 0, g_bl_diff[40];
int g_bl_printed = 0, g_win_printed = 0;
uint64_t g_own_cmp = 0, g_own_diff = 0, g_rule_cmp = 0, g_rule_diff = 0, g_flushes = 0;                 // set by the draw hook for snapshot_draw (game draw thread)
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
        auto rd = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
        s.vsObj = rd(dev + 0x3294);
        s.vsUcode = s.vsObj ? rd(s.vsObj + 0x28) : 0;
        s.vsBytes = s.vsObj ? rd(s.vsObj + 0x258) : 0;
        s.psObj = rd(dev + 0x3290);
        s.psUcode = s.psObj ? rd(s.psObj + 0x0C) : 0;
        s.psBytes = s.psObj ? rd(s.psObj + 0x3C) : 0;
        s.declObj = rd(dev + 0x2C90);
    }
    {
        const uint32_t* b = reinterpret_cast<const uint32_t*>(base + dev + 0x2780);
        for (uint32_t i = 0; i < 40; i++) s.bl[i] = __builtin_bswap32(b[i]);
    }
    std::memcpy(s.own, g_cur_own, sizeof s.own);
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
            const bool own = sn->own[v >> 6] >> (v & 63) & 1;
            if (own) { g_own_cmp++; g_own_diff += !same; }
            else {
                // Not written by this draw's blocks: the shadow value, for the record of comparison.
                bool sh = true;
                for (uint32_t j = 0; j < 4; j++) sh &= sn->vs[v * 4 + j] == regs[0x4000 + v * 4 + j];
                g_rule_cmp++;
                g_rule_diff += !sh;
            }
            if (!same) {
                g_mir_diff++;
                g_mir_vec[v]++;
                if (g_mir_printed < 12 && g_mir_cmp > 20000000) {
                    g_mir_printed++;
                    std::fprintf(stderr, "[tap] mirror vec4 %u%s: game %08x %08x %08x %08x cp %08x %08x %08x %08x (packet %08x)\n", v, own ? " (own block)" : "",
                                 sn->mir[v * 4], sn->mir[v * 4 + 1], sn->mir[v * 4 + 2], sn->mir[v * 4 + 3],
                                 regs[0x4000 + v * 4], regs[0x4001 + v * 4], regs[0x4002 + v * 4], regs[0x4003 + v * 4], pkt);
                }
            }
        }
    }
    for (uint32_t i = 0; i < 40; i++) {
        g_bl_cmp++;
        if (sn->bl[i] != regs[0x4900 + i]) {
            g_bl_diff[i]++;
            if (g_bl_printed < 8) {
                g_bl_printed++;
                std::fprintf(stderr, "[tap] bool/loop reg %04x: game %08x cp %08x (packet %08x)\n", 0x4900 + i, sn->bl[i], regs[0x4900 + i], pkt);
            }
        }
    }
    if (g_win_printed < 6) {
        g_win_printed++;
        std::fprintf(stderr, "[tap] cp window regs 2080..2082: %08x %08x %08x (packet %08x)\n", regs[0x2080], regs[0x2081], regs[0x2082], pkt);
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


// FA_DIRECT bit 2 (stage A): the CP hands the game's draw record to Backend::DrawDirect. The register image is the
// CP's register file with everything the record holds taken from the game (constants with the per-object block rule,
// fetch, bool/loop, render-state windows); the payload carries the dwords that changed since the last record sent.
std::vector<uint32_t> g_dd_sent;   // CP thread only
std::vector<uint8_t> g_dd_payload; // CP thread only
uint64_t g_dd_draws = 0, g_dd_missing = 0, g_dd_refused = 0, g_dd_dwords = 0;
uint32_t g_dd_seq = 0;

bool direct_draw(const fa::gpu::DrawCall& dc, const fa::gpu::RegisterFile& regs, fa::gpu::Backend& backend) {
    if (!(g_direct & 4) || !dc.indexed || !dc.inlineIndices.empty()) return false;
    // Only the register ranges the renderer reads are diffed: 0x2000..0x23FF (render state, scissor, window offset,
    // copy registers) and 0x4000..0x4927 (float, fetch, bool and loop constants).
    static std::vector<uint32_t> img(fa::gpu::kRegisterCount);
    std::memcpy(img.data() + 0x2000, regs.r.data() + 0x2000, 0x400 * 4);
    std::memcpy(img.data() + 0x4000, regs.r.data() + 0x4000, 0x928 * 4);
    fa::gpu::DirectDraw rec;
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        const Snap* sn = lookup_snap_locked(dc.packetAddr);
        if (!sn) { g_dd_missing++; return false; }
        for (uint32_t v = 0; v < 256; v++) {
            const bool tch = touched(v);
            for (uint32_t j = 0; j < 4; j++) img[kRegVs + v * 4 + j] = tch ? sn->mir[v * 4 + j] : sn->vs[v * 4 + j];
        }
        for (uint32_t i = 0; i < kPsDwords; i++) if (!touched(256 + (i >> 2))) img[kRegPs + i] = sn->ps[i];
        for (uint32_t i = 0; i < kTexDwords; i++) img[kRegTex + i] = sn->tex[i];
        for (uint32_t i = 0; i < 40; i++) img[0x4900 + i] = sn->bl[i];
        for (const StWin& w : kStWins)
            for (uint32_t j = 0; j < w.n; j++) {
                const uint32_t reg = w.reg + j;
                if (reg == 0x2208 || reg == 0x2011 || reg == 0x2388) continue;
                img[reg] = sn->st[(w.off - kStBase) / 4 + j];
            }
        rec.vsObj = sn->vsObj; rec.vsUcode = sn->vsUcode; rec.vsBytes = sn->vsBytes;
        rec.psObj = sn->psObj; rec.psUcode = sn->psUcode; rec.psBytes = sn->psBytes;
        rec.declObj = sn->declObj;
    }
    rec.seq = g_dd_seq++;
    rec.packetAddr = dc.packetAddr;
    rec.primType = dc.primType;
    rec.indexCount = dc.indexCount;
    rec.indexAddr = dc.indexGuestAddr;
    rec.index32 = dc.index32;
    rec.indexEndian = uint8_t(dc.indexEndian);
    // Payload: runs of dwords that differ from what the backend last received.
    const bool full = g_dd_sent.size() != img.size();
    g_dd_payload.clear();
    static const uint32_t kRanges[2][2] = {{0x2000, 0x2400}, {0x4000, 0x4928}};
    for (const auto& rg : kRanges)
    for (uint32_t i = rg[0]; i < rg[1];) {
        if (!full && img[i] == g_dd_sent[i]) { i++; continue; }
        uint32_t j = i;
        while (j < rg[1] && (full || img[j] != g_dd_sent[j])) j++;
        fa::gpu::DirectRegRun run{i, j - i};
        const size_t at = g_dd_payload.size();
        g_dd_payload.resize(at + sizeof run + run.count * 4);
        std::memcpy(g_dd_payload.data() + at, &run, sizeof run);
        std::memcpy(g_dd_payload.data() + at + sizeof run, img.data() + i, run.count * 4);
        g_dd_dwords += run.count;
        i = j;
    }
    rec.payloadBytes = uint32_t(g_dd_payload.size());
    rec.tileRows = regs.tileRows;
    if (!backend.DrawDirect(rec, g_dd_payload.data())) {
        g_dd_refused++;
        g_dd_sent.clear();  // the backend did not take the state: resend everything next time
        return false;
    }
    if (g_dd_sent.size() != img.size()) g_dd_sent.assign(img.size(), 0);
    std::memcpy(g_dd_sent.data() + 0x2000, img.data() + 0x2000, 0x400 * 4);
    std::memcpy(g_dd_sent.data() + 0x4000, img.data() + 0x4000, 0x928 * 4);
    g_dd_draws++;
    return true;
}

// ---- FA_DIRECT bit 3 (stage B): draw record queue ----------------------------------------------------
// Stage A copied a full 13.5 KB snapshot per draw into a map on the game thread and rebuilt and diffed the whole
// register image per draw on the CP thread (run 103: race 15 to 18 fps against 23 to 25). Here the game thread
// diffs the game's state against its previous record and pushes only the changed dwords into a single-producer
// single-consumer ring, in draw order. The CP thread applies records in order to its image of the game state
// until it reaches the record whose packet matches the draw, so its image is exactly the game state at that draw,
// and sends the backend only the 16-dword blocks those records touched.
//
// Game state image G: VS constants (mirror rule), PS constants, fetch, bool/loop and the render-state windows
// without the three registers the game writes straight into the stream (those come from the CP).
struct GSeg { uint32_t reg, n; };
constexpr GSeg kGSegs[] = {{0x4000, 1024}, {0x4400, 1024}, {0x4800, 192}, {0x4900, 40},
                           {0x2000, 17}, {0x2100, 21}, {0x2180, 5}, {0x2200, 8}, {0x2209, 3},
                           {0x2280, 21}, {0x2300, 38}, {0x2380, 8}};
constexpr uint32_t kGDwords = 1024 + 1024 + 192 + 40 + 17 + 21 + 5 + 8 + 3 + 21 + 38 + 8;
// Registers the record does not hold but the renderer reads: window offset and scissor (not in the shadow) and the
// directly written 0x2011, 0x2208 (RB_MODECONTROL), 0x2388. The CP image takes them from the CP's register file.
constexpr uint32_t kCpRegs[] = {0x2080, 0x2081, 0x2082, 0x2011, 0x2208, 0x2388};

constexpr uint32_t kQWords = 1u << 22, kQMask = kQWords - 1;  // 16 MB ring
std::vector<uint32_t> g_q(kQWords);
std::atomic<uint64_t> g_q_w{0}, g_q_r{0};
// Record: [0] words, [1] flags (bit 0: full state), [2] packet key, [3] prim, [4] index count, [5] index address,
// [6..12] vsObj vsUcode vsBytes psObj psUcode psBytes declObj, then runs [first reg][count][values...].
constexpr uint32_t kRecHdr = 13;
uint32_t g_prev_g[kGDwords];  // game thread
bool g_prev_valid = false;    // game thread
uint64_t g_q_pushed = 0, g_q_dropped = 0, g_q_words_pushed = 0, g_q_nonidx = 0;  // game thread
std::vector<uint32_t> g_rec_buf;                                // game thread

// Packet address of the draw the game just wrote, from the device's write pointer (which points at the last dword
// written). Indexed draws (82315500): the 5-dword DRAW_INDX starts 16 bytes back; in the recording variant
// (predicated tiling) 48 bytes back (run 104: every lookup matched there). Non-indexed draws (823152C0): the 3-dword
// auto-index DRAW_INDX starts 8 bytes back; in the recording variant it is the 4th of 14 dwords, 40 bytes back.
uint32_t draw_key(const uint8_t* base, uint32_t dev, bool indexed = true) {
    const uint32_t wptr = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + dev));
    const bool rec = (base[dev + 10432] & 4) != 0;
    const uint32_t back = indexed ? (rec ? 48 : 16) : (rec ? 40 : 8);
    return (wptr - back) & 0x1FFFFFFF;
}

void push_record(const uint8_t* base, uint32_t dev, const DrawArgs& a, bool indexed = true) {
    uint32_t g[kGDwords];
    {
        auto rd_block = [&](uint32_t* dst, uint32_t off, uint32_t n) {
            const uint32_t* src = reinterpret_cast<const uint32_t*>(base + dev + off);
            for (uint32_t i = 0; i < n; i++) dst[i] = __builtin_bswap32(src[i]);
        };
        uint32_t* o = g;
        rd_block(o, kOffVs, 1024);
        {
            std::lock_guard<std::mutex> lk(g_set_mu);
            for (uint32_t v = 0; v < 256; v++)
                if (touched(v)) std::memcpy(o + v * 4, g_mirror + v * 4, 16);
        }
        o += 1024;
        rd_block(o, kOffPs, 1024); o += 1024;
        rd_block(o, kOffTex, 192); o += 192;
        rd_block(o, 0x2780, 40); o += 40;
        rd_block(o, 0x2CC0, 17); o += 17;
        rd_block(o, 0x2D0C, 21); o += 21;
        rd_block(o, 0x2D60, 5); o += 5;
        rd_block(o, 0x2D74, 8); o += 8;
        rd_block(o, 0x2D74 + 9 * 4, 3); o += 3;
        rd_block(o, 0x2DA4, 21); o += 21;
        rd_block(o, 0x2DF8, 38); o += 38;
        rd_block(o, 0x2E90, 8); o += 8;
    }
    auto rd = [&](uint32_t addr) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr)); };
    const bool full = !g_prev_valid;
    std::vector<uint32_t>& r = g_rec_buf;
    r.clear();
    r.resize(kRecHdr);
    r[1] = full ? 1 : 0;
    r[2] = draw_key(base, dev, indexed);
    r[3] = a.prim;
    r[4] = a.count;
    r[5] = indexed && a.ibBase ? a.ibBase + a.start * 2 : 0;
    r[6] = rd(dev + 0x3294);
    r[7] = r[6] ? rd(r[6] + 0x28) : 0;
    r[8] = r[6] ? rd(r[6] + 0x258) : 0;
    r[9] = rd(dev + 0x3290);
    r[10] = r[9] ? rd(r[9] + 0x0C) : 0;
    r[11] = r[9] ? rd(r[9] + 0x3C) : 0;
    r[12] = rd(dev + 0x2C90);
    uint32_t gi = 0;
    for (const GSeg& sg : kGSegs) {
        for (uint32_t i = 0; i < sg.n;) {
            // Skip equal 4-dword groups quickly; most of the image is unchanged between draws.
            if (!full && g[gi + i] == g_prev_g[gi + i]) {
                if ((i & 3) == 0 && i + 4 <= sg.n && std::memcmp(g + gi + i, g_prev_g + gi + i, 16) == 0) { i += 4; continue; }
                i++;
                continue;
            }
            uint32_t j = i;
            while (j < sg.n && (full || g[gi + j] != g_prev_g[gi + j])) j++;
            r.push_back(sg.reg + i);
            r.push_back(j - i);
            r.insert(r.end(), g + gi + i, g + gi + j);
            i = j;
        }
        gi += sg.n;
    }
    r[0] = uint32_t(r.size());
    const uint64_t w = g_q_w.load(std::memory_order_relaxed), rr = g_q_r.load(std::memory_order_acquire);
    if (w + r.size() - rr > kQWords) {
        // The CP is too far behind: drop, and send the full state with the next record that fits.
        g_q_dropped++;
        g_prev_valid = false;
        return;
    }
    for (size_t i = 0; i < r.size(); i++) g_q[(w + i) & kQMask] = r[i];
    g_q_w.store(w + r.size(), std::memory_order_release);
    std::memcpy(g_prev_g, g, sizeof g);
    g_prev_valid = true;
    g_q_pushed++;
    g_q_words_pushed += r.size();
}

// Resolve records (stage B): the game's D3D resolve (82142490) writes its copy setup straight into the stream (copy
// registers, RB_MODECONTROL, the rectangle's vertex fetch constant, a copy-mode draw) without the device shadow. Until
// its arguments are decoded, the hook reads back the register writes the function itself just emitted (a narrow
// read of the game's own output, not the CP) and queues them in order with the draw records. Key: the copy-mode draw
// packet inside that output, which is what the CP resolves on.
uint64_t g_rs_pushed = 0, g_rs_wrapped = 0, g_rs_nodraw = 0;  // game thread
std::vector<std::pair<uint32_t, uint32_t>> g_rs_writes;       // game thread

void push_resolve_record(const uint8_t* base, uint32_t before, uint32_t after) {
    namespace pm4 = fa::gpu::pm4;
    if (after <= before || after - before > 0x10000) { g_rs_wrapped++; g_prev_valid = false; return; }
    auto rd = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
    auto keep = [](uint32_t reg) { return (reg >= 0x2000 && reg < 0x2400) || (reg >= 0x4000 && reg < 0x4928); };
    g_rs_writes.clear();
    uint32_t key = 0xFFFFFFFFu, prim = 0, count = 0;
    for (uint32_t a = before + 4; a <= after;) {
        const uint32_t h = rd(a);
        switch (pm4::TypeOf(h)) {
            case pm4::kType0: {
                const uint32_t n = pm4::T0Count(h), reg = pm4::T0Base(h);
                for (uint32_t i = 0; i < n && a + 4 + i * 4 <= after; i++) {
                    const uint32_t rg = pm4::T0OneReg(h) ? reg : reg + i;
                    if (keep(rg)) g_rs_writes.push_back({rg, rd(a + 4 + i * 4)});
                }
                a += 4 + n * 4;
                break;
            }
            case pm4::kType1:
                if (keep(pm4::T1Reg0(h))) g_rs_writes.push_back({pm4::T1Reg0(h), rd(a + 4)});
                if (keep(pm4::T1Reg1(h))) g_rs_writes.push_back({pm4::T1Reg1(h), rd(a + 8)});
                a += 12;
                break;
            case pm4::kType2: a += 4; break;
            case pm4::kType3: {
                const uint32_t n = pm4::T3Count(h), op = pm4::T3Opcode(h);
                if (a + n * 4 > after) { a = after + 4; break; }
                if (op == pm4::kSetConstant && n >= 2) {
                    const uint32_t d0 = rd(a + 4), index = d0 & 0x7FF, type = (d0 >> 16) & 0xFF;
                    const uint32_t b = type == 0 ? 0x4000 : type == 1 ? 0x4800 : type == 2 ? 0x4900 : type == 3 ? 0x4908 : type == 4 ? 0x2000 : 0;
                    if (b) for (uint32_t i = 1; i < n; i++) if (keep(b + index + i - 1)) g_rs_writes.push_back({b + index + i - 1, rd(a + 4 + i * 4)});
                } else if ((op == pm4::kDrawIndx || op == pm4::kDrawIndx2) && key == 0xFFFFFFFFu) {
                    const uint32_t init = rd(a + 4 + (op == pm4::kDrawIndx ? 4 : 0));
                    key = a & 0x1FFFFFFF;
                    prim = init & 0x3F;
                    count = init >> 16;
                    // Stop at the copy draw: after it the game clears RB_COPY_CONTROL and RB_COPY_DEST_INFO (run 113:
                    // all 5085 resolve images had 0 there), and those later writes must not reach the copy.
                    a = after + 4;
                    break;
                }
                a += 4 + n * 4;
                break;
            }
        }
    }
    if (key == 0xFFFFFFFFu) { g_rs_nodraw++; return; }
    std::vector<uint32_t>& r = g_rec_buf;
    r.assign(kRecHdr, 0);
    r[1] = 2;  // resolve
    r[2] = key;
    r[3] = prim;
    r[4] = count;
    // Runs in write order (later writes win when applied), consecutive registers merged.
    for (size_t i = 0; i < g_rs_writes.size();) {
        size_t j = i + 1;
        while (j < g_rs_writes.size() && g_rs_writes[j].first == g_rs_writes[j - 1].first + 1) j++;
        r.push_back(g_rs_writes[i].first);
        r.push_back(uint32_t(j - i));
        for (size_t k = i; k < j; k++) r.push_back(g_rs_writes[k].second);
        i = j;
    }
    r[0] = uint32_t(r.size());
    const uint64_t w = g_q_w.load(std::memory_order_relaxed), rr = g_q_r.load(std::memory_order_acquire);
    // The game restores its state after a resolve through its dirty bits, which the shadow diff does not see: the
    // next draw record carries the full state.
    g_prev_valid = false;
    if (w + r.size() - rr > kQWords) { g_q_dropped++; return; }
    for (size_t i = 0; i < r.size(); i++) g_q[(w + i) & kQMask] = r[i];
    g_q_w.store(w + r.size(), std::memory_order_release);
    g_rs_pushed++;
}

// CP thread state for the queue path.
constexpr uint32_t kImg = 0x4928, kBlocks = (kImg + 15) / 16;
uint32_t g_cur[kImg], g_sent[kImg];
uint64_t g_blk_dirty[(kBlocks + 63) / 64];
bool g_cur_init = false, g_sent_valid = false;
uint64_t g_b_flushed = 0, g_b_nonidx = 0, g_b_draws = 0, g_b_missing = 0, g_b_refused = 0, g_b_dwords = 0, g_b_skipped = 0, g_b_recs = 0;
uint64_t g_b_cmp_draws = 0, g_b_diff_draws = 0, g_b_diff_seg[16], g_b_bad_args = 0;
int g_b_printed = 0, g_b_sh_printed = 0;
uint64_t g_b_bad_sh[2][2];  // [vs/ps][indexed/non-indexed]
std::vector<uint8_t> g_b_payload;

inline void mark(uint32_t reg) { g_blk_dirty[(reg >> 4) >> 6] |= 1ull << ((reg >> 4) & 63); }

bool direct_draw_queue(const fa::gpu::DrawCall& dc, const fa::gpu::RegisterFile& regs, fa::gpu::Backend& backend) {
    // FA_DIRECT bit 4 (16) adds non-indexed draws (auto indices). Their records are queued either way, so a draw that
    // stays on the PM4 path still consumes its record (run 109: with FA_DIRECT=8 the menus' runs of more than 256
    // non-indexed records were never consumed, the CP stopped finding records and the producer dropped).
    const bool eligible = dc.inlineIndices.empty() && (dc.indexed || (g_direct & 16));
    if (!g_cur_init) {
        std::memcpy(g_cur + 0x2000, regs.r.data() + 0x2000, 0x400 * 4);
        std::memcpy(g_cur + 0x4000, regs.r.data() + 0x4000, 0x928 * 4);
        g_cur_init = true;
    }
    // Find the record for this packet among the next records, in order.
    const uint64_t w = g_q_w.load(std::memory_order_acquire);
    uint64_t r = g_q_r.load(std::memory_order_relaxed), at = r, match = ~0ull;
    for (int n = 0; n < 256 && at < w; n++) {
        if (g_q[(at + 2) & kQMask] == dc.packetAddr) { match = at; break; }
        at += g_q[at & kQMask];
    }
    if (match == ~0ull) {
        g_b_missing++;
        // Safety valve: if the backlog grows past half the ring, apply everything queued (applying in order always
        // keeps the image consistent; only the lookups for those draws are lost).
        if (w - r > kQWords / 2) match = ~0ull - 1;
        else return false;
    }
    // Apply every record up to and including the match (records the CP never draws still carry state).
    uint64_t hdr = 0;
    while (r <= match && r < w) {
        const uint32_t len = g_q[r & kQMask];
        for (uint32_t i = kRecHdr; i < len;) {
            const uint32_t first = g_q[(r + i) & kQMask], cnt = g_q[(r + i + 1) & kQMask];
            for (uint32_t k = 0; k < cnt; k++) g_cur[first + k] = g_q[(r + i + 2 + k) & kQMask];
            for (uint32_t b = first >> 4; b <= (first + cnt - 1) >> 4; b++) g_blk_dirty[b >> 6] |= 1ull << (b & 63);
            i += 2 + cnt;
        }
        if (r != match) g_b_skipped++;
        g_b_recs++;
        hdr = r;
        r += len;
    }
    g_q_r.store(r, std::memory_order_release);
    if (match == ~0ull - 1) { g_b_flushed++; return false; }
    // A resolve record matched by a draw that is not a copy (no shader identities in it): leave it to the PM4 path.
    if (!eligible || (g_q[(hdr + 1) & kQMask] & 2)) return false;
    for (uint32_t reg : kCpRegs)
        if (g_cur[reg] != regs[reg]) { g_cur[reg] = regs[reg]; mark(reg); }
    auto hv = [&](uint32_t i) { return g_q[(hdr + i) & kQMask]; };
    if (g_tap_report) {
        // Check the image against the CP's register file over everything the record holds, except the vec4s the
        // per-object blocks write (the mirror rule; checked by the stage A tap).
        g_b_cmp_draws++;
        bool any = false;
        int si = 0;
        for (const GSeg& sg : kGSegs) {
            for (uint32_t i = 0; i < sg.n; i++) {
                const uint32_t reg = sg.reg + i;
                if (reg >= 0x4000 && reg < 0x4800 && touched((reg - 0x4000) >> 2)) continue;
                if (g_cur[reg] != regs[reg]) {
                    any = true;
                    g_b_diff_seg[si]++;
                    if (g_b_printed < 12) {
                        g_b_printed++;
                        std::fprintf(stderr, "[direct] queue image reg %04x: game %08x cp %08x (packet %08x)\n", reg, g_cur[reg], regs[reg], dc.packetAddr);
                    }
                }
            }
            si++;
        }
        g_b_diff_draws += any;
        // Shader identity: the game's microcode (object pointers at draw time) against what the CP loaded.
        for (int t = 0; t < 2; t++) {
            const uint32_t addr = hv(t ? 10 : 7), bytes = hv(t ? 11 : 8);
            const uint32_t* cpu = t ? dc.psUcode : dc.vsUcode;
            const uint32_t cpn = t ? dc.psDwords : dc.vsDwords;
            uint32_t g = addr < 0x20000000 ? addr + 0xA0000000u : addr >= 0xC0000000u ? (0xA0000000u | (addr & 0x1FFFFFFFu)) : addr;
            bool same = cpu && bytes / 4 == cpn && bytes && fa::rt::g_mem.is_committed(g) && fa::rt::g_mem.is_committed(g + bytes - 1);
            for (uint32_t i = 0; same && i < cpn; i++) same = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(fa::rt::host(g + i * 4))) == cpu[i];
            if (!same) {
                g_b_bad_sh[t][dc.indexed ? 0 : 1]++;
                if (g_b_sh_printed < 8) {
                    g_b_sh_printed++;
                    std::fprintf(stderr, "[direct] queue %s shader differs (%s draw, packet %08x): game ucode %08x bytes %u, CP dwords %u first %08x\n", t ? "ps" : "vs",
                                 dc.indexed ? "indexed" : "non-indexed", dc.packetAddr, addr, bytes, cpn, cpu && cpn ? cpu[0] : 0);
                }
            }
        }
        g_b_bad_args += hv(3) != dc.primType || hv(4) != dc.indexCount || (dc.indexed && (hv(5) & 0x1FFFFFFF) != (dc.indexGuestAddr & 0x1FFFFFFF));
    }
    fa::gpu::DirectDraw rec;
    rec.seq = g_dd_seq++;
    rec.packetAddr = dc.packetAddr;
    rec.primType = dc.primType;
    rec.indexCount = dc.indexCount;
    rec.indexAddr = dc.indexed ? dc.indexGuestAddr : 0;
    rec.index32 = dc.index32;
    rec.indexEndian = uint8_t(dc.indexEndian);
    if (!dc.indexed) g_b_nonidx++;
    rec.vsObj = hv(6); rec.vsUcode = hv(7); rec.vsBytes = hv(8);
    rec.psObj = hv(9); rec.psUcode = hv(10); rec.psBytes = hv(11);
    rec.declObj = hv(12);
    rec.tileRows = regs.tileRows;
    // Payload: runs inside the dirty blocks that differ from what the backend has (everything after a refusal).
    g_b_payload.clear();
    auto emit = [&](uint32_t i, uint32_t j) {
        fa::gpu::DirectRegRun run{i, j - i};
        const size_t o = g_b_payload.size();
        g_b_payload.resize(o + sizeof run + run.count * 4);
        std::memcpy(g_b_payload.data() + o, &run, sizeof run);
        std::memcpy(g_b_payload.data() + o + sizeof run, g_cur + i, run.count * 4);
        g_b_dwords += run.count;
    };
    if (!g_sent_valid) {
        emit(0x2000, 0x2400);
        emit(0x4000, kImg);
    } else {
        for (uint32_t wd = 0; wd < sizeof g_blk_dirty / 8; wd++) {
            uint64_t m = g_blk_dirty[wd];
            while (m) {
                const uint32_t b = wd * 64 + __builtin_ctzll(m);
                m &= m - 1;
                const uint32_t lo = b * 16, hi = std::min(lo + 16, kImg);
                for (uint32_t i = lo; i < hi;) {
                    if (g_cur[i] == g_sent[i]) { i++; continue; }
                    uint32_t j = i;
                    while (j < hi && g_cur[j] != g_sent[j]) j++;
                    emit(i, j);
                    i = j;
                }
            }
        }
    }
    rec.payloadBytes = uint32_t(g_b_payload.size());
    if (!backend.DrawDirect(rec, g_b_payload.data())) {
        g_b_refused++;
        g_sent_valid = false;
        return false;
    }
    if (!g_sent_valid) {
        std::memcpy(g_sent + 0x2000, g_cur + 0x2000, 0x400 * 4);
        std::memcpy(g_sent + 0x4000, g_cur + 0x4000, (kImg - 0x4000) * 4);
        g_sent_valid = true;
    } else {
        for (uint32_t wd = 0; wd < sizeof g_blk_dirty / 8; wd++) {
            uint64_t m = g_blk_dirty[wd];
            while (m) {
                const uint32_t b = wd * 64 + __builtin_ctzll(m);
                m &= m - 1;
                const uint32_t lo = b * 16, hi = std::min(lo + 16, kImg);
                std::memcpy(g_sent + lo, g_cur + lo, (hi - lo) * 4);
            }
        }
    }
    std::memset(g_blk_dirty, 0, sizeof g_blk_dirty);
    g_b_draws++;
    return true;
}

// CP resolve (copy-mode draw): consume records up to the resolve record of this packet; with FA_TAP=1 compare the
// image with the CP's registers the resolve reads; with FA_DIRECT bit 5 (32) resolve from the image.
uint64_t g_rb_resolves = 0, g_rb_missing = 0, g_rb_direct = 0, g_rb_cmp = 0, g_rb_diff = 0, g_rb_reg[0x30];
int g_rb_printed = 0;
constexpr uint32_t kResolveRegs[] = {0x2000, 0x2001, 0x2002, 0x2080, 0x2081, 0x2082, 0x2205, 0x2208, 0x2302,
                                     0x2318, 0x2319, 0x231A, 0x231B, 0x231C, 0x231D, 0x231E, 0x231F, 0x4800, 0x4801};

bool direct_resolve_queue(const fa::gpu::DrawCall& dc, const fa::gpu::RegisterFile& regs, fa::gpu::Backend& backend) {
    g_rb_resolves++;
    if (!g_cur_init) return false;
    const uint64_t w = g_q_w.load(std::memory_order_acquire);
    uint64_t r = g_q_r.load(std::memory_order_relaxed), at = r, match = ~0ull;
    for (int n = 0; n < 256 && at < w; n++) {
        if (g_q[(at + 2) & kQMask] == dc.packetAddr) { match = at; break; }
        at += g_q[at & kQMask];
    }
    if (match == ~0ull) { g_rb_missing++; return false; }
    while (r <= match) {
        const uint32_t len = g_q[r & kQMask];
        for (uint32_t i = kRecHdr; i < len;) {
            const uint32_t first = g_q[(r + i) & kQMask], cnt = g_q[(r + i + 1) & kQMask];
            for (uint32_t k = 0; k < cnt; k++) g_cur[first + k] = g_q[(r + i + 2 + k) & kQMask];
            for (uint32_t b = first >> 4; b <= (first + cnt - 1) >> 4; b++) g_blk_dirty[b >> 6] |= 1ull << (b & 63);
            i += 2 + cnt;
        }
        if (r != match) g_b_skipped++;
        g_b_recs++;
        r += len;
    }
    g_q_r.store(r, std::memory_order_release);
    // Window offset and scissor stay with the CP (not in any record yet).
    for (uint32_t reg : {0x2080u, 0x2081u, 0x2082u})
        if (g_cur[reg] != regs[reg]) { g_cur[reg] = regs[reg]; mark(reg); }
    if (g_tap_report) {
        g_rb_cmp++;
        bool any = false;
        for (uint32_t reg : kResolveRegs) {
            if (g_cur[reg] == regs[reg]) continue;
            any = true;
            g_rb_reg[reg >= 0x4800 ? 0x2E + (reg - 0x4800) : reg >= 0x2300 ? 0x10 + (reg - 0x2310) : reg - 0x2000 < 0x10 ? reg - 0x2000 : 0x2F]++;
            if (g_rb_printed < 12) {
                g_rb_printed++;
                std::fprintf(stderr, "[direct] resolve image reg %04x: game %08x cp %08x (packet %08x)\n", reg, g_cur[reg], regs[reg], dc.packetAddr);
            }
        }
        g_rb_diff += any;
    }
    if (!(g_direct & 32)) return false;
    static fa::gpu::RegisterFile rf;
    rf = regs;  // registers no record covers yet keep the CP's values
    std::memcpy(rf.r.data() + 0x2000, g_cur + 0x2000, 0x400 * 4);
    std::memcpy(rf.r.data() + 0x4000, g_cur + 0x4000, (kImg - 0x4000) * 4);
    backend.Resolve(rf);
    g_rb_direct++;
    return true;
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

void apply_pending_locked(const uint8_t* base);

// D3D resolve (82142490): r3 device. Its own stream output becomes a resolve record (see push_resolve_record).
PPC_FUNC(sub_82142490) {
    const uint32_t dev = ctx.r3.u32;
    const uint32_t before = (g_direct & 8) ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + dev)) : 0;
    __imp__sub_82142490(ctx, base);
    if (g_direct & 8) {
        const uint32_t after = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + dev));
        push_resolve_record(base, before, after);
    }
}

// Non-indexed draw (823152C0): r3 device, r4 primitive type, r5 start vertex (written to the shadow as 0x2102
// VGT_INDX_OFFSET before the flush), r6 vertex count. Counts above 65535 are split into several packets by the
// game; those are left to the PM4 path (no record).
PPC_FUNC(sub_823152C0) {
    if (g_on) g_tap2_calls.fetch_add(1, std::memory_order_relaxed);
    const uint32_t dev = ctx.r3.u32;
    const DrawArgs a{ctx.r4.u32, 0, ctx.r6.u32, 0, 0};
    __imp__sub_823152C0(ctx, base);
    if ((g_direct & 8) && a.count <= 65535) {
        {
            std::lock_guard<std::mutex> lk(g_set_mu);
            apply_pending_locked(base);
        }
        push_record(base, dev, a, false);
        g_q_nonidx++;
    }
}

// Applies the per-object constant blocks issued so far to the mirror, in stream order (caller holds g_set_mu).
void apply_pending_locked(const uint8_t* base) {
    for (const PendingBlock& b : g_pending) {
        for (uint32_t v = b.first; v < b.first + b.count && v < 256; v++) g_cur_own[v >> 6] |= 1ull << (v & 63);
        for (uint32_t i = 0; i < b.count * 4 && b.first * 4 + i < 2048; i++)
            g_mirror[b.first * 4 + i] = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + b.payload + i * 4));
        for (uint32_t v = b.first; v < b.first + b.count && v < 512; v++) g_mirror_valid[v >> 6] |= 1ull << (v & 63);
    }
    g_pending.clear();
}

// Dirty-state flush (82148508): the blocks issued before it are in the stream first, then the flush writes the dirty
// constant groups. The VS dirty word is read by the vec4 range writer 82147160, not at flush entry: the shader-change
// helpers the flush calls first (82147288, 82147E08 via 82147D18) OR more VS bits into device+0x10 (run 102: 7.3% of
// touched vec4s differed, in whole 4-vec4 groups 68..103, because the mirror read the word too early).
PPC_FUNC(sub_82148508) {
    if (g_on) {
        std::lock_guard<std::mutex> lk(g_set_mu);
        apply_pending_locked(base);
        g_flushes++;
    }
    __imp__sub_82148508(ctx, base);
}

// Vec4 range writer (82147160): r4 dirty bits (MSB first, 4 vec4 per bit), r5 first register, r6 shadow pointer.
// Writing VS constants (r5 = 0x4000) from the shadow is exactly what the GPU's constant file then holds.
PPC_FUNC(sub_82147160) {
    if (g_on && ctx.r5.u32 == 0x4000) {
        const uint64_t dirty = ctx.r4.u64;
        const uint32_t* shadow = reinterpret_cast<const uint32_t*>(base + ctx.r6.u32);
        std::lock_guard<std::mutex> lk(g_set_mu);
        for (uint32_t k = 0; k < 64; k++) {
            if (!(dirty >> (63 - k) & 1)) continue;
            for (uint32_t i = 0; i < 16; i++) g_mirror[k * 16 + i] = __builtin_bswap32(shadow[k * 16 + i]);
        }
    }
    __imp__sub_82147160(ctx, base);
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
            apply_pending_locked(base);
        }
        g_cur_args = args0;
        const uint32_t ib = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + dev + 0x30F4));
        if (ib) {
            g_cur_args.ibFlags = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ib));
            g_cur_args.ibBase = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ib + 0xC));
        }
        // Stage A snapshots are only needed by the stage A path and the tap.
        if ((g_direct & 7) || g_tap_report) snapshot_draw(base, dev);
        if (g_direct & 8) push_record(base, dev, g_cur_args);
        std::memset(g_cur_own, 0, sizeof g_cur_own);
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
    if ((g_direct & 8) && !fa::gpu::g_direct_resolve) fa::gpu::g_direct_resolve = direct_resolve_queue;
    if ((g_direct & 8) && !fa::gpu::g_direct_draw) fa::gpu::g_direct_draw = direct_draw_queue;
    else if ((g_direct & 4) && !fa::gpu::g_direct_draw) fa::gpu::g_direct_draw = direct_draw;
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
        if (g_direct & 4)
            std::fprintf(stderr, "[direct] stage A: draws through DrawDirect %llu, no record %llu, refused by backend %llu, payload dwords %llu\n",
                         (unsigned long long)g_dd_draws, (unsigned long long)g_dd_missing, (unsigned long long)g_dd_refused, (unsigned long long)g_dd_dwords);
    }
    if ((g_direct & 8) && g_swaps % 300 == 0) {
        std::fprintf(stderr, "[direct] stage B queue: records pushed %llu (non-indexed %llu, drawn direct %llu) (avg %.1f words), dropped %llu | CP draws through DrawDirect %llu, no record %llu, refused %llu, records applied %llu (not drawn by the CP %llu), backlog flushes %llu, payload dwords %llu\n",
                     (unsigned long long)g_q_pushed, (unsigned long long)g_q_nonidx, (unsigned long long)g_b_nonidx, g_q_pushed ? double(g_q_words_pushed) / double(g_q_pushed) : 0.0, (unsigned long long)g_q_dropped,
                     (unsigned long long)g_b_draws, (unsigned long long)g_b_missing, (unsigned long long)g_b_refused,
                     (unsigned long long)g_b_recs, (unsigned long long)g_b_skipped, (unsigned long long)g_b_flushed, (unsigned long long)g_b_dwords);
        std::fprintf(stderr, "[direct] stage B resolves: records pushed %llu, output not contiguous %llu, no copy draw %llu | CP resolves %llu, no record %llu, resolved from the record %llu\n",
                     (unsigned long long)g_rs_pushed, (unsigned long long)g_rs_wrapped, (unsigned long long)g_rs_nodraw,
                     (unsigned long long)g_rb_resolves, (unsigned long long)g_rb_missing, (unsigned long long)g_rb_direct);
        if (g_tap_report) {
            std::fprintf(stderr, "[direct] stage B resolve check: compared %llu, image differs %llu | by reg:", (unsigned long long)g_rb_cmp, (unsigned long long)g_rb_diff);
            for (uint32_t reg : kResolveRegs) {
                const uint64_t c = g_rb_reg[reg >= 0x4800 ? 0x2E + (reg - 0x4800) : reg >= 0x2300 ? 0x10 + (reg - 0x2310) : reg - 0x2000 < 0x10 ? reg - 0x2000 : 0x2F];
                if (c) std::fprintf(stderr, " %04x:%llu", reg, (unsigned long long)c);
            }
            std::fprintf(stderr, "\n");
        }
        if (g_tap_report) {
            std::fprintf(stderr, "[direct] stage B check: draws %llu, image differs from CP %llu, args differ %llu | by segment (vs ps fetch boolloop 2000 2100 2180 2200 2209 2280 2300 2380):",
                         (unsigned long long)g_b_cmp_draws, (unsigned long long)g_b_diff_draws, (unsigned long long)g_b_bad_args);
            for (size_t i = 0; i < sizeof kGSegs / sizeof kGSegs[0]; i++) std::fprintf(stderr, " %llu", (unsigned long long)g_b_diff_seg[i]);
            std::fprintf(stderr, " | shader differs: vs indexed %llu non-indexed %llu, ps indexed %llu non-indexed %llu\n",
                         (unsigned long long)g_b_bad_sh[0][0], (unsigned long long)g_b_bad_sh[0][1], (unsigned long long)g_b_bad_sh[1][0], (unsigned long long)g_b_bad_sh[1][1]);
        }
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
        std::fprintf(stderr, "[tap]   vec4s written by the draw's own blocks: compared %llu, differ %llu\n", (unsigned long long)g_own_cmp, (unsigned long long)g_own_diff);
        std::fprintf(stderr, "[tap]   touched vec4s not written by the draw's own blocks, shadow value vs CP: compared %llu, differ %llu | flushes seen %llu (mirror now follows flushes)\n", (unsigned long long)g_rule_cmp, (unsigned long long)g_rule_diff, (unsigned long long)g_flushes);
        std::fprintf(stderr, "[tap]   bool/loop constants (shadow +0x2780 vs regs 0x4900..0x4927): compared %llu, differ by reg:", (unsigned long long)g_bl_cmp);
        for (uint32_t i = 0; i < 40; i++) if (g_bl_diff[i]) std::fprintf(stderr, " %04x:%llu", 0x4900 + i, (unsigned long long)g_bl_diff[i]);
        std::fprintf(stderr, "\n");
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
