#ifndef VFS_H
#define VFS_H

#include <string>
#include <map>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json/json.hpp> // We'll use nlohmann/json

namespace VFS {
    struct AssetInfo {
        std::string type;
        std::string physical_path;
    };

    class Loader {
    public:
        bool Initialize(const std::string& manifest_path);
        std::string GetPhysicalPath(const std::string& virtual_path);
        void ListAssets();

    private:
        std::map<std::string, std::string> manifest_map;
        std::string base_path;
    };

    static Loader& GetInstance() {
        static Loader instance;
        return instance;
    }
};

#endif
