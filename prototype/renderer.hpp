#ifndef RENDERER_HPP
#define RENDERER_HPP

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <vulkan/vulkan.h>

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool Init(const std::string& window_title, int width, int height);
    bool LoadTexture(const std::string& path, void* rawData, uint32_t width, uint32_t height, int format);
    void RenderTexture(const std::string& path);
    void create_surface();
    void create_swapchain();
    void create_render_pass();
    void create_command_buffers();
    void create_framebuffers();
    void create_graphics_pipeline();
    void create_logical_device();
    void create_render_buffers();
    void Run();
    void Shutdown();

private:
    VkInstance instance;
    VkDevice device;
    VkSwapchainKHR swapchain;
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkRenderPass render_pass;
    VkPipeline graphics_pipeline;
    std::map<std::string, VkImageView*> texture_registry;
};

#endif
