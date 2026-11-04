// Events, semaphores, mutants, critical sections, spin locks, waits and handles.
#include "fa/rt/kernel.hpp"
#include "fa/rt/import.hpp"
#include "fa/rt/memory.hpp"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

using namespace fa::rt;

namespace {

std::shared_ptr<KObject> by_handle(uint32_t h) { return g_kernel.get_handle(h); }

template <class T>
std::shared_ptr<T> as(std::shared_ptr<KObject> o, KObject::Type t) {
    if (!o || o->type != t) return nullptr;
    return std::static_pointer_cast<T>(o);
}

// Wait on a dispatcher object pointer, creating an event wrapper on first sight if the
// guest passed a pointer we have not seen (it initialised the header itself).
std::shared_ptr<KObject> by_ptr(uint32_t p) {
    auto o = g_kernel.get_dispatch(p);
    if (o || p < 0x10000) return o;
    // The guest initialised the header itself: DISPATCHER_HEADER type at +0, signal state at +4.
    uint8_t type = ld8(p);
    std::shared_ptr<KObject> n;
    if (type == 0 || type == 1) n = std::make_shared<KEvent>(type == 0, ld32(p + 4) != 0);
    else if (type == 5) n = std::make_shared<KSemaphore>(int32_t(ld32(p + 4)), int32_t(ld32(p + 0x10)));
    else if (type == 2) n = std::make_shared<KMutant>();
    if (!n) {
        std::fprintf(stderr, "warning: dispatcher object 0x%08X has unsupported header type %u\n", p, type);
        return nullptr;
    }
    g_kernel.register_dispatch(p, n);
    return g_kernel.get_dispatch(p);
}

uint32_t set_event(KEvent& e) {
    std::lock_guard<std::mutex> lk(g_kmu);
    bool prev = e.set;
    e.set = true;
    g_kcv.notify_all();
    return prev;
}
uint32_t reset_event(KEvent& e) {
    std::lock_guard<std::mutex> lk(g_kmu);
    bool prev = e.set;
    e.set = false;
    return prev;
}
uint32_t pulse_event(KEvent& e) {
    bool prev;
    {
        std::lock_guard<std::mutex> lk(g_kmu);
        prev = e.set;
        e.set = true;
        g_kcv.notify_all();
    }
    std::this_thread::yield();
    std::lock_guard<std::mutex> lk(g_kmu);
    e.set = false;
    return prev;
}
uint32_t release_sem(KSemaphore& s, int32_t n, bool& overflow) {
    std::lock_guard<std::mutex> lk(g_kmu);
    int32_t prev = s.count;
    overflow = int64_t(s.count) + n > s.limit;
    if (!overflow) s.count += n;
    g_kcv.notify_all();
    return uint32_t(prev);
}

}  // namespace

// ---- handles -------------------------------------------------------------------------

