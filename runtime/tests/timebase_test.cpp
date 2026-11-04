// Patches compiler-generated __rdtsc() calls (the shape XenonRecomp emits for mftb) and checks
// that they then tick at the 50 MHz guest timebase.
#include "fa/rt/timebase.hpp"

#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace fa::rt;

struct Ctx { uint64_t r[8]; };

// Kept in their own section so the test patches only them.
#define PROBE __attribute__((noinline, section("fa_tb_probe"), used))
extern "C" PROBE uint64_t probe_ret() { return __rdtsc(); }
extern "C" PROBE void probe_store(Ctx& c) { c.r[3] = __rdtsc(); }
extern "C" PROBE void probe_two(Ctx& c) {
    c.r[3] = __rdtsc();
    if (c.r[3] > c.r[4]) c.r[5] = 1;
    c.r[6] = __rdtsc() - c.r[7];
}
extern "C" uint8_t __start_fa_tb_probe[], __stop_fa_tb_probe[];

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    uint64_t tsc_hz = calibrate_guest_timebase();
    std::printf("host TSC %.1f MHz\n", double(tsc_hz) / 1e6);
    CHECK(tsc_hz > 100000000);

    long page = sysconf(_SC_PAGESIZE);
    auto* pb = reinterpret_cast<uint8_t*>(uintptr_t(__start_fa_tb_probe) & ~uintptr_t(page - 1));
    size_t len = size_t(__stop_fa_tb_probe - pb);
    CHECK(mprotect(pb, len, PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
    size_t skipped = 0;
    size_t n = patch_rdtsc_sites(__start_fa_tb_probe, __stop_fa_tb_probe, &skipped);
    mprotect(pb, len, PROT_READ | PROT_EXEC);
    std::printf("patched %zu sites, %zu other\n", n, skipped);
    CHECK(n == 4);
    CHECK(skipped == 0);

    Ctx c{};
    uint64_t a = probe_ret();
    probe_store(c);
    uint64_t b = c.r[3];
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t z = probe_ret();
    c.r[4] = ~0ull;
    c.r[7] = 0;
    probe_two(c);
    uint64_t now = guest_timebase_now();
    double secs = double(z - a) / double(kGuestTimebaseHz);
    std::printf("a=%llu b=%llu z=%llu two=%llu,%llu now=%llu  -> %.3f s over a 0.2 s sleep\n",
                (unsigned long long)a, (unsigned long long)b, (unsigned long long)z,
                (unsigned long long)c.r[3], (unsigned long long)c.r[6], (unsigned long long)now, secs);
    CHECK(b >= a && z >= b);
    CHECK(secs > 0.19 && secs < 0.3);
    CHECK(c.r[3] >= z && c.r[6] >= c.r[3] && c.r[5] == 0);
    CHECK(now >= c.r[6] && now - c.r[6] < kGuestTimebaseHz / 100);
    std::printf(g_fail ? "%d failures\n" : "ok\n", g_fail);
    return g_fail ? 1 : 0;
}
