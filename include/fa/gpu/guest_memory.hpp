#pragma once
// Interface between the GPU layer and the runtime core's guest memory.
// The runtime core owns the 4 GB guest address space (big-endian PowerPC view).
// The GPU layer only needs word access, so the core can implement this however it likes.
#include <cstdint>

namespace fa::gpu {

class GuestMemory {
public:
    virtual ~GuestMemory() = default;
    // Values are in HOST order: the implementation does the big-endian swap.
    virtual uint32_t Read32(uint32_t guestAddr) const = 0;
    virtual void Write32(uint32_t guestAddr, uint32_t value) = 0;
    // Raw host pointer to guest bytes (still big-endian), for bulk uploads. May return nullptr.
    virtual const uint8_t* HostPtr(uint32_t guestAddr, uint32_t size) const = 0;
    // Writable raw pointer (resolves write render targets back to guest memory). nullptr = use
    // Write32 instead.
    virtual uint8_t* HostPtrWrite(uint32_t guestAddr, uint32_t size) { return nullptr; }
    // File descriptor of the guest's physical memory (physical address p at offset p), so the
    // renderer can map it for the GPU. -1 = not available.
    virtual int PhysicalFd() const { return -1; }
    virtual uint32_t PhysicalSize() const { return 0; }
    // The renderer pinned physical memory for the GPU (udmabuf): freeing must zero pages in place
    // instead of dropping them, or the GPU would keep seeing the old pages.
    virtual void PhysicalPinned() {}
    // Host mappings through which guest physical memory is read and written: host address `base`
    // shows physical bytes [physOffset, physOffset + size). Fills at most `max`, returns the count.
    struct PhysicalView {
        uint8_t* base;
        uint32_t physOffset;
        uint32_t size;
    };
    virtual int PhysicalViews(PhysicalView* out, int max) const { return 0; }
};

}  // namespace fa::gpu
