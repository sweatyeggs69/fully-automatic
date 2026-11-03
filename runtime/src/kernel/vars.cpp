// Kernel variable exports (data imports such as KeTimeStampBundle). The loader writes the
// address returned here into each variable's import slot.
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"
#include "fa/rt/vars.hpp"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <cstring>
#include <thread>

namespace fa::rt {

static uint32_t g_bundle = 0;

static void ticker() {
    using namespace std::chrono;
    auto start = steady_clock::now();
    constexpr uint64_t kEpochDelta = 116444736000000000ull;
    for (;;) {
        auto el = duration_cast<milliseconds>(steady_clock::now() - start).count();
        auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        st64(g_bundle + 0, uint64_t(el) * 10000);
        st64(g_bundle + 8, kEpochDelta + uint64_t(ns) / 100);
        st32(g_bundle + 0x10, uint32_t(el));
        std::this_thread::sleep_for(milliseconds(1));
    }
}

uint32_t kernel_variable(const std::string& name) {
    static std::map<std::string, uint32_t> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;

    uint32_t a = g_mem.malloc(0x200);
    if (name == "KeTimeStampBundle") {
        g_bundle = a;
        std::thread(ticker).detach();
    } else if (name == "XboxHardwareInfo") {
        st32(a + 0, 0x00000000);  // flags
        st8(a + 4, 6);            // processors
    } else if (name == "XboxKrnlVersion") {
        st16(a + 0, 2); st16(a + 2, 0); st16(a + 4, 17559); st16(a + 6, 0x80);
    } else if (name == "ExConsoleGameRegion") {
        st32(a, 0xFFFFFFFF);
    } else if (name == "ExLoadedImageName") {
        std::strcpy(reinterpret_cast<char*>(host(a)), "\\Device\\Cdrom0\\default.xex");
    } else if (name == "XexExecutableModuleHandle") {
        // Points at a module record; only the image base is filled in (set by the loader).
        uint32_t m = g_mem.malloc(0x200);
        st32(a, m);
    } else {
        std::fprintf(stderr, "note: kernel variable %s is a zeroed placeholder\n", name.c_str());
    }
    cache[name] = a;
    return a;
}

}  // namespace fa::rt
