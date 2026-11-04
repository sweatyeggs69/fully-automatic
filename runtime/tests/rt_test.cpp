// Runtime unit test: runs hand-written "recompiled" guest functions against the real kernel
// layer (memory, threads, events, semaphores, critical sections, TLS, heaps).
#include "fa/rt/kernel.hpp"
#include "fa/rt/loader.hpp"
#include "fa/rt/memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace fa::rt;

PPC_EXTERN_FUNC(NtCreateEvent);
PPC_EXTERN_FUNC(NtSetEvent);
PPC_EXTERN_FUNC(NtWaitForSingleObjectEx);
PPC_EXTERN_FUNC(NtClose);
PPC_EXTERN_FUNC(ExCreateThread);
PPC_EXTERN_FUNC(NtCreateSemaphore);
PPC_EXTERN_FUNC(NtReleaseSemaphore);
PPC_EXTERN_FUNC(RtlInitializeCriticalSection);
PPC_EXTERN_FUNC(RtlEnterCriticalSection);
PPC_EXTERN_FUNC(RtlLeaveCriticalSection);
PPC_EXTERN_FUNC(RtlTryEnterCriticalSection);
PPC_EXTERN_FUNC(KeTlsAlloc);
PPC_EXTERN_FUNC(KeTlsSetValue);
PPC_EXTERN_FUNC(KeTlsGetValue);
PPC_EXTERN_FUNC(ExAllocatePoolTypeWithTag);
PPC_EXTERN_FUNC(ExFreePool);
PPC_EXTERN_FUNC(MmAllocatePhysicalMemoryEx);
PPC_EXTERN_FUNC(MmGetPhysicalAddress);
PPC_EXTERN_FUNC(NtAllocateVirtualMemory);
PPC_EXTERN_FUNC(NtFreeVirtualMemory);
PPC_EXTERN_FUNC(KeInitializeEvent);
PPC_EXTERN_FUNC(KeSetEvent);
PPC_EXTERN_FUNC(KeWaitForSingleObject);

static int g_fail = 0;
#define CHECK(c) do { if (getenv("T")) std::printf("line %d\n", __LINE__);  if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

template <class... A>
static uint32_t K(PPCContext& ctx, PPCFunc* f, A... a) {
    uint32_t v[] = {uint32_t(a)..., 0};
    PPCRegister* r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (size_t i = 0; i < sizeof...(a); ++i) r[i]->u64 = v[i];
    f(ctx, g_base);
    return ctx.r3.u32;
}

// Worker thread: r3 = scratch block. [0] = event handle, [8] = result word, [12] = TLS check.
static void worker(PPCContext& ctx, uint8_t*) {
    uint32_t scratch = ctx.r3.u32;
    uint32_t idx = K(ctx, KeTlsAlloc);
    K(ctx, KeTlsSetValue, idx, 0x1234);
    st32(scratch + 12, K(ctx, KeTlsGetValue, idx));
    st32(scratch + 8, 42);
    K(ctx, NtSetEvent, ld32(scratch), 0);
}

