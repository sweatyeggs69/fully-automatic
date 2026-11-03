#include "vfs.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace VFS {

bool Loader::Initialize(const std::string& manifest_path) {
    std::ifstream file(manifest_path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open manifest at " << manifest_path << std::endl;
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Basic manual parsing of the manifest.json (searching for path and type)
        // We are looking for "path": "..." and "type": "..."
        if (line.find("\"path\":") != std::string::npos) {
            size_t start = line.find('"') + 1;
            size_t end = line.find('"', start);
            std::string path = line.substr(start, end - start);
            
            // Find the type in the next few lines or same line
            // For now, we'll just map the path.
            manifest_map[path] = path; // Temporary mapping
        }
    }

    base_path = manifest_path;
    std::cout << "VFS: Loaded manifest from " << manifest_path << std::endl;
    return true;
}

std::string Loader::GetPhysicalPath(const std::string& virtual_path) {
    if (manifest_map.find(virtual_path) != manifest_map.end()) {
        return manifest_map[virtual_path];
    }
    
    // Fallback: if not in manifest, try to find in base assets folder
    // This is a simple search.
    std::cout << "Warning: Virtual path " << virtual_path << " not in manifest." << std::endl;
    return "";
}

void Loader::ListAssets() {
    std::cout << "--- Asset Manifest ---" << std::endl;
    for (auto const& [path, physical] : manifest_map) {
        std::cout << "Virtual: " << path << " -> Physical: " << physical << std::endl;
    }
    std::cout << "-----------------------" << std::endl;
}

} // namespace VFS
