#pragma once
// Virtual file system for the recompiled game.
//
// Maps Xbox 360 device paths ("game:\Media\x.xwb", "d:\...", "\Device\Cdrom0\...",
// "cache:\...", "hdd:\...") to either the game ISO (read-only XDVDFS image), an
// extracted game folder, or a writable folder on the host.
//
// This layer is host-side only. It knows nothing about guest memory, NT handles,
// OBJECT_ATTRIBUTES or IO_STATUS_BLOCK: the kernel layer (runtime/src/kernel)
// owns those and calls into this API. Results are NTSTATUS codes so they can be
// handed straight back to the game.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fa {
class XdvdfsImage;
}

namespace fa::hle {

// NTSTATUS values used by the file layer.
namespace status {
constexpr uint32_t kSuccess = 0x00000000;
constexpr uint32_t kNoMoreFiles = 0x80000006;
constexpr uint32_t kInvalidParameter = 0xC000000D;
constexpr uint32_t kEndOfFile = 0xC0000011;
constexpr uint32_t kAccessDenied = 0xC0000022;
constexpr uint32_t kObjectNameInvalid = 0xC0000033;
constexpr uint32_t kObjectNameNotFound = 0xC0000034;
constexpr uint32_t kObjectNameCollision = 0xC0000035;
constexpr uint32_t kObjectPathNotFound = 0xC000003A;
constexpr uint32_t kMediaWriteProtected = 0xC00000A2;
constexpr uint32_t kFileIsADirectory = 0xC00000BA;
constexpr uint32_t kNotADirectory = 0xC0000103;
constexpr uint32_t kDirectoryNotEmpty = 0xC0000101;
constexpr uint32_t kUnsuccessful = 0xC0000001;
}  // namespace status

// NtCreateFile CreateDisposition values.
enum class Disposition : uint32_t {
    Supersede = 0,
    Open = 1,
    Create = 2,
    OpenIf = 3,
    Overwrite = 4,
    OverwriteIf = 5,
};

// IO_STATUS_BLOCK.Information values for NtCreateFile.
namespace create_info {
constexpr uint32_t kSuperseded = 0;
constexpr uint32_t kOpened = 1;
constexpr uint32_t kCreated = 2;
constexpr uint32_t kOverwritten = 3;
constexpr uint32_t kExists = 4;
constexpr uint32_t kDoesNotExist = 5;
}  // namespace create_info

// NtCreateFile CreateOptions bits we look at.
namespace create_options {
constexpr uint32_t kDirectoryFile = 0x00000001;
constexpr uint32_t kNonDirectoryFile = 0x00000040;
constexpr uint32_t kDeleteOnClose = 0x00001000;
}  // namespace create_options

// FILE_ATTRIBUTE_* bits.
namespace attr {
constexpr uint32_t kReadOnly = 0x01;
constexpr uint32_t kDirectory = 0x10;
constexpr uint32_t kNormal = 0x80;
}  // namespace attr

struct FileInfo {
    std::string name;           // last path component, original case
    uint64_t size = 0;          // EndOfFile
    uint64_t allocation_size = 0;
    uint32_t attributes = attr::kNormal;
    uint64_t create_time = 0;   // FILETIME (100 ns since 1601-01-01)
    uint64_t access_time = 0;
    uint64_t write_time = 0;
    uint64_t change_time = 0;
    bool is_dir() const { return (attributes & attr::kDirectory) != 0; }
};

// Access mask bits that imply writing.
constexpr uint32_t kWriteAccessMask = 0x40000000 /*GENERIC_WRITE*/ | 0x10000000 /*GENERIC_ALL*/ |
                                      0x0002 /*FILE_WRITE_DATA*/ | 0x0004 /*FILE_APPEND_DATA*/ |
                                      0x00010000 /*DELETE*/;

struct OpenRequest {
    uint32_t access = 0x80000000;   // GENERIC_READ
    Disposition disposition = Disposition::Open;
    uint32_t options = 0;           // create_options bits
    bool wants_write() const { return (access & kWriteAccessMask) != 0; }
};

// An open file or directory.
class VfsFile {
public:
    virtual ~VfsFile() = default;
    virtual FileInfo info() = 0;

    // Positional I/O. Returns bytes transferred; 0 at or past end of file.
    virtual size_t read(uint64_t offset, void* buf, size_t n) = 0;
    virtual uint32_t write(uint64_t offset, const void* buf, size_t n, size_t& written) {
        (void)offset; (void)buf; (void)n; written = 0;
        return status::kMediaWriteProtected;
    }
    virtual uint32_t set_size(uint64_t size) { (void)size; return status::kMediaWriteProtected; }
    virtual uint32_t flush() { return status::kSuccess; }

