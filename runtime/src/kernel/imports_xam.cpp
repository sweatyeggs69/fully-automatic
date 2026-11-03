// Small xam/xboxkrnl system queries the game makes at startup.
#include "fa/rt/import.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include <cstring>

using namespace fa::rt;

FA_IMPORT(KeGetCurrentProcessType) { ret32(ctx, 1); }  // user process

FA_IMPORT(KeEnterCriticalRegion) { }
FA_IMPORT(KeLeaveCriticalRegion) { }

FA_IMPORT(RtlNtStatusToDosError) {
    uint32_t s = arg32(ctx, 0);
    switch (s) {
        case 0: ret32(ctx, 0); break;
        case 0xC000000F: case 0xC0000034: ret32(ctx, 2); break;   // file not found
        case 0xC000003A: ret32(ctx, 3); break;                    // path not found
        case 0xC0000022: ret32(ctx, 5); break;                    // access denied
        case 0xC0000008: ret32(ctx, 6); break;                    // invalid handle
        case 0xC0000017: ret32(ctx, 8); break;                    // no memory
        case 0xC0000011: ret32(ctx, 38); break;                   // EOF
        case 0xC000000D: ret32(ctx, 87); break;                   // invalid parameter
        default: ret32(ctx, 317); break;                          // ERROR_MR_MID_NOT_FOUND
    }
}

FA_IMPORT(MmQueryStatistics) {
    // MM_STATISTICS {u32 length; u32 total_physical_pages; u32 kernel_pages; ...}: report 512 MiB.
    uint32_t p = arg32(ctx, 0);
    if (!p) { ret32(ctx, STATUS_INVALID_PARAMETER); return; }
    uint32_t len = ld32(p);
    if (len > 0x100) len = 0x100;
    std::memset(host(p + 4), 0, len > 4 ? len - 4 : 0);
    st32(p + 4, 0x20000);   // total physical pages (4 KiB)
    st32(p + 8, 0x1000);    // kernel pages
    st32(p + 12, 0x10000);  // available pages
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(ExRegisterTitleTerminateNotification) { }

// Launch data is only used to pass data between titles; none is present on a cold boot.
FA_IMPORT(XamLoaderGetLaunchData) { ret32(ctx, STATUS_NO_SUCH_FILE); }
FA_IMPORT(XamLoaderSetLaunchData) { ret32(ctx, STATUS_SUCCESS); }

FA_IMPORT(XamNotifyCreateListener) {
    // (mask, max_version) -> handle. Notifications are never delivered yet.
    ret32(ctx, g_kernel.add_handle(std::make_shared<KEvent>(true, false)));
}
FA_IMPORT(XNotifyGetNext) { ret32(ctx, 0); }  // nothing pending

FA_IMPORT(XamGetExecutionId) { ret32(ctx, STATUS_NOT_IMPLEMENTED); }
FA_IMPORT(XGetLanguage) { ret32(ctx, 1); }  // English

FA_IMPORT(XGetVideoMode) {
    // XVIDEO_MODE {u32 display_width, display_height, is_interlaced, is_widescreen,
    //              is_hi_def, refresh_rate (float), video_standard, ...}: 1280x720 progressive 16:9
    uint32_t p = arg32(ctx, 0);
    std::memset(host(p), 0, 0x30);
    st32(p + 0, 1280);
    st32(p + 4, 720);
    st32(p + 8, 0);
    st32(p + 12, 1);
    st32(p + 16, 1);
    uint32_t refresh = 0x42700000;  // 60.0f
    st32(p + 20, refresh);
    st32(p + 24, 1);
}

FA_IMPORT(ExGetXConfigSetting) {
    // (category, setting, buffer, buffer_size, required_size_ptr): report 0 bytes of data.
    uint32_t buf = arg32(ctx, 2), size = arg32(ctx, 3), req = arg32(ctx, 4);
    if (buf && size) std::memset(host(buf), 0, size < 0x40 ? size : 0x40);
    if (req) st16(req, 4);
    ret32(ctx, STATUS_SUCCESS);
}
