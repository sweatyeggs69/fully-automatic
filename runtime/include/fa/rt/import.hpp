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

#define FA_IMPORT(name) PPC_FUNC(name)
