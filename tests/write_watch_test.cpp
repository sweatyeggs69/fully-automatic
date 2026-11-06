// WriteWatch on a memfd mapped twice (like guest physical memory and its aliases): writes through
// either view are seen, unwritten ranges stay clean, protection survives mprotect, and the cost of
// a first write to a watched page is printed.
#include "fa/mem/write_watch.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

using fa::mem::WriteWatch;

static int failures = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } \
    } while (0)

int main() {
    const uint32_t size = 64u << 20;
    int fd = memfd_create("ww-test", MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, size) != 0) { std::printf("memfd failed\n"); return 1; }
    auto* a = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    // Like the runtime's 0xE0000000 view: mapped PROT_NONE and committed later, piece by piece.
    auto* b = static_cast<uint8_t*>(mmap(nullptr, size - 4096, PROT_NONE, MAP_SHARED, fd, 4096));
    if (a == MAP_FAILED || b == MAP_FAILED) { std::printf("mmap failed\n"); return 1; }
    std::memset(a, 1, 1u << 20);  // some populated pages, the rest unpopulated

    WriteWatch ww;
    const char* why = nullptr;
    if (!ww.Init({{a, 0, size}, {b, 4096, size - 4096}}, size, &why)) {
        std::printf("write_watch_test: SKIPPED, not supported here: %s\n", why);
        return 0;
    }
    ww.SetSkipCold(true);  // cover the opt-in quiet-memory rule too

    mprotect(b, 1u << 20, PROT_READ | PROT_WRITE);  // committed after Init, as allocations are

    // A watched range is clean until written.
    uint64_t t1 = ww.Watch(0x10000, 0x4000);
    CHECK(t1 != 0);
    ww.Harvest();
    CHECK(ww.Clean(0x10000, 0x4000, t1));
    // Written through the aliased view (physical 0x12000 is b + 0x11000).
    b[0x11000 + 5] = 7;
    ww.Harvest();
    CHECK(!ww.Clean(0x10000, 0x4000, t1));
    CHECK(ww.Clean(0x10000, 0x2000, t1));  // pages before the written one stay clean
    // A new copy after the write is clean again.
    uint64_t t2 = ww.Watch(0x10000, 0x4000);
    ww.Harvest();
    CHECK(ww.Clean(0x10000, 0x4000, t2));
    // Written through the first view, in an unpopulated page.
    uint64_t t3 = ww.Watch(0x800000, 0x1000);
    ww.Harvest();
    CHECK(ww.Clean(0x800000, 0x1000, t3));
    a[0x800010] = 1;
    ww.Harvest();
    CHECK(!ww.Clean(0x800000, 0x1000, t3));
    // A write since the last Harvest is seen right away on memory written recently (hot)...
    ww.Watch(0x60000, 0x2000);
    ww.Harvest();
    b[0x61000 - 4096 + 4] = 4;  // physical 0x61004: makes the page hot
    ww.Harvest();
    uint64_t t3b = ww.Watch(0x60000, 0x2000);
    ww.Harvest();
    CHECK(ww.Clean(0x60000, 0x2000, t3b));
    b[0x61000 - 4096 + 8] = 5;  // physical 0x61008, through the aliased view
    CHECK(!ww.Clean(0x60000, 0x2000, t3b));
    CHECK(ww.Clean(0x60000, 0x1000, t3b));
    // ...and at the next Harvest on memory that was quiet (cold): no kernel query for it.
    uint64_t t3e = ww.Watch(0x70000, 0x1000);
    ww.Harvest();
    b[0x70000 - 4096 + 8] = 5;
    CHECK(ww.Clean(0x70000, 0x1000, t3e));
    ww.Harvest();
    CHECK(!ww.Clean(0x70000, 0x1000, t3e));
    // A view never seen writing in a 2 MB region is not asked, even for hot memory: the first write
    // there through it is seen at the next Harvest (or right away with SetQueryAllViews).
    mprotect(b + 0x1400000 - 4096, 0x2000, PROT_READ | PROT_WRITE);
    uint64_t t3c = ww.Watch(0x1400000, 0x1000);  // region 10: no writes through either view so far
    ww.Harvest();
    a[0x1400000 + 4] = 4;  // hot, through view a only
    ww.Harvest();
    t3c = ww.Watch(0x1400000, 0x1000);
    ww.Harvest();
    b[0x1400000 - 4096 + 8] = 5;
    CHECK(ww.Clean(0x1400000, 0x1000, t3c));
    ww.SetQueryAllViews(true);
    CHECK(!ww.Clean(0x1400000, 0x1000, t3c));
    ww.SetQueryAllViews(false);
    ww.Harvest();
    CHECK(!ww.Clean(0x1400000, 0x1000, t3c));
    uint64_t t3d = ww.Watch(0x1400000, 0x1000);
    ww.Harvest();
    b[0x1400000 - 4096 + 9] = 6;  // view b is known to write here now: seen right away
    CHECK(!ww.Clean(0x1400000, 0x1000, t3d));
    // Unwatched memory is never clean.
    CHECK(!ww.Clean(0x900000, 0x1000, ww.Watch(0, 0)));
    // Touch marks host writes.
    uint64_t t4 = ww.Watch(0x20000, 0x1000);
    ww.Harvest();
    ww.Touch(0x20000, 16);
    CHECK(!ww.Clean(0x20000, 0x1000, t4));
    uint64_t t5 = ww.Watch(0x20000, 0x1000);
    CHECK(ww.Clean(0x20000, 0x1000, t5));
    // mprotect round trip (the runtime decommits with PROT_NONE) keeps the watch.
    uint64_t t6 = ww.Watch(0x30000, 0x1000);
    ww.Harvest();
    mprotect(a + 0x30000, 0x1000, PROT_NONE);
    mprotect(a + 0x30000, 0x1000, PROT_READ | PROT_WRITE);
    ww.Harvest();
    CHECK(ww.Clean(0x30000, 0x1000, t6));
    a[0x30000] = 9;
    ww.Harvest();
    CHECK(!ww.Clean(0x30000, 0x1000, t6));

    // Kernel writes (read() into guest memory, as file I/O does) are seen too.
    uint64_t t7 = ww.Watch(0x40000, 0x1000);
    ww.Harvest();
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    CHECK(write(pfd[1], "abcd", 4) == 4);
    CHECK(read(pfd[0], a + 0x40100, 4) == 4);
    close(pfd[0]);
    close(pfd[1]);
    ww.Harvest();
    CHECK(!ww.Clean(0x40000, 0x1000, t7));
    // Zeroing in place (the runtime's free while memory is pinned) counts as a write.
    uint64_t t8 = ww.Watch(0x50000, 0x2000);
    ww.Harvest();
    std::memset(a + 0x51000, 0, 0x1000);
    ww.Harvest();
    CHECK(ww.Clean(0x50000, 0x1000, t8));
    CHECK(!ww.Clean(0x51000, 0x1000, t8));

    // Cost: watch 16 MB, write one byte per page, harvest.
    const uint32_t big = 16u << 20, base = 32u << 20;
    std::memset(a + base, 2, big);  // populated, like live guest data
    ww.Watch(base, big);
    ww.Harvest();
    ww.TakeStats();
    auto t0 = std::chrono::steady_clock::now();
    for (uint32_t off = 0; off < big; off += 4096) a[base + off] = 3;
    auto t1w = std::chrono::steady_clock::now();
    ww.Harvest();
    auto t2h = std::chrono::steady_clock::now();
    auto st = ww.TakeStats();
    CHECK(st.pagesWritten == big / 4096);
    ww.Harvest();
    auto t3h = std::chrono::steady_clock::now();
    // Cost of Clean on a 64 KB range (one kernel query per view).
    auto tq0 = std::chrono::steady_clock::now();
    uint64_t tq = ww.Watch(base, big);
    ww.Harvest();
    int cleanCount = 0;
    for (uint32_t i = 0; i < 1000; i++) cleanCount += ww.Clean(base + (i % 256) * 65536, 65536, tq);
    CHECK(cleanCount == 1000);
    const double perClean = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tq0).count() / 1000.0;
    const auto qs = ww.TakeStats();
    std::printf("write_watch_test: Clean of 64 KB about %.2f us (including one harvest), %llu queries, %llu views skipped, %llu cold\n", perClean,
                (unsigned long long)qs.queryCalls, (unsigned long long)qs.querySkips, (unsigned long long)qs.queryCold);
    auto ms = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
    std::printf("write_watch_test: %u first writes %.2f ms (%.2f us each), harvest %.2f ms (%llu pages), idle harvest %.2f ms\n",
                big / 4096, ms(t0, t1w), ms(t0, t1w) * 1000.0 / (big / 4096), ms(t1w, t2h),
                (unsigned long long)st.pagesWritten, ms(t2h, t3h));

    if (failures) { std::printf("write_watch_test: %d failure(s)\n", failures); return 1; }
    std::printf("write_watch_test: ok\n");
    return 0;
}
