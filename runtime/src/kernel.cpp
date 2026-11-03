#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <new>

extern PPCFuncMapping PPCFuncMappings[];

namespace fa::rt {

std::mutex g_kmu;
std::condition_variable g_kcv;
Kernel g_kernel;

static thread_local KThread* t_current = nullptr;
static constexpr uint32_t kNtCurrentThread = 0xFFFFFFFE;
static constexpr uint32_t kDefaultStack = 0x10000;
static constexpr uint32_t kMinStack = 0x4000;

// ---- guest function table ---------------------------------------------------------

PPCFunc* lookup_guest(uint32_t addr) {
    if (addr < PPC_CODE_BASE || addr >= PPC_CODE_BASE + PPC_CODE_SIZE) return nullptr;
    return PPC_LOOKUP_FUNC(g_base, addr);
}

void call_guest(PPCContext& ctx, uint32_t addr) {
    PPCFunc* f = lookup_guest(addr);
    if (!f) {
        std::fprintf(stderr, "fatal: call to unresolved guest function 0x%08X\n", addr);
        std::abort();
    }
    f(ctx, g_base);
}

// ---- handles and dispatcher objects ------------------------------------------------

uint32_t Kernel::add_handle(std::shared_ptr<KObject> o) {
    std::lock_guard<std::mutex> lk(tmu_);
    uint32_t h = next_handle_;
    next_handle_ += 4;
    o->handle = h;
    handles_[h] = std::move(o);
    return h;
}

std::shared_ptr<KObject> Kernel::get_handle(uint32_t h) {
    if (h == kNtCurrentThread) {
        // Wrap the current thread without owning it.
        std::lock_guard<std::mutex> lk(tmu_);
        for (auto& t : threads_) if (t.get() == t_current) return t;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(tmu_);
    auto it = handles_.find(h);
    return it == handles_.end() ? nullptr : it->second;
}

bool Kernel::close_handle(uint32_t h) {
    std::lock_guard<std::mutex> lk(tmu_);
    return handles_.erase(h) != 0;
}

void Kernel::register_dispatch(uint32_t p, std::shared_ptr<KObject> o) {
    std::lock_guard<std::mutex> lk(tmu_);
    dispatch_[p] = std::move(o);
}

std::shared_ptr<KObject> Kernel::get_dispatch(uint32_t p) {
    std::lock_guard<std::mutex> lk(tmu_);
    auto it = dispatch_.find(p);
    return it == dispatch_.end() ? nullptr : it->second;
}

// ---- waits ----------------------------------------------------------------------------

uint32_t Kernel::wait(const std::vector<std::shared_ptr<KObject>>& objs, bool wait_all,
                      uint32_t timeout_ptr) {
    KThread* me = t_current;
    bool infinite = timeout_ptr == 0;
    bool poll = false;
    std::chrono::steady_clock::time_point deadline;
    if (!infinite) {
        int64_t t = static_cast<int64_t>(ld64(timeout_ptr));
        if (t == 0) {
            poll = true;
        } else if (t < 0) {
            deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(-t * 100);
        } else {
            // Absolute time (100ns since 1601); treat as "wait until", approximated by polling.
            poll = true;
        }
    }

    std::unique_lock<std::mutex> lk(g_kmu);
    for (;;) {
        if (wait_all) {
            bool all = !objs.empty();
            for (auto& o : objs) if (!o->signaled(me)) { all = false; break; }
            if (all) {
                for (auto& o : objs) o->acquire(me);
                return STATUS_SUCCESS;
            }
        } else {
            for (size_t i = 0; i < objs.size(); ++i) {
                if (objs[i]->signaled(me)) {
                    objs[i]->acquire(me);
                    return static_cast<uint32_t>(i);
                }
            }
        }
        if (poll) return STATUS_TIMEOUT;
        if (infinite) {
            g_kcv.wait(lk);
        } else if (g_kcv.wait_until(lk, deadline) == std::cv_status::timeout) {
            // One last check happens on the next loop iteration only if time remains; report timeout.
            bool any = false;
            for (auto& o : objs) if (o->signaled(me)) { any = true; break; }
            if (!any || wait_all) return STATUS_TIMEOUT;
        }
    }
}

// ---- threads ----------------------------------------------------------------------------

KThread* Kernel::current() const { return t_current; }

bool Kernel::setup_thread_memory(KThread& t) {
    if (t.stack_size < kMinStack) t.stack_size = kDefaultStack;
    t.stack_base = g_mem.alloc(Heap::System, t.stack_size, kPageSize);
    uint32_t tls_bytes = ((tls_.data_size + 15) & ~15u) + tls_.slot_count * 4 + 64;
    t.block = g_mem.alloc(Heap::System, 0x2000 + tls_bytes, kPageSize);
    if (!t.stack_base || !t.block) return false;

    t.pcr = t.block;
    t.kthread = t.block + 0x1000;
    t.tls_static = t.block + 0x2000;
    t.tls_dynamic = t.tls_static + ((tls_.data_size + 15) & ~15u);
    if (tls_.raw_size && tls_.raw_address)
        std::memcpy(host(t.tls_static), host(tls_.raw_address), tls_.raw_size);

    // Processor Control Region (PCR), reached through r13 on the 360.
    st32(t.pcr + 0x00, t.tls_static);
    st32(t.pcr + 0x30, t.pcr);
    st32(t.pcr + 0x70, t.stack_base + t.stack_size);  // stack base (high)
    st32(t.pcr + 0x74, t.stack_base);                 // stack limit (low)
    st32(t.pcr + 0x100, t.kthread);
    st8(t.pcr + 0x10C, 0);                            // current processor
    st32(t.kthread + 0x14C, t.tid);
    st32(t.kthread + 0x150, t.tid);
    return true;
}

void Kernel::init_context(KThread& t) {
    void* mem = ::operator new(sizeof(PPCContext), std::align_val_t(alignof(PPCContext)));
    t.ctx = new (mem) PPCContext();
    PPCContext& c = *t.ctx;
    c.r1.u64 = (t.stack_base + t.stack_size - 0x100) & ~0xFull;
    c.r13.u64 = t.pcr;
    c.fpscr.loadFromHost();
    c.fpscr.disableFlushModeUnconditional();
}

void Kernel::thread_main(std::shared_ptr<KThread> t) {
    t_current = t.get();
    {
        std::unique_lock<std::mutex> lk(g_kmu);
        g_kcv.wait(lk, [&] { return t->suspend_count <= 0; });
        t->started = true;
    }
    PPCContext& c = *t->ctx;
    uint32_t entry = t->start_address;
    c.r3.u64 = t->start_context;
    if (t->xapi_startup) {
        // XAPI startup shim: calls start_address(start_context) and then ExTerminateThread.
        entry = t->xapi_startup;
        c.r3.u64 = t->start_address;
        c.r4.u64 = t->start_context;
    }
    try {
        call_guest(c, entry);
        t->exit_code = c.r3.u32;
    } catch (const ThreadExit& e) {
        t->exit_code = e.code;
    }
    {
        std::lock_guard<std::mutex> lk(g_kmu);
        t->finished = true;
    }
    g_kcv.notify_all();
}

std::shared_ptr<KThread> Kernel::create_thread(uint32_t stack_size, uint32_t xapi_startup,
                                               uint32_t start_address, uint32_t start_context,
                                               bool suspended) {
    auto t = std::make_shared<KThread>();
    t->tid = next_tid();
    t->stack_size = stack_size;
    t->xapi_startup = xapi_startup;
    t->start_address = start_address;
    t->start_context = start_context;
    t->suspend_count = suspended ? 1 : 0;
    if (!setup_thread_memory(*t)) return nullptr;
    init_context(*t);
    {
        std::lock_guard<std::mutex> lk(tmu_);
        threads_.push_back(t);
    }
    t->host = std::thread([this, t] { thread_main(t); });
    return t;
}

uint32_t Kernel::resume(KThread& t) {
    std::lock_guard<std::mutex> lk(g_kmu);
    uint32_t prev = t.suspend_count > 0 ? t.suspend_count : 0;
    if (t.suspend_count > 0) --t.suspend_count;
    g_kcv.notify_all();
    return prev;
}

uint32_t Kernel::suspend(KThread& t) {
    // Only threads that have not started running yet can really be held back; a running
    // host thread cannot be stopped portably. The count is still tracked for the guest.
    std::lock_guard<std::mutex> lk(g_kmu);
    return t.suspend_count++;
}

uint32_t Kernel::run_main_thread(uint32_t entry, uint32_t stack_size) {
    auto t = std::make_shared<KThread>();
    t->tid = next_tid();
    t->name = "main";
    t->stack_size = stack_size;
    t->start_address = entry;
    if (!setup_thread_memory(*t)) {
        std::fprintf(stderr, "fatal: cannot allocate main thread memory\n");
        return 0xFFFFFFFF;
    }
    init_context(*t);
    {
        std::lock_guard<std::mutex> lk(tmu_);
        threads_.push_back(t);
    }
    t_current = t.get();
    t->started = true;
    try {
        call_guest(*t->ctx, entry);
        t->exit_code = t->ctx->r3.u32;
    } catch (const ThreadExit& e) {
        t->exit_code = e.code;
    }
    {
        std::lock_guard<std::mutex> lk(g_kmu);
        t->finished = true;
    }
    g_kcv.notify_all();
    return t->exit_code;
}

bool Kernel::setup_thread_memory_public(KThread& t) {
    if (!setup_thread_memory(t)) return false;
    init_context(t);
    return true;
}

void Kernel::register_external_thread(std::shared_ptr<KThread> t) {
    std::lock_guard<std::mutex> lk(tmu_);
    external_.push_back(std::move(t));  // kept alive for the process lifetime
}

void Kernel::join_all() {
    std::vector<std::shared_ptr<KThread>> ts;
    {
        std::lock_guard<std::mutex> lk(tmu_);
        ts = threads_;
    }
    for (auto& t : ts)
        if (t->host.joinable()) t->host.join();
}

// ---- TLS slots ---------------------------------------------------------------------------

uint32_t Kernel::tls_alloc() {
    std::lock_guard<std::mutex> lk(tmu_);
    if (tls_used_.empty()) tls_used_.assign(tls_.slot_count ? tls_.slot_count : 64, false);
    for (uint32_t i = 0; i < tls_used_.size(); ++i)
        if (!tls_used_[i]) { tls_used_[i] = true; return i; }
    return 0xFFFFFFFF;
}

bool Kernel::tls_free(uint32_t idx) {
    std::lock_guard<std::mutex> lk(tmu_);
    if (idx >= tls_used_.size() || !tls_used_[idx]) return false;
    tls_used_[idx] = false;
    return true;
}

uint32_t Kernel::tls_get(uint32_t idx) {
    KThread* t = t_current;
    if (!t || idx >= (tls_.slot_count ? tls_.slot_count : 64)) return 0;
    return ld32(t->tls_dynamic + idx * 4);
}

bool Kernel::tls_set(uint32_t idx, uint32_t v) {
    KThread* t = t_current;
    if (!t || idx >= (tls_.slot_count ? tls_.slot_count : 64)) return false;
    st32(t->tls_dynamic + idx * 4, v);
    return true;
}

// ---- host threads that call into the guest ------------------------------------------------

void attach_guest_thread(const std::string& name) {
    if (t_current) return;
    auto t = std::make_shared<KThread>();
    t->tid = g_kernel.next_tid();
    t->name = name;
    t->stack_size = kDefaultStack;
    if (!g_kernel.setup_thread_memory_public(*t)) return;
    t_current = t.get();
    t->started = true;
    g_kernel.register_external_thread(t);
}

uint32_t invoke_guest(uint32_t func, uint32_t arg) {
    KThread* t = t_current;
    if (!t || !t->ctx) {
        std::fprintf(stderr, "invoke_guest: thread has no guest context (call attach_guest_thread)\n");
        return 0;
    }
    t->ctx->r3.u64 = arg;
    call_guest(*t->ctx, func);
    return t->ctx->r3.u32;
}

}  // namespace fa::rt