FA_IMPORT(NtClose) {
    ret32(ctx, g_kernel.close_handle(arg32(ctx, 0)) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
}

FA_IMPORT(NtDuplicateObject) {
    // (handle, new_handle_ptr, options)
    auto o = by_handle(arg32(ctx, 0));
    if (!o) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t h = g_kernel.add_handle(o);
    // add_handle overwrites o->handle; keep the original for the first handle's owner.
    if (arg32(ctx, 1)) st32(arg32(ctx, 1), h);
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(ObReferenceObjectByHandle) {
    // (handle, object_type_ptr, out_object_ptr)
    auto o = by_handle(arg32(ctx, 0));
    if (!o) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    if (!o->guest_ptr) {
        bool is_thread = o->type == KObject::Type::Thread;
        o->guest_ptr = g_mem.malloc(is_thread ? 0x400 : 0x30);
        g_kernel.register_dispatch(o->guest_ptr, o);
        if (is_thread) {
            std::lock_guard<std::mutex> lk(g_kmu);
            sync_thread_header(*std::static_pointer_cast<KThread>(o));
        }
    }
    if (arg32(ctx, 2)) st32(arg32(ctx, 2), o->guest_ptr);
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(ObDereferenceObject) { }

// ---- events --------------------------------------------------------------------------

FA_IMPORT(KeInitializeEvent) {
    // (event_ptr, type, state)  type 0 = notification (manual reset), 1 = synchronization
    uint32_t p = arg32(ctx, 0);
    st8(p, uint8_t(arg32(ctx, 1)));
    st32(p + 4, arg32(ctx, 2));
    g_kernel.register_dispatch(p, std::make_shared<KEvent>(arg32(ctx, 1) == 0, arg32(ctx, 2) != 0));
}

FA_IMPORT(KeSetEvent) {
    auto e = as<KEvent>(by_ptr(arg32(ctx, 0)), KObject::Type::Event);
    ret32(ctx, e ? set_event(*e) : 0);
}
FA_IMPORT(KeResetEvent) {
    auto e = as<KEvent>(by_ptr(arg32(ctx, 0)), KObject::Type::Event);
    ret32(ctx, e ? reset_event(*e) : 0);
}
FA_IMPORT(KePulseEvent) {
    auto e = as<KEvent>(by_ptr(arg32(ctx, 0)), KObject::Type::Event);
    ret32(ctx, e ? pulse_event(*e) : 0);
}

FA_IMPORT(NtCreateEvent) {
    // (handle_ptr, object_attributes_ptr, event_type, initial_state)
    auto e = std::make_shared<KEvent>(arg32(ctx, 2) == 0, arg32(ctx, 3) != 0);
    uint32_t h = g_kernel.add_handle(e);
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), h);
    ret32(ctx, STATUS_SUCCESS);
}

static void nt_event_op(PPCContext& ctx, uint32_t (*op)(KEvent&), bool has_prev = true) {
    auto e = as<KEvent>(by_handle(arg32(ctx, 0)), KObject::Type::Event);
    if (!e) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t prev = op(*e);
    if (has_prev && arg32(ctx, 1)) st32(arg32(ctx, 1), prev);
    ret32(ctx, STATUS_SUCCESS);
}
FA_IMPORT(NtSetEvent) { nt_event_op(ctx, set_event); }
FA_IMPORT(NtClearEvent) { nt_event_op(ctx, reset_event, false); }  // (handle) only
FA_IMPORT(NtResetEvent) { nt_event_op(ctx, reset_event); }
FA_IMPORT(NtPulseEvent) { nt_event_op(ctx, pulse_event); }

// ---- semaphores ------------------------------------------------------------------------

FA_IMPORT(KeInitializeSemaphore) {
    // (sem_ptr, count, limit)
    g_kernel.register_dispatch(arg32(ctx, 0),
        std::make_shared<KSemaphore>(int32_t(arg32(ctx, 1)), int32_t(arg32(ctx, 2))));
}

FA_IMPORT(KeReleaseSemaphore) {
    // (sem_ptr, increment, adjustment, wait)
    auto s = as<KSemaphore>(by_ptr(arg32(ctx, 0)), KObject::Type::Semaphore);
    if (!s) { ret32(ctx, 0); return; }
    bool overflow;
    ret32(ctx, release_sem(*s, int32_t(arg32(ctx, 2)), overflow));
}

FA_IMPORT(NtCreateSemaphore) {
    // (handle_ptr, object_attributes_ptr, count, limit)
    auto s = std::make_shared<KSemaphore>(int32_t(arg32(ctx, 2)), int32_t(arg32(ctx, 3)));
    uint32_t h = g_kernel.add_handle(s);
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), h);
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(NtReleaseSemaphore) {
    // (handle, release_count, previous_count_ptr)
    auto s = as<KSemaphore>(by_handle(arg32(ctx, 0)), KObject::Type::Semaphore);
    if (!s) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    bool overflow;
    uint32_t prev = release_sem(*s, int32_t(arg32(ctx, 1)), overflow);
    if (arg32(ctx, 2)) st32(arg32(ctx, 2), prev);
    ret32(ctx, overflow ? STATUS_SEMAPHORE_LIMIT_EXCEEDED : STATUS_SUCCESS);
}

// ---- mutants -----------------------------------------------------------------------------

