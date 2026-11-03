#pragma once
// Connects the HLE services (file system, input, audio, XMA; see
// include/fa/hle/services.hpp) to the runtime's guest memory and threads.
#include "fa/hle/services.hpp"

namespace fa::rt {

// GuestHooks backed by g_mem, attach_guest_thread and invoke_guest.
fa::hle::GuestHooks make_runtime_hooks();

}  // namespace fa::rt
