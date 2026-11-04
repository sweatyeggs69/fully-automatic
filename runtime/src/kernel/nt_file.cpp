// NT file imports on top of fa::hle::Vfs (owned by the audio/input/file layer).
// This layer owns handles, OBJECT_ATTRIBUTES, IO_STATUS_BLOCK and guest memory; Vfs returns
// NTSTATUS values that are handed back unchanged. All I/O is synchronous.
#include "fa/hle/services.hpp"
#include "fa/rt/import.hpp"
#include "fa/rt/kernel.hpp"
#include "fa/rt/memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>

using namespace fa::rt;
namespace hle = fa::hle;

namespace {

constexpr uint32_t STATUS_INVALID_INFO_CLASS = 0xC0000003;
constexpr uint32_t STATUS_NOT_A_DIRECTORY = 0xC0000103;
constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull;

struct KFile : KObject {
    explicit KFile(std::shared_ptr<hle::VfsFile> f) : KObject(Type::File), file(std::move(f)) {}
    bool signaled(const KThread*) const override { return true; }
    void acquire(KThread*) override {}
    std::shared_ptr<hle::VfsFile> file;
    uint64_t pos = 0;
};

// Parameters past r10 live in the caller's frame, 8 byte slots with the value in the low word.
uint32_t stack_arg(PPCContext& ctx, int n) { return arg32(ctx, n); }

std::string ansi_string(uint32_t p) {  // guest ANSI_STRING {u16 len, u16 max, u32 buffer}
    if (!p) return {};
    uint16_t len = ld16(p);
    uint32_t buf = ld32(p + 4);
    return buf ? std::string(reinterpret_cast<const char*>(host(buf)), len) : std::string();
}

std::shared_ptr<KFile> file_of(uint32_t h) {
    auto o = g_kernel.get_handle(h);
    return (o && o->type == KObject::Type::File) ? std::static_pointer_cast<KFile>(o) : nullptr;
}

// OBJECT_ATTRIBUTES {root_directory, object_name (ANSI_STRING*), attributes}
std::string path_from_attributes(uint32_t oa) {
    std::string name = ansi_string(ld32(oa + 4));
    uint32_t root = ld32(oa);
    if (root) {
        if (auto d = file_of(root)) name = d->file->guest_path() + "\\" + name;
    }
    return name;
}

void iosb(uint32_t p, uint32_t status, uint32_t info) {
    if (!p) return;
    st32(p, status);
    st32(p + 4, info);
}

void signal_event(uint32_t h) {
    if (!h) return;
    auto o = g_kernel.get_handle(h);
    if (o && o->type == KObject::Type::Event) {
        auto* e = static_cast<KEvent*>(o.get());
        std::lock_guard<std::mutex> lk(g_kmu);
        e->set = true;
        g_kcv.notify_all();
    }
}

void create(PPCContext& ctx, uint32_t handle_ptr, uint32_t access, uint32_t oa, uint32_t io,
            hle::Disposition disp, uint32_t options) {
    hle::OpenRequest req;
    req.access = access;
    req.disposition = disp;
    req.options = options;
    std::shared_ptr<hle::VfsFile> f;
    uint32_t info = 0;
    std::string path = path_from_attributes(oa);
    // FA_SKIP_MOVIES=1 hides the .wmv movies (intro, attract loop) so the game skips them.
    static const bool skip_movies = std::getenv("FA_SKIP_MOVIES") != nullptr;
    bool movie = path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".wmv") == 0;
    uint32_t st = skip_movies && movie ? STATUS_OBJECT_NAME_NOT_FOUND : hle::services().vfs.open(path, req, f, info);
    if (st == STATUS_SUCCESS) {
        f->set_guest_path(f->guest_path().empty() ? path : f->guest_path());
        st32(handle_ptr, g_kernel.add_handle(std::make_shared<KFile>(f)));
    }
    if (g_trace_imports)
        std::fprintf(stderr, "[file] open %s -> %08x\n", path.c_str(), st);
    iosb(io, st, st == STATUS_SUCCESS ? info : 0);
    ret32(ctx, st);
}

void put_times(uint32_t p, const hle::FileInfo& i) {
    st64(p, i.create_time);
    st64(p + 8, i.access_time);
    st64(p + 16, i.write_time);
    st64(p + 24, i.change_time);
}

}  // namespace

