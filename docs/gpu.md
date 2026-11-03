# Xenos GPU layer

Status: PM4 command processor done and tested; Vulkan backend and shader translation not started.

## Pieces
- `fa/gpu/guest_memory.hpp`: what the GPU needs from the runtime core (Read32/Write32/HostPtr on guest addresses, host-order values).
- `fa/gpu/command_processor.*`: reads the PM4 ring buffer, tracks the Xenos register file, handles indirect buffers, WAIT_REG_MEM stalls, fences, interrupts. Calls a `Backend` for draws, resolves and swaps.
- `fa/gpu/backend.hpp`: backend interface. `NullBackend` records calls for tests.

## Interface the runtime core must provide
- Implement `GuestMemory` over the guest address space.
- Kernel imports map onto `CommandProcessor`: VdInitializeRingBuffer -> `SetRingBuffer`, VdEnableRingBufferRPtrWriteBack -> `SetReadPtrWriteback`, CP_RB_WPTR MMIO write -> `SetWritePtr` then `Pump()`, VdSetGraphicsInterruptCallback -> `SetInterruptHandler`, VdSwap -> emits XE_SWAP into the ring.
- Run `Pump()` on a dedicated GPU thread. `Status::Blocked` means a WAIT_REG_MEM is pending: poll again shortly.

## Next steps
1. Vulkan backend: window (GLFW/SDL), swapchain, present on `Swap`.
2. Xenos ucode decoder + translation to SPIR-V (XenosRecomp approach), run at draw time from SQ_PGM_START_* registers.
3. Texture fetch constants (tiling, endian, DXT formats), vertex fetch.
4. EDRAM emulation for `Resolve` (render targets as Vulkan images, copy on resolve).
