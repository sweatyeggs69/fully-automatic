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
| `FA_RENDER_DUMP=dir` | Debug dumps for frames 0-5 and every 30th frame: every draw logged with its textures, vertex buffers, blend state and constants; PNGs of the colour target after each draw, each resolve (as rendered and as it decodes from memory), each bound texture and the front buffer |
| `FA_RENDER_DUMP_TRIGGER=file` | With `FA_RENDER_DUMP`: touch that file while the game runs and the next frame is dumped (checked every 8th swap), then the file is removed. For catching the exact screen someone is looking at |
| `FA_RENDER_DUMP_AFTER=secs` | With `FA_RENDER_DUMP`: dump the first frame at least that many seconds after start |
| `FA_MIPS=0` | Sample only the base level of every texture (default: mip levels 1.. are read from the fetch constant's mip address) |
| `FA_RENDER_DUMP_FRAME=N` | With `FA_RENDER_DUMP`: dump only swap N (all its resolves, targets and textures) instead of the early frames; the 3D menu is around swap 600+ |
| `FA_RENDER_DUMP_DRAWS=A-B` | With `FA_RENDER_DUMP_FRAME`: trace and dump only draws A..B of that frame (numbered from 0). Texture lines carry a mean colour for every format, including cube, 3D and compressed ones |

The window title adds `rendered N, skipped N, shader errors N`. Skipped draws are ones the
renderer cannot handle yet (missing shader, unsupported primitive, no render target).

## Tests

`gpu_shader_test` compiles every opcode and fetch format. `gpu_render_test` runs a PM4 stream
through the command processor and renderer (a vertex coloured triangle, a textured rectangle
list, a clear and a resolve) and checks the resolved pixels. It runs on lavapipe
(`mesa-vulkan-drivers`) and skips itself when there is no Vulkan device.

## Predicated tiling

The game renders 720p in three screen tiles (4x MSAA scene passes, 256 + 256 + 208 rows).
D3D replays the frame once per tile and marks tile-specific packets with the type-3
predicate bit; `SET_BIN_SELECT` / `SET_BIN_MASK` pick the tile. The command processor skips
predicated packets whose tile is not selected, so each tile gets its own window offset and
resolve destination. EDRAM targets ignore the MSAA mode, so the tiled 4x passes and the 1x
passes that read the same tile share pixels.

## Not done yet

MSAA (targets are single sampled), EDRAM aliasing between formats sharing memory, tessellation,
memory export, packed mip tails (the smallest levels, 16 texels or less; sampling clamps to the last full level) and mips of cube, 3D and linear textures, and 3D textures (sampled as 2D). Each
resolve and swap waits for the GPU, which is simple but not fast.
