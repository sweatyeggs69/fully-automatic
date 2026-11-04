// Built-in profiler (debug aid): FA_PROFILE=<seconds>, default 10.
//
// Every <seconds> it prints, for the interval just ended:
//   * CPU time per thread (from /proc/self/task, named after the guest thread),
//   * the hottest functions from a SIGPROF sampler (self time, and the innermost recompiled
//     guest function sub_XXXXXXXX on the stack, which is what the game was doing),
//   * the most called kernel/xam imports with calls per second and the time spent inside them
//     (time inside an import includes waiting, so a wait import with a large time is a thread
//     that sleeps, not one that burns CPU).
// The fullauto binary is linked with -rdynamic, so dladdr names the recompiled functions.
#include "fa/rt/import.hpp"

#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fa::rt {

bool g_profile = std::getenv("FA_PROFILE") != nullptr;

static std::atomic<ImportStat*> g_import_stats{nullptr};

ImportStat::ImportStat(const char* n) : name(n) {
    ImportStat* head = g_import_stats.load(std::memory_order_relaxed);
    do next = head;
    while (!g_import_stats.compare_exchange_weak(head, this, std::memory_order_release, std::memory_order_relaxed));
}

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

namespace {

constexpr int kFrames = 14;
constexpr uint32_t kSlots = 1u << 16;
constexpr int kPeriodUs = 2000;  // one sample per 2 ms of process CPU time

struct Sample {
    std::atomic<uint64_t> seq{0};  // index + 1 once written
    int32_t tid;
    int32_t n;
    void* pc[kFrames];
};
Sample* g_samples = nullptr;
std::atomic<uint64_t> g_sample_w{0};

void prof_handler(int, siginfo_t*, void* uc) {
    int saved = errno;
    uint64_t i = g_sample_w.fetch_add(1, std::memory_order_relaxed);
    Sample& s = g_samples[i % kSlots];
    s.seq.store(0, std::memory_order_relaxed);
    s.tid = int32_t(::syscall(SYS_gettid));
    void* raw[kFrames + 2];
    int n = backtrace(raw, kFrames + 2);
    // raw[0] is this handler and raw[1] the signal trampoline; the interrupted pc comes from
    // the signal context (the trampoline frame's caller entry can be off by one call).
    s.pc[0] = reinterpret_cast<void*>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]);
    int k = 1;
    for (int j = 3; j < n && k < kFrames; ++j) s.pc[k++] = raw[j];
    s.n = k;
    s.seq.store(i + 1, std::memory_order_release);
    errno = saved;
}

std::unordered_map<void*, std::string> g_sym_cache;

const std::string& symbol(void* pc) {
    auto it = g_sym_cache.find(pc);
    if (it != g_sym_cache.end()) return it->second;
    Dl_info info{};
    std::string name;
    // pc - 1 so a return address resolves to the calling function.
    if (dladdr(static_cast<char*>(pc) - 1, &info) && info.dli_sname) {
        int st = 0;
        char* d = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &st);
        name = (st == 0 && d) ? d : info.dli_sname;
        std::free(d);
        if (name.size() > 70) name = name.substr(0, 70) + "...";
    } else if (info.dli_fname) {
        const char* f = std::strrchr(info.dli_fname, '/');
        char buf[64];
        std::snprintf(buf, sizeof buf, "[%s]", f ? f + 1 : info.dli_fname);
        name = buf;
    } else {
        name = "[unknown]";
    }
    return g_sym_cache.emplace(pc, std::move(name)).first->second;
}

struct ThreadCpu { std::string name; uint64_t ticks; };

std::map<long, ThreadCpu> read_threads() {
    std::map<long, ThreadCpu> out;
    DIR* d = opendir("/proc/self/task");
    if (!d) return out;
    while (dirent* de = readdir(d)) {
        long tid = std::atol(de->d_name);
        if (tid <= 0) continue;
        char path[64], buf[512];
        std::snprintf(path, sizeof path, "/proc/self/task/%ld/stat", tid);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        size_t len = std::fread(buf, 1, sizeof buf - 1, f);
        std::fclose(f);
        buf[len] = 0;
        char* open = std::strchr(buf, '(');
        char* close = std::strrchr(buf, ')');
        if (!open || !close) continue;
        ThreadCpu t;
        t.name.assign(open + 1, close);
        // Fields after the name: state(3) ... utime(14) stime(15).
        unsigned long long ut = 0, st = 0;
        char* p = close + 2;
        for (int field = 3; field < 14 && p; ++field) p = std::strchr(p, ' '), p = p ? p + 1 : nullptr;
        if (p && std::sscanf(p, "%llu %llu", &ut, &st) == 2) t.ticks = ut + st;
        else t.ticks = 0;
        out[tid] = t;
    }
    closedir(d);
    return out;
}

template <class M>
void print_top(const char* title, const M& counts, uint64_t total, int limit) {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& [k, n] : counts) v.push_back({n, k});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[profile] %s (%llu samples):\n", title, (unsigned long long)total);
    for (int i = 0; i < int(v.size()) && i < limit; ++i)
        std::fprintf(stderr, "[profile]   %5.1f%%  %s\n", total ? 100.0 * v[i].first / total : 0.0, v[i].second.c_str());
}

