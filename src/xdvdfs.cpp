#include "fa/xdvdfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include <fcntl.h>
#include <unistd.h>

namespace fa {

static constexpr uint32_t kSector = 2048;
static constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// Game partition offsets for raw/XGD1/XGD2/XGD3 images.
static constexpr uint64_t kPartitions[] = {0, 0x18300000, 0xFD90000, 0x2080000, 0x89D80000};

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
static uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

XdvdfsImage::~XdvdfsImage() {
    if (fd_ >= 0) ::close(fd_);
}

bool XdvdfsImage::read_at(uint64_t off, void* buf, size_t n) const {
    auto* p = static_cast<uint8_t*>(buf);
    while (n) {
        ssize_t r = ::pread(fd_, p, n, static_cast<off_t>(off));
        if (r <= 0) return false;
        p += r; off += uint64_t(r); n -= size_t(r);
    }
    return true;
}

size_t XdvdfsImage::read_range(const IsoEntry& e, uint64_t offset, void* buf, size_t n) const {
    if (e.is_dir || offset >= e.size) return 0;
    n = size_t(std::min<uint64_t>(n, e.size - offset));
    return read_at(e.offset + offset, buf, n) ? n : 0;
}

bool XdvdfsImage::open(const std::string& path) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) { error_ = "cannot open " + path; return false; }
    for (uint64_t p : kPartitions) {
        uint8_t vd[28];
        if (!read_at(p + 32ull * kSector, vd, sizeof vd)) continue;
        if (std::memcmp(vd, kMagic, 20) != 0) continue;
        part_off_ = p;
        root_sector_ = le32(vd + 20);
        root_size_ = le32(vd + 24);
        return true;
    }
    error_ = "no XDVDFS volume descriptor found (not an Xbox 360 game image?)";
    return false;
}

void XdvdfsImage::walk(uint64_t dir_off, uint32_t dir_size, const std::string& prefix,
                       std::vector<IsoEntry>& out, int depth) {
    if (depth > 32 || dir_size == 0 || dir_size > (64u << 20)) return;
    std::vector<uint8_t> d(dir_size);
    if (!read_at(dir_off, d.data(), d.size())) return;

    // Entries form a binary tree of dword offsets; visit via an explicit stack.
    std::vector<uint32_t> stack{0};
    std::vector<bool> seen(dir_size / 4 + 1, false);
    while (!stack.empty()) {
        uint32_t o = stack.back();
        stack.pop_back();
        if (o + 14 > dir_size || seen[o / 4]) continue;
        seen[o / 4] = true;
        const uint8_t* e = &d[o];
        uint16_t left = le16(e), right = le16(e + 2);
        if (left == 0xFFFF && right == 0xFFFF && le32(e + 4) == 0xFFFFFFFF) continue;
        uint32_t sector = le32(e + 4), size = le32(e + 8);
        uint8_t attr = e[12], nlen = e[13];
        if (o + 14u + nlen > dir_size) continue;

        IsoEntry ent;
        ent.path = prefix + std::string(reinterpret_cast<const char*>(e + 14), nlen);
        ent.offset = part_off_ + uint64_t(sector) * kSector;
        ent.size = size;
        ent.is_dir = (attr & 0x10) != 0;
        out.push_back(ent);
        if (ent.is_dir) walk(ent.offset, size, ent.path + "/", out, depth + 1);

        if (left) stack.push_back(uint32_t(left) * 4);
        if (right) stack.push_back(uint32_t(right) * 4);
    }
}

std::vector<IsoEntry> XdvdfsImage::list() {
    std::vector<IsoEntry> out;
    walk(part_off_ + uint64_t(root_sector_) * kSector, root_size_, "", out, 0);
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.path < b.path; });
    return out;
}

bool XdvdfsImage::find(const std::string& path, IsoEntry& out) {
    std::string want = lower(path);
    for (auto& e : list())
        if (lower(e.path) == want) { out = e; return true; }
    return false;
}

bool XdvdfsImage::read(const IsoEntry& e, std::vector<uint8_t>& out) {
    out.resize(e.size);
    return read_at(e.offset, out.data(), out.size());
}

bool XdvdfsImage::extract(const IsoEntry& e, const std::string& dest) {
    std::ofstream o(dest, std::ios::binary);
    if (!o) return false;
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t pos = 0; pos < e.size;) {
        size_t n = std::min<uint64_t>(buf.size(), e.size - pos);
        if (!read_at(e.offset + pos, buf.data(), n)) return false;
        o.write(reinterpret_cast<char*>(buf.data()), n);
        pos += n;
    }
    return bool(o);
}

}  // namespace fa
