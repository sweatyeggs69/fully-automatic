#pragma once
#include <cstdint>
namespace fa::rt {
constexpr uint64_t kGuestWindow = 0x100000000ull;
// Installs handlers that print the fault address and a backtrace. Call first thing in main.
void install_crash_handler();
// Debug aid: FA_FPE=1 traps invalid FP operations (NaN creation) and logs the instruction. Call on each guest thread.
void fpe_enable_this_thread();
// Debug aid: FA_WATCH=<hexaddr>:<hexlen> logs every guest write to that range (call after g_mem.init).
void install_write_watch();
// Debug aid: FA_PROFILE=<seconds> prints thread CPU, hot functions and import counts periodically.
void install_profiler();
// Names the calling host thread (shows in /proc, perf, gdb and the FA_PROFILE report).
void set_host_thread_name(const char* name);
}
