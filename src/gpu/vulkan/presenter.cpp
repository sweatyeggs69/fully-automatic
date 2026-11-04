// SDL2 window + Vulkan swapchain presenter. See fa/gpu/presenter.hpp.
#include "fa/gpu/presenter.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fa::gpu {

namespace {
const char* VkErr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: return "VkResult(other)";
    }
}
}  // namespace

struct Presenter::Impl {
    Options opt;
    SDL_Window* win = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkFormat scFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D scExtent{};
    std::vector<VkImage> scImages;
    std::vector<VkSemaphore> doneSems;  // one per swapchain image
    bool scDirty = true;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquireSem = VK_NULL_HANDLE;

    // The current frame: device-local image + host-visible staging buffer.
    VkImage src = VK_NULL_HANDLE;
    VkDeviceMemory srcMem = VK_NULL_HANDLE;
    uint32_t srcW = 0, srcH = 0;
    bool haveFrame = false;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
    VkDeviceSize stagingSize = 0;
    bool linearFilter = true;

    std::string err;

    bool Fail(const char* what, VkResult r) {
        char b[256];
        std::snprintf(b, sizeof b, "%s failed: %s (%d)", what, VkErr(r), int(r));
        err = b;
        std::fprintf(stderr, "[vulkan] %s\n", b);
        return false;
    }

