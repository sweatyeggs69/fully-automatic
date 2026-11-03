// Threads, TLS, delays and time.
#include "fa/rt/kernel.hpp"
#include "fa/rt/import.hpp"
#include "fa/rt/memory.hpp"

#include <chrono>
#include <thread>

using namespace fa::rt;

FA_IMPORT(ExCreateThread) {
    // (handle_ptr, stack_size, thread_id_ptr, xapi_thread_startup, start_address, start_context, flags)
    bool suspended = (arg32(ctx, 6) & 1) != 0;
    auto t = g_kernel.create_thread(arg32(ctx, 1), arg32(ctx, 3), arg32(ctx, 4), arg32(ctx, 5), suspended);
    if (!t) { ret32(ctx, STATUS_NO_MEMORY); return; }
    uint32_t h = g_kernel.add_handle(t);
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), h);
    if (arg32(ctx, 2)) st32(arg32(ctx, 2), t->tid);
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(ExTerminateThread) { throw ThreadExit{arg32(ctx, 0)}; }

static std::shared_ptr<KThread> thread_of(uint32_t h) {
    auto o = g_kernel.get_handle(h);
    return (o && o->type == KObject::Type::Thread) ? std::static_pointer_cast<KThread>(o) : nullptr;
}

FA_IMPORT(NtResumeThread) {
    auto t = thread_of(arg32(ctx, 0));
    if (!t) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t prev = g_kernel.resume(*t);
    if (arg32(ctx, 1)) st32(arg32(ctx, 1), prev);
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(NtSuspendThread) {
    auto t = thread_of(arg32(ctx, 0));
    if (!t) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t prev = g_kernel.suspend(*t);
    if (arg32(ctx, 1)) st32(arg32(ctx, 1), prev);
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(KeResumeThread) {
    auto o = g_kernel.get_dispatch(arg32(ctx, 0));
    if (!o || o->type != KObject::Type::Thread) { ret32(ctx, 0); return; }
    ret32(ctx, g_kernel.resume(static_cast<KThread&>(*o)));
}

FA_IMPORT(NtYieldExecution) { std::this_thread::yield(); }
FA_IMPORT(KeSetAffinityThread) { ret32(ctx, 0x3F); }
FA_IMPORT(KeSetBasePriorityThread) { ret32(ctx, 0); }
FA_IMPORT(KeQueryBasePriorityThread) { ret32(ctx, 0); }
FA_IMPORT(KeEnableFpuExceptions) { }

FA_IMPORT(KeDelayExecutionThread) {
    // (processor_mode, alertable, interval_ptr)
    uint32_t p = arg32(ctx, 2);
    if (p) {
        int64_t t = int64_t(ld64(p));
        if (t < 0) std::this_thread::sleep_for(std::chrono::nanoseconds(-t * 100));
        else std::this_thread::yield();
    }
    ret32(ctx, STATUS_SUCCESS);
}

// ---- TLS -------------------------------------------------------------------------------------

FA_IMPORT(KeTlsAlloc) { ret32(ctx, g_kernel.tls_alloc()); }
FA_IMPORT(KeTlsFree) { ret32(ctx, g_kernel.tls_free(arg32(ctx, 0)) ? 1 : 0); }
FA_IMPORT(KeTlsGetValue) { ret32(ctx, g_kernel.tls_get(arg32(ctx, 0))); }
FA_IMPORT(KeTlsSetValue) { ret32(ctx, g_kernel.tls_set(arg32(ctx, 0), arg32(ctx, 1)) ? 1 : 0); }

// ---- time ------------------------------------------------------------------------------------

namespace {
uint64_t filetime_now() {
    using namespace std::chrono;
    // 100ns ticks between 1601-01-01 and 1970-01-01.
    constexpr uint64_t kEpochDelta = 116444736000000000ull;
    auto ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    return kEpochDelta + uint64_t(ns) / 100;
}
}  // namespace

FA_IMPORT(KeQuerySystemTime) { st64(arg32(ctx, 0), filetime_now()); }
FA_IMPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }
FA_IMPORT(KeQueryInterruptTime) {
    using namespace std::chrono;
    ctx.r3.u64 = uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
}
