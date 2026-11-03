#ifndef RENDERER_HPP
#define RENDERER_HPP

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool Init(const std::string& window_title, int width, int height);
    void Run();
    void Shutdown();

private:
    void create_instance();
    void create_window(int width, int height, const std::string& title);
    void create_surface();
    void create_logical_device();
    void create_swapchain();
    void create_render_pass();
    void create_command_buffers();
    void create_framebuffers();
    void create_graphics_pipeline();
    void create_render_buffers();

    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    bool LoadTexture(const std::string& path, void* rawData, uint32_t width, uint32_t height, int format);
    void RenderTexture(const std::string& path);

private:
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    VkQueue present_queue;
    VkSwapchainKHR swapchain;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_image_views;
    VkRenderPass render_pass;
    VkPipeline graphics_pipeline;
    VkFramebuffer framebuffers[MAX_FRAMES_IN_FLIGHT];
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore image_available_semaphores[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore render_finished_semaphores[MAX_FRAMES_IN_FLIGHT];
    VkFence in_flight_fences[MAX_FRAMES_IN_FLIGHT];
    
    std::map<std::string, VkImageView> texture_registry;
    
    size_t current_frame = 0;
    const int MAX_FRAMES_IN_FLIGHT = 2;
    
    // Helper for Vulkan instance creation
    static VkResult create_instance_info(VkInstance* instance, const std::string& app_name);
};

#endif
