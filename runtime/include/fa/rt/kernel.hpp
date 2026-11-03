#pragma once
// Kernel object model: handles, dispatcher objects, waits, and guest threads.
//
// All dispatcher state is protected by one global mutex/condition variable pair. That
// is simple and correct; it can be split per object later if profiling asks for it.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fa/rt/guest.hpp"

namespace fa::rt {

extern std::mutex g_kmu;
extern std::condition_variable g_kcv;

struct KThread;

struct KObject {
    enum class Type { Event, Semaphore, Mutant, Thread, File };
    explicit KObject(Type t) : type(t) {}
    virtual ~KObject() = default;
    // Both are called with g_kmu held.
    virtual bool signaled(const KThread* waiter) const = 0;
    virtual void acquire(KThread* waiter) = 0;  // consume a satisfied wait
    const Type type;
    uint32_t handle = 0;
    uint32_t guest_ptr = 0;  // set when exposed to the guest as a dispatcher object (ObReferenceObjectByHandle)
};

struct KEvent : KObject {
    KEvent(bool manual_reset, bool state) : KObject(Type::Event), manual(manual_reset), set(state) {}
    bool signaled(const KThread*) const override { return set; }
    void acquire(KThread*) override { if (!manual) set = false; }
    bool manual, set;
};

struct KSemaphore : KObject {
    KSemaphore(int32_t c, int32_t l) : KObject(Type::Semaphore), count(c), limit(l) {}
    bool signaled(const KThread*) const override { return count > 0; }
    void acquire(KThread*) override { --count; }
    int32_t count, limit;
};

struct KMutant : KObject {
    KMutant() : KObject(Type::Mutant) {}
    bool signaled(const KThread* w) const override { return owner == nullptr || owner == w; }
    void acquire(KThread* w) override { owner = w; ++recursion; }
    const KThread* owner = nullptr;
    int32_t recursion = 0;
};

struct KThread : KObject {
    KThread() : KObject(Type::Thread) {}
    bool signaled(const KThread*) const override { return finished; }
    void acquire(KThread*) override {}

    uint32_t tid = 0;
    std::string name;
    uint32_t start_address = 0, start_context = 0, xapi_startup = 0;
    uint32_t stack_base = 0, stack_size = 0;  // guest allocation
    uint32_t pcr = 0, kthread = 0, tls_static = 0, tls_dynamic = 0, block = 0;
    int32_t suspend_count = 0;
    bool started = false, finished = false;
    uint32_t exit_code = 0;
    std::thread host;
    PPCContext* ctx = nullptr;
};

// Thrown through guest frames by ExTerminateThread.
struct ThreadExit { uint32_t code; };

struct TlsInfo { uint32_t slot_count = 0, raw_address = 0, data_size = 0, raw_size = 0; };

class Kernel {
public:
    // ---- handles and dispatcher objects ----
    uint32_t add_handle(std::shared_ptr<KObject> o);
    std::shared_ptr<KObject> get_handle(uint32_t h);  // also resolves NtCurrentThread
    bool close_handle(uint32_t h);
    // Dispatcher objects embedded in guest memory (KeInitializeEvent etc.), keyed by address.
    void register_dispatch(uint32_t guest_ptr, std::shared_ptr<KObject> o);
    std::shared_ptr<KObject> get_dispatch(uint32_t guest_ptr);

    // ---- waits ----
    // timeout_ptr: guest pointer to a signed 64-bit 100ns count (negative = relative), or 0 to wait forever.
    uint32_t wait(const std::vector<std::shared_ptr<KObject>>& objs, bool wait_all, uint32_t timeout_ptr);
    void signal_changed() { g_kcv.notify_all(); }

    // ---- threads ----
    void set_tls_info(const TlsInfo& t) { tls_ = t; }
    const TlsInfo& tls_info() const { return tls_; }
    std::shared_ptr<KThread> create_thread(uint32_t stack_size, uint32_t xapi_startup,
                                            uint32_t start_address, uint32_t start_context,
                                            bool suspended);
    // Turns the calling host thread into the main guest thread and runs `entry` on it.
    uint32_t run_main_thread(uint32_t entry, uint32_t stack_size);
    uint32_t resume(KThread& t);
    uint32_t suspend(KThread& t);
    KThread* current() const;

    // Dynamic TLS slots (KeTlsAlloc and friends).
    uint32_t tls_alloc();
    bool tls_free(uint32_t idx);
    uint32_t tls_get(uint32_t idx);
    bool tls_set(uint32_t idx, uint32_t v);

    uint32_t next_tid() { return tid_counter_++; }
    // Blocks until every non-main guest thread has finished (used at shutdown).
    void join_all();
    // Used by attach_guest_thread.
    bool setup_thread_memory_public(KThread& t);
    void register_external_thread(std::shared_ptr<KThread> t);

private:
    void thread_main(std::shared_ptr<KThread> t);
    bool setup_thread_memory(KThread& t);
    void init_context(KThread& t);

    std::mutex tmu_;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> handles_;
    std::unordered_map<uint32_t, std::shared_ptr<KObject>> dispatch_;
    std::vector<std::shared_ptr<KThread>> threads_;
    std::vector<std::shared_ptr<KThread>> external_;
    uint32_t next_handle_ = 0x100;
    std::atomic<uint32_t> tid_counter_{0x100};
    TlsInfo tls_;
    std::vector<bool> tls_used_;
};

extern Kernel g_kernel;

// Calls a guest function through the function table on the given context.
void call_guest(PPCContext& ctx, uint32_t addr);
// For host threads created outside the kernel (audio, etc): gives the calling thread a guest
// context (PCR, stack, TLS) so invoke_guest works on it. Safe to call once per thread.
void attach_guest_thread(const std::string& name);
// Runs a guest function (r3 = arg) on the calling thread's guest context and returns r3.
uint32_t invoke_guest(uint32_t func, uint32_t arg, uint32_t arg2 = 0);
// Resolves a guest address to its recompiled function, or nullptr.
PPCFunc* lookup_guest(uint32_t addr);

}  // namespace fa::rt