static void guest_main(PPCContext& ctx, uint8_t*) {
    uint32_t s = K(ctx, ExAllocatePoolTypeWithTag, 0x100, 0, 0);
    CHECK(s != 0);
    CHECK(host(s)[0] == 0);

    // Event + thread + cross-thread signalling + TLS isolation.
    CHECK(K(ctx, NtCreateEvent, s, 0, 1, 0) == 0);
    uint32_t ev = ld32(s);
    st32(s + 8, 0);
    uint32_t th = s + 0x20, tid = s + 0x24;
    CHECK(K(ctx, ExCreateThread, th, 0x10000, tid, 0, 0x82000100, s, 0) == 0);
    CHECK(K(ctx, NtWaitForSingleObjectEx, ev, 0, 0, 0) == 0);
    CHECK(ld32(s + 8) == 42);
    CHECK(ld32(s + 12) == 0x1234);
    uint32_t idx = K(ctx, KeTlsAlloc);
    CHECK(K(ctx, KeTlsGetValue, idx) == 0);  // main thread's slot untouched by the worker

    // Timeout on an unsignalled auto-reset event.
    uint32_t to = s + 0x30;
    st64(to, uint64_t(-100000));  // 10 ms relative
    CHECK(K(ctx, NtCreateEvent, s + 0x28, 0, 1, 0) == 0);
    CHECK(K(ctx, NtWaitForSingleObjectEx, ld32(s + 0x28), 0, 0, to) == 0x102);

    // Auto-reset event is consumed by one wait.
    K(ctx, NtSetEvent, ld32(s + 0x28), 0);
    CHECK(K(ctx, NtWaitForSingleObjectEx, ld32(s + 0x28), 0, 0, to) == 0);
    CHECK(K(ctx, NtWaitForSingleObjectEx, ld32(s + 0x28), 0, 0, to) == 0x102);

    // Dispatcher event living in guest memory.
    uint32_t ke = s + 0x40;
    K(ctx, KeInitializeEvent, ke, 0, 1);
    CHECK(K(ctx, KeWaitForSingleObject, ke, 0, 0, 0, to) == 0);

    // Semaphore.
    CHECK(K(ctx, NtCreateSemaphore, s + 0x60, 0, 1, 2) == 0);
    uint32_t sem = ld32(s + 0x60);
    CHECK(K(ctx, NtWaitForSingleObjectEx, sem, 0, 0, to) == 0);
    CHECK(K(ctx, NtWaitForSingleObjectEx, sem, 0, 0, to) == 0x102);
    CHECK(K(ctx, NtReleaseSemaphore, sem, 2, 0) == 0);
    CHECK(K(ctx, NtReleaseSemaphore, sem, 1, 0) == STATUS_SEMAPHORE_LIMIT_EXCEEDED);

    // Recursive critical section.
    uint32_t cs = s + 0x80;
    K(ctx, RtlInitializeCriticalSection, cs);
    K(ctx, RtlEnterCriticalSection, cs);
    K(ctx, RtlEnterCriticalSection, cs);
    K(ctx, RtlLeaveCriticalSection, cs);
    K(ctx, RtlLeaveCriticalSection, cs);
    CHECK(ld32(cs + 0x18) == 0);

    // Contended critical section: four host threads bump one unprotected counter.
    {
        uint32_t counter = s + 0xA0;
        st32(counter, 0);
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i)
            ts.emplace_back([cs, counter, i] {
                attach_guest_thread("cs" + std::to_string(i));
                PPCContext c{};
                for (int k = 0; k < 20000; ++k) {
                    K(c, RtlEnterCriticalSection, cs);
                    st32(counter, ld32(counter) + 1);
                    if (k % 1000 == 0) std::this_thread::yield();  // hold it a while sometimes
                    K(c, RtlLeaveCriticalSection, cs);
                }
            });
        for (auto& t : ts) t.join();
        CHECK(ld32(counter) == 80000);
        CHECK(ld32(cs + 0x18) == 0);
        CHECK(K(ctx, RtlTryEnterCriticalSection, cs) == 1);
        K(ctx, RtlLeaveCriticalSection, cs);
    }

    // The committed-page map follows allocations and physical aliases.
    {
        uint32_t ph = K(ctx, MmAllocatePhysicalMemoryEx, 0, 0x10000, 4, 0, 0xFFFFFFFF, 0x10000);
        CHECK(g_mem.is_committed(ph) && g_mem.is_committed(ph + 0xFFFF));
        CHECK(g_mem.is_committed(0xC0000000u + (ph & 0x1FFFFFFF)));
        CHECK(!g_mem.is_committed(0x50000000u));
    }

    // Memory: pool, physical, virtual.
    K(ctx, ExFreePool, s);
    uint32_t p = K(ctx, MmAllocatePhysicalMemoryEx, 0, 0x20000, 4, 0, 0xFFFFFFFF, 0x10000);
    CHECK(p >= 0xA0000000);
    CHECK(K(ctx, MmGetPhysicalAddress, p) == (p & 0x1FFFFFFF));
    st32(p, 0xDEADBEEF);
    CHECK(ld32(p) == 0xDEADBEEF);
    uint32_t v = K(ctx, ExAllocatePoolTypeWithTag, 0x1000, 0, 0);
    uint32_t sz = v + 4;
    st32(v, 0);
    st32(sz, 0x30000);
    CHECK(K(ctx, NtAllocateVirtualMemory, v, sz, 0x3000, 4, 0) == 0);
    uint32_t va = ld32(v);
    CHECK(va != 0 && ld32(va) == 0);
    st32(va + 0x2FFFC, 7);
    CHECK(K(ctx, NtFreeVirtualMemory, v, sz, 0x8000, 0) == 0);
    ctx.r3.u64 = g_fail;
}

PPCFuncMapping PPCFuncMappings[] = {
    {0x82000100, worker},
    {0x82000200, guest_main},
    {0, nullptr},
};

int main() {
    CHECK(g_mem.init());
    std::vector<uint8_t> image(0x10000, 0);
    CHECK(map_image(image.data(), 0x82000000, 0x10000));
    CHECK(lookup_guest(0x82000200) == guest_main);
    uint32_t rc = g_kernel.run_main_thread(0x82000200, 0x40000);
    g_kernel.join_all();
    CHECK(rc == 0);
    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