    // Directory enumeration in NtQueryDirectoryFile style. The pattern is only
    // used on the first call or when restart is true (as on real hardware).
    // Returns kSuccess with one entry, or kNoMoreFiles / kObjectNameNotFound.
    uint32_t next_entry(const std::string* pattern, bool restart, FileInfo& out);

    const std::string& guest_path() const { return guest_path_; }
    void set_guest_path(std::string p) { guest_path_ = std::move(p); }

protected:
    // Directory children (empty for files).
    virtual std::vector<FileInfo> list_children() { return {}; }

private:
    std::string guest_path_;
    std::string pattern_ = "*";
    std::vector<FileInfo> listing_;
    size_t cursor_ = 0;
    bool listed_ = false;
};

struct VolumeInfo {
    std::string label;
    uint32_t serial = 0x12345678;
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    uint32_t bytes_per_sector = 2048;
    uint32_t sectors_per_unit = 1;
    std::string fs_name = "XDVDFS";
};

// A mounted device. Paths given to a device are relative to its root,
// use '\' separators, contain no "." or ".." parts and no leading '\'.
class Device {
public:
    virtual ~Device() = default;
    virtual uint32_t open(const std::string& rel, const OpenRequest& req,
                          std::shared_ptr<VfsFile>& out, uint32_t& info) = 0;
    virtual uint32_t stat(const std::string& rel, FileInfo& out) = 0;
    virtual uint32_t remove(const std::string& rel) { (void)rel; return status::kMediaWriteProtected; }
    virtual uint32_t rename(const std::string& from, const std::string& to) {
        (void)from; (void)to; return status::kMediaWriteProtected;
    }
    virtual bool read_only() const = 0;
    virtual VolumeInfo volume() = 0;
};

// Read-only device backed by an XDVDFS image (the game ISO).
std::unique_ptr<Device> make_iso_device(std::shared_ptr<XdvdfsImage> image);
// Device backed by a folder on the host (extracted game folder, or save data).
// The folder is created if writable and missing. Lookups are case-insensitive.
std::unique_ptr<Device> make_host_device(const std::string& dir, bool read_only);

class Vfs {
public:
    // Mounts a device under a name such as "game:", "d:", "\Device\Cdrom0".
    // Names are case-insensitive. A later mount with the same name replaces it.
    void mount(const std::string& name, std::shared_ptr<Device> dev);
    void unmount(const std::string& name);
    bool is_mounted(const std::string& name) const;

    // Adds a symbolic link, e.g. "\??\game:" -> "\Device\Cdrom0".
    // Used by ObCreateSymbolicLink and for default aliases.
    void add_link(const std::string& from, const std::string& to);
    void remove_link(const std::string& from);

    uint32_t open(const std::string& guest_path, const OpenRequest& req,
                  std::shared_ptr<VfsFile>& out, uint32_t& info);
    uint32_t stat(const std::string& guest_path, FileInfo& out);
    uint32_t remove(const std::string& guest_path);
    uint32_t rename(const std::string& from, const std::string& to);
    uint32_t volume(const std::string& guest_path, VolumeInfo& out);

    // Splits a guest path into (device, device-relative path) after
    // normalisation and link resolution. Exposed for tests and logging.
    bool resolve(const std::string& guest_path, std::shared_ptr<Device>& dev, std::string& rel) const;

    // Sets up the standard mounts: the game source (ISO image or extracted
    // folder) as game:, d:, \Device\Cdrom0, and the writable data folder as
    // cache:, hdd:, \Device\Harddisk0\Partition1 (subfolders cache/ and hdd/).
    // Returns false and sets err if the game source cannot be opened.
    bool mount_defaults(const std::string& game_source, const std::string& data_dir, std::string& err);

private:
    struct Mount { std::string key; std::shared_ptr<Device> dev; };
    std::string apply_links(const std::string& norm) const;

    mutable std::mutex mu_;
    std::vector<Mount> mounts_;                              // key is lower-case
    std::vector<std::pair<std::string, std::string>> links_; // lower-case from -> to
};

// Normalises a guest path: '/' -> '\', removes "\??\" and duplicate
// separators, resolves "." and "..". Keeps case.
std::string normalize_guest_path(const std::string& p);
// Case-insensitive NT wildcard match supporting '*', '?', and the DOS
// wildcards '<', '>', '"'.
bool wildcard_match(const std::string& pattern, const std::string& name);
// $XDG_DATA_HOME/fully-automatic, or ~/.local/share/fully-automatic.
std::string default_data_dir();
// $XDG_CONFIG_HOME/fully-automatic, or ~/.config/fully-automatic.
std::string default_config_dir();

}  // namespace fa::hle
