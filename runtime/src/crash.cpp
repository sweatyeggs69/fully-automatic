// Crash reporting: on SIGSEGV and friends print the fault address (and the guest address
// when it lies inside the guest window) plus a native backtrace. Recompiled functions keep
// their sub_XXXXXXXX names (the fullauto binary is linked with -rdynamic), so the backtrace
// names the guest functions directly.
#include "fa/rt/crash.hpp"
#include "fa/rt/guest.hpp"

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

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

static void handler(int sig, siginfo_t* info, void*) {
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
    _exit(128 + sig);
}

void install_crash_handler() {
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);
}

}  // namespace fa::rt
