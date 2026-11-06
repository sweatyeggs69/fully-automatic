#include "fa/mem/write_watch.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace fa::mem {

namespace {
constexpr uint32_t kPage = 4096;
}

WriteWatch::~WriteWatch() {
    if (uffd_ >= 0) close(uffd_);
    if (pagemap_ >= 0) close(pagemap_);
}

bool WriteWatch::Init(const std::vector<View>& views, uint32_t physSize, const char** why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        if (uffd_ >= 0) close(uffd_);
        if (pagemap_ >= 0) close(pagemap_);
        uffd_ = pagemap_ = -1;
        return false;
    };
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC)
    int fd = int(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
    if (fd < 0) return fail("userfaultfd unavailable");
    uffd_ = fd;
    uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED | UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
    if (ioctl(uffd_, UFFDIO_API, &api) != 0) return fail("userfaultfd lacks async write protect (kernel < 6.7?)");
    for (const View& v : views) {
        uffdio_register reg{};
        reg.range.start = reinterpret_cast<uint64_t>(v.base);
        reg.range.len = v.size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd_, UFFDIO_REGISTER, &reg) != 0) return fail("cannot register a view for write protect");
    }
    pagemap_ = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap_ < 0) return fail("cannot open /proc/self/pagemap");
    // Probe PAGEMAP_SCAN once on an empty result.
    page_region r{};
    pm_scan_arg a{};
    a.size = sizeof a;
    a.start = reinterpret_cast<uint64_t>(views.empty() ? nullptr : views[0].base);
    a.end = a.start + kPage;
    a.vec = reinterpret_cast<uint64_t>(&r);
    a.vec_len = 1;
    a.category_mask = PAGE_IS_WRITTEN;
    a.return_mask = PAGE_IS_WRITTEN;
    if (views.empty() || ioctl(pagemap_, PAGEMAP_SCAN, &a) < 0) return fail("PAGEMAP_SCAN unavailable");
    views_ = views;
    pages_ = physSize / kPage;
    written_.assign(pages_, 0);
    watched_.assign(pages_, 0);
    seenAt_.assign(pages_, 0);
    viewSeen_.assign(views_.size(), std::vector<uint8_t>((pages_ >> kRegionShift) + 1, 0));
    regionAt_.assign(views_.size(), std::vector<uint32_t>((pages_ >> kRegionShift) + 1, 0));
    nowAt_.assign(pages_, 0);
    if (why) *why = "";
    return true;
#else
    (void)views;
    (void)physSize;
    return fail("built without PAGEMAP_SCAN / UFFD_FEATURE_WP_ASYNC headers");
#endif
}

void WriteWatch::Protect(uint32_t firstPage, uint32_t count) {
    for (const View& v : views_) {
        const uint32_t vFirst = v.physOffset / kPage, vEnd = vFirst + v.size / kPage;
        const uint32_t a = std::max(firstPage, vFirst), b = std::min(firstPage + count, vEnd);
        if (a >= b) continue;
        uffdio_writeprotect wp{};
        wp.range.start = reinterpret_cast<uint64_t>(v.base) + uint64_t(a - vFirst) * kPage;
        wp.range.len = uint64_t(b - a) * kPage;
        wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
        ioctl(uffd_, UFFDIO_WRITEPROTECT, &wp);
    }
    // Protected from now on: a region query's earlier answer for these pages no longer holds.
    for (uint32_t p = firstPage; p < firstPage + count && p < pages_; p++) nowAt_[p] = 0;
}

uint64_t WriteWatch::Watch(uint32_t phys, uint32_t size) {
    if (uffd_ < 0 || !size) return 0;
    const uint32_t first = phys / kPage;
    const uint64_t end64 = (uint64_t(phys) + size + kPage - 1) / kPage;
    if (end64 > pages_) return 0;
    const uint32_t end = uint32_t(end64);
    for (uint32_t p = first; p < end;) {
        if (watched_[p]) { p++; continue; }
        uint32_t q = p;
        while (q < end && !watched_[q]) watched_[q++] = 1;
        Protect(p, q - p);
        stats_.pagesWatched += q - p;
        runsDirty_ = true;
        p = q;
    }
    return epoch_;
}

