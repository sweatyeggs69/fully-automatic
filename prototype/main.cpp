#include <iostream>
#include <vector>
#include <Khr_surface.h>
#include <glfw/glfw3.h>
#include <vulkan/vulkan.h>

#include "vfs.hpp"
#include <iostream>

int main() {
    std::cout << "--- FullAuto Native Recomp Bootloader ---" << std::endl;

    // 1. Initialize the Virtual File System
    // This points the app to our manifest of 228 assets
    VFS::Loader& vfs = VFS::GetInstance();
    if (!vfs.Initialize("/home/kmcclain/Games/X360/FullAuto-Native/assets/manifest.json")) {
        std::cerr << "Critical Error: Could not load asset manifest!" << std::endl;
        return -1;
    }

    // 3. Perform Visual Boot Check
    std::cout << "Performing Visual Boot Check..." << std::endl;
    
    // Identify the first .pix file in the rtdXenon folder
    std::string testPath = "FullAuto_Extracted_v2/rtdXenon/fa_T511.pix";
    
    Xbox360AssetLoader loader;
    AssetMetadata meta;

    if (loader.LoadHeader(testPath, meta)) {
        std::cout << "SUCCESS: Parsed " << testPath << std::endl;
        std::cout << "-> Resolution: " << meta.width << "x" << meta.height << std::endl;
        std::cout << "-> Format ID: " << (int)meta.format << std::endl;
        std::cout << "-> Data Offset: " << meta.dataOffset << std::endl;
        std::cout << "-> Data Size: " << meta.dataSize << " bytes" << std::endl;

        // Load the raw data from the file
        std::vector<uint8_t> rawData(meta.dataSize);
        if (loader.StreamData(testPath, meta, rawData)) {
            renderer.LoadTexture(testPath, rawData.data(), meta.width, meta.height, (int)meta.format);
            renderer.RenderTexture(testPath);
        }
    } else {
        std::cerr << "FAILURE: Could not parse " << testPath << " - check paths!" << std::endl;
    }

    std::cout << "\nBoot Check Complete. System Ready for Graphics Pipeline." << std::endl;

    // Keep the window open for a moment to see results
    renderer.Run();

    renderer.Shutdown();
    return 0;
}

