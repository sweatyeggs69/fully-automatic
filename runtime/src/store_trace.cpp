// Guest store trace: which recompiled functions write the GPU command stream? See store_trace.h.
#include <dlfcn.h>
#include <signal.h>
#include <execinfo.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fa/rt/store_trace_ctl.hpp"

extern "C" {
volatile uint8_t fa_st_armed = 0;
}

namespace {
constexpr int kDepth = 10, kSkip = 2;
struct Range { uint32_t lo, hi; };  // physical, [lo,hi)
struct Key {
    void* pc[kDepth];
    bool operator==(const Key& o) const { return std::equal(pc, pc + kDepth, o.pc); }
};
struct KeyHash {
    size_t operator()(const Key& k) const {
        size_t h = 1469598103934665603ull;
        for (void* p : k.pc) h = (h ^ reinterpret_cast<size_t>(p)) * 1099511628211ull;
        return h;
    }
};
struct Agg { uint64_t stores = 0, bytes = 0; std::map<uint32_t, uint64_t> ops; };

std::mutex g_mu;
std::vector<Range> g_ranges;
std::unordered_map<Key, Agg, KeyHash> g_agg;
std::unordered_map<Key, Agg, KeyHash> g_out;  // PM4-header-looking stores outside every known range
std::map<uint32_t, uint64_t> g_out_pages;     // 64 KB physical pages those stores landed in
int g_tus = 0;
uint64_t g_hits = 0, g_hits_phys = 0, g_hits_in = 0;
int g_swap = 0, g_first = -1, g_frames = 1, g_period = 0;
bool g_cfg_done = false;
volatile sig_atomic_t g_usr2 = 0;
void on_usr2(int) { g_usr2 = 1; }

void configure() {
    g_cfg_done = true;
    const char* e = std::getenv("FA_STORE_TRACE");
    if (!e || !*e) return;
    signal(SIGUSR2, on_usr2);  // kill -USR2 <pid> arms a capture at the next swap, whatever the swap count
    g_first = std::atoi(e);
    if (const char* c = std::strchr(e, ',')) {
        g_frames = std::max(1, std::atoi(c + 1));
        if (const char* d = std::strchr(c + 1, ',')) g_period = std::max(0, std::atoi(d + 1));  // repeat every N swaps
    }
}

std::string name_of(void* pc) {
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname) return di.dli_sname;
    char b[32];
    std::snprintf(b, sizeof b, "?%p", pc);
    return b;
}

void dump() {
    const char* base = std::getenv("FA_STORE_TRACE_OUT");
    static char path_buf[600];
    std::snprintf(path_buf, sizeof path_buf, "%s", base && *base ? base : "store-trace.txt");
    if (g_period) std::snprintf(path_buf, sizeof path_buf, "%s.swap%d", base && *base ? base : "store-trace.txt", g_first);
    const char* path = path_buf;
    FILE* f = std::fopen(path, "w");
    if (!f) f = stderr;
    uint64_t totalStores = 0, totalBytes = 0;
    std::map<std::string, Agg> leaf;
    for (auto& [k, a] : g_agg) {
        totalStores += a.stores;
        totalBytes += a.bytes;
        Agg& l = leaf[name_of(k.pc[0])];
        l.stores += a.stores;
        l.bytes += a.bytes;
        for (auto& [op, n] : a.ops) l.ops[op] += n;
    }
    std::fprintf(f, "# guest stores into GPU command buffers, %d frame(s) from swap %d\n", g_frames, g_first);
    std::fprintf(f, "# total stores %llu, bytes %llu, distinct leaf functions %zu, distinct call chains %zu\n",
                 (unsigned long long)totalStores, (unsigned long long)totalBytes, leaf.size(), g_agg.size());
    std::vector<std::pair<std::string, Agg*>> byLeaf;
    for (auto& [n, a] : leaf) byLeaf.push_back({n, &a});
    std::sort(byLeaf.begin(), byLeaf.end(), [](auto& a, auto& b) { return a.second->bytes > b.second->bytes; });
    std::fprintf(f, "\n## by writing function (leaf), most bytes first\n# bytes%% cumulative%% stores bytes function  [type-3 opcode:count]\n");
    double cum = 0;
    for (auto& [n, a] : byLeaf) {
        double p = totalBytes ? 100.0 * a->bytes / totalBytes : 0;
        cum += p;
        std::fprintf(f, "%5.1f %5.1f %9llu %10llu %s  [", p, cum, (unsigned long long)a->stores, (unsigned long long)a->bytes,
                     n.c_str());
        for (auto& [op, c] : a->ops) std::fprintf(f, "%02x:%llu ", op, (unsigned long long)c);
        std::fprintf(f, "]\n");
    }
    std::fprintf(f, "\n## counters: instrumented stores seen %llu, in physical windows %llu, inside known ranges %llu, ranges %zu, instrumented TUs %d\n",
                 (unsigned long long)g_hits, (unsigned long long)g_hits_phys, (unsigned long long)g_hits_in, g_ranges.size(), g_tus);
    {
        std::map<std::string, Agg> oleaf;
        for (auto& [k, a] : g_out) {
            Agg& l = oleaf[name_of(k.pc[0])];
            l.stores += a.stores;
            for (auto& [op, n] : a.ops) l.ops[op] += n;
        }
        std::fprintf(f, "## PM4-header-looking 32-bit stores OUTSIDE known ranges (%zu leaf functions)\n", oleaf.size());
        for (auto& [n, a] : oleaf) {
            std::fprintf(f, "%9llu %s  [", (unsigned long long)a.stores, n.c_str());
            for (auto& [op, c] : a.ops) std::fprintf(f, "%02x:%llu ", op, (unsigned long long)c);
            std::fprintf(f, "]\n");
        }
        std::fprintf(f, "## 64 KB physical pages receiving them (page: count)\n");
        for (auto& [pg, c] : g_out_pages) std::fprintf(f, "%05x0000: %llu\n", pg, (unsigned long long)c);
        std::fprintf(f, "## known ranges (first 20)\n");
        for (size_t i = 0; i < g_ranges.size() && i < 20; i++) std::fprintf(f, "%08x-%08x\n", g_ranges[i].lo, g_ranges[i].hi);
    }
    std::vector<std::pair<const Key*, const Agg*>> chains;
    for (auto& [k, a] : g_agg) chains.push_back({&k, &a});
    std::sort(chains.begin(), chains.end(), [](auto& a, auto& b) { return a.second->bytes > b.second->bytes; });
    std::fprintf(f, "\n## top call chains (leaf first)\n");
    for (size_t i = 0; i < chains.size() && i < 150; i++) {
        std::fprintf(f, "%10llu B %8llu st  ", (unsigned long long)chains[i].second->bytes,
                     (unsigned long long)chains[i].second->stores);
        for (int d = 0; d < kDepth && chains[i].first->pc[d]; d++)
            std::fprintf(f, "%s%s", d ? " < " : "", name_of(chains[i].first->pc[d]).c_str());
        std::fprintf(f, "\n");
    }
    if (f != stderr) std::fclose(f);
    std::fprintf(stderr, "[store-trace] %llu stores, %llu bytes, %zu leaf functions -> %s\n", (unsigned long long)totalStores,
                 (unsigned long long)totalBytes, leaf.size(), path);
}
}  // namespace

