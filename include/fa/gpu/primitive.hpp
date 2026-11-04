#pragma once
// Turns a Xenos draw (primitive type + DMA / immediate / auto indices) into a plain host index
// list for one of four host primitive classes. Strips, fans, loops and quads become lists here
// on the CPU; rectangle lists stay triangles and get their fourth corner in a geometry shader.
#include <cstdint>
#include <vector>

#include "fa/gpu/backend.hpp"
#include "fa/gpu/guest_memory.hpp"

namespace fa::gpu {

enum class HostPrim : uint32_t { Points = 0, Lines = 1, Triangles = 2, Rects = 3 };

struct HostDraw {
    HostPrim prim = HostPrim::Triangles;
    std::vector<uint32_t> indices;  // final vertex indices (VGT_INDX_OFFSET applied)
    bool supported = true;
};

// Reads the guest index buffer if needed. Returns false when there is nothing to draw.
bool BuildHostDraw(const DrawCall& dc, const RegisterFile& regs, const GuestMemory& mem, HostDraw& out);

}  // namespace fa::gpu