void reporter(int secs) {
    long hz = sysconf(_SC_CLK_TCK);
    auto prev_threads = read_threads();
    std::unordered_map<ImportStat*, std::pair<uint64_t, uint64_t>> prev_imports;
    uint64_t r = 0;
    uint64_t t_prev = now_ns(), tsc_prev = profile_ticks();
    for (int round = 1;; ++round) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        uint64_t t_now = now_ns();
        uint64_t tsc_now = profile_ticks();
        double dt = (t_now - t_prev) / 1e9;
        double tsc_per_ns = double(tsc_now - tsc_prev) / double(t_now - t_prev);
        t_prev = t_now;
        tsc_prev = tsc_now;
        std::fprintf(stderr, "\n[profile] ===== interval %d (%.1f s) =====\n", round, dt);

        // Threads by CPU use.
        auto threads = read_threads();
        std::vector<std::pair<double, std::string>> tv;
        for (auto& [tid, t] : threads) {
            auto it = prev_threads.find(tid);
            uint64_t before = it == prev_threads.end() ? 0 : it->second.ticks;
            double pct = 100.0 * double(t.ticks - before) / double(hz) / dt;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%-16s tid %ld", t.name.c_str(), tid);
            tv.push_back({pct, buf});
        }
        prev_threads = std::move(threads);
        std::sort(tv.begin(), tv.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::fprintf(stderr, "[profile] thread CPU (100%% = one core busy):\n");
        for (auto& [pct, name] : tv)
            if (pct >= 0.5) std::fprintf(stderr, "[profile]   %6.1f%%  %s\n", pct, name.c_str());

        // Samples since the last report.
        uint64_t w = g_sample_w.load(std::memory_order_acquire);
        if (w - r > kSlots) r = w - kSlots;
        std::unordered_map<std::string, uint64_t> self, guest;
        std::unordered_map<long, std::unordered_map<std::string, uint64_t>> guest_by_tid;
        uint64_t total = 0;
        for (; r < w; ++r) {
            Sample& s = g_samples[r % kSlots];
            if (s.seq.load(std::memory_order_acquire) != r + 1) continue;
            ++total;
            self[symbol(s.pc[0])]++;
            const char* g = nullptr;
            for (int j = 0; j < s.n; ++j) {
                const std::string& name = symbol(s.pc[j]);
                if (name.compare(0, 4, "sub_") == 0) { g = name.c_str(); break; }
            }
            std::string gname = g ? g : "(no guest frame)";
            guest[gname]++;
            guest_by_tid[s.tid][gname]++;
        }
        print_top("hottest functions, self time", self, total, 20);
        print_top("innermost guest function on the stack", guest, total, 20);
        for (auto& [tid, m] : guest_by_tid) {
            uint64_t n = 0;
            for (auto& [k, c] : m) n += c;
            if (n * 10 < total) continue;  // only threads with at least 10% of the samples
            auto it = prev_threads.find(tid);
            char title[96];
            std::snprintf(title, sizeof title, "guest functions on thread %s (tid %ld)",
                          it == prev_threads.end() ? "?" : it->second.name.c_str(), tid);
            print_top(title, m, n, 8);
        }

        // Imports by call count.
        struct Row { const char* name; uint64_t calls, ns; };
        std::vector<Row> rows;
        for (ImportStat* s = g_import_stats.load(std::memory_order_acquire); s; s = s->next) {
            uint64_t c = s->calls.load(std::memory_order_relaxed), ns = s->ticks.load(std::memory_order_relaxed);
            auto& p = prev_imports[s];
            if (c != p.first) rows.push_back({s->name, c - p.first, ns - p.second});
            p = {c, ns};
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.calls > b.calls; });
        std::fprintf(stderr, "[profile] imports by calls (calls/s, ms inside per s):\n");
        for (size_t i = 0; i < rows.size() && i < 30; ++i)
            std::fprintf(stderr, "[profile]   %10.0f/s  %8.1f ms/s  %s\n", rows[i].calls / dt, rows[i].ns / tsc_per_ns / 1e6 / dt,
                         rows[i].name);
        std::fflush(stderr);
    }
}

}  // namespace

void install_profiler() {
    const char* e = std::getenv("FA_PROFILE");
    if (!e) return;
    int secs = std::atoi(e);
    if (secs <= 0) secs = 10;
    g_samples = new Sample[kSlots];
    void* warm[4];
    backtrace(warm, 4);  // loads the unwinder outside the signal handler
    struct sigaction sa{};
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval it{};
    it.it_interval.tv_usec = kPeriodUs;
    it.it_value.tv_usec = kPeriodUs;
    setitimer(ITIMER_PROF, &it, nullptr);
    std::fprintf(stderr, "[profile] reporting every %d s\n", secs);
    std::thread(reporter, secs).detach();
}

}  // namespace fa::rt
