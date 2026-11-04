// Debug output, string helpers, process control.
#include "fa/rt/kernel.hpp"
#include "fa/rt/import.hpp"
#include "fa/rt/memory.hpp"
#include "fa/rt/crash.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

FA_IMPORT(XexCheckExecutablePrivilege) { ret32(ctx, 0); }
