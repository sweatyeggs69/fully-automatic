#include "fa/hle/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fa/xdvdfs.hpp"

namespace fs = std::filesystem;

namespace fa::hle {

// ---------------------------------------------------------------- helpers

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::vector<std::string> split(const std::string& p) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

std::string normalize_guest_path(const std::string& in) {
    std::string p = in;
    std::replace(p.begin(), p.end(), '/', '\\');
    if (p.rfind("\\??\\", 0) == 0) p = p.substr(4);
    bool absolute = !p.empty() && p[0] == '\\';
    std::vector<std::string> out;
    for (auto& part : split(p)) {
        if (part == ".") continue;
        if (part == "..") {
            // Never climb above the device name (first component).
            if (out.size() > 1) out.pop_back();
            continue;
        }
        out.push_back(part);
    }
    std::string r = absolute ? "\\" : "";
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) r += '\\';
        r += out[i];
    }
    return r;
}

static bool match_impl(const char* p, const char* n) {
    // Iterative glob with backtracking on the last '*'.
    const char* star = nullptr;
    const char* resume = nullptr;
    while (*n) {
        char pc = *p;
        if (pc == '<') pc = '*';
        if (pc == '>') pc = '?';
        if (pc == '"') pc = '.';
        if (pc == '*') {
            star = p++;
            resume = n;
            continue;
        }
        if (pc && (pc == '?' || std::tolower((unsigned char)pc) == std::tolower((unsigned char)*n))) {
            ++p;
            ++n;
            continue;
        }
        if (star) {
            p = star + 1;
            n = ++resume;
            continue;
        }
        return false;
    }
    while (*p == '*' || *p == '<' || *p == '>' || *p == '"') ++p;
    return *p == 0;
}

bool wildcard_match(const std::string& pattern, const std::string& name) {
    if (pattern.empty() || pattern == "*" || pattern == "*.*") return true;
    return match_impl(pattern.c_str(), name.c_str());
}

static std::string env_dir(const char* xdg, const char* fallback_rel) {
    if (const char* x = std::getenv(xdg); x && *x) return std::string(x) + "/fully-automatic";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/" + fallback_rel + "/fully-automatic";
}
std::string default_data_dir() { return env_dir("XDG_DATA_HOME", ".local/share"); }
std::string default_config_dir() { return env_dir("XDG_CONFIG_HOME", ".config"); }

// ---------------------------------------------------------------- VfsFile

uint32_t VfsFile::next_entry(const std::string* pattern, bool restart, FileInfo& out) {
    if (!(info().attributes & attr::kDirectory)) return status::kInvalidParameter;
    bool first = !listed_;
    if (!listed_ || restart) {
        listing_ = list_children();
        std::sort(listing_.begin(), listing_.end(),
                  [](const FileInfo& a, const FileInfo& b) { return lower(a.name) < lower(b.name); });
        cursor_ = 0;
        listed_ = true;
    }
    if ((first || restart) && pattern) pattern_ = *pattern;
    while (cursor_ < listing_.size()) {
        const FileInfo& fi = listing_[cursor_++];
        if (wildcard_match(pattern_, fi.name)) {
            out = fi;
            return status::kSuccess;
        }
    }
    // The first query of a pattern with no match reports "not found".
    return first ? status::kObjectNameNotFound : status::kNoMoreFiles;
}

// ---------------------------------------------------------------- ISO device

