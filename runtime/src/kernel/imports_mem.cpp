// Memory related kernel imports: Mm*, NtAllocateVirtualMemory, Ex pools, Rtl heaps.
#include <cstdio>
#include <cstring>
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"
#include "fa/rt/import.hpp"

using namespace fa::rt;

enum : uint32_t {
    MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000,
    PAGE_READWRITE = 4,
};

FA_IMPORT(MmAllocatePhysicalMemoryEx) {
    // (type, size, protect, min_addr, max_addr, alignment)
    uint32_t size = arg32(ctx, 1), align = arg32(ctx, 5);
    if (align < kAllocGranularity) align = kAllocGranularity;
    ret32(ctx, g_mem.alloc(Heap::Physical, size, align));
}

FA_IMPORT(MmFreePhysicalMemory) {
    g_mem.free(arg32(ctx, 1));
}

FA_IMPORT(MmGetPhysicalAddress) {
    uint32_t a = arg32(ctx, 0);
    uint32_t p = a & 0x1FFFFFFF;
    if (a >= 0xE0000000) p += 0x1000;
    ret32(ctx, p);
}

FA_IMPORT(MmQueryAddressProtect) { ret32(ctx, PAGE_READWRITE); }
FA_IMPORT(MmSetAddressProtect) { }

FA_IMPORT(MmQueryAllocationSize) {
    uint32_t start, size;
    ret32(ctx, g_mem.find_allocation(arg32(ctx, 0), start, size) ? size : 0);
}

FA_IMPORT(NtAllocateVirtualMemory) {
    // (base_addr_ptr, region_size_ptr, alloc_type, protect, debug_memory)
    uint32_t base_ptr = arg32(ctx, 0), size_ptr = arg32(ctx, 1), type = arg32(ctx, 2);
    uint32_t want = base_ptr ? ld32(base_ptr) : 0;
    uint32_t size = size_ptr ? ld32(size_ptr) : 0;
    if (size == 0) { ret32(ctx, STATUS_INVALID_PARAMETER); return; }
    uint32_t start, asize;
    uint32_t result = 0;
    // Everything but the 0x90000000 view range uses 64 KiB pages: bases round down and sizes
    // round up, and the rounded values are reported back (the game's heap relies on them).
    bool small_pages = want >= 0x90000000 && want < 0xA0000000;
    uint32_t pg = small_pages ? kPageSize : kAllocGranularity;
    if (want && g_mem.find_allocation(want, start, asize)) {
        // Committing inside an earlier reservation: already backed.
        uint32_t base_r = want & ~(pg - 1);
        result = base_r;
        size = (size + (want - base_r) + pg - 1) & ~(pg - 1);
    } else {
        size = (size + pg - 1) & ~(pg - 1);
        Heap h = (want >= 0xA0000000 && want < 0xE0000000) ? Heap::Physical
               : small_pages ? Heap::View : Heap::User;
        result = g_mem.alloc(h, size, kAllocGranularity, want);
        if (!result) std::fprintf(stderr, "NtAllocateVirtualMemory failed: base=%08x size=%08x type=%x\n", want, size, type);
        if (!result && want == 0) result = 0;
        size = g_mem.allocation_size(result);
    }
    if (!result) { ret32(ctx, STATUS_NO_MEMORY); return; }
    if (base_ptr) st32(base_ptr, result);
    if (size_ptr) st32(size_ptr, size);
    if (fa::rt::g_trace_imports)
        std::fprintf(stderr, "[mem] NtAllocateVirtualMemory want=%08x type=%08x -> base=%08x size=%08x\n", want, type, result, size);
    ret32(ctx, STATUS_SUCCESS);
    (void)type;
}

FA_IMPORT(NtFreeVirtualMemory) {
    // (base_addr_ptr, region_size_ptr, free_type, debug_memory)
    uint32_t base_ptr = arg32(ctx, 0), size_ptr = arg32(ctx, 1), type = arg32(ctx, 2);
    uint32_t addr = base_ptr ? ld32(base_ptr) : 0;
    if (fa::rt::g_trace_imports)
        std::fprintf(stderr, "[mem] NtFreeVirtualMemory base=%08x size=%08x type=%08x\n", addr,
                     size_ptr ? ld32(size_ptr) : 0, type);
    if ((type & MEM_DECOMMIT) && !(type & MEM_RELEASE)) {
        // Decommitted pages come back zeroed when committed again.
        uint32_t pg = (addr >= 0x90000000 && addr < 0xA0000000) ? kPageSize : kAllocGranularity;
        uint32_t a = addr & ~(pg - 1);
        uint32_t sz = size_ptr ? ld32(size_ptr) : 0;
        uint32_t end = (addr + sz + pg - 1) & ~(pg - 1);
        uint32_t st, as;
        if (end > a && g_mem.find_allocation(a, st, as) && uint64_t(end) <= uint64_t(st) + as) {
            std::memset(host(a), 0, end - a);
            if (base_ptr) st32(base_ptr, a);
            if (size_ptr) st32(size_ptr, end - a);
        }
    }
    if (type & MEM_RELEASE) {
        uint32_t freed = 0;
        if (!g_mem.free(addr, &freed)) { ret32(ctx, STATUS_INVALID_PARAMETER); return; }
        if (size_ptr) st32(size_ptr, freed);
    }
    ret32(ctx, STATUS_SUCCESS);
}

// ---- pools ---------------------------------------------------------------------------

FA_IMPORT(ExAllocatePool) { ret32(ctx, g_mem.malloc(arg32(ctx, 0))); }
FA_IMPORT(ExAllocatePoolWithTag) { ret32(ctx, g_mem.malloc(arg32(ctx, 0))); }
FA_IMPORT(ExAllocatePoolTypeWithTag) { ret32(ctx, g_mem.malloc(arg32(ctx, 0))); }
FA_IMPORT(ExFreePool) { g_mem.mfree(arg32(ctx, 0)); }

// ---- heaps ---------------------------------------------------------------------------
// All heaps share the guest malloc heap; the heap handle is only a token.

static uint32_t g_next_heap = 0x7F000010;

FA_IMPORT(RtlCreateHeap) { ret32(ctx, g_next_heap += 0x10); }
FA_IMPORT(RtlDestroyHeap) { ret32(ctx, 1); }
FA_IMPORT(RtlAllocateHeap) { ret32(ctx, g_mem.malloc(arg32(ctx, 2))); }
FA_IMPORT(RtlFreeHeap) { g_mem.mfree(arg32(ctx, 2)); ret32(ctx, 1); }
FA_IMPORT(RtlSizeHeap) { ret32(ctx, g_mem.msize(arg32(ctx, 2))); }
FA_IMPORT(RtlReAllocateHeap) {
    uint32_t old = arg32(ctx, 2), size = arg32(ctx, 3);
    uint32_t n = g_mem.malloc(size);
    if (n && old) {
        uint32_t keep = g_mem.msize(old);
        std::memcpy(host(n), host(old), keep < size ? keep : size);
        g_mem.mfree(old);
    }
    ret32(ctx, n);
}
