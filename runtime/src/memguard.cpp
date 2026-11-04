// Memory guard: a watchdog thread that ends the process before the whole machine runs out of
// memory. A runaway GPU or guest allocation (GPU memory on AMD spills into system RAM) can freeze
// the desktop; exiting early with a message is far better than a hard hang.
//   FA_MEMGUARD=0          turn it off
//   FA_MIN_AVAIL_MB=N      exit when the system's MemAvailable falls below N MB (default 1536)
//   FA_MAX_RSS_MB=N        exit when this process's resident set exceeds N MB (default: off)
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "fa/rt/crash.hpp"

namespace fa::rt {
namespace {
uint64_t env_mb(const char* name, uint64_t def) {
    const char* e = std::getenv(name);
    return e && *e ? std::strtoull(e, nullptr, 10) : def;
}
uint64_t mem_available_kb() {
    FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return ~0ull;
    char line[256];
    uint64_t v = ~0ull;
    while (std::fgets(line, sizeof line, f))
        if (!std::strncmp(line, "MemAvailable:", 13)) { v = std::strtoull(line + 13, nullptr, 10); break; }
    std::fclose(f);
    return v;
}
uint64_t rss_kb() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long total = 0, res = 0;
    if (std::fscanf(f, "%lu %lu", &total, &res) != 2) res = 0;
    std::fclose(f);
    return uint64_t(res) * uint64_t(sysconf(_SC_PAGESIZE)) / 1024;
}
}  // namespace

void install_memory_guard() {
    const char* off = std::getenv("FA_MEMGUARD");
    if (off && off[0] == '0') return;
    const uint64_t minAvail = env_mb("FA_MIN_AVAIL_MB", 1536) * 1024;
    const uint64_t maxRss = env_mb("FA_MAX_RSS_MB", 0) * 1024;
    std::thread([=] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const uint64_t avail = mem_available_kb(), rss = rss_kb();
            if ((minAvail && avail < minAvail) || (maxRss && rss > maxRss)) {
                char msg[256];
                int n = std::snprintf(msg, sizeof msg,
                                      "\n[memguard] stopping: system MemAvailable %llu MB (limit %llu MB), process RSS %llu MB "
                                      "(limit %llu MB)\n",
                                      (unsigned long long)(avail / 1024), (unsigned long long)(minAvail / 1024),
                                      (unsigned long long)(rss / 1024), (unsigned long long)(maxRss / 1024));
                if (n > 0 && write(2, msg, size_t(n)) < 0) {}
                _exit(86);
            }
        }
    }).detach();
}
}  // namespace fa::rt