namespace {

constexpr uint64_t kIsoTime = 0x01C6B5E0A0000000ull;  // roughly 2006, any fixed value

struct IsoNode {
    IsoEntry entry;
    std::string name;
    std::vector<size_t> children;
};

class IsoDevice;

class IsoFile : public VfsFile {
public:
    IsoFile(std::shared_ptr<XdvdfsImage> img, const IsoNode* node, const std::vector<IsoNode>* nodes)
        : img_(std::move(img)), node_(node), nodes_(nodes) {}
    FileInfo info() override { return to_info(*node_); }
    size_t read(uint64_t offset, void* buf, size_t n) override {
        if (node_->entry.is_dir) return 0;
        return img_->read_range(node_->entry, offset, buf, n);
    }
    static FileInfo to_info(const IsoNode& n) {
        FileInfo fi;
        fi.name = n.name;
        fi.size = n.entry.is_dir ? 0 : n.entry.size;
        fi.allocation_size = (fi.size + 2047) & ~uint64_t(2047);
        fi.attributes = n.entry.is_dir ? (attr::kDirectory | attr::kReadOnly) : attr::kReadOnly;
        fi.create_time = fi.access_time = fi.write_time = fi.change_time = kIsoTime;
        return fi;
    }

protected:
    std::vector<FileInfo> list_children() override {
        std::vector<FileInfo> v;
        for (size_t c : node_->children) v.push_back(to_info((*nodes_)[c]));
        return v;
    }

private:
    std::shared_ptr<XdvdfsImage> img_;
    const IsoNode* node_;
    const std::vector<IsoNode>* nodes_;
};

class IsoDevice : public Device {
public:
    explicit IsoDevice(std::shared_ptr<XdvdfsImage> img) : img_(std::move(img)) {
        nodes_.push_back({});  // root
        nodes_[0].entry.is_dir = true;
        index_[""] = 0;
        for (auto& e : img_->list()) {
            std::string p = e.path;
            std::replace(p.begin(), p.end(), '/', '\\');
            auto slash = p.rfind('\\');
            std::string parent = slash == std::string::npos ? "" : p.substr(0, slash);
            size_t idx = nodes_.size();
            IsoNode n;
            n.entry = e;
            n.name = slash == std::string::npos ? p : p.substr(slash + 1);
            nodes_.push_back(n);
            index_[lower(p)] = idx;
            pending_.emplace_back(lower(parent), idx);
            total_ += e.is_dir ? 0 : e.size;
        }
        // Link children after all nodes exist (list() is sorted, but be safe).
        for (auto& [parent, idx] : pending_) {
            auto it = index_.find(parent);
            if (it != index_.end()) nodes_[it->second].children.push_back(idx);
        }
        pending_.clear();
    }

    uint32_t lookup(const std::string& rel, const IsoNode*& out) const {
        auto it = index_.find(lower(rel));
        if (it != index_.end()) {
            out = &nodes_[it->second];
            return status::kSuccess;
        }
        auto slash = rel.rfind('\\');
        std::string parent = slash == std::string::npos ? "" : rel.substr(0, slash);
        auto pit = index_.find(lower(parent));
        if (pit == index_.end()) return status::kObjectPathNotFound;
        if (!nodes_[pit->second].entry.is_dir) return status::kObjectPathNotFound;
        return status::kObjectNameNotFound;
    }

    uint32_t open(const std::string& rel, const OpenRequest& req, std::shared_ptr<VfsFile>& out,
                  uint32_t& info) override {
        const IsoNode* n = nullptr;
        uint32_t st = lookup(rel, n);
        info = create_info::kDoesNotExist;
        if (st != status::kSuccess) {
            if (req.disposition != Disposition::Open && req.disposition != Disposition::Overwrite)
                return status::kMediaWriteProtected;
            return st;
        }
        if (req.disposition != Disposition::Open && req.disposition != Disposition::OpenIf)
            return status::kMediaWriteProtected;
        if (req.wants_write()) return status::kMediaWriteProtected;
        if ((req.options & create_options::kDirectoryFile) && !n->entry.is_dir) return status::kNotADirectory;
        if ((req.options & create_options::kNonDirectoryFile) && n->entry.is_dir) return status::kFileIsADirectory;
        out = std::make_shared<IsoFile>(img_, n, &nodes_);
        info = create_info::kOpened;
        return status::kSuccess;
    }

    uint32_t stat(const std::string& rel, FileInfo& out) override {
        const IsoNode* n = nullptr;
        uint32_t st = lookup(rel, n);
        if (st == status::kSuccess) out = IsoFile::to_info(*n);
        return st;
    }

