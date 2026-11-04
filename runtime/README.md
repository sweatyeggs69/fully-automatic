# runtime

Runs the XenonRecomp output natively: guest memory, XEX loading, kernel layer, threads.

## Build and test

    cmake -S runtime -B runtime/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build runtime/out && ctest --test-dir runtime/out                     # unit tests, no game needed
    cmake --build runtime/out --target fullauto                                   # needs ppc/ from XenonRecomp
    ./runtime/out/fullauto /path/to/Full.Auto.iso      # or private/default.xex

Needs `third_party/XenonRecomp` (with submodules) as described in the top level README.

## Layout

- `src/memory.cpp`: one 4 GiB mmap, guest address = offset from `g_base`. Heap ranges are in `include/fa/rt/memory.hpp`.
- `src/loader.cpp`: decodes default.xex with XenonUtils, maps it at 0x82000000, fills the indirect-call function table from `PPCFuncMappings`, links kernel data imports (`KeTimeStampBundle`, ...).
- `src/kernel.cpp`: handles, dispatcher objects, waits, guest threads (one host thread each, own `PPCContext`, PCR in r13, TLS).
- `src/timebase.cpp`: XenonRecomp emits mftb as `__rdtsc()` (host TSC, GHz). At startup the runtime patches those inlined sites in the executable to jump to trampolines that rescale the TSC to the 50 MHz guest timebase, so no ppc/ recompile is needed. `FA_TIMEBASE=off` turns it off, `FA_TIMEBASE_HZ=<n>` changes the guest rate. The startup log says how many sites were patched.
- `src/kernel/*.cpp`: kernel imports (memory, events/semaphores/mutants/critical sections, threads, TLS, time, misc).
- `tools/gen_import_stubs.py`: at build time, creates a logging no-op for every import the game uses that nobody implemented, so gaps show up in the log as `unimplemented import: Name`.

## Adding an import

Imports are weak functions in the recompiled code named after the export. Define the same name:

    FA_IMPORT(NtClose) { ret32(ctx, ...); }     // args: arg32(ctx, n), result: ret32(ctx, v)

Other layers add files under `src/gpu/`, `src/audio/`, `src/input/`, `src/vfs/`; CMake picks them up automatically.

## For other layers

- Guest memory: `fa::rt::g_base`, `ld8/16/32/64`, `st8/16/32/64` (big-endian), `g_mem.malloc/mfree` (zeroed guest heap), `g_mem.alloc(Heap::Physical, ...)`.
- Physical addresses: `MmGetPhysicalAddress` returns `addr & 0x1FFFFFFF`; physical allocations live at guest `0xA0000000 + phys`.
- Calling into the guest from a host thread: `attach_guest_thread(name)` once, then `invoke_guest(func, arg)`.
- The XMA/APU register page at 0x7FEA0000 is plain committed memory.

## Known limits

- One global lock for all waits (fine for now, split if profiling shows contention).
- `NtSuspendThread` cannot stop an already running thread, only tracks the count (suspended-at-create works).
- Named kernel objects are not shared by name; the SEH/unwind imports are not implemented.
- No file imports yet (`NtCreateFile` and friends): to be wired to `fa::hle::services().vfs` (see `docs/hle.md`).

## HLE services

`main.cpp` starts `fa::hle` services (files, input, audio, XMA) after guest memory is up, with hooks
from `include/fa/rt/hle_hooks.hpp`. XAudio/XMA imports are in `src/audio/`, XamInput imports in
`src/input/`. Needs SDL2 and FFmpeg dev packages. `hle_import_test` exercises them without the game.
