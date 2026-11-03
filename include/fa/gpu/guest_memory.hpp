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
};

}  // namespace fa::gpu
