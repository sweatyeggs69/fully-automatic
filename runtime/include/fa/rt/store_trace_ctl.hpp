#pragma once
// Control side of the guest store trace (always compiled; only does anything when the ppc
// sources were built with the store_trace.h macros). Enable with FA_STORE_TRACE=<first_swap>,<frames>
// (for example 600,1). Output: stderr summary and $FA_STORE_TRACE_OUT (default store-trace.txt).
#include <cstdint>
namespace fa::rt {
// A GPU command buffer lives at this guest address (ring buffer, indirect buffer).
void store_trace_add_range(uint32_t guest_addr, uint32_t bytes);
// A frame ended (VdSwap). Arms and disarms the capture window.
void store_trace_frame_end();
}
