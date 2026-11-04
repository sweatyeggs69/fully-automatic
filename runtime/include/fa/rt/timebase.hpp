#pragma once
#include <cstddef>
#include <cstdint>

namespace fa::rt {

// Guest timebase frequency. The Xbox 360 timebase (mftb) ticks at 50 MHz, and the game also gets
// this value from KeQueryPerformanceFrequency.
constexpr uint64_t kGuestTimebaseHz = 50000000;

// XenonRecomp turns mftb into `__rdtsc()`, so the recompiled code reads the host TSC (about
// 3 to 5 GHz) while the game divides by 50 MHz: every timer ran 60 to 100 times too fast.
// install_guest_timebase() fixes that without recompiling ppc/: it finds the inlined
// `rdtsc; shl rdx,32` sequences in the executable's code and redirects each one to a small
// trampoline that returns the TSC rescaled to 50 MHz (counting from startup).
//
// Call once before any guest code runs. FA_TIMEBASE=off skips it; FA_TIMEBASE_HZ=<n> sets the
// guest frequency the TSC is scaled to (default 50000000, a bigger value runs the game faster).
void install_guest_timebase();

// Lower level parts, for tests.
// Measures the host TSC frequency and sets up the scale. Returns the TSC frequency in Hz.
uint64_t calibrate_guest_timebase(uint64_t guest_hz = kGuestTimebaseHz);
// Patches every rdtsc site in [begin, end). The memory must be writable and executable.
// Returns the number of patched sites; `skipped` gets the other 0F 31 byte pairs (mostly data
// or the middle of other instructions, but also any rdtsc in a shape it does not know).
size_t patch_rdtsc_sites(uint8_t* begin, uint8_t* end, size_t* skipped = nullptr);
// The current guest timebase value, as the patched code sees it.
uint64_t guest_timebase_now();

// Time since the runtime started, in 100 ns units. The one base for KeQueryInterruptTime and the
// InterruptTime/TickCount fields of KeTimeStampBundle, so the game never mixes two origins.
uint64_t guest_uptime_100ns();

}  // namespace fa::rt
