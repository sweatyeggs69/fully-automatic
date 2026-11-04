// Debug output, string helpers, process control.
#include "fa/rt/kernel.hpp"
#include "fa/rt/import.hpp"
#include "fa/rt/memory.hpp"
#include "fa/rt/crash.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

using namespace fa::rt;

FA_IMPORT(DbgPrint) {
    std::fprintf(stderr, "[guest] %s\n", reinterpret_cast<const char*>(host(arg32(ctx, 0))));
}

FA_IMPORT(RtlInitAnsiString) {
    uint32_t d = arg32(ctx, 0), s = arg32(ctx, 1);
    uint16_t len = s ? uint16_t(std::strlen(reinterpret_cast<const char*>(host(s)))) : 0;
    st16(d, len);
    st16(d + 2, s ? len + 1 : 0);
    st32(d + 4, s);
}

FA_IMPORT(RtlInitUnicodeString) {
    uint32_t d = arg32(ctx, 0), s = arg32(ctx, 1);
    uint16_t n = 0;
    if (s) while (ld16(s + n * 2)) ++n;
    st16(d, n * 2);
    st16(d + 2, s ? n * 2 + 2 : 0);
    st32(d + 4, s);
}

FA_IMPORT(RtlCompareMemory) {
    uint32_t a = arg32(ctx, 0), b = arg32(ctx, 1), n = arg32(ctx, 2), i = 0;
    while (i < n && ld8(a + i) == ld8(b + i)) ++i;
    ret32(ctx, i);
}

FA_IMPORT(RtlFillMemoryUlong) {
    uint32_t d = arg32(ctx, 0), n = arg32(ctx, 1), v = arg32(ctx, 2);
    for (uint32_t i = 0; i + 4 <= n; i += 4) st32(d + i, v);
}

// MS_VC_EXCEPTION (0x406D1388) is how guests name their threads.
FA_IMPORT(RtlRaiseException) {
    uint32_t rec = arg32(ctx, 0);
    if (ld32(rec) == 0x406D1388 && g_kernel.current()) {
        uint32_t name = ld32(rec + 0x18);
        if (name) {
            g_kernel.current()->name = reinterpret_cast<const char*>(host(name));
            set_host_thread_name(g_kernel.current()->name.c_str());
        }
    }
}

[[noreturn]] static void die(const char* what, uint32_t code) {
    std::fprintf(stderr, "guest %s (0x%08X)\n", what, code);
    std::fflush(stderr);
    std::_Exit(1);
}
FA_IMPORT(KeBugCheck) { die("KeBugCheck", arg32(ctx, 0)); }
FA_IMPORT(KeBugCheckEx) { die("KeBugCheckEx", arg32(ctx, 0)); }

FA_IMPORT(HalReturnToFirmware) { std::fflush(stderr); std::_Exit(0); }
FA_IMPORT(XamLoaderTerminateTitle) { std::fflush(stderr); std::_Exit(0); }

// Launching another title (or relaunching this one, which is how the game leaves a race) means
// replacing the running process on a console. We cannot load another title, so log the request and
// end the process cleanly. FA_RELAUNCH=1 restarts this program with the same arguments instead.
// (path*, flags)
FA_IMPORT(XamLoaderLaunchTitle) {
    uint32_t path = arg32(ctx, 0);
    std::fprintf(stderr, "XamLoaderLaunchTitle(\"%s\", flags=%08X): ending the process\n",
                 path ? reinterpret_cast<const char*>(host(path)) : "", arg32(ctx, 1));
    std::fflush(stderr);
    if (std::getenv("FA_RELAUNCH")) {
        std::string cmd;
        if (FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) cmd.append(buf, n);
            std::fclose(f);
        }
        std::vector<char*> argv;
        for (size_t i = 0; i < cmd.size(); i += std::strlen(cmd.c_str() + i) + 1) argv.push_back(&cmd[i]);
        argv.push_back(nullptr);
        if (argv.size() > 1) execv("/proc/self/exe", argv.data());
        std::fprintf(stderr, "XamLoaderLaunchTitle: relaunch failed, exiting\n");
    }
    std::_Exit(0);
}

FA_IMPORT(XexCheckExecutablePrivilege) { ret32(ctx, 0); }

// ---- code page conversion (the game only uses ASCII/Latin-1 text) -----------------------

// (wchar_t* out, max_bytes, ULONG* bytes_written, const char* in, in_bytes)
FA_IMPORT(RtlMultiByteToUnicodeN) {
    uint32_t out = arg32(ctx, 0), max_bytes = arg32(ctx, 1), written = arg32(ctx, 2);
    uint32_t in = arg32(ctx, 3), in_bytes = arg32(ctx, 4);
    uint32_t n = in_bytes < max_bytes / 2 ? in_bytes : max_bytes / 2;
    for (uint32_t i = 0; i < n; ++i) st16(out + i * 2, ld8(in + i));
    if (written) st32(written, n * 2);
    ret32(ctx, STATUS_SUCCESS);
}

// (ULONG* bytes_needed, const char* in, in_bytes)
FA_IMPORT(RtlMultiByteToUnicodeSize) {
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), arg32(ctx, 2) * 2);
    ret32(ctx, STATUS_SUCCESS);
}

// (char* out, max_bytes, ULONG* bytes_written, const wchar_t* in, in_bytes); non-Latin-1 becomes '?'
FA_IMPORT(RtlUnicodeToMultiByteN) {
    uint32_t out = arg32(ctx, 0), max_bytes = arg32(ctx, 1), written = arg32(ctx, 2);
    uint32_t in = arg32(ctx, 3), in_bytes = arg32(ctx, 4);
    uint32_t n = in_bytes / 2 < max_bytes ? in_bytes / 2 : max_bytes;
    for (uint32_t i = 0; i < n; ++i) {
        uint16_t c = ld16(in + i * 2);
        st8(out + i, c < 0x100 ? uint8_t(c) : '?');
    }
    if (written) st32(written, n);
    ret32(ctx, STATUS_SUCCESS);
}

// (ULONG* bytes_needed, const wchar_t* in, in_bytes)
FA_IMPORT(RtlUnicodeToMultiByteSize) {
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), arg32(ctx, 2) / 2);
    ret32(ctx, STATUS_SUCCESS);
}
