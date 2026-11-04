# Rendering (Xenos draws on Vulkan)

`VulkanRenderer` (`src/gpu/vulkan/renderer.cpp`) is the real GPU backend. It sits in front of
`FrameBackend`: the command processor hands it every draw, resolve and swap, it renders them on
its own headless Vulkan 1.3 device, and forwards each call to `FrameBackend`, which shows the
front buffer in the window as before.

Build: needs shaderc on top of the window dependencies.

    sudo pacman -S --needed shaderc vulkan-headers vulkan-icd-loader sdl2

CMake prints `Vulkan renderer: on` when it finds it. Without shaderc the game still runs and
the window shows the texture debug view.

## Pipeline

1. **Packets** (`command_processor.cpp`): `IM_LOAD` / `IM_LOAD_IMMEDIATE` load vertex and pixel
   microcode (hashed per load). A draw whose `RB_MODECONTROL` is copy mode (6) is a resolve,
   every other draw goes to `Draw` with the current shaders attached.
2. **Shaders** (`shader_translator.cpp`): Xenos microcode (control flow, all vector and scalar
   ALU ops, vertex and texture fetches) becomes GLSL 450, compiled to SPIR-V with shaderc and
   cached by hash. Vertex data is pulled in the vertex shader from a storage buffer holding the
   raw guest bytes, with the fetch constant's endian swap. `FA_DUMP_SHADERS=<dir>` writes each
   translated shader and its disassembly there.
3. **Primitives** (`primitive.cpp`): strips, fans, quads, polygons and line loops are expanded
   to lists on the CPU (with `VGT_INDX_OFFSET` and primitive reset). Rectangle lists get their
   fourth corner in a geometry shader.
4. **EDRAM**: render targets are Vulkan images keyed by EDRAM base, format, pitch and MSAA.
   Blend, depth/stencil, cull, scissor, colour masks, alpha test and the viewport transform
   come from the registers.
5. **Resolves**: the copy-mode draw's rectangle is copied out of the render target, converted to
   the destination format, tiled and endian swapped into guest memory, then the rectangle is
   cleared if asked. Textures are decoded on the CPU and re-uploaded when their guest bytes
   change, so render-to-texture and the front buffer both go through guest memory.

## Switches

| Variable | Effect |
| --- | --- |
| `FA_RENDERER=0` | Turn the renderer off (front buffer stays black, texture view still works) |
| `FA_RENDER_TRACE=N` | Log the first N draws in detail |
| `FA_DUMP_SHADERS=dir` | Write translated GLSL and disassembly |
| `FA_VK_DEVICE=N` | Pick the Vulkan device (default: first discrete GPU) |
| `FA_VK_VALIDATION=1` | Enable the Khronos validation layer |

The window title adds `rendered N, skipped N, shader errors N`. Skipped draws are ones the
renderer cannot handle yet (missing shader, unsupported primitive, no render target).

## Tests

`gpu_shader_test` compiles every opcode and fetch format. `gpu_render_test` runs a PM4 stream
through the command processor and renderer (a vertex coloured triangle, a textured rectangle
list, a clear and a resolve) and checks the resolved pixels. It runs on lavapipe
(`mesa-vulkan-drivers`) and skips itself when there is no Vulkan device.

## Not done yet

MSAA (targets are single sampled), EDRAM aliasing between formats sharing memory, tessellation,
memory export, mip levels (the base level is sampled), and 3D textures (sampled as 2D). Each
resolve and swap waits for the GPU, which is simple but not fast.
