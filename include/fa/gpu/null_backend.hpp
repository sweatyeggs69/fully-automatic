#pragma once
#include <vector>
#include "fa/gpu/backend.hpp"

namespace fa::gpu {

// Records calls instead of rendering. Used by tests and headless runs.
class NullBackend : public Backend {
public:
    struct SwapRecord { uint32_t addr, width, height; };
    std::vector<DrawCall> draws;
    std::vector<SwapRecord> swaps;
    int resolves = 0;

    void Draw(const DrawCall& d, const RegisterFile&) override { draws.push_back(d); }
    void Resolve(const RegisterFile&) override { ++resolves; }
    void Swap(uint32_t addr, uint32_t w, uint32_t h, const RegisterFile&) override { swaps.push_back({addr, w, h}); }
};

}  // namespace fa::gpu
