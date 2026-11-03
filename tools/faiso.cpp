// faiso: list/extract files from an Xbox 360 ISO and inspect default.xex.
#include <cstdio>
#include <cstring>
#include <string>

#include "fa/xdvdfs.hpp"
#include "fa/xex.hpp"

static int usage() {
    std::fprintf(stderr,
        "usage: faiso <image.iso> list\n"
        "       faiso <image.iso> extract <path-in-iso> <dest>\n"
        "       faiso <image.iso> xex [path-in-iso, default: default.xex]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    fa::XdvdfsImage img;
    if (!img.open(argv[1])) { std::fprintf(stderr, "error: %s\n", img.error().c_str()); return 1; }
    std::string cmd = argv[2];

    if (cmd == "list") {
        for (auto& e : img.list())
            std::printf("%10u  %s%s\n", e.size, e.path.c_str(), e.is_dir ? "/" : "");
        return 0;
    }
    if (cmd == "extract" && argc == 5) {
        fa::IsoEntry e;
        if (!img.find(argv[3], e) || e.is_dir) { std::fprintf(stderr, "not found: %s\n", argv[3]); return 1; }
        if (!img.extract(e, argv[4])) { std::fprintf(stderr, "extract failed\n"); return 1; }
        std::printf("wrote %u bytes to %s\n", e.size, argv[4]);
        return 0;
    }
    if (cmd == "xex") {
        fa::IsoEntry e;
        const char* p = argc > 3 ? argv[3] : "default.xex";
        std::vector<uint8_t> data;
        fa::XexInfo x;
        if (!img.find(p, e) || !img.read(e, data)) { std::fprintf(stderr, "not found: %s\n", p); return 1; }
        if (!fa::parse_xex(data, x)) { std::fprintf(stderr, "not a valid XEX2 file\n"); return 1; }
        std::printf("title id:      %08X\nmedia id:      %08X\nversion:       %08X\n"
                    "module flags:  %08X\nPE offset:     0x%X\nsecurity info: 0x%X\n"
                    "image size:    0x%X\nbase address:  0x%08X\nentry point:   0x%08X\n"
                    "optional hdrs: %zu\n",
                    x.title_id, x.media_id, x.version, x.module_flags, x.pe_offset,
                    x.security_offset, x.image_size, x.base_address, x.entry_point,
                    x.optional_headers.size());
        return 0;
    }
    return usage();
}
