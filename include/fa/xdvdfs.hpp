#pragma once
// Minimal read-only XDVDFS (Xbox 360 game disc) reader.
#include <cstdint>
#include <string>
#include <vector>

namespace fa {

struct IsoEntry {
    std::string path;      // e.g. "Media/stream.xwb"
    uint64_t offset = 0;   // absolute byte offset in the image
    uint32_t size = 0;
    bool is_dir = false;
};

class XdvdfsImage {
public:
    // Opens the image and locates the game partition. Returns false and sets error().
    bool open(const std::string& path);
    const std::string& error() const { return error_; }

    // Walks the whole directory tree.
    std::vector<IsoEntry> list();

    // Finds a file by case-insensitive path, e.g. "default.xex".
    bool find(const std::string& path, IsoEntry& out);

    // Reads a whole file's bytes.
    bool read(const IsoEntry& e, std::vector<uint8_t>& out);
    bool extract(const IsoEntry& e, const std::string& dest);

    // Reads up to n bytes of a file starting at offset. Safe to call from
    // several threads at once. Returns the number of bytes read.
    size_t read_range(const IsoEntry& e, uint64_t offset, void* buf, size_t n) const;

    XdvdfsImage() = default;
    XdvdfsImage(const XdvdfsImage&) = delete;
    XdvdfsImage& operator=(const XdvdfsImage&) = delete;
    ~XdvdfsImage();

private:
    void walk(uint64_t dir_off, uint32_t dir_size, const std::string& prefix,
              std::vector<IsoEntry>& out, int depth);
    bool read_at(uint64_t off, void* buf, size_t n) const;

    int fd_ = -1;
    uint64_t part_off_ = 0;
    uint32_t root_sector_ = 0, root_size_ = 0;
    std::string error_;
};

}  // namespace fa
