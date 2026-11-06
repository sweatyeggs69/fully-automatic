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
int g_swap = 0, g_first = -1, g_frames = 1;
bool g_cfg_done = false;
volatile sig_atomic_t g_usr2 = 0;
void on_usr2(int) { g_usr2 = 1; }

void configure() {
    g_cfg_done = true;
    const char* e = std::getenv("FA_STORE_TRACE");
    if (!e || !*e) return;
    signal(SIGUSR2, on_usr2);  // kill -USR2 <pid> arms a capture at the next swap, whatever the swap count
    g_first = std::atoi(e);
    if (const char* c = std::strchr(e, ',')) g_frames = std::max(1, std::atoi(c + 1));
}

std::string name_of(void* pc) {
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname) return di.dli_sname;
    char b[32];
    std::snprintf(b, sizeof b, "?%p", pc);
    return b;
}

void dump() {
    const char* path = std::getenv("FA_STORE_TRACE_OUT");
    FILE* f = std::fopen(path && *path ? path : "store-trace.txt", "w");
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
                 (unsigned long long)totalBytes, leaf.size(), path && *path ? path : "store-trace.txt");
}
}  // namespace

extern "C" void fa_st_hit(uint32_t ea, uint32_t size, uint64_t value) {
    if (ea < 0xA0000000u) return;
    const uint32_t phys = ea & 0x1FFFFFFFu;
    std::lock_guard<std::mutex> lk(g_mu);
    bool in = false;
    for (const Range& r : g_ranges)
        if (phys >= r.lo && phys < r.hi) { in = true; break; }
    if (!in) return;
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
        std::fprintf(stderr, "[store-trace] armed at swap %d (%zu ranges)\n", n, g_ranges.size());
        { std::lock_guard<std::mutex> lk(g_mu); g_agg.clear(); }
        fa_st_armed = 1;
    } else if (n == g_first + g_frames) {
        fa_st_armed = 0;
        std::lock_guard<std::mutex> lk(g_mu);
        dump();
    }
}
}  // namespace fa::rt
