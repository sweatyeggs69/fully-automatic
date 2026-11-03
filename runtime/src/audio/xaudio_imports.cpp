// xboxkrnl audio imports: the XAudio render driver and the XMA decoder.
// The work happens in fa::hle::AudioSystem / XmaDecoder (src/hle/).
#include <chrono>
#include <cstring>
#include <thread>

#include "fa/hle/services.hpp"
#include "fa/rt/import.hpp"

using namespace fa::rt;
using fa::hle::services;
using fa::hle::XmaContextData;

namespace {
constexpr uint32_t kXFalse = 1;  // X_E_FALSE

uint32_t to_phys(uint32_t guest) { return guest & 0x1FFFFFFFu; }

void store_float(uint32_t addr, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    st32(addr, v);
}
}  // namespace

// ---------------------------------------------------------------- render driver

FA_IMPORT(XAudioGetSpeakerConfig) {
    uint32_t out = arg32(ctx, 0);
    if (out) st32(out, 0x00010001);  // stereo / digital 5.1 capable
    ret32(ctx, 0);
}

FA_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask) {
    uint32_t out = arg32(ctx, 1);
    if (out) st32(out, 0);
    std::this_thread::yield();
    ret32(ctx, 0);
}

FA_IMPORT(XAudioGetVoiceCategoryVolume) {
    uint32_t out = arg32(ctx, 1);
    if (out) store_float(out, 1.0f);
    ret32(ctx, 0);
}

FA_IMPORT(XAudioEnableDucker) { ret32(ctx, 0); }

FA_IMPORT(XAudioRegisterRenderDriverClient) {
    uint32_t cb_ptr = arg32(ctx, 0), driver_out = arg32(ctx, 1);
    uint32_t driver = 0;
    uint32_t rc = services().audio.register_client(ld32(cb_ptr), ld32(cb_ptr + 4), driver);
    if (rc == 0 && driver_out) st32(driver_out, driver);
    ret32(ctx, rc);
}

FA_IMPORT(XAudioUnregisterRenderDriverClient) {
    ret32(ctx, services().audio.unregister_client(arg32(ctx, 0)));
}

FA_IMPORT(XAudioSubmitRenderDriverFrame) {
    ret32(ctx, services().audio.submit_frame(arg32(ctx, 0), arg32(ctx, 1)));
}

// ---------------------------------------------------------------- XMA

FA_IMPORT(XMACreateContext) {
    uint32_t out = arg32(ctx, 0);
    uint32_t p = services().xma.create_context();
    if (out) st32(out, p);
    ret32(ctx, p ? STATUS_SUCCESS : STATUS_NO_MEMORY);
}

FA_IMPORT(XMAReleaseContext) {
    services().xma.release_context(arg32(ctx, 0));
    ret32(ctx, 0);
}

// XMA_CONTEXT_INIT (56 bytes, big-endian):
//   +0 input_buffer_0_ptr, +4 input_buffer_0_packet_count, +8 input_buffer_1_ptr,
//   +12 input_buffer_1_packet_count, +16 input_buffer_read_offset, +20 output_buffer_ptr,
//   +24 output_buffer_block_count, +28 work_buffer, +32 subframe_decode_count,
//   +36 channel_count, +40 sample_rate, +44 XMA_LOOP_DATA (12 bytes)
FA_IMPORT(XMAInitializeContext) {
    uint32_t c = arg32(ctx, 0), init = arg32(ctx, 1);
    auto& xma = services().xma;
    std::memset(host(c), 0, fa::hle::XmaDecoder::kContextSize);
    xma.modify_context(c, [&](XmaContextData& d) {
        uint32_t in0 = ld32(init + 0), in1 = ld32(init + 8);
        d.input_buffer_0_ptr = in0 ? to_phys(in0) : 0;
        d.input_buffer_0_packet_count = ld32(init + 4);
        d.input_buffer_1_ptr = in1 ? to_phys(in1) : 0;
        d.input_buffer_1_packet_count = ld32(init + 12);
        d.input_buffer_read_offset = ld32(init + 16);
        d.output_buffer_ptr = to_phys(ld32(init + 20));
        d.output_buffer_block_count = ld32(init + 24);
        d.work_buffer_ptr = ld32(init + 28);
        d.subframe_decode_count = ld32(init + 32);
        d.is_stereo = ld32(init + 36) >= 1;
        d.sample_rate = ld32(init + 40);
        d.loop_start = ld32(init + 44);
        d.loop_end = ld32(init + 48);
        d.loop_count = ld8(init + 52);
        d.loop_subframe_end = ld8(init + 53);
        d.loop_subframe_skip = ld8(init + 54);
    });
    xma.clear_context(c);
    ret32(ctx, 0);
}

