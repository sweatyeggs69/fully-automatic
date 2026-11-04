// Kernel variable exports (data imports such as KeTimeStampBundle). The loader writes the
// address returned here into each variable's import slot.
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"
#include "fa/rt/vars.hpp"

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <cstring>
#include <thread>

namespace fa::rt {

static uint32_t g_bundle = 0;

// One epoch for every interrupt-time reader (KeTimeStampBundle +0 and KeQueryInterruptTime), so the game
// never sees two different bases for the same quantity. First use sets it.
uint64_t interrupt_time_100ns() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return uint64_t(duration_cast<nanoseconds>(steady_clock::now() - start).count() / 100);
}

static void ticker() {
    using namespace std::chrono;
    interrupt_time_100ns();  // fix the epoch before the first bundle write
    constexpr uint64_t kEpochDelta = 116444736000000000ull;
    for (;;) {
        const uint64_t now100 = interrupt_time_100ns();
        auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        st64(g_bundle + 0, now100);
        st64(g_bundle + 8, kEpochDelta + uint64_t(ns) / 100);
        st32(g_bundle + 0x10, uint32_t(now100 / 10000));
        std::this_thread::sleep_for(milliseconds(1));
    }
}

bool is_kernel_variable(const std::string& name) {
    static const std::set<std::string> vars = {
        "ExConsoleGameRegion", "ExEventObjectType", "ExMutantObjectType", "ExSemaphoreObjectType",
        "ExThreadObjectType", "ExTimerObjectType", "IoCompletionObjectType", "IoDeviceObjectType",
        "IoFileObjectType", "KeDebugMonitorData", "KeTimeStampBundle", "KiBugCheckData",
        "Nls844UnicodeCaseTable", "ObDirectoryObjectType", "ObSymbolicLinkObjectType",
        "XboxHardwareInfo", "XboxKrnlBaseVersion", "XboxKrnlVersion", "XexExecutableModuleHandle",
        "ExLoadedCommandLine", "ExLoadedImageName", "VdGlobalDevice", "VdGlobalXamDevice",
        "VdGpuClockInMHz", "VdHSIOCalibrationLock", "HalFsbInterruptCount", "KeCertMonitorData",
        "VdHSIOTrainCount", "VdHSIOTrainingStatus", "RgcBindInfo", "HalFsbResetCount",
        "UsbdBootEnumerationDoneEvent", "StfsDeviceErrorEvent", "UsbdDriverLoadRequiredEvent",
        "XboxKrnlVersion4Digit", "g_XuiAutomation", "XamSwapDiscPatchAddress"};
    return vars.count(name) != 0;
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
