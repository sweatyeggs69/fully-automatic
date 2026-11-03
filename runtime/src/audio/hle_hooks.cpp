#include "fa/rt/hle_hooks.hpp"

#include "fa/rt/guest.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

namespace fa::rt {

fa::hle::GuestHooks make_runtime_hooks() {
    fa::hle::GuestHooks h;
    h.membase = g_base;
    h.alloc = [](uint32_t size, uint32_t align) -> uint32_t {
        // Anything the APU touches must live in physical memory.
        if (align >= 256 || size >= 4096) return g_mem.alloc(Heap::Physical, size, align < kPageSize ? kPageSize : align);
        return g_mem.malloc(size, align);
    };
    h.free = [](uint32_t addr) {
        if (addr >= 0xA0000000u) g_mem.free(addr);
        else g_mem.mfree(addr);
    };
    h.attach_thread = [](const char* name) { attach_guest_thread(name); };
    h.invoke = [](uint32_t func, uint32_t arg) { invoke_guest(func, arg); };
    h.phys_to_host = [](uint32_t phys) -> uint8_t* {
        // Physical memory is mapped at 0xA0000000 (64 KiB pages) or 0xC0000000
        // (16 MiB pages) without aliasing; use whichever window holds it.
        phys &= 0x1FFFFFFFu;
        uint32_t start, size;
        for (uint32_t window : {0xA0000000u, 0xC0000000u, 0xE0000000u})
            if (g_mem.find_allocation(window + phys, start, size)) return g_base + window + phys;
        return g_base + 0xA0000000u + phys;
    };
    return h;
}

}  // namespace fa::rt
