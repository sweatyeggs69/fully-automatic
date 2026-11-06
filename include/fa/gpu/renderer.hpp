#pragma once
// VulkanRenderer: the real Xenos backend. Draws go to offscreen Vulkan render targets that stand
// in for EDRAM; resolves copy them back into guest memory (tiled, endian swapped, converted to
// the destination format) exactly where the game expects its textures and front buffer. Swaps
// are then forwarded to FrameBackend, which decodes the front buffer for the window as before.
//
// Shaders: Xenos microcode -> GLSL (shader_translator.cpp) -> SPIR-V (shaderc), cached by hash.
// Vertex data is pulled in the vertex shader from a storage buffer holding the raw guest bytes.
// Textures are decoded on the CPU (texture.cpp) and re-uploaded when their guest bytes change.
//
// Runs on the GPU (command processor) thread with its own headless Vulkan device. Needs Vulkan
// 1.3 (dynamic rendering); geometry shaders are used for rectangle lists when available.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "fa/gpu/backend.hpp"
#include "fa/gpu/guest_memory.hpp"

namespace fa::gpu {

class VulkanRenderer : public Backend {
public:
    struct Options {
        bool validation = false;    // VK_LAYER_KHRONOS_validation
        std::string dumpShaders;    // directory for translated GLSL + disassembly (empty = off)
        int traceDraws = 0;         // log this many draws in detail
        int deviceIndex = -1;       // FA_VK_DEVICE; -1 = prefer a discrete GPU
    };
    struct Stats {
        std::atomic<uint64_t> draws{0}, drawsSkipped{0}, resolves{0}, clears{0}, shadersCompiled{0},
            shaderErrors{0}, pipelines{0}, textureUploads{0}, submits{0};
        // Kernel write tracking: 1 when on; texture checks it answered without hashing; textures it
        // called unchanged that had changed (FA_WW_VERIFY=1 only, should stay 0).
        std::atomic<uint64_t> writeWatch{0}, textureWwClean{0}, textureWwMiss{0};
    };

    // `next` receives every call too (FrameBackend: stats, texture debug view, window frames).
    VulkanRenderer(GuestMemory& mem, Backend* next);
    ~VulkanRenderer() override;
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;

    bool Init(const Options& opt, std::string& err);
    std::string DeviceName() const;
    const Stats& GetStats() const;

    void Draw(const DrawCall& draw, const RegisterFile& regs) override;
    void Resolve(const RegisterFile& regs) override;
    void Swap(uint32_t frontBufferGuestAddr, uint32_t width, uint32_t height, const RegisterFile& regs) override;
    void Sync() override;
    bool WriteWhenDone(uint32_t addr, uint32_t value) override;
    void Idle() override;

    // Submits pending GPU work and waits for it (tests).
    void Flush();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace fa::gpu
