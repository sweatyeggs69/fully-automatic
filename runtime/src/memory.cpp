#include "fa/rt/memory.hpp"
#include "fa/rt/guest.hpp"

#include <sys/mman.h>
#include <cstdio>

namespace fa::rt {

uint8_t* g_base = nullptr;
GuestMemory g_mem;

static uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

bool GuestMemory::init() {
    void* p = mmap(nullptr, kGuestSpace + 0x1000, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap guest space");
        return false;
    }
    // The generated code assumes the base is 32-byte aligned; mmap gives page alignment.
    base_ = static_cast<uint8_t*>(p);
    g_base = base_;
    return true;
}

bool GuestMemory::commit_fixed(uint32_t addr, uint32_t size) {
    uint32_t a = addr & ~(kPageSize - 1);
    uint64_t end = (uint64_t(addr) + size + kPageSize - 1) & ~uint64_t(kPageSize - 1);
    return mprotect(base_ + a, end - a, PROT_READ | PROT_WRITE) == 0;
}

GuestMemory::Range GuestMemory::range_of(Heap h) {
    switch (h) {
        case Heap::User: return {0x00010000, 0x40000000};
        case Heap::System: return {0x60000000, 0x70000000};
        case Heap::Malloc: return {0x70000000, 0x7F000000};
        case Heap::Physical: return {0xA0000000, 0xE0000000};
        case Heap::View: return {0x90000000, 0xA0000000};
    }
    return {0, 0};
}

uint32_t GuestMemory::alloc(Heap heap, uint32_t size, uint32_t align, uint32_t fixed) {
    if (size == 0) size = 1;
    size = round_up(size, kPageSize);
    if (align < kPageSize) align = kPageSize;
    std::lock_guard<std::mutex> lk(mu_);
    Range r = range_of(heap);

    auto try_at = [&](uint32_t addr) -> bool {
        if (addr < r.lo || uint64_t(addr) + size > r.hi) return false;
        auto it = allocs_.upper_bound(addr);
        if (it != allocs_.end() && uint64_t(addr) + size > it->first) return false;
        if (it != allocs_.begin()) {
            auto prev = std::prev(it);
            if (uint64_t(prev->first) + prev->second > addr) return false;
        }
        return true;
    };

    uint32_t addr = 0;
    if (fixed) {
        if (!try_at(fixed)) return 0;
        addr = fixed;
    } else {
        uint64_t cand = round_up(r.lo, align);
        for (auto it = allocs_.lower_bound(r.lo); ; ++it) {
            uint64_t limit = (it == allocs_.end() || it->first >= r.hi) ? r.hi : it->first;
            if (cand + size <= limit) { addr = uint32_t(cand); break; }
            if (it == allocs_.end() || it->first >= r.hi) return 0;
            cand = round_up(it->first + it->second, align);
        }
    }
    if (mprotect(base_ + addr, size, PROT_READ | PROT_WRITE) != 0) return 0;
    allocs_[addr] = size;
    return addr;
}

bool GuestMemory::free(uint32_t addr, uint32_t* freed_size) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = allocs_.find(addr);
    if (it == allocs_.end()) return false;
    uint32_t size = it->second;
    // Drop the pages so a later allocation sees zeroed memory, and trap stale accesses.
    madvise(base_ + addr, size, MADV_DONTNEED);
    mprotect(base_ + addr, size, PROT_NONE);
    allocs_.erase(it);
    if (freed_size) *freed_size = size;
    return true;
}

uint32_t GuestMemory::allocation_size(uint32_t addr) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = allocs_.find(addr);
    return it == allocs_.end() ? 0 : it->second;
}

bool GuestMemory::find_allocation(uint32_t addr, uint32_t& start, uint32_t& size) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = allocs_.upper_bound(addr);
    if (it == allocs_.begin()) return false;
    --it;
    if (addr >= it->first && uint64_t(addr) < uint64_t(it->first) + it->second) {
        start = it->first;
        size = it->second;
        return true;
    }
    return false;
}

// ---- guest malloc -------------------------------------------------------------
// Block layout: [16 byte header][user data]. Header: u32 block_bytes, u32 block_start.
// block_bytes is the size class (power of two up to 64 KiB) or a page multiple for
// large blocks (flagged by the top bit of block_start's companion word).

static constexpr uint32_t kChunk = 1u << 20;
static constexpr uint32_t kMaxClass = 1u << 16;
static constexpr uint32_t kLargeFlag = 0x80000000u;

uint32_t GuestMemory::malloc_locked(uint32_t size, uint32_t align) {
    if (align < 16) align = 16;
    uint64_t need = uint64_t(size) + 16 + (align - 16);
    if (need > kMaxClass) {
        uint32_t bytes = round_up(uint32_t(need), kPageSize);
        uint32_t blk = alloc(Heap::Malloc, bytes, kPageSize);
        if (!blk) return 0;
        uint32_t user = round_up(blk + 16, align);
        st32(user - 16, bytes | kLargeFlag);
        st32(user - 12, blk);
        return user;
    }
    uint32_t cls = 32;
    while (cls < need) cls <<= 1;
    uint32_t blk = 0;
    auto& fl = free_lists_[cls];
    if (!fl.empty()) {
        blk = fl.back();
        fl.pop_back();
    } else {
        uint32_t a = cls < 4096 ? cls : 4096;
        uint32_t cur = round_up(chunk_.cur, a);
        if (chunk_.cur == 0 || cur + cls > chunk_.end) {
            uint32_t c = alloc(Heap::Malloc, kChunk, kPageSize);
            if (!c) return 0;
            chunk_ = {c, c + kChunk};
            cur = round_up(chunk_.cur, a);
        }
        blk = cur;
        chunk_.cur = cur + cls;
    }
    uint32_t user = round_up(blk + 16, align);
    st32(user - 16, cls);
    st32(user - 12, blk);
    return user;
}

uint32_t GuestMemory::malloc(uint32_t size, uint32_t align) {
    uint32_t r;
    {
        std::lock_guard<std::mutex> lk(malloc_mu_);
        r = malloc_locked(size, align);
    }
    if (r) std::memset(host(r), 0, size);
    return r;
}

void GuestMemory::mfree(uint32_t addr) {
    if (!addr) return;
    uint32_t bytes = ld32(addr - 16), blk = ld32(addr - 12);
    if (bytes & kLargeFlag) {
        free(blk);
        return;
    }
    std::lock_guard<std::mutex> lk(malloc_mu_);
    free_lists_[bytes].push_back(blk);
}

uint32_t GuestMemory::msize(uint32_t addr) {
    if (!addr) return 0;
    uint32_t bytes = ld32(addr - 16), blk = ld32(addr - 12);
    return (bytes & ~kLargeFlag) - (addr - blk);
}

}  // namespace fa::rt
