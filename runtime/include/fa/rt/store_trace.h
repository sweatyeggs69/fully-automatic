// Diagnostic only (cmake -DFA_STORE_TRACE=ON, or FA_STORE_TRACE=1 tools/fa-build.sh).
// Force-included into every recompiled ppc_recomp.*.cpp. It wraps the guest store macros so that,
// while a capture is armed, every store into a GPU command buffer range reports itself with the
// host backtrace (recompiled functions are real host functions named sub_XXXXXXXX, so the
// backtrace is the guest call stack). See runtime/src/store_trace.cpp and
// notes/direct-gpu-hooks-design.md (Phase 0). Costs one load and branch per guest store.
// Not covered: vector stores that do not go through PPC_STORE_*.
#pragma once
#include FA_XENON_CONTEXT_H
#include <cstdint>

extern "C" volatile uint8_t fa_st_armed;
extern "C" void fa_st_hit(uint32_t ea, uint32_t size, uint64_t value);
extern "C" int fa_st_register_tu();
// One per instrumented translation unit; the count is printed when a capture arms, so a build
// where this header did not take effect is obvious.
[[maybe_unused]] static const int fa_st_tu_marker = fa_st_register_tu();

#undef PPC_STORE_U8
#undef PPC_STORE_U16
#undef PPC_STORE_U32
#undef PPC_STORE_U64
template <class T>
static inline void fa_st_put(uint8_t* base, uint32_t ea, T swapped, uint64_t raw) {
    if (__builtin_expect(fa_st_armed, 0)) fa_st_hit(ea, sizeof(T), raw);
    *reinterpret_cast<volatile T*>(base + ea) = swapped;
}
#define PPC_STORE_U8(x, y) fa_st_put<uint8_t>(base, (x), (y), (y))
#define PPC_STORE_U16(x, y) fa_st_put<uint16_t>(base, (x), __builtin_bswap16(y), (y))
#define PPC_STORE_U32(x, y) fa_st_put<uint32_t>(base, (x), __builtin_bswap32(y), (y))
#define PPC_STORE_U64(x, y) fa_st_put<uint64_t>(base, (x), __builtin_bswap64(y), (y))
