#pragma once
// Bridge between the Vd/GPU glue (kernel/imports_vd.cpp) and the window (main.cpp).
#include "fa/gpu/frame_backend.hpp"

namespace fa::rt {
// The backend the command processor draws into. The window thread enables it and takes frames
// from its mailbox; while nobody does, swaps cost nothing.
fa::gpu::FrameBackend& gpu_frame_backend();
// Guest memory as the GPU sees it (physical addresses), for a renderer.
fa::gpu::GuestMemory& gpu_memory();
// Replaces the backend the command processor draws into (default: gpu_frame_backend()). Must be
// called before the game creates its ring buffer; the backend should forward to
// gpu_frame_backend() so the window still gets frames.
void set_gpu_backend(fa::gpu::Backend* backend);
}  // namespace fa::rt