bool WriteWatch::Clean(uint32_t phys, uint32_t size, uint64_t token) const {
    if (uffd_ < 0 || !token || !size) return false;
    const uint32_t first = phys / kPage;
    const uint64_t end64 = (uint64_t(phys) + size + kPage - 1) / kPage;
    if (end64 > pages_) return false;
    bool hot = queryAllViews_ || !skipCold_;
    for (uint32_t p = first; p < uint32_t(end64); p++) {
        if (!watched_[p] || written_[p] > token) return false;
        hot |= seenAt_[p] && harvestNo_ - seenAt_[p] < kHotHarvests;
    }
    if (!hot) { stats_.queryCold++; return true; }
    // Writes since the last Harvest: the kernel still shows those pages as written.
    return !WrittenNow(first, uint32_t(end64) - first);
}

bool WriteWatch::WrittenNow(uint32_t firstPage, uint32_t count) const {
#if defined(PAGEMAP_SCAN)
    const auto t0 = std::chrono::steady_clock::now();
    struct Done {
        const std::chrono::steady_clock::time_point t0;
        Stats& st;
        ~Done() { st.queryMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } done{t0, stats_};
    for (size_t vi = 0; vi < views_.size(); vi++) {
        const View& v = views_[vi];
        const uint32_t vFirst = v.physOffset / kPage, vEnd = vFirst + v.size / kPage;
        const uint32_t a = std::max(firstPage, vFirst), b = std::min(firstPage + count, vEnd);
        if (a >= b) continue;
        if (!queryAllViews_) {
            bool seen = false;
            for (uint32_t r = a >> kRegionShift; r <= (b - 1) >> kRegionShift && !seen; r++) seen = viewSeen_[vi][r];
            if (!seen) { stats_.querySkips++; continue; }
        }
        if (regionQueries_) {
            const uint32_t stamp = harvestNo_ + 1;
            for (uint32_t r = a >> kRegionShift; r <= (b - 1) >> kRegionShift; r++)
                if (regionAt_[vi][r] != stamp) QueryRegion(vi, r);
            for (uint32_t p = a; p < b; p++)
                if (nowAt_[p] == stamp) return true;
            continue;
        }
        const uint64_t base = reinterpret_cast<uint64_t>(v.base) - uint64_t(vFirst) * kPage;
        page_region r{};
        pm_scan_arg s{};
        s.size = sizeof s;
        s.start = base + uint64_t(a) * kPage;
        s.end = base + uint64_t(b) * kPage;
        s.vec = reinterpret_cast<uint64_t>(&r);
        s.vec_len = 1;
        s.max_pages = 1;
        s.category_mask = PAGE_IS_WRITTEN;
        s.return_mask = PAGE_IS_WRITTEN;
        const long n = ioctl(pagemap_, PAGEMAP_SCAN, &s);
        stats_.queryCalls++;
        if (n != 0) return true;  // written, or unknown
    }
    return false;
#else
    (void)firstPage;
    (void)count;
    return true;
#endif
}

void WriteWatch::QueryRegion(size_t view, uint32_t region) const {
#if defined(PAGEMAP_SCAN)
    const uint32_t stamp = harvestNo_ + 1;
    regionAt_[view][region] = stamp;
    const View& v = views_[view];
    const uint32_t vFirst = v.physOffset / kPage, vEnd = vFirst + v.size / kPage;
    const uint32_t a = std::max(region << kRegionShift, vFirst), b = std::min((region + 1) << kRegionShift, std::min(vEnd, pages_));
    if (a >= b) return;
    const uint64_t base = reinterpret_cast<uint64_t>(v.base) - uint64_t(vFirst) * kPage;
    page_region regions[64];
    pm_scan_arg s{};
    s.size = sizeof s;
    s.start = base + uint64_t(a) * kPage;
    s.end = base + uint64_t(b) * kPage;
    s.vec = reinterpret_cast<uint64_t>(regions);
    s.vec_len = 64;
    s.category_mask = PAGE_IS_WRITTEN;
    s.return_mask = PAGE_IS_WRITTEN;
    while (s.start < s.end) {
        const long n = ioctl(pagemap_, PAGEMAP_SCAN, &s);
        stats_.queryCalls++;
        if (n < 0) {  // unknown: count the rest as written
            for (uint64_t p = (s.start - base) / kPage; p < b; p++) nowAt_[p] = stamp;
            return;
        }
        for (long i = 0; i < n; i++) {
            const uint64_t p0 = (regions[i].start - base) / kPage, p1 = (regions[i].end - base) / kPage;
            for (uint64_t p = p0; p < p1 && p < b; p++) nowAt_[p] = stamp;
        }
        if (!s.walk_end || s.walk_end <= s.start) break;
        s.start = s.walk_end;
    }
#else
    (void)view;
    (void)region;
#endif
}

void WriteWatch::Touch(uint32_t phys, uint32_t size) {
    if (uffd_ < 0 || !size) return;
    // A new epoch: copies taken before this call are stale, copies taken after it are fresh.
    const uint64_t e = ++epoch_;
    const uint32_t first = phys / kPage;
    const uint64_t end64 = std::min<uint64_t>((uint64_t(phys) + size + kPage - 1) / kPage, pages_);
    for (uint32_t p = first; p < uint32_t(end64); p++) written_[p] = e;
}

void WriteWatch::Scan(size_t view, uint32_t firstPage, uint32_t count) {
#if defined(PAGEMAP_SCAN)
    const View& v = views_[view];
    std::vector<uint8_t>& seen = viewSeen_[view];
    const uint32_t vFirst = v.physOffset / kPage, vEnd = vFirst + v.size / kPage;
    const uint32_t a = std::max(firstPage, vFirst), b = std::min(firstPage + count, vEnd);
    if (a >= b) return;
    const uint64_t base = reinterpret_cast<uint64_t>(v.base) - uint64_t(vFirst) * kPage;  // host address of physical page 0
    static page_region regions[512];
    pm_scan_arg s{};
    s.size = sizeof s;
    s.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
    s.start = base + uint64_t(a) * kPage;
    s.end = base + uint64_t(b) * kPage;
    s.vec = reinterpret_cast<uint64_t>(regions);
    s.vec_len = 512;
    s.category_mask = PAGE_IS_WRITTEN;
    s.return_mask = PAGE_IS_WRITTEN;
    while (s.start < s.end) {
        const long n = ioctl(pagemap_, PAGEMAP_SCAN, &s);
        stats_.scanCalls++;
        if (n < 0) {
            // Unknown state: count the rest of the run as written.
            for (uint64_t p = (s.start - base) / kPage; p < b; p++) written_[p] = epoch_, seen[p >> kRegionShift] = 1, seenAt_[p] = harvestNo_;
            return;
        }
        for (long i = 0; i < n; i++) {
            const uint64_t p0 = (regions[i].start - base) / kPage, p1 = (regions[i].end - base) / kPage;
            for (uint64_t p = p0; p < p1 && p < pages_; p++) written_[p] = epoch_, seen[p >> kRegionShift] = 1, seenAt_[p] = harvestNo_;
            stats_.pagesWritten += p1 - p0;
        }
        if (!s.walk_end || s.walk_end <= s.start) break;
        s.start = s.walk_end;
    }
#else
    (void)view;
    (void)firstPage;
    (void)count;
#endif
}

void WriteWatch::Harvest() {
    if (uffd_ < 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    // Pages seen written now get the new epoch, which is larger than every token handed out so far.
    ++epoch_;
    ++harvestNo_;
    if (runsDirty_) {
        runs_.clear();
        // Runs closer than kMergeGap pages are scanned as one: a scan call costs more than walking a
        // short gap. Unwatched pages in a gap may be reported (and protected) too, which is harmless:
        // unwatched pages never count as clean.
        constexpr uint32_t kMergeGap = 256;
        for (uint32_t p = 0; p < pages_;) {
            if (!watched_[p]) { p++; continue; }
            uint32_t q = p;
            while (q < pages_ && watched_[q]) q++;
            if (!runs_.empty() && p - (runs_.back().first + runs_.back().count) <= kMergeGap)
                runs_.back().count = q - runs_.back().first;
            else
                runs_.push_back({p, q - p});
            p = q;
        }
        runsDirty_ = false;
    }
    for (const Run& r : runs_)
        for (size_t vi = 0; vi < views_.size(); vi++) Scan(vi, r.first, r.count);
    stats_.harvests++;
    stats_.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

WriteWatch::Stats WriteWatch::TakeStats() {
    Stats s = stats_;
    stats_ = Stats{};
    return s;
}

}  // namespace fa::mem
