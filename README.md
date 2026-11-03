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