FA_IMPORT(NtCreateFile) {
    // (handle_ptr, access, object_attributes, io_status_block, allocation_size, file_attributes,
    //  share_access, creation_disposition, create_options)
    create(ctx, arg32(ctx, 0), arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3),
           static_cast<hle::Disposition>(arg32(ctx, 7)), stack_arg(ctx, 8));
}

FA_IMPORT(NtOpenFile) {
    // (handle_ptr, access, object_attributes, io_status_block, share_access, open_options)
    create(ctx, arg32(ctx, 0), arg32(ctx, 1), arg32(ctx, 2), arg32(ctx, 3), hle::Disposition::Open,
           arg32(ctx, 5));
}

FA_IMPORT(NtReadFile) {
    // (handle, event, apc_routine, apc_context, io_status_block, buffer, length, byte_offset_ptr)
    auto f = file_of(arg32(ctx, 0));
    if (!f) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t buf = arg32(ctx, 5), len = arg32(ctx, 6), off_ptr = arg32(ctx, 7);
    uint64_t off = f->pos;
    bool use_ptr = true;
    if (off_ptr) {
        uint64_t o = ld64(off_ptr);
        if (o != kUseFilePointer && o != 0xFFFFFFFFFFFFFFFFull) { off = o; use_ptr = false; }
    }
    size_t n = len ? f->file->read(off, host(buf), len) : 0;
    if (use_ptr) f->pos = off + n;
    uint32_t st = (n == 0 && len > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    iosb(arg32(ctx, 4), st, uint32_t(n));
    signal_event(arg32(ctx, 1));
    ret32(ctx, st);
}

FA_IMPORT(NtWriteFile) {
    auto f = file_of(arg32(ctx, 0));
    if (!f) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t buf = arg32(ctx, 5), len = arg32(ctx, 6), off_ptr = arg32(ctx, 7);
    uint64_t off = f->pos;
    bool use_ptr = true;
    if (off_ptr) {
        uint64_t o = ld64(off_ptr);
        if (o != kUseFilePointer && o != 0xFFFFFFFFFFFFFFFFull) { off = o; use_ptr = false; }
    }
    size_t written = 0;
    uint32_t st = f->file->write(off, host(buf), len, written);
    if (use_ptr) f->pos = off + written;
    iosb(arg32(ctx, 4), st, uint32_t(written));
    signal_event(arg32(ctx, 1));
    ret32(ctx, st);
}

FA_IMPORT(NtFlushBuffersFile) {
    auto f = file_of(arg32(ctx, 0));
    if (!f) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t st = f->file->flush();
    iosb(arg32(ctx, 1), st, 0);
    ret32(ctx, st);
}

FA_IMPORT(NtQueryInformationFile) {
    // (handle, io_status_block, info_ptr, length, info_class)
    auto f = file_of(arg32(ctx, 0));
    if (!f) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t p = arg32(ctx, 2), cls = arg32(ctx, 4);
    hle::FileInfo i = f->file->info();
    uint32_t st = STATUS_SUCCESS, written = 0;
    switch (cls) {
        case 4:  // FileBasicInformation
            put_times(p, i); st32(p + 32, i.attributes); st32(p + 36, 0); written = 40; break;
        case 5:  // FileStandardInformation
            st64(p, i.allocation_size); st64(p + 8, i.size); st32(p + 16, 1);
            st8(p + 20, 0); st8(p + 21, i.is_dir()); st16(p + 22, 0); written = 24; break;
        case 14:  // FilePositionInformation
            st64(p, f->pos); written = 8; break;
        case 34:  // FileNetworkOpenInformation
            put_times(p, i); st64(p + 32, i.allocation_size); st64(p + 40, i.size);
            st32(p + 48, i.attributes); st32(p + 52, 0); written = 56; break;
        default: st = STATUS_INVALID_INFO_CLASS;
    }
    iosb(arg32(ctx, 1), st, written);
    ret32(ctx, st);
}

FA_IMPORT(NtSetInformationFile) {
    // (handle, io_status_block, info_ptr, length, info_class)
    auto f = file_of(arg32(ctx, 0));
    if (!f) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t p = arg32(ctx, 2), cls = arg32(ctx, 4);
    uint32_t st = STATUS_SUCCESS;
    switch (cls) {
        case 14: f->pos = ld64(p); break;                       // FilePositionInformation
        case 20: st = f->file->set_size(ld64(p)); break;        // FileEndOfFileInformation
        case 13: case 4: case 19: break;                        // disposition, basic, allocation
        default: st = STATUS_INVALID_INFO_CLASS;
    }
    iosb(arg32(ctx, 1), st, 0);
    ret32(ctx, st);
}

FA_IMPORT(NtQueryFullAttributesFile) {
    // (object_attributes, file_info_ptr) -> FILE_NETWORK_OPEN_INFORMATION
    hle::FileInfo i;
    uint32_t st = hle::services().vfs.stat(path_from_attributes(arg32(ctx, 0)), i);
    if (st == STATUS_SUCCESS) {
        uint32_t p = arg32(ctx, 1);
        put_times(p, i); st64(p + 32, i.allocation_size); st64(p + 40, i.size);
        st32(p + 48, i.attributes); st32(p + 52, 0);
    }
    ret32(ctx, st);
}

FA_IMPORT(NtQueryDirectoryFile) {
    // (handle, event, apc_routine, apc_context, io_status_block, file_info_ptr, length,
    //  file_info_class, file_name (ANSI_STRING*), restart_scan)
    auto f = file_of(arg32(ctx, 0));
    if (!f) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    uint32_t out = arg32(ctx, 5), len = arg32(ctx, 6);
    uint32_t name_ptr = stack_arg(ctx, 8);
    bool restart = stack_arg(ctx, 9) != 0;
    std::string pattern;
    if (name_ptr) pattern = ansi_string(name_ptr);
    hle::FileInfo e;
    uint32_t st = f->file->next_entry(name_ptr ? &pattern : nullptr, restart, e);
    uint32_t written = 0;
    if (st == STATUS_SUCCESS) {
        // FILE_DIRECTORY_INFORMATION with an ANSI name (the 360 does not use UTF-16 here).
        uint32_t need = 0x40 + uint32_t(e.name.size());
        if (need > len) {
            st = 0x80000005;  // STATUS_BUFFER_OVERFLOW
        } else {
            std::memset(host(out), 0, need);
            st32(out + 0x00, 0);
            put_times(out + 0x08, e);
            st64(out + 0x28, e.size);
            st64(out + 0x30, e.allocation_size);
            st32(out + 0x38, e.attributes);
            st32(out + 0x3C, uint32_t(e.name.size()));
            std::memcpy(host(out + 0x40), e.name.data(), e.name.size());
            written = need;
        }
    }
    iosb(arg32(ctx, 4), st, written);
    signal_event(arg32(ctx, 1));
    ret32(ctx, st);
}

FA_IMPORT(NtQueryVolumeInformationFile) {
    // (handle, io_status_block, fs_info_ptr, length, fs_info_class)
    auto f = file_of(arg32(ctx, 0));
    if (!f) { ret32(ctx, STATUS_INVALID_HANDLE); return; }
    hle::VolumeInfo v;
    uint32_t st = hle::services().vfs.volume(f->file->guest_path(), v);
    uint32_t p = arg32(ctx, 2), written = 0;
    if (st == STATUS_SUCCESS) {
        switch (arg32(ctx, 4)) {
            case 3:  // FileFsSizeInformation
                st64(p, v.total_bytes / (uint64_t(v.bytes_per_sector) * v.sectors_per_unit));
                st64(p + 8, v.free_bytes / (uint64_t(v.bytes_per_sector) * v.sectors_per_unit));
                st32(p + 16, v.sectors_per_unit);
                st32(p + 20, v.bytes_per_sector);
                written = 24; break;
            case 5:  // FileFsAttributeInformation
                st32(p, 0x3); st32(p + 4, 255);
                st32(p + 8, uint32_t(v.fs_name.size()));
                std::memcpy(host(p + 12), v.fs_name.data(), v.fs_name.size());
                written = 12 + uint32_t(v.fs_name.size()); break;
            default: st = STATUS_INVALID_INFO_CLASS;
        }
    }
    iosb(arg32(ctx, 1), st, written);
    ret32(ctx, st);
}

FA_IMPORT(ObCreateSymbolicLink) {
    // (name ANSI_STRING*, target ANSI_STRING*)
    hle::services().vfs.add_link(ansi_string(arg32(ctx, 0)), ansi_string(arg32(ctx, 1)));
    ret32(ctx, STATUS_SUCCESS);
}

FA_IMPORT(ObDeleteSymbolicLink) {
    hle::services().vfs.remove_link(ansi_string(arg32(ctx, 0)));
    ret32(ctx, STATUS_SUCCESS);
}
