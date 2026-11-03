#pragma once
// Guest memory map and allocators.
//
//   0x00010000 - 0x3FFFFFFF  user virtual heap (NtAllocateVirtualMemory)
//   0x60000000 - 0x6FFFFFFF  system heap (kernel objects, stacks, PCR, TLS)
//   0x70000000 - 0x7EFFFFFF  guest malloc heap (ExAllocatePool, RtlAllocateHeap)
//   0x7FEA0000 - 0x7FEAFFFF  XMA/APU register page (plain memory, committed at startup)
//   0x80000000 - 0x8FFFFFFF  XEX image at 0x82000000 + function table after it
//   0xA0000000 - 0xBFFFFFFF  physical memory, 64 KiB pages (MmAllocatePhysicalMemoryEx)
//   0xC0000000 - 0xDFFFFFFF  physical memory, 16 MiB pages (treated like 0xA0000000)
//
// The physical ranges are not aliased the way real hardware does it; the physical
// address returned by MmGetPhysicalAddress is the guest address masked to 29 bits.
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace fa::rt {

constexpr uint64_t kGuestSpace = 0x100000000ull;
constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kAllocGranularity = 0x10000;

enum class Heap { User, System, Malloc, Physical };

class GuestMemory {
public:
    // Reserve the 4 GiB window. Returns false on failure.
    bool init();
    uint8_t* base() const { return base_; }

    // Commit and zero a fixed range (used for the XEX image and function table).
    bool commit_fixed(uint32_t addr, uint32_t size);

    // Allocate `size` bytes (rounded to pages) aligned to `align` from a heap.
    // `fixed` != 0 requests that exact address. Returns 0 on failure.
    uint32_t alloc(Heap heap, uint32_t size, uint32_t align = kAllocGranularity, uint32_t fixed = 0);
    bool free(uint32_t addr, uint32_t* freed_size = nullptr);
    // Size of the allocation that starts at addr, or 0.
    uint32_t allocation_size(uint32_t addr);
    // Start and size of the allocation containing addr, false if none.
    bool find_allocation(uint32_t addr, uint32_t& start, uint32_t& size);

    // Small-object guest allocator (malloc/free style) backed by Heap::Malloc.
    uint32_t malloc(uint32_t size, uint32_t align = 16);
    void mfree(uint32_t addr);
    uint32_t msize(uint32_t addr);

private:
    struct Range { uint32_t lo, hi; };
    static Range range_of(Heap h);
    uint8_t* base_ = nullptr;
    std::mutex mu_;
    std::mutex malloc_mu_;
    std::map<uint32_t, uint32_t> allocs_;  // start -> size (page rounded)

    // malloc heap: power-of-two size classes carved out of 1 MiB chunks.
    struct Chunk { uint32_t cur, end; };
    Chunk chunk_{0, 0};
    std::map<uint32_t, std::vector<uint32_t>> free_lists_;
    uint32_t malloc_locked(uint32_t size, uint32_t align);
};

extern GuestMemory g_mem;

}  // namespace fa::rt