FA_IMPORT(NtCreateMutant) {
    // (handle_ptr, object_attributes_ptr, initial_owner)
    auto m = std::make_shared<KMutant>();
    if (arg32(ctx, 2) && g_kernel.current()) {
        m->owner = g_kernel.current();
        m->recursion = 1;
    }
    uint32_t h = g_kernel.add_handle(m);
    if (arg32(ctx, 0)) st32(arg32(ctx, 0), h);
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(NtReleaseMutant) {
    auto m = as<KMutant>(by_handle(arg32(ctx, 0)), KObject::Type::Mutant);
    if (!m) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    std::lock_guard<std::mutex> lk(g_kmu);
    if (m->owner != g_kernel.current()) { ret32(ctx, STATUS_MUTANT_NOT_OWNED); return; }
    if (--m->recursion == 0) m->owner = nullptr;
    g_kcv.notify_all();
    ret32(ctx, STATUS_SUCCESS);
}

// ---- waits ---------------------------------------------------------------------------------

FA_IMPORT(KeWaitForSingleObject) {
    // (object_ptr, wait_reason, processor_mode, alertable, timeout_ptr)
    auto o = by_ptr(arg32(ctx, 0));
    if (!o) { ret32(ctx, STATUS_INVALID_PARAMETER); return; }
    ret32(ctx, g_kernel.wait({o}, false, arg32(ctx, 4)));
}

FA_IMPORT(NtWaitForSingleObjectEx) {
    // (handle, wait_mode, alertable, timeout_ptr)
    auto o = by_handle(arg32(ctx, 0));
    if (!o) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    ret32(ctx, g_kernel.wait({o}, false, arg32(ctx, 3)));
}

FA_IMPORT(KeWaitForMultipleObjects) {
    // (count, objects_ptr, wait_type, wait_reason, wait_mode, alertable, timeout_ptr, wait_block)
    std::vector<std::shared_ptr<KObject>> objs;
    for (uint32_t i = 0; i < arg32(ctx, 0); ++i) {
        auto o = by_ptr(ld32(arg32(ctx, 1) + i * 4));
        if (!o) { ret32(ctx, STATUS_INVALID_PARAMETER); return; }
        objs.push_back(o);
    }
    ret32(ctx, g_kernel.wait(objs, arg32(ctx, 2) == 0, arg32(ctx, 6)));
}

FA_IMPORT(NtWaitForMultipleObjectsEx) {
    // (count, handles_ptr, wait_type, wait_mode, alertable, timeout_ptr)
    std::vector<std::shared_ptr<KObject>> objs;
    for (uint32_t i = 0; i < arg32(ctx, 0); ++i) {
        auto o = by_handle(ld32(arg32(ctx, 1) + i * 4));
        if (!o) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
        objs.push_back(o);
    }
    ret32(ctx, g_kernel.wait(objs, arg32(ctx, 2) == 0, arg32(ctx, 5)));
}

FA_IMPORT(NtSignalAndWaitForSingleObjectEx) {
    // (signal_handle, wait_handle, alertable, unknown, timeout_ptr)
    auto sig = by_handle(arg32(ctx, 0));
    auto w = by_handle(arg32(ctx, 1));
    if (!sig || !w) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    if (sig->type == KObject::Type::Event) set_event(static_cast<KEvent&>(*sig));
    else if (sig->type == KObject::Type::Semaphore) { bool o; release_sem(static_cast<KSemaphore&>(*sig), 1, o); }
    ret32(ctx, g_kernel.wait({w}, false, arg32(ctx, 4)));
}

// ---- critical sections ---------------------------------------------------------------------
// Guest layout (X_RTL_CRITICAL_SECTION): 0x10 byte dispatcher header, then
// +0x10 lock_count, +0x14 recursion_count, +0x18 owning thread id.
//
// The owner word is taken with a compare-and-swap, so an uncontended enter/leave is a few
// instructions and takes no host lock. A thread that cannot get the section spins briefly, then
// sleeps on a futex on the owner word. Only the owner touches the two counters.

namespace {
uint32_t me() { return g_kernel.current() ? g_kernel.current()->tid : 1; }

std::atomic<int> g_cs_waiters{0};

uint32_t* cs_owner(uint32_t p) { return reinterpret_cast<uint32_t*>(host(p + 0x18)); }

long futex(uint32_t* addr, int op, uint32_t val) {
    return syscall(SYS_futex, addr, op, val, nullptr, nullptr, 0);
}

void cs_init(uint32_t p) {
    st32(p + 0x00, 1);
    st32(p + 0x10, 0xFFFFFFFF);
    st32(p + 0x14, 0);
    st32(p + 0x18, 0);
}

// Fast path shared by enter and try-enter: true if the caller now owns the section.
bool cs_try(uint32_t p, uint32_t self_be) {
    uint32_t* own = cs_owner(p);
    if (__atomic_load_n(own, __ATOMIC_RELAXED) == self_be) {  // recursive enter
        uint32_t rc = ld32(p + 0x14) + 1;
        st32(p + 0x14, rc);
        st32(p + 0x10, rc - 1);
        return true;
    }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(own, &expected, self_be, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return false;
    st32(p + 0x14, 1);
    st32(p + 0x10, 0);
    return true;
}
}  // namespace

FA_IMPORT(RtlInitializeCriticalSection) { cs_init(arg32(ctx, 0)); }
FA_IMPORT(RtlInitializeCriticalSectionAndSpinCount) { cs_init(arg32(ctx, 0)); }
FA_IMPORT(RtlDeleteCriticalSection) { }

FA_IMPORT(RtlEnterCriticalSection) {
    uint32_t p = arg32(ctx, 0), self_be = __builtin_bswap32(me());
    for (int spin = 0; !cs_try(p, self_be); ++spin) {
        if (spin < 100) { __builtin_ia32_pause(); continue; }
        // Announce the waiter before the final check, so a leaving owner either sees the count
        // or this thread sees the free owner word (both sides use sequentially consistent ops).
        g_cs_waiters.fetch_add(1);
        uint32_t cur = __atomic_load_n(cs_owner(p), __ATOMIC_SEQ_CST);
        if (cur != 0) futex(cs_owner(p), FUTEX_WAIT_PRIVATE, cur);
        g_cs_waiters.fetch_sub(1);
    }
}

FA_IMPORT(RtlTryEnterCriticalSection) {
    ret32(ctx, cs_try(arg32(ctx, 0), __builtin_bswap32(me())) ? 1 : 0);
}

FA_IMPORT(RtlLeaveCriticalSection) {
    uint32_t p = arg32(ctx, 0);
    uint32_t rc = ld32(p + 0x14);
    if (rc == 0) return;
    --rc;
    st32(p + 0x14, rc);
    st32(p + 0x10, rc - 1);
    if (rc == 0) {
        __atomic_store_n(cs_owner(p), 0u, __ATOMIC_SEQ_CST);
        if (g_cs_waiters.load()) futex(cs_owner(p), FUTEX_WAKE_PRIVATE, 1);
    }
}

// ---- IRQL and spin locks (single address space, so these only need mutual exclusion) --------

FA_IMPORT(KeRaiseIrqlToDpcLevel) { ret32(ctx, 0); }
FA_IMPORT(KfRaiseIrql) { ret32(ctx, 0); }
FA_IMPORT(KfLowerIrql) { }

static void spin_acquire(uint32_t p) {
    auto* w = reinterpret_cast<uint32_t*>(host(p));
    uint32_t t = me();
    for (int spin = 0; !__sync_bool_compare_and_swap(w, 0u, t); ++spin) {
        if (spin < 200) __builtin_ia32_pause();
        else std::this_thread::yield();
    }
}
FA_IMPORT(KfAcquireSpinLock) { spin_acquire(arg32(ctx, 0)); ret32(ctx, 0); }
FA_IMPORT(KfReleaseSpinLock) { __sync_lock_release(reinterpret_cast<uint32_t*>(host(arg32(ctx, 0)))); }
FA_IMPORT(KeAcquireSpinLockAtRaisedIrql) { spin_acquire(arg32(ctx, 0)); }
FA_IMPORT(KeReleaseSpinLockFromRaisedIrql) { __sync_lock_release(reinterpret_cast<uint32_t*>(host(arg32(ctx, 0)))); }
FA_IMPORT(KeTryToAcquireSpinLockAtRaisedIrql) {
    auto* w = reinterpret_cast<uint32_t*>(host(arg32(ctx, 0)));
    ret32(ctx, __sync_bool_compare_and_swap(w, 0u, me()) ? 1 : 0);
}