    bool read_only() const override { return true; }
    VolumeInfo volume() override {
        VolumeInfo v;
        v.label = "FULLAUTO";
        v.total_bytes = total_;
        v.free_bytes = 0;
        return v;
    }

private:
    std::shared_ptr<XdvdfsImage> img_;
    std::vector<IsoNode> nodes_;
    std::unordered_map<std::string, size_t> index_;
    std::vector<std::pair<std::string, size_t>> pending_;
    uint64_t total_ = 0;
};

// ---------------------------------------------------------------- host device

uint64_t to_filetime(const struct timespec& ts) {
    // 11644473600 seconds between 1601-01-01 and 1970-01-01.
    return (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
}

FileInfo host_info(const fs::path& p, const std::string& name, bool read_only) {
    FileInfo fi;
    fi.name = name;
    struct stat st {};
    if (::stat(p.c_str(), &st) == 0) {
        bool dir = S_ISDIR(st.st_mode);
        fi.size = dir ? 0 : uint64_t(st.st_size);
        fi.allocation_size = (fi.size + 4095) & ~uint64_t(4095);
        fi.attributes = dir ? attr::kDirectory : attr::kNormal;
        if (read_only) fi.attributes = (fi.attributes & ~attr::kNormal) | attr::kReadOnly;
        fi.create_time = to_filetime(st.st_ctim);
        fi.access_time = to_filetime(st.st_atim);
        fi.write_time = to_filetime(st.st_mtim);
        fi.change_time = fi.write_time;
    }
    return fi;
}

class HostFile : public VfsFile {
public:
    HostFile(fs::path p, std::string name, int fd, bool read_only)
        : path_(std::move(p)), name_(std::move(name)), fd_(fd), ro_(read_only) {}
    ~HostFile() override {
        if (fd_ >= 0) ::close(fd_);
    }
    FileInfo info() override { return host_info(path_, name_, ro_); }
    size_t read(uint64_t offset, void* buf, size_t n) override {
        if (fd_ < 0) return 0;
        size_t done = 0;
        auto* b = static_cast<uint8_t*>(buf);
        while (done < n) {
            ssize_t r = ::pread(fd_, b + done, n - done, off_t(offset + done));
            if (r <= 0) break;
            done += size_t(r);
        }
        return done;
    }
    uint32_t write(uint64_t offset, const void* buf, size_t n, size_t& written) override {
        written = 0;
        if (ro_ || fd_ < 0) return status::kMediaWriteProtected;
        auto* b = static_cast<const uint8_t*>(buf);
        while (written < n) {
            ssize_t r = ::pwrite(fd_, b + written, n - written, off_t(offset + written));
            if (r <= 0) return status::kUnsuccessful;
            written += size_t(r);
        }
        return status::kSuccess;
    }
    uint32_t set_size(uint64_t size) override {
        if (ro_ || fd_ < 0) return status::kMediaWriteProtected;
        return ::ftruncate(fd_, off_t(size)) == 0 ? status::kSuccess : status::kUnsuccessful;
    }
    uint32_t flush() override {
        if (fd_ >= 0) ::fsync(fd_);
        return status::kSuccess;
    }

protected:
    std::vector<FileInfo> list_children() override {
        std::vector<FileInfo> v;
        std::error_code ec;
        for (auto& de : fs::directory_iterator(path_, ec))
            v.push_back(host_info(de.path(), de.path().filename().string(), ro_));
        return v;
    }

private:
    fs::path path_;
    std::string name_;
    int fd_;
    bool ro_;
};

class HostDevice : public Device {
public:
    HostDevice(std::string dir, bool ro) : root_(std::move(dir)), ro_(ro) {
        std::error_code ec;
        if (!ro_) fs::create_directories(root_, ec);
    }

    // Resolves each component case-insensitively. If the final component is
    // missing, returns its would-be path and sets exists=false. Returns false
    // if a parent directory is missing.
    bool resolve(const std::string& rel, fs::path& out, std::string& name, bool& exists) const {
        fs::path cur = root_;
        auto parts = split(rel);
        name = parts.empty() ? "" : parts.back();
        for (size_t i = 0; i < parts.size(); ++i) {
            fs::path exact = cur / parts[i];
            std::error_code ec;
            if (fs::exists(exact, ec)) {
                cur = exact;
                continue;
            }
            bool found = false;
            std::string want = lower(parts[i]);
            for (auto& de : fs::directory_iterator(cur, ec)) {
                if (lower(de.path().filename().string()) == want) {
                    cur = de.path();
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (i + 1 != parts.size()) return false;
                out = exact;
                exists = false;
                return true;
            }
        }
        out = cur;
        exists = true;
        return true;
    }

    uint32_t open(const std::string& rel, const OpenRequest& req, std::shared_ptr<VfsFile>& out,
                  uint32_t& info) override {
        fs::path p;
        std::string name;
        bool exists = false;
        info = create_info::kDoesNotExist;
        if (!resolve(rel, p, name, exists)) return status::kObjectPathNotFound;
        std::error_code ec;
        bool is_dir = exists && fs::is_directory(p, ec);
        bool want_dir = (req.options & create_options::kDirectoryFile) != 0;
        auto d = req.disposition;

        if (exists) {
            if (d == Disposition::Create) return status::kObjectNameCollision;
            if (want_dir && !is_dir) return status::kNotADirectory;
            if ((req.options & create_options::kNonDirectoryFile) && is_dir) return status::kFileIsADirectory;
        } else {
            if (d == Disposition::Open || d == Disposition::Overwrite) return status::kObjectNameNotFound;
        }
        bool writing = req.wants_write() || !exists || d == Disposition::Overwrite ||
                       d == Disposition::OverwriteIf || d == Disposition::Supersede;
        if (writing && ro_) return status::kMediaWriteProtected;

        if (is_dir || (!exists && want_dir)) {
            if (!exists) {
                if (!fs::create_directory(p, ec)) return status::kUnsuccessful;
                info = create_info::kCreated;
            } else {
                info = create_info::kOpened;
            }
            out = std::make_shared<HostFile>(p, name, -1, ro_);
            return status::kSuccess;
        }

        int flags = O_CLOEXEC | ((ro_ || !req.wants_write()) && exists ? O_RDONLY : O_RDWR);
        if (!exists) flags |= O_CREAT;
        bool truncate = exists && (d == Disposition::Overwrite || d == Disposition::OverwriteIf ||
                                   d == Disposition::Supersede);
        if (truncate) flags |= O_TRUNC;
        int fd = ::open(p.c_str(), flags, 0644);
        if (fd < 0) return errno == EACCES ? status::kAccessDenied : status::kUnsuccessful;
        if (!exists) info = create_info::kCreated;
        else if (d == Disposition::Supersede) info = create_info::kSuperseded;
        else if (truncate) info = create_info::kOverwritten;
        else info = create_info::kOpened;
        out = std::make_shared<HostFile>(p, name, fd, ro_);
        return status::kSuccess;
    }

    uint32_t stat(const std::string& rel, FileInfo& out) override {
        fs::path p;
        std::string name;
        bool exists = false;
        if (!resolve(rel, p, name, exists)) return status::kObjectPathNotFound;
        if (!exists) return status::kObjectNameNotFound;
        out = host_info(p, name, ro_);
        return status::kSuccess;
    }

    uint32_t remove(const std::string& rel) override {
        if (ro_) return status::kMediaWriteProtected;
        fs::path p;
        std::string name;
        bool exists = false;
        if (!resolve(rel, p, name, exists)) return status::kObjectPathNotFound;
        if (!exists) return status::kObjectNameNotFound;
        std::error_code ec;
        if (fs::is_directory(p, ec) && !fs::is_empty(p, ec)) return status::kDirectoryNotEmpty;
        return fs::remove(p, ec) ? status::kSuccess : status::kUnsuccessful;
    }

    uint32_t rename(const std::string& from, const std::string& to) override {
        if (ro_) return status::kMediaWriteProtected;
        fs::path a, b;
        std::string n;
        bool ea = false, eb = false;
        if (!resolve(from, a, n, ea) || !resolve(to, b, n, eb)) return status::kObjectPathNotFound;
        if (!ea) return status::kObjectNameNotFound;
        std::error_code ec;
        fs::rename(a, b, ec);
        return ec ? status::kUnsuccessful : status::kSuccess;
    }

    bool read_only() const override { return ro_; }
    VolumeInfo volume() override {
        VolumeInfo v;
        v.label = ro_ ? "FULLAUTO" : "HDD";
        v.fs_name = ro_ ? "XDVDFS" : "FATX";
        v.bytes_per_sector = 512;
        v.sectors_per_unit = 32;
        std::error_code ec;
        auto sp = fs::space(root_, ec);
        if (!ec) {
            // Report at most 4 GiB free: old titles can overflow on larger values.
            v.total_bytes = std::min<uint64_t>(sp.capacity, 0x100000000ull);
            v.free_bytes = ro_ ? 0 : std::min<uint64_t>(sp.available, 0xF0000000ull);
        }
        return v;
    }

private:
    fs::path root_;
    bool ro_;
};

}  // namespace

std::unique_ptr<Device> make_iso_device(std::shared_ptr<XdvdfsImage> image) {
    return std::make_unique<IsoDevice>(std::move(image));
}

std::unique_ptr<Device> make_host_device(const std::string& dir, bool read_only) {
    return std::make_unique<HostDevice>(dir, read_only);
}

// ---------------------------------------------------------------- Vfs

void Vfs::mount(const std::string& name, std::shared_ptr<Device> dev) {
    std::lock_guard lk(mu_);
    std::string key = lower(normalize_guest_path(name));
    for (auto& m : mounts_)
        if (m.key == key) {
            m.dev = std::move(dev);
            return;
        }
    mounts_.push_back({key, std::move(dev)});
    // Longest names first so "\device\harddisk0\partition1" wins over shorter ones.
    std::sort(mounts_.begin(), mounts_.end(),
              [](const Mount& a, const Mount& b) { return a.key.size() > b.key.size(); });
}

void Vfs::unmount(const std::string& name) {
    std::lock_guard lk(mu_);
    std::string key = lower(normalize_guest_path(name));
    mounts_.erase(std::remove_if(mounts_.begin(), mounts_.end(), [&](auto& m) { return m.key == key; }),
                  mounts_.end());
}

bool Vfs::is_mounted(const std::string& name) const {
    std::lock_guard lk(mu_);
    std::string key = lower(normalize_guest_path(name));
    for (auto& m : mounts_)
        if (m.key == key) return true;
    return false;
}

void Vfs::add_link(const std::string& from, const std::string& to) {
    std::lock_guard lk(mu_);
    std::string f = lower(normalize_guest_path(from));
    std::string t = normalize_guest_path(to);
    for (auto& l : links_)
        if (l.first == f) {
            l.second = t;
            return;
        }
    links_.emplace_back(f, t);
    std::sort(links_.begin(), links_.end(),
              [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
}

void Vfs::remove_link(const std::string& from) {
    std::lock_guard lk(mu_);
    std::string f = lower(normalize_guest_path(from));
    links_.erase(std::remove_if(links_.begin(), links_.end(), [&](auto& l) { return l.first == f; }),
                 links_.end());
}

static bool prefix_match(const std::string& lower_path, const std::string& key) {
    if (lower_path.compare(0, key.size(), key) != 0) return false;
    return lower_path.size() == key.size() || lower_path[key.size()] == '\\' || key.back() == ':';
}

std::string Vfs::apply_links(const std::string& norm) const {
    std::string p = norm;
    for (int depth = 0; depth < 8; ++depth) {
        std::string lp = lower(p);
        bool changed = false;
        for (auto& [from, to] : links_) {
            if (prefix_match(lp, from)) {
                std::string rest = p.substr(from.size());
                if (!rest.empty() && rest[0] != '\\') rest = "\\" + rest;
                p = normalize_guest_path(to + rest);
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    return p;
}

bool Vfs::resolve(const std::string& guest_path, std::shared_ptr<Device>& dev, std::string& rel) const {
    std::lock_guard lk(mu_);
    std::string p = apply_links(normalize_guest_path(guest_path));
    std::string lp = lower(p);
    for (auto& m : mounts_) {
        if (!prefix_match(lp, m.key)) continue;
        std::string rest = p.substr(m.key.size());
        while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
        dev = m.dev;
        rel = rest;
        return true;
    }
    return false;
}

uint32_t Vfs::open(const std::string& guest_path, const OpenRequest& req, std::shared_ptr<VfsFile>& out,
                   uint32_t& info) {
    std::shared_ptr<Device> dev;
    std::string rel;
    info = create_info::kDoesNotExist;
    if (!resolve(guest_path, dev, rel)) return status::kObjectPathNotFound;
    uint32_t st = dev->open(rel, req, out, info);
    if (st == status::kSuccess) out->set_guest_path(guest_path);
    return st;
}

uint32_t Vfs::stat(const std::string& guest_path, FileInfo& out) {
    std::shared_ptr<Device> dev;
    std::string rel;
    if (!resolve(guest_path, dev, rel)) return status::kObjectPathNotFound;
    return dev->stat(rel, out);
}

uint32_t Vfs::remove(const std::string& guest_path) {
    std::shared_ptr<Device> dev;
    std::string rel;
    if (!resolve(guest_path, dev, rel)) return status::kObjectPathNotFound;
    return dev->remove(rel);
}

uint32_t Vfs::rename(const std::string& from, const std::string& to) {
    std::shared_ptr<Device> d1, d2;
    std::string r1, r2;
    if (!resolve(from, d1, r1)) return status::kObjectPathNotFound;
    // NtSetInformationFile renames may give a bare name or a device-relative path.
    if (!resolve(to, d2, r2)) {
        d2 = d1;
        auto slash = r1.rfind('\\');
        r2 = (slash == std::string::npos ? "" : r1.substr(0, slash + 1)) + normalize_guest_path(to);
        while (!r2.empty() && r2[0] == '\\') r2.erase(0, 1);
    }
    if (d1 != d2) return status::kAccessDenied;  // no cross-device moves
    return d1->rename(r1, r2);
}

uint32_t Vfs::volume(const std::string& guest_path, VolumeInfo& out) {
    std::shared_ptr<Device> dev;
    std::string rel;
    if (!resolve(guest_path, dev, rel)) return status::kObjectPathNotFound;
    out = dev->volume();
    return status::kSuccess;
}

bool Vfs::mount_defaults(const std::string& game_source, const std::string& data_dir, std::string& err) {
    std::shared_ptr<Device> game;
    std::error_code ec;
    if (fs::is_directory(game_source, ec)) {
        game = make_host_device(game_source, true);
    } else {
        auto img = std::make_shared<XdvdfsImage>();
        if (!img->open(game_source)) {
            err = img->error();
            return false;
        }
        game = make_iso_device(img);
    }
    mount("\\Device\\Cdrom0", game);
    add_link("game:", "\\Device\\Cdrom0");
    add_link("d:", "\\Device\\Cdrom0");

    std::shared_ptr<Device> hdd = make_host_device(data_dir + "/hdd", false);
    std::shared_ptr<Device> cache = make_host_device(data_dir + "/cache", false);
    mount("\\Device\\Harddisk0\\Partition1", hdd);
    mount("\\Device\\Harddisk0\\Cache0", cache);
    add_link("hdd:", "\\Device\\Harddisk0\\Partition1");
    add_link("e:", "\\Device\\Harddisk0\\Partition1");
    add_link("cache:", "\\Device\\Harddisk0\\Cache0");
    add_link("z:", "\\Device\\Harddisk0\\Cache0");
    return true;
}

}  // namespace fa::hle