    int FindMemory(uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) return int(i);
        return -1;
    }

    bool CreateInstance() {
        unsigned n = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(win, &n, nullptr)) { err = SDL_GetError(); return false; }
        std::vector<const char*> exts(n);
        SDL_Vulkan_GetInstanceExtensions(win, &n, exts.data());
        std::vector<const char*> layers;
        if (opt.validation) {
            uint32_t lc = 0;
            vkEnumerateInstanceLayerProperties(&lc, nullptr);
            std::vector<VkLayerProperties> lp(lc);
            vkEnumerateInstanceLayerProperties(&lc, lp.data());
            for (auto& l : lp)
                if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) layers.push_back("VK_LAYER_KHRONOS_validation");
            if (layers.empty()) std::fprintf(stderr, "[vulkan] validation layer not installed (vulkan-validation-layers)\n");
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "fullauto";
        app.pEngineName = "fully-automatic";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        ci.enabledLayerCount = uint32_t(layers.size());
        ci.ppEnabledLayerNames = layers.data();
        VkResult r = vkCreateInstance(&ci, nullptr, &inst);
        if (r != VK_SUCCESS) return Fail("vkCreateInstance", r);
        if (!SDL_Vulkan_CreateSurface(win, inst, &surf)) { err = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError(); return false; }
        return true;
    }

    bool PickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst, &n, nullptr);
        if (!n) { err = "no Vulkan devices (is vulkan-radeon / nvidia-utils / vulkan-intel installed?)"; return false; }
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst, &n, devs.data());
        int bestScore = -1;
        for (auto d : devs) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qp(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qp.data());
            for (uint32_t q = 0; q < qn; q++) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, q, surf, &present);
                if (!present || !(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                          : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                if (score > bestScore) { bestScore = score; phys = d; qfam = q; props = p; }
                break;
            }
        }
        if (!phys) { err = "no Vulkan device can present to this window"; return false; }
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        linearFilter = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        return true;
    }

    bool CreateDevice() {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qci;
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = &ext;
        VkResult r = vkCreateDevice(phys, &ci, nullptr, &dev);
        if (r != VK_SUCCESS) return Fail("vkCreateDevice", r);
        vkGetDeviceQueue(dev, qfam, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = qfam;
        if ((r = vkCreateCommandPool(dev, &pci, nullptr, &pool)) != VK_SUCCESS) return Fail("vkCreateCommandPool", r);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if ((r = vkAllocateCommandBuffers(dev, &ai, &cb)) != VK_SUCCESS) return Fail("vkAllocateCommandBuffers", r);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if ((r = vkCreateFence(dev, &fci, nullptr, &fence)) != VK_SUCCESS) return Fail("vkCreateFence", r);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if ((r = vkCreateSemaphore(dev, &sci, nullptr, &acquireSem)) != VK_SUCCESS) return Fail("vkCreateSemaphore", r);
        return true;
    }

    void DestroySwapchainSems() {
        for (auto s : doneSems) vkDestroySemaphore(dev, s, nullptr);
        doneSems.clear();
    }

    // Returns false on a fatal error; a zero-sized (minimised) window leaves scDirty set.
    bool CreateSwapchain() {
        vkDeviceWaitIdle(dev);
        VkSurfaceCapabilitiesKHR caps;
        VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
        if (r != VK_SUCCESS) return Fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu) {
            int w = 0, h = 0;
            SDL_Vulkan_GetDrawableSize(win, &w, &h);
            ext.width = std::clamp(uint32_t(w), caps.minImageExtent.width, caps.maxImageExtent.width);
            ext.height = std::clamp(uint32_t(h), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (ext.width == 0 || ext.height == 0) { scDirty = true; return true; }
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            err = "the swapchain cannot be a transfer destination on this driver";
            return false;
        }

        uint32_t fn = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &fn, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fn);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &fn, fmts.data());
        // UNORM, not SRGB: the front buffer already holds display-gamma values.
        VkSurfaceFormatKHR fmt = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
        for (auto want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32})
            for (auto& f : fmts)
                if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { fmt = f; goto picked; }
    picked:
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, fmt.format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            err = "the swapchain format cannot be a blit destination on this driver";
            return false;
        }

        uint32_t pn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &pn, nullptr);
        std::vector<VkPresentModeKHR> modes(pn);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &pn, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!opt.vsync)
            for (auto want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
                if (std::find(modes.begin(), modes.end(), want) != modes.end()) { mode = want; break; }

        uint32_t count = std::max(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surf;
        ci.minImageCount = count;
        ci.imageFormat = fmt.format;
        ci.imageColorSpace = fmt.colorSpace;
        ci.imageExtent = ext;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                              ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
            for (auto a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                           VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
                if (caps.supportedCompositeAlpha & a) { ci.compositeAlpha = a; break; }
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = sc;
        VkSwapchainKHR next = VK_NULL_HANDLE;
        r = vkCreateSwapchainKHR(dev, &ci, nullptr, &next);
        if (sc) vkDestroySwapchainKHR(dev, sc, nullptr);
        sc = next;
        if (r != VK_SUCCESS) { sc = VK_NULL_HANDLE; return Fail("vkCreateSwapchainKHR", r); }
        scFormat = fmt.format;
        scExtent = ext;
        uint32_t in = 0;
        vkGetSwapchainImagesKHR(dev, sc, &in, nullptr);
        scImages.resize(in);
        vkGetSwapchainImagesKHR(dev, sc, &in, scImages.data());
        DestroySwapchainSems();
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        doneSems.resize(in);
        for (auto& s : doneSems) vkCreateSemaphore(dev, &sci, nullptr, &s);
        scDirty = false;
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[vulkan] swapchain %ux%u, %u images, format %d, %s\n", ext.width, ext.height, in,
                         int(fmt.format), mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "no vsync");
        }
        return true;
    }

    void DestroyFrameResources() {
        if (stagingMap) vkUnmapMemory(dev, stagingMem);
        stagingMap = nullptr;
        if (staging) vkDestroyBuffer(dev, staging, nullptr);
        if (stagingMem) vkFreeMemory(dev, stagingMem, nullptr);
        if (src) vkDestroyImage(dev, src, nullptr);
        if (srcMem) vkFreeMemory(dev, srcMem, nullptr);
        staging = VK_NULL_HANDLE; stagingMem = VK_NULL_HANDLE; src = VK_NULL_HANDLE; srcMem = VK_NULL_HANDLE;
        srcW = srcH = 0;
        stagingSize = 0;
        haveFrame = false;
    }

    bool EnsureFrameResources(uint32_t w, uint32_t h) {
        if (src && w == srcW && h == srcH) return true;
        vkDeviceWaitIdle(dev);
        DestroyFrameResources();
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {w, h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(dev, &ici, nullptr, &src);
        if (r != VK_SUCCESS) return Fail("vkCreateImage", r);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, src, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        int mt = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) mt = FindMemory(req.memoryTypeBits, 0);
        mai.memoryTypeIndex = uint32_t(mt);
        if ((r = vkAllocateMemory(dev, &mai, nullptr, &srcMem)) != VK_SUCCESS) return Fail("vkAllocateMemory(image)", r);
        vkBindImageMemory(dev, src, srcMem, 0);

        stagingSize = VkDeviceSize(w) * h * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = stagingSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ((r = vkCreateBuffer(dev, &bci, nullptr, &staging)) != VK_SUCCESS) return Fail("vkCreateBuffer", r);
        vkGetBufferMemoryRequirements(dev, staging, &req);
        mai.allocationSize = req.size;
        mt = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) { err = "no host-visible coherent memory"; return false; }
        mai.memoryTypeIndex = uint32_t(mt);
        if ((r = vkAllocateMemory(dev, &mai, nullptr, &stagingMem)) != VK_SUCCESS) return Fail("vkAllocateMemory(staging)", r);
        vkBindBufferMemory(dev, staging, stagingMem, 0);
        if ((r = vkMapMemory(dev, stagingMem, 0, VK_WHOLE_SIZE, 0, &stagingMap)) != VK_SUCCESS) return Fail("vkMapMemory", r);
        srcW = w;
        srcH = h;
        return true;
    }

    static void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                        VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    bool Present(const Frame* f) {
        if (scDirty && !CreateSwapchain()) return false;
        if (scDirty) return true;  // minimised

        VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) return Fail("vkWaitForFences", r);

        bool upload = false;
        if (f && f->width && f->height && f->rgba.size() >= size_t(f->width) * f->height * 4) {
            if (!EnsureFrameResources(f->width, f->height)) return false;
            std::memcpy(stagingMap, f->rgba.data(), size_t(stagingSize));
            upload = true;
        }

        uint32_t idx = 0;
        r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { scDirty = true; return true; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return Fail("vkAcquireNextImageKHR", r);
        if (r == VK_SUBOPTIMAL_KHR) scDirty = true;
        vkResetFences(dev, 1, &fence);

        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);

        if (upload) {
            Barrier(cb, src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {srcW, srcH, 1};
            vkCmdCopyBufferToImage(cb, staging, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            Barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            haveFrame = true;
        }

        VkImage dst = scImages[idx];
        Barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue clear{};
        clear.float32[0] = haveFrame ? 0.0f : 0.05f;
        clear.float32[1] = haveFrame ? 0.0f : 0.06f;
        clear.float32[2] = haveFrame ? 0.0f : 0.10f;
        clear.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        if (haveFrame) {
            // Scale to fit, keep the aspect ratio, centre (letterbox / pillarbox).
            double sx = double(scExtent.width) / srcW, sy = double(scExtent.height) / srcH, s = std::min(sx, sy);
            int32_t w = std::max<int32_t>(1, int32_t(srcW * s)), h = std::max<int32_t>(1, int32_t(srcH * s));
            int32_t x0 = (int32_t(scExtent.width) - w) / 2, y0 = (int32_t(scExtent.height) - h) / 2;
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(srcW), int32_t(srcH), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {x0, y0, 0};
            blit.dstOffsets[1] = {x0 + w, y0 + h, 1};
            vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        Barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquireSem;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &doneSems[idx];
        if ((r = vkQueueSubmit(queue, 1, &si, fence)) != VK_SUCCESS) return Fail("vkQueueSubmit", r);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &doneSems[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) scDirty = true;
        else if (r != VK_SUCCESS) return Fail("vkQueuePresentKHR", r);
        return true;
    }

    ~Impl() {
        if (dev) {
            vkDeviceWaitIdle(dev);
            DestroyFrameResources();
            DestroySwapchainSems();
            if (sc) vkDestroySwapchainKHR(dev, sc, nullptr);
            if (acquireSem) vkDestroySemaphore(dev, acquireSem, nullptr);
            if (fence) vkDestroyFence(dev, fence, nullptr);
            if (pool) vkDestroyCommandPool(dev, pool, nullptr);
            vkDestroyDevice(dev, nullptr);
        }
        if (surf) vkDestroySurfaceKHR(inst, surf, nullptr);
        if (inst) vkDestroyInstance(inst, nullptr);
        if (win) SDL_DestroyWindow(win);
    }
};

Presenter::Presenter() : p_(std::make_unique<Impl>()) {}
Presenter::~Presenter() = default;

bool Presenter::Init(const Options& opt, std::string& err) {
    p_->opt = opt;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { err = std::string("SDL video init: ") + SDL_GetError(); return false; }
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (opt.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    p_->win = SDL_CreateWindow(opt.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.width, opt.height, flags);
    if (!p_->win) { err = std::string("SDL_CreateWindow: ") + SDL_GetError(); return false; }
    if (!p_->CreateInstance() || !p_->PickDevice() || !p_->CreateDevice() || !p_->CreateSwapchain()) {
        err = p_->err.empty() ? "Vulkan init failed" : p_->err;
        return false;
    }
    std::fprintf(stderr, "[vulkan] presenting on %s (driver %u.%u.%u)\n", p_->props.deviceName,
                 VK_API_VERSION_MAJOR(p_->props.apiVersion), VK_API_VERSION_MINOR(p_->props.apiVersion),
                 VK_API_VERSION_PATCH(p_->props.apiVersion));
    return true;
}

bool Presenter::Present(const Frame* f) { return p_->Present(f); }
void Presenter::Resized() { p_->scDirty = true; }
void Presenter::SetTitle(const std::string& t) { if (p_->win) SDL_SetWindowTitle(p_->win, t.c_str()); }
SDL_Window* Presenter::Window() const { return p_->win; }
std::string Presenter::DeviceName() const { return p_->props.deviceName; }

void Presenter::ToggleFullscreen() {
    if (!p_->win) return;
    bool fs = SDL_GetWindowFlags(p_->win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(p_->win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    p_->scDirty = true;
}

void RunWindowLoop(Presenter& p, FrameMailbox& frames, const std::function<void(const SDL_Event&)>& on_event,
                   const std::function<std::string()>& title) {
    using clock = std::chrono::steady_clock;
    Frame frame;
    auto lastPresent = clock::now() - std::chrono::seconds(1), lastTitle = lastPresent;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return;
            if (e.type == SDL_WINDOWEVENT &&
                (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED ||
                 e.window.event == SDL_WINDOWEVENT_RESTORED || e.window.event == SDL_WINDOWEVENT_MAXIMIZED))
                p.Resized();
            if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))))
                p.ToggleFullscreen();
            if (on_event) on_event(e);
        }
        bool got = frames.Take(frame);
        auto now = clock::now();
        if (got || now - lastPresent > std::chrono::milliseconds(100)) {
            if (!p.Present(got ? &frame : nullptr)) {
                std::fprintf(stderr, "[window] presenting failed, closing the window\n");
                return;
            }
            lastPresent = now;
        } else {
            SDL_Delay(1);
        }
        if (title && now - lastTitle > std::chrono::seconds(1)) {
            lastTitle = now;
            p.SetTitle(title());
        }
    }
}

}  // namespace fa::gpu