// XMA_LOOP_DATA: +0 loop_start, +4 loop_end (bit offsets), +8 count, +9 subframe end, +10 subframe skip.
FA_IMPORT(XMASetLoopData) {
    uint32_t c = arg32(ctx, 0), loop = arg32(ctx, 1);
    services().xma.modify_context(c, [&](XmaContextData& d) {
        d.loop_start = ld32(loop + 0);
        d.loop_end = ld32(loop + 4);
        d.loop_count = ld8(loop + 8);
        d.loop_subframe_end = ld8(loop + 9);
        d.loop_subframe_skip = ld8(loop + 10);
    });
    ret32(ctx, 0);
}

#define XMA_GETTER(name, field)                       \
    FA_IMPORT(name) {                                 \
        XmaContextData d;                             \
        services().xma.read_context(arg32(ctx, 0), d); \
        ret32(ctx, d.field);                          \
    }

XMA_GETTER(XMAGetInputBufferReadOffset, input_buffer_read_offset)
XMA_GETTER(XMAIsInputBuffer0Valid, input_buffer_0_valid)
XMA_GETTER(XMAIsInputBuffer1Valid, input_buffer_1_valid)
XMA_GETTER(XMAIsOutputBufferValid, output_buffer_valid)
XMA_GETTER(XMAGetOutputBufferReadOffset, output_buffer_read_offset)
XMA_GETTER(XMAGetOutputBufferWriteOffset, output_buffer_write_offset)
XMA_GETTER(XMAGetPacketMetadata, packet_metadata)

FA_IMPORT(XMASetInputBufferReadOffset) {
    uint32_t v = arg32(ctx, 1);
    services().xma.modify_context(arg32(ctx, 0), [&](XmaContextData& d) { d.input_buffer_read_offset = v; });
    ret32(ctx, 0);
}

FA_IMPORT(XMASetInputBuffer0) {
    uint32_t buf = arg32(ctx, 1), count = arg32(ctx, 2);
    services().xma.modify_context(arg32(ctx, 0), [&](XmaContextData& d) {
        d.input_buffer_0_ptr = to_phys(buf);
        d.input_buffer_0_packet_count = count;
    });
    ret32(ctx, 0);
}

FA_IMPORT(XMASetInputBuffer1) {
    uint32_t buf = arg32(ctx, 1), count = arg32(ctx, 2);
    services().xma.modify_context(arg32(ctx, 0), [&](XmaContextData& d) {
        d.input_buffer_1_ptr = to_phys(buf);
        d.input_buffer_1_packet_count = count;
    });
    ret32(ctx, 0);
}

FA_IMPORT(XMASetInputBuffer0Valid) {
    services().xma.modify_context(arg32(ctx, 0), [](XmaContextData& d) { d.input_buffer_0_valid = 1; });
    ret32(ctx, 0);
}

FA_IMPORT(XMASetInputBuffer1Valid) {
    services().xma.modify_context(arg32(ctx, 0), [](XmaContextData& d) { d.input_buffer_1_valid = 1; });
    ret32(ctx, 0);
}

FA_IMPORT(XMASetOutputBufferValid) {
    services().xma.modify_context(arg32(ctx, 0), [](XmaContextData& d) { d.output_buffer_valid = 1; });
    ret32(ctx, 0);
}

FA_IMPORT(XMASetOutputBufferReadOffset) {
    uint32_t v = arg32(ctx, 1);
    services().xma.modify_context(arg32(ctx, 0), [&](XmaContextData& d) { d.output_buffer_read_offset = v; });
    ret32(ctx, 0);
}

FA_IMPORT(XMAEnableContext) {
    services().xma.enable_context(arg32(ctx, 0));
    ret32(ctx, 0);
}

FA_IMPORT(XMADisableContext) {
    bool ok = services().xma.disable_context(arg32(ctx, 0), arg32(ctx, 1) != 0);
    ret32(ctx, ok ? 0 : kXFalse);
}

FA_IMPORT(XMABlockWhileInUse) {
    uint32_t c = arg32(ctx, 0);
    for (;;) {
        XmaContextData d;
        if (!services().xma.read_context(c, d)) break;
        if (!d.input_buffer_0_valid && !d.input_buffer_1_valid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ret32(ctx, 0);
}
