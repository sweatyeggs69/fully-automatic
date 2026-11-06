#pragma once
// Kernel-tracked guest writes: which pages of guest physical memory were written since a cached
// copy (texture, vertex or index data, a resolve snapshot) was taken from them, without hashing
// the bytes again every frame.
//
// How: the host mappings ("views") of guest physical memory are registered with userfaultfd in
// asynchronous write-protect mode. Watch() write-protects the pages a copy is made from. A guest
// write to such a page then costs the writing thread one minor fault, which the kernel resolves
// by itself (no signal, no handler thread) and leaves the page marked as written. Harvest(), once
// per frame, asks the kernel which watched pages were written (PAGEMAP_SCAN) and write-protects
// them again in the same call.
//
// Needs Linux 6.7+ (UFFD_FEATURE_WP_ASYNC, PAGEMAP_SCAN). Uses UFFD_USER_MODE_ONLY, so it works
// with vm.unprivileged_userfaultfd = 0. When anything is missing it reports disabled, and every
// range counts as written (callers fall back to hashing): a failure only costs speed.
//
// Single threaded: call Init, Watch, Clean, Touch and Harvest from one thread (the GPU thread).
// Guest threads may write the memory at any time.
#include <cstdint>
#include <vector>

namespace fa::mem {

class WriteWatch {
public:
    // One host mapping of guest physical memory: host address `base` shows physical bytes
    // [physOffset, physOffset + size). Every view through which the guest can write must be listed.
    struct View {
        void* base;
        uint32_t physOffset;
        uint32_t size;
    };

    WriteWatch() = default;
    ~WriteWatch();
    WriteWatch(const WriteWatch&) = delete;
    WriteWatch& operator=(const WriteWatch&) = delete;

    // physSize: bytes of physical memory (page granular). Returns false (and stays disabled)
    // when the kernel lacks the features; `why` then says what failed.
    bool Init(const std::vector<View>& views, uint32_t physSize, const char** why = nullptr);
    bool Enabled() const { return uffd_ >= 0; }

    // Before reading [phys, phys + size) for a copy: makes sure its pages are watched and returns
    // the token to keep with the copy. 0 means not watched (never clean).
    uint64_t Watch(uint32_t phys, uint32_t size);
    // True when no page of the range was written after the copy that got `token` was taken.
    // Writes since the last Harvest are seen too (asks the kernel, one call per view). Unwatched
    // pages and token 0 count as written.
    bool Clean(uint32_t phys, uint32_t size, uint64_t token) const;
    // The host wrote the range itself (or freed it): treat it as written.
    void Touch(uint32_t phys, uint32_t size);
    // Once per frame, before lookups: collects the pages written since the last call.
    void Harvest();
    // Clean's kernel query (writes since the last Harvest) is only made for ranges with a page that
    // Harvest saw written in the last kHotHarvests harvests, and only through views Harvest saw
    // writing in that 2 MB region. Memory the game rewrites keeps being asked about; memory that has
    // been quiet (most textures) is not. The cost: the first write after a quiet spell, or through a
    // new view, is seen at the next Harvest instead of right away. true asks every view every time.
    void SetQueryAllViews(bool all) { queryAllViews_ = all; }

    struct Stats {
        uint64_t harvests = 0, pagesWritten = 0, pagesWatched = 0, scanCalls = 0, queryCalls = 0, querySkips = 0, queryCold = 0;
        double ms = 0, queryMs = 0;
    };
    // Since the last call.
    Stats TakeStats();

private:
    struct Run { uint32_t first, count; };  // physical pages
    void Protect(uint32_t firstPage, uint32_t count);
    void Scan(size_t view, uint32_t firstPage, uint32_t count);
    bool WrittenNow(uint32_t firstPage, uint32_t count) const;

    int uffd_ = -1;
    int pagemap_ = -1;
    std::vector<View> views_;
    uint32_t pages_ = 0;
    uint64_t epoch_ = 1;                 // tokens handed out until the next Harvest
    std::vector<uint64_t> written_;      // per page: epoch of the harvest that saw it written
    std::vector<uint8_t> watched_;       // per page
    std::vector<Run> runs_;              // watched pages, coalesced (rebuilt lazily)
    bool runsDirty_ = false;
    mutable Stats stats_;
    static constexpr uint32_t kRegionShift = 9;  // 512 pages = 2 MB
    std::vector<std::vector<uint8_t>> viewSeen_;  // per view, per region: Harvest saw a write through it
    static constexpr uint32_t kHotHarvests = 120;  // about 4 s at 30 fps
    std::vector<uint32_t> seenAt_;                 // per page: harvest number that last saw it written (0 = never)
    uint32_t harvestNo_ = 0;
    bool queryAllViews_ = false;
};

}  // namespace fa::mem
