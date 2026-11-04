# Game window and frame presentation

`fullauto` opens a window (SDL2) and presents with Vulkan. The process main thread owns the
window: it pumps SDL events (forwarding controller hotplug to the input layer, so keyboard
input works while the window has focus) and shows frames. The guest main thread runs on its own
host thread with a 64 MB stack.

## Where frames come from

There is no rasterizer yet (draws and shaders are the next step), so `FrameBackend`
(`src/gpu/frame_backend.cpp`) turns each swap into an RGBA8 picture on the CPU:

- **frontbuffer**: `VdSwap` loads the front buffer's texture fetch constant into fetch slot 0
  and emits `XE_SWAP`. The backend reads that fetch constant (format, endian, tiling, pitch,
  swizzle), untiles and endian swaps the front buffer from guest memory and decodes it. Until
  draws and EDRAM resolves are implemented this stays black, because nothing writes it.
- **texture** (debug view): the largest 2D texture bound by any draw of the frame. Splash
  screens, menus and movie frames are usually drawn as one full screen textured quad, so this
  shows them before shaders exist. Movies decoded to YUV planes show as a grey luma picture.

`FA_PRESENT=auto` (default) shows the front buffer when it holds anything, else the texture
view. `FA_PRESENT=frontbuffer` or `FA_PRESENT=texture` forces one.

Decoded formats: `k_8`, `k_8_8`, `k_5_6_5`, `k_8_8_8_8`, `k_2_10_10_10`, DXT1, DXT2/3, DXT4/5
(and their `_AS_16_16_16_16` variants). Untiling uses the Xenos 2D tiled layout.

Frames go through a one-slot mailbox: the GPU thread skips decoding while the window has not
picked up the previous frame, so presenting never stalls the command processor.

## Presentation

`src/gpu/vulkan/presenter.cpp`: one Vulkan device and queue, a FIFO (vsync) swapchain, a
host-visible staging buffer and one image. Each frame is copied into the image and blitted into
the swapchain image, scaled with the aspect ratio kept (black bars). No shaders are involved.

## Keys and switches

- F11 or Alt+Enter: toggle fullscreen. Closing the window exits the game.
- `FA_HEADLESS=1`: no window (old behaviour). Also used automatically when the window or
  Vulkan cannot start (the log says why).
- `FA_FULLSCREEN=1`: start fullscreen. `FA_VSYNC=0`: mailbox or immediate present mode.
- `FA_VK_VALIDATION=1`: load `VK_LAYER_KHRONOS_validation` (`vulkan-validation-layers`).
- `FA_FB_TILED=0|1`, `FA_FB_ENDIAN=0..3`: override the front buffer's tiling and endian swap.
- Build with `-DFA_NO_WINDOW=ON` to leave the presenter out.

The window title shows swaps per second, draws in the last frame and the resolve count.
The log prints `[present] front buffer ...` once with the front buffer's format.

## Test without the game

    ./build/fawindow              # moving colour bars until you close it
    ./build/fawindow --frames 300 # quits by itself

Red should rise from left to right, green from top to bottom, with moving blue bars.
