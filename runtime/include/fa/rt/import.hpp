#pragma once
// Defining a kernel/xam import.
//
// Every import the game calls is a function in the recompiled code named after its
// export (for example NtCreateEvent), declared weak. Defining a function with the same
// name and signature here replaces it at link time:
//
//     FA_IMPORT(NtClose) {
//         ret32(ctx, g_kernel.close_handle(arg32(ctx, 0)) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
//     }
//
// Arguments are in ctx.r3..r10 (arg32(ctx, n)), the result goes in ctx.r3 (ret32).
// FA_IMPORT lines are scanned by tools/gen_import_stubs.py, which generates a logging
// no-op for every import that has no implementation.
#include "fa/rt/guest.hpp"
#include <atomic>

// XenonRecomp's generated code resolves imports through __imp__<name> (the weak <name> is an
// alias of it), so the implementation is defined under that name; <name> forwards to it for
// callers that use the plain name.
namespace fa::rt {
// FA_TRACE_IMPORTS_CALLS=1 logs every import call with its first arguments and return value.
extern bool g_trace_imports;
void trace_import(const char* name, PPCContext& ctx, bool entry);
// Called when one import has been called a very large number of times (a guest spin loop).
void report_spin(const char* name, PPCContext& ctx, uint64_t count);
}  // namespace fa::rt

#define FA_IMPORT(name)                                                       \
    static void fa_impl_##name(PPCContext& ctx, uint8_t* base);               \
    PPC_FUNC(__imp__##name) {                                                 \
        static std::atomic<uint64_t> fa_calls;                                \
        uint64_t fa_n = ++fa_calls;                                           \
        if (fa_n == 1000000 || fa_n == 30000000) fa::rt::report_spin(#name, ctx, fa_n); \
        if (fa::rt::g_trace_imports) fa::rt::trace_import(#name, ctx, true);  \
        fa_impl_##name(ctx, base);                                            \
        if (fa::rt::g_trace_imports) fa::rt::trace_import(#name, ctx, false); \
    }                                                                         \
    PPC_FUNC(name) { __imp__##name(ctx, base); }                              \
    static void fa_impl_##name(PPCContext& ctx, uint8_t* base)
