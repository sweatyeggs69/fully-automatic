#ifndef VFS_HPP
#define VFS_HPP

#include <string>
#include <map>
#include <iostream>

namespace VFS {
    class Loader {
    public:
        static Loader& GetInstance();
        bool Initialize(const std::string& manifest_path);
        std::string GetPhysicalPath(const std::string& virtual_path);
        void ListAssets();

    private:
        Loader() = default;
        std::string base_path;
        std::map<std::string, std::string> manifest_map;
    };
}

#endif
