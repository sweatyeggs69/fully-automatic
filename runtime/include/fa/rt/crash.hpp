#pragma once
#include <cstdint>
namespace fa::rt {
constexpr uint64_t kGuestWindow = 0x100000000ull;
// Installs handlers that print the fault address and a backtrace. Call first thing in main.
void install_crash_handler();
// Debug aid: FA_WATCH=<hexaddr>:<hexlen> logs every guest write to that range (call after g_mem.init).
void install_write_watch();
}
