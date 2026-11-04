// Crash reporting: on SIGSEGV and friends print the fault address (and the guest address
// when it lies inside the guest window) plus a native backtrace. Recompiled functions keep
// their sub_XXXXXXXX names (the fullauto binary is linked with -rdynamic), so the backtrace
// names the guest functions directly.
#include "fa/rt/crash.hpp"
#include "fa/rt/guest.hpp"
#include "fa/rt/kernel.hpp"

#include <dirent.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <cstring>

namespace fa::rt {

static void write_str(const char* s) { ssize_t r = ::write(2, s, std::strlen(s)); (void)r; }

static void write_hex(const char* label, uint64_t v) {
    char buf[64];
    int n = std::snprintf(buf, sizeof buf, "%s0x%llx\n", label, (unsigned long long)v);
    ssize_t r = ::write(2, buf, n);
    (void)r;
}

// ---- guest write watch (debug aid): FA_WATCH=<hexaddr>:<hexlen> -------------------------------
// The watched pages are kept read-only by a poller thread; a write faults, is logged with a
// backtrace, then the instruction is single-stepped with the page writable.
static uint32_t g_watch_lo = 0, g_watch_hi = 0;
static std::atomic<bool> g_watch_on{false}, g_stepping{false};
static std::atomic<int> g_watch_logs{0};
static uint8_t* g_step_page = nullptr;

static bool watch_fault(siginfo_t* info, void* uc) {
    if (!g_watch_on) return false;
    uint64_t a = reinterpret_cast<uint64_t>(info->si_addr);
    uint64_t b = reinterpret_cast<uint64_t>(g_base);
    if (a < b || a >= b + kGuestWindow) return false;
    uint32_t ga = uint32_t(a - b);
    if (ga < (g_watch_lo & ~0xFFFu) || ga >= ((g_watch_hi + 0xFFF) & ~0xFFFu)) return false;
    g_step_page = g_base + (ga & ~0xFFFu);
    g_stepping = true;
    mprotect(g_step_page, 0x1000, PROT_READ | PROT_WRITE);
    if (ga >= g_watch_lo && ga < g_watch_hi && g_watch_logs.fetch_add(1) < 60) {
        char buf[96];
        int n = std::snprintf(buf, sizeof buf, "[watch] write to guest %08x, backtrace:\n", ga);
        ssize_t r = ::write(2, buf, n); (void)r;
        void* frames[16];
        int nf = backtrace(frames, 16);
        backtrace_symbols_fd(frames + 2, nf - 2, 2);
        dump_guest_context_short();
    }
    static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_EFL] |= 0x100;  // trap flag
    return true;
}

static void trap_handler(int, siginfo_t*, void* uc) {
    static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_EFL] &= ~0x100ull;
    if (g_step_page) mprotect(g_step_page, 0x1000, PROT_READ);
    g_stepping = false;
}

void install_write_watch() {
    const char* e = std::getenv("FA_WATCH");
    if (!e) return;
    unsigned long lo = 0, len = 0x10;
    if (std::sscanf(e, "%lx:%lx", &lo, &len) < 1) return;
    g_watch_lo = uint32_t(lo);
    g_watch_hi = uint32_t(lo + len);
    g_watch_on = true;
    struct sigaction sa{};
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGTRAP, &sa, nullptr);
    std::fprintf(stderr, "write watch on guest %08x-%08x\n", g_watch_lo, g_watch_hi);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (g_stepping) continue;
            uint32_t a = g_watch_lo & ~0xFFFu, e2 = (g_watch_hi + 0xFFF) & ~0xFFFu;
            if (!g_stepping) mprotect(g_base + a, e2 - a, PROT_READ);
        }
    }).detach();
}

static void handler(int sig, siginfo_t* info, void* uc) {
    if (sig == SIGSEGV && watch_fault(info, uc)) return;
    signal(sig, SIG_DFL);
    write_str("\n*** fullauto crashed: ");
    write_str(sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL"
              : sig == SIGFPE ? "SIGFPE" : "SIGABRT");
    write_str("\n");
    uint64_t a = reinterpret_cast<uint64_t>(info->si_addr);
    write_hex("fault address: ", a);
    uint64_t b = reinterpret_cast<uint64_t>(g_base);
    if (g_base && a >= b && a < b + kGuestWindow) write_hex("  = guest address: ", a - b);
    void* frames[48];
    int n = backtrace(frames, 48);
    write_str("backtrace (innermost first):\n");
    backtrace_symbols_fd(frames, n, 2);
    dump_guest_context();
    _exit(128 + sig);
}

// ---- stack sampler (debug aid): FA_DUMP_AFTER=<seconds> ----------------------------------------
// After the delay, every thread is sent SIGUSR2 once, one thread at a time and prints where it is,
// which shows what a stuck thread is executing.
static void sample_handler(int) {
    char buf[64];
    int n = std::snprintf(buf, sizeof buf, "\n[sample] thread %ld\n", (long)::syscall(SYS_gettid));
    ssize_t r = ::write(2, buf, n); (void)r;
    void* frames[24];
    int nf = backtrace(frames, 24);
    backtrace_symbols_fd(frames + 1, nf - 1, 2);
    dump_guest_context_short();
}

// FA_PEEK=<chain>[,<chain>...] prints guest words after the samples. A chain is hex values joined
// by '>': the first is an address, each later one is an offset added to the word loaded so far,
// e.g. 827007e8>2890>0 reads the pointer at 827007e8, then the pointer at +0x2890, then that word.
static void dump_peeks() {
    const char* e = std::getenv("FA_PEEK");
    if (!e) return;
    const char* p = e;
    while (*p) {
        char* end;
        uint32_t a = uint32_t(std::strtoul(p, &end, 16));
        std::fprintf(stderr, "[peek] %08x", a);
        p = end;
        bool ok = true;
        while (*p == '>') {
            uint32_t off = uint32_t(std::strtoul(p + 1, &end, 16));
            p = end;
            if (a >= kGuestWindow - 4) { ok = false; break; }
            a = ld32(a) + off;
            std::fprintf(stderr, " -> %08x", a);
        }
        if (ok && a < kGuestWindow - 4) std::fprintf(stderr, " = %08x\n", ld32(a));
        else std::fprintf(stderr, " (out of range)\n");
        if (*p == ',') p++;
        else if (*p) break;
    }
}

static void install_sampler() {
    const char* e = std::getenv("FA_DUMP_AFTER");
    if (!e) return;
    int secs = std::atoi(e);
    struct sigaction sa{};
    sa.sa_handler = sample_handler;
    sa.sa_flags = SA_ONSTACK | SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);
    std::thread([secs] {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        for (int round = 0; round < 1; round++) {
            if (DIR* d = opendir("/proc/self/task")) {
                while (dirent* de = readdir(d)) {
                    long tid = std::atol(de->d_name);
                    if (tid > 0 && tid != ::syscall(SYS_gettid))
                    {
                        ::syscall(SYS_tgkill, ::getpid(), tid, SIGUSR2);
                        // one thread at a time so the dumps do not interleave
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    }
                }
                closedir(d);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        dump_peeks();
    }).detach();
}

void install_crash_handler() {
    install_sampler();
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);
}

}  // namespace fa::rt
