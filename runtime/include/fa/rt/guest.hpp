#pragma once
// Guest (Xbox 360) memory access helpers shared by the runtime and kernel code.
//
// The whole 4 GiB guest address space is one host mapping. A guest address is an
// offset from g_base, and guest memory is big-endian.
#include <cstdint>
#include <cstring>

#include "ppc_context.h"

namespace fa::rt {

extern uint8_t* g_base;

inline uint8_t* host(uint32_t guest) { return g_base + guest; }
inline uint32_t guest_addr(const void* p) {
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - g_base);
}

inline uint8_t ld8(uint32_t a) { return *host(a); }
inline uint16_t ld16(uint32_t a) { uint16_t v; std::memcpy(&v, host(a), 2); return __builtin_bswap16(v); }
inline uint32_t ld32(uint32_t a) { uint32_t v; std::memcpy(&v, host(a), 4); return __builtin_bswap32(v); }
inline uint64_t ld64(uint32_t a) { uint64_t v; std::memcpy(&v, host(a), 8); return __builtin_bswap64(v); }
inline void st8(uint32_t a, uint8_t v) { *host(a) = v; }
inline void st16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(host(a), &v, 2); }
inline void st32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(host(a), &v, 4); }
inline void st64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(host(a), &v, 8); }

// Guest ABI: integer arguments r3..r10, result in r3.
inline uint32_t arg32(PPCContext& c, int n) {
    switch (n) {
        case 0: return c.r3.u32; case 1: return c.r4.u32; case 2: return c.r5.u32;
        case 3: return c.r6.u32; case 4: return c.r7.u32; case 5: return c.r8.u32;
        case 6: return c.r9.u32; case 7: return c.r10.u32;
        // Later arguments are in the caller's parameter save area, one 8-byte slot each.
        default: return ld32(c.r1.u32 + 0x54 + (n - 8) * 8);
    }
}
inline void ret32(PPCContext& c, uint32_t v) { c.r3.u64 = v; }

// Guest function result codes (NTSTATUS).
enum : uint32_t {
    STATUS_SUCCESS = 0x00000000,
    STATUS_USER_APC = 0x000000C0,
    STATUS_TIMEOUT = 0x00000102,
    STATUS_PENDING = 0x00000103,
    STATUS_UNSUCCESSFUL = 0xC0000001,
    STATUS_NOT_IMPLEMENTED = 0xC0000002,
    STATUS_INVALID_HANDLE = 0xC0000008,
    STATUS_INVALID_PARAMETER = 0xC000000D,
    STATUS_NO_SUCH_FILE = 0xC000000F,
    STATUS_END_OF_FILE = 0xC0000011,
    STATUS_NO_MEMORY = 0xC0000017,
    STATUS_ACCESS_DENIED = 0xC0000022,
    STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
    STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
    STATUS_MUTANT_NOT_OWNED = 0xC0000046,
    STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
};

}  // namespace fa::rt
