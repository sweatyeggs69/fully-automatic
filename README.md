# fully-automatic
Full Auto (Xbox 360) running natively on Linux via static recompilation.

## Plan
1. Read the game ISO (XDVDFS) and extract `default.xex`  (done: `faiso`)
2. Parse the XEX header                                   (basic info done)
3. Decrypt/decompress the XEX image and feed it to XenonRecomp (PPC -> C++)
4. Convert shaders with XenosRecomp
5. Build a native runtime (Vulkan, SDL2 window, audio, input) around the recompiled code

`prototype/` holds the earlier Vulkan/GLFW bootloader sketch. It does not build and is not part of the CMake targets.

## Build and test
    cmake -S . -B build && cmake --build build && ctest --test-dir build

Needs SDL2 and FFmpeg (libavcodec/libavutil): `sudo pacman -S --needed sdl2 ffmpeg` on CachyOS.
The game window also needs Vulkan: `sudo pacman -S --needed vulkan-headers vulkan-icd-loader` plus
your driver (`vulkan-radeon`, `nvidia-utils` or `vulkan-intel`). Without them everything still builds
and the game runs headless.

## Game window
See [docs/window.md](docs/window.md). `./build/fawindow` opens the window with a moving test
pattern, which checks SDL2 + Vulkan without the game.

## Files, input and audio
See [docs/hle.md](docs/hle.md): `game:\` over the ISO, save folders, controllers and keyboard,
XAudio output, the XMA decoder, and `fawave` for listing/decoding the game's wave banks.

    ./build/fawave game.iso banks

## Tool
    faiso game.iso list
    faiso game.iso extract default.xex ./default.xex
    faiso game.iso xex

## Recompilation steps (local, needs your own game copy)
    ./build/faiso game.iso extract default.xex private/default.xex
    git clone --recursive https://github.com/hedge-dev/XenonRecomp third_party/XenonRecomp
    cmake -S third_party/XenonRecomp -B third_party/XenonRecomp/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build third_party/XenonRecomp/out --target XenonAnalyse XenonRecomp
    ./third_party/XenonRecomp/out/XenonAnalyse/XenonAnalyse private/default.xex private/switch_tables.toml
    cat config/extra_switch_tables.toml >> private/switch_tables.toml
    mkdir -p ppc
    ./third_party/XenonRecomp/out/XenonRecomp/XenonRecomp config/fullauto.toml third_party/XenonRecomp/XenonUtils/ppc_context.h

## Patching XenonRecomp
Full Auto uses a few PowerPC instructions XenonRecomp does not implement (16-bit vector ops, `cror`/`crorc`, `bso`/`bns`).
Apply the patches before recompiling, in this order (each one builds on the ones above it, so
`xenonrecomp-keep-entries.patch` must come after `xenonrecomp-mulhd-fix.patch`):

    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-vmx-halfword.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-jumptables-and-more-vmx.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-vmx-round3.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-diag.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-overlap.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-mulhd-fix.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-keep-entries.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-switch-u32.patch
    cmake --build third_party/XenonRecomp/out --target XenonRecomp

`xenonrecomp-keep-entries.patch` stops the jump table merge from deleting real function entries
(called, in `.pdata`, or referenced as code pointers) that it swallows, adds functions that are only
reached through pointers (vtables, callbacks), and allows `size = 0` in the `functions` list of
`config/fullauto.toml`. XenonRecomp prints how many it added and kept. An indirect call that still
finds no entry crashes in the runtime: add that address to `functions` and recompile.

`xenonrecomp-switch-u32.patch` makes jump tables switch on the low 32 bits of the index register, like the
PowerPC code does (`rlwinm ...,0,29`). With `.u64` and `__builtin_unreachable()` as default, clang indexed its
host table with the full 64-bit register after only a 32-bit bound check, so stale upper bits jumped to a wild address.

## Check that the generated C++ compiles (slow, needs clang)
    cmake -S tools/recomp-build -B tools/recomp-build/out -G Ninja -DCMAKE_CXX_COMPILER=clang++
    cmake --build tools/recomp-build/out -j4
