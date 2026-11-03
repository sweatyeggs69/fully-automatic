# fully-automatic
Full Auto (Xbox 360) running natively on Linux via static recompilation.

## Plan
1. Read the game ISO (XDVDFS) and extract `default.xex`  (done: `faiso`)
2. Parse the XEX header                                   (basic info done)
3. Decrypt/decompress the XEX image and feed it to XenonRecomp (PPC -> C++)
4. Convert shaders with XenosRecomp
5. Build a native runtime (Vulkan, GLFW, audio, input) around the recompiled code

`prototype/` holds the earlier Vulkan/GLFW bootloader sketch. It does not build and is not part of the CMake targets.

## Build and test
    cmake -S . -B build && cmake --build build && ctest --test-dir build

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
    mkdir -p ppc
    ./third_party/XenonRecomp/out/XenonRecomp/XenonRecomp config/fullauto.toml third_party/XenonRecomp/XenonUtils/ppc_context.h

## Patching XenonRecomp
Full Auto uses a few PowerPC instructions XenonRecomp does not implement (16-bit vector ops, `cror`/`crorc`, `bso`/`bns`).
Apply the patch before recompiling:

    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-vmx-halfword.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-jumptables-and-more-vmx.patch
    git -C third_party/XenonRecomp apply ../../patches/xenonrecomp-vmx-round3.patch
    cmake --build third_party/XenonRecomp/out --target XenonRecomp