extern "C" int fa_st_register_tu() {
    static std::atomic<int> n{0};
    g_tus = n.fetch_add(1) + 1;
    return g_tus;
}

extern "C" void fa_st_hit(uint32_t ea, uint32_t size, uint64_t value) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hits++;
    if (ea < 0xA0000000u) return;
    g_hits_phys++;
    // 0xE0000000 is the 4 KB-page view: physical = offset + 0x1000.
    const uint32_t phys = (ea & 0x1FFFFFFFu) + (ea >= 0xE0000000u ? 0x1000u : 0u);
    bool in = false;
    for (const Range& r : g_ranges)
        if (phys >= r.lo && phys < r.hi) { in = true; break; }
    if (in) g_hits_in++;
    if (!in) {
        // Not in a known command buffer. Still note stores that look like a type-3 PM4 header, so a
        // wrong range (or a command buffer we never learned about) shows up in the output.
        if (size != 4 || (value >> 30) != 3) return;
        void* bt2[kDepth + kSkip] = {};
        backtrace(bt2, kDepth + kSkip);
        Key k2{};
        for (int i = 0; i < kDepth; i++) k2.pc[i] = bt2[i + kSkip];
        Agg& a2 = g_out[k2];
        a2.stores++;
        a2.bytes += size;
        a2.ops[(value >> 8) & 0x7F]++;
        g_out_pages[phys >> 16]++;
        return;
    }
    void* bt[kDepth + kSkip] = {};
    backtrace(bt, kDepth + kSkip);
    Key k{};
    for (int i = 0; i < kDepth; i++) k.pc[i] = bt[i + kSkip];
    Agg& a = g_agg[k];
    a.stores++;
    a.bytes += size;
    // A 32-bit store that looks like a type-3 PM4 header: record its opcode.
    if (size == 4 && (value >> 30) == 3) a.ops[(value >> 8) & 0x7F]++;
}

namespace fa::rt {
void store_trace_add_range(uint32_t guest_addr, uint32_t bytes) {
    if (!g_cfg_done) configure();
    if (g_first < 0) return;
    std::lock_guard<std::mutex> lk(g_mu);
    const uint32_t lo = guest_addr & 0x1FFFFFFFu;
    for (const Range& r : g_ranges)
        if (r.lo == lo && r.hi == lo + bytes) return;
    g_ranges.push_back({lo, lo + bytes});
}

void store_trace_frame_end() {
    if (!g_cfg_done) configure();
    if (g_first < 0) return;
    const int n = ++g_swap;
    if (g_usr2) {
        g_usr2 = 0;
        g_first = n;  // restart the window here
    }
    if (n == g_first) {
        std::fprintf(stderr, "[store-trace] armed at swap %d (%zu ranges, %d instrumented translation units)\n", n, g_ranges.size(), g_tus);
        { std::lock_guard<std::mutex> lk(g_mu); g_agg.clear(); g_out.clear(); g_out_pages.clear(); g_hits = g_hits_phys = g_hits_in = 0; }
        fa_st_armed = 1;
    } else if (n == g_first + g_frames) {
        fa_st_armed = 0;
        std::lock_guard<std::mutex> lk(g_mu);
        dump();
        if (g_period) g_first += g_period;  // next periodic capture
    }
}
}  // namespace fa::rt
