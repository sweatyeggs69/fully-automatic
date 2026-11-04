// XMA hardware decoder emulation.
//
// Context layout, register offsets and the overall decode loop follow what the
// Xenia project documented (BSD licence, https://github.com/xenia-project/xenia,
// src/xenia/apu/xma_context.*). The decoding itself is different: Xenia uses a
// patched FFmpeg with a custom "xmaframes" codec. We use the stock WMA Pro
// decoder instead, by re-wrapping each XMA frame in a synthetic WMA Pro packet,
// so the system FFmpeg works unchanged.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

#include "fa/hle/audio.hpp"

namespace fa::hle {

namespace {

constexpr uint32_t kBitsPerPacket = XmaDecoder::kBitsPerPacket;
constexpr uint32_t kHeaderBits = 32;
constexpr uint32_t kMaxFrameBits = 0x7FFF;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

inline uint32_t get_bit(const uint8_t* p, size_t bit) { return (p[bit >> 3] >> (7 - (bit & 7))) & 1; }
inline void set_bit(uint8_t* p, size_t bit, uint32_t v) {
    uint8_t m = uint8_t(0x80 >> (bit & 7));
    if (v) p[bit >> 3] |= m; else p[bit >> 3] &= uint8_t(~m);
}
uint32_t get_bits(const uint8_t* p, size_t bit, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | get_bit(p, bit + size_t(i));
    return v;
}
void put_bits(uint8_t* p, size_t bit, int n, uint32_t v) {
    for (int i = 0; i < n; ++i) set_bit(p, bit + size_t(i), (v >> (n - 1 - i)) & 1);
}

// XMA packet header: frame count (6) | first frame offset (15) | metadata (3) | skip (8).
uint32_t packet_first_frame(const uint8_t* pkt) { return get_bits(pkt, 6, 15) + kHeaderBits; }
uint32_t packet_metadata(const uint8_t* pkt) { return pkt[2] & 7; }
uint32_t packet_skip(const uint8_t* pkt) { return pkt[3]; }
bool first_frame_valid(uint32_t ffo) { return ffo >= kHeaderBits && ffo + 16 <= kBitsPerPacket; }

int sample_rate_hz(uint32_t id) {
    static const int r[4] = {24000, 32000, 44100, 48000};
    return r[id & 3];
}

// ------------------------------------------------------------ frame decoder

// Wraps one XMA frame at a time into a WMA Pro packet and decodes it to 512
// float samples per channel.
class FrameDecoder {
public:
    ~FrameDecoder() { close(); }

    bool configure(int rate, int channels) {
        if (ctx_ && rate == rate_ && channels == channels_) return true;
        close();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
        if (!codec) return false;
        ctx_ = avcodec_alloc_context3(codec);
        ctx_->sample_rate = rate;
        av_channel_layout_default(&ctx_->ch_layout, channels);
        ctx_->block_align = int(XmaDecoder::kBytesPerPacket);  // gives 15-bit frame lengths, like XMA
        // WAVEFORMATEX extension for WMA Pro: bits per sample, channel mask,
        // and decode flags 0x10D6 (the flags FFmpeg uses for XMA).
        ctx_->extradata = static_cast<uint8_t*>(av_mallocz(18 + AV_INPUT_BUFFER_PADDING_SIZE));
        ctx_->extradata_size = 18;
        uint8_t* e = ctx_->extradata;
        e[0] = 16;
        uint32_t mask = channels == 2 ? 0x3 : 0x4;
        e[2] = uint8_t(mask);
        e[14] = 0xD6;
        e[15] = 0x10;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            close();
            return false;
        }
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        rate_ = rate;
        channels_ = channels;
        seq_ = 0;
        return true;
    }

    void flush() {
        if (ctx_) avcodec_flush_buffers(ctx_);
    }

    // frame: MSB-first bits of one complete XMA frame (including its 15-bit
    // length). Writes 512 samples per channel into out[ch]. The first frame
    // after (re)configuration only primes the overlap and comes out silent.
    bool decode(const uint8_t* frame, uint32_t nbits, float* out[2]) {
        for (int c = 0; c < channels_; ++c) std::fill(out[c], out[c] + XmaDecoder::kSamplesPerFrame, 0.0f);
        if (!ctx_ || nbits < 16 || nbits > kMaxFrameBits) return false;
        // Build one or two WMA Pro packets: seq(4) | 2 unused bits | bits of the
        // previous frame carried in this packet (15) | frame data.
        constexpr uint32_t kPktHeader = 21;
        constexpr uint32_t kCap = kBitsPerPacket - kPktHeader;
        uint32_t first = std::min(nbits, kCap);
        uint8_t buf[2][XmaDecoder::kBytesPerPacket + AV_INPUT_BUFFER_PADDING_SIZE];
        std::memset(buf, 0, sizeof buf);
        int npk = 0;
        put_bits(buf[0], 0, 4, seq_++ & 15);
        put_bits(buf[0], 6, 15, 0);
        for (uint32_t i = 0; i < first; ++i) set_bit(buf[0], kPktHeader + i, get_bit(frame, i));
        npk = 1;
        if (nbits > kCap) {
            uint32_t rest = nbits - kCap;
            put_bits(buf[1], 0, 4, seq_++ & 15);
            put_bits(buf[1], 6, 15, rest);
            for (uint32_t i = 0; i < rest; ++i) set_bit(buf[1], kPktHeader + i, get_bit(frame, kCap + i));
            npk = 2;
        }
        // Clear the "more frames follow" trailer bit so each packet ends after this frame.
        uint32_t last = nbits - 1;
        if (last < kCap) set_bit(buf[0], kPktHeader + last, 0);
        else set_bit(buf[1], kPktHeader + (last - kCap), 0);

        bool got = false;
        for (int k = 0; k < npk; ++k) {
            pkt_->data = buf[k];
            pkt_->size = int(XmaDecoder::kBytesPerPacket);
            if (avcodec_send_packet(ctx_, pkt_) < 0) return false;
            while (avcodec_receive_frame(ctx_, frame_) == 0) {
                int n = std::min<int>(frame_->nb_samples, XmaDecoder::kSamplesPerFrame);
                for (int c = 0; c < channels_; ++c)
                    std::memcpy(out[c], frame_->extended_data[c], sizeof(float) * size_t(n));
                got = true;
                av_frame_unref(frame_);
            }
        }
        (void)got;
        return true;
    }

    int channels() const { return channels_; }

private:
    void close() {
        if (ctx_) avcodec_free_context(&ctx_);
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        rate_ = channels_ = 0;
    }
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    int rate_ = 0, channels_ = 0;
    uint32_t seq_ = 0;
};

// ------------------------------------------------------------ frame extraction

// Bits of a frame collected so far; persists across input buffer swaps.
struct Carry {
    uint8_t bits[(kMaxFrameBits + 7) / 8 + 8] = {};
    uint32_t have = 0;
    uint32_t len = 0;  // 0 until the 15-bit length has been read
    bool active() const { return have != 0; }
    void reset() { have = 0; len = 0; }
};

enum class Extract { Done, NeedMore, Invalid };

// Collects a frame starting at packet p, bit b of buf (packet_count packets),
// following this stream's packets via their skip counts. On Done, (p, b) is
// the bit just after the frame and crossed says whether it left its packet.
Extract extract_frame(const uint8_t* buf, uint32_t packet_count, uint32_t& p, uint32_t& b, Carry& c,
                      bool& crossed) {
    for (;;) {
        if (c.len == 0 && c.have >= 15) {
            c.len = get_bits(c.bits, 0, 15);
            if (c.len < 16 || c.len == kMaxFrameBits) return Extract::Invalid;
        }
        if (c.len && c.have == c.len) return Extract::Done;
        if (b >= kBitsPerPacket) {
            p = p + 1 + packet_skip(buf + size_t(p) * XmaDecoder::kBytesPerPacket);
            b = kHeaderBits;
            crossed = true;
            if (p >= packet_count) return Extract::NeedMore;
        }
        uint32_t need = c.len ? c.len - c.have : 15 - c.have;
        uint32_t take = std::min(need, kBitsPerPacket - b);
        const uint8_t* src = buf + size_t(p) * XmaDecoder::kBytesPerPacket;
        for (uint32_t i = 0; i < take; ++i) set_bit(c.bits, c.have + i, get_bit(src, b + i));
        c.have += take;
        b += take;
    }
}

// Finds the first frame start at or after packet p. Returns false at buffer end.
bool seek_frame_from_packet(const uint8_t* buf, uint32_t packet_count, uint32_t p, uint32_t& out_bit) {
    while (p < packet_count) {
        const uint8_t* pkt = buf + size_t(p) * XmaDecoder::kBytesPerPacket;
        uint32_t ffo = packet_first_frame(pkt);
        if (first_frame_valid(ffo)) {
            out_bit = p * kBitsPerPacket + ffo;
            return true;
        }
        p = p + 1 + packet_skip(pkt);
    }
    return false;
}

// Where the next frame starts after a frame that ended at (p, b).
bool next_frame_position(const uint8_t* buf, uint32_t packet_count, uint32_t p, uint32_t b, bool crossed,
                         bool trailer, uint32_t& out_bit) {
    const uint8_t* pkt = buf + size_t(p) * XmaDecoder::kBytesPerPacket;
    if (!crossed && trailer && b < kBitsPerPacket) {
        out_bit = p * kBitsPerPacket + b;
        return true;
    }
    if (crossed) {
        uint32_t ffo = packet_first_frame(pkt);
        if (first_frame_valid(ffo) && ffo >= b) {
            out_bit = p * kBitsPerPacket + ffo;
            return true;
        }
    }
    return seek_frame_from_packet(buf, packet_count, p + 1 + packet_skip(pkt), out_bit);
}

void to_be16(const float* const* in, int channels, uint8_t* out) {
    for (uint32_t i = 0; i < XmaDecoder::kSamplesPerFrame; ++i)
        for (int c = 0; c < channels; ++c) {
            float v = std::clamp(in[c][i], -1.0f, 1.0f) * 32767.0f;
            auto s = uint16_t(int16_t(v));
            out[0] = uint8_t(s >> 8);
            out[1] = uint8_t(s);
            out += 2;
        }
}

}  // namespace

// ------------------------------------------------------------ context data

void XmaContextData::load(const uint8_t* g) {
    uint32_t d[16];
    for (int i = 0; i < 16; ++i) d[i] = be32(g + 4 * i);
    auto f = [](uint32_t v, int shift, int bits) { return (v >> shift) & ((1u << bits) - 1); };
    input_buffer_0_packet_count = f(d[0], 0, 12);
    loop_count = f(d[0], 12, 8);
    input_buffer_0_valid = f(d[0], 20, 1);
    input_buffer_1_valid = f(d[0], 21, 1);
    output_buffer_block_count = f(d[0], 22, 5);
    output_buffer_write_offset = f(d[0], 27, 5);
    input_buffer_1_packet_count = f(d[1], 0, 12);
    loop_subframe_start = f(d[1], 12, 2);
    loop_subframe_end = f(d[1], 14, 3);
    loop_subframe_skip = f(d[1], 17, 3);
    subframe_decode_count = f(d[1], 20, 4);
    subframe_skip_count = f(d[1], 24, 3);
    sample_rate = f(d[1], 27, 2);
    is_stereo = f(d[1], 29, 1);
    unk_dword_1_c = f(d[1], 30, 1);
    output_buffer_valid = f(d[1], 31, 1);
    input_buffer_read_offset = f(d[2], 0, 26);
    unk_dword_2 = f(d[2], 26, 6);
    loop_start = f(d[3], 0, 26);
    unk_dword_3 = f(d[3], 26, 6);
    loop_end = f(d[4], 0, 26);
    packet_metadata = f(d[4], 26, 5);
    current_buffer = f(d[4], 31, 1);
    input_buffer_0_ptr = d[5];
    input_buffer_1_ptr = d[6];
    output_buffer_ptr = d[7];
    work_buffer_ptr = d[8];
    output_buffer_read_offset = f(d[9], 0, 5);
    unk_dword_9 = f(d[9], 5, 25);
    stop_when_done = f(d[9], 30, 1);
    interrupt_when_done = f(d[9], 31, 1);
    for (int i = 0; i < 6; ++i) reserved[i] = d[10 + i];
}

void XmaContextData::store(uint8_t* g) const {
    auto f = [](uint32_t v, int shift, int bits) { return (v & ((1u << bits) - 1)) << shift; };
    uint32_t d[16];
    d[0] = f(input_buffer_0_packet_count, 0, 12) | f(loop_count, 12, 8) | f(input_buffer_0_valid, 20, 1) |
           f(input_buffer_1_valid, 21, 1) | f(output_buffer_block_count, 22, 5) |
           f(output_buffer_write_offset, 27, 5);
    d[1] = f(input_buffer_1_packet_count, 0, 12) | f(loop_subframe_start, 12, 2) | f(loop_subframe_end, 14, 3) |
           f(loop_subframe_skip, 17, 3) | f(subframe_decode_count, 20, 4) | f(subframe_skip_count, 24, 3) |
           f(sample_rate, 27, 2) | f(is_stereo, 29, 1) | f(unk_dword_1_c, 30, 1) | f(output_buffer_valid, 31, 1);
    d[2] = f(input_buffer_read_offset, 0, 26) | f(unk_dword_2, 26, 6);
    d[3] = f(loop_start, 0, 26) | f(unk_dword_3, 26, 6);
    d[4] = f(loop_end, 0, 26) | f(packet_metadata, 26, 5) | f(current_buffer, 31, 1);
    d[5] = input_buffer_0_ptr;
    d[6] = input_buffer_1_ptr;
    d[7] = output_buffer_ptr;
    d[8] = work_buffer_ptr;
    d[9] = f(output_buffer_read_offset, 0, 5) | f(unk_dword_9, 5, 25) | f(stop_when_done, 30, 1) |
           f(interrupt_when_done, 31, 1);
    for (int i = 0; i < 6; ++i) d[10 + i] = reserved[i];
    for (int i = 0; i < 16; ++i) put_be32(g + 4 * i, d[i]);
}

// ------------------------------------------------------------ hardware context

struct XmaDecoder::Context {
    uint32_t guest_ptr = 0;
    bool allocated = false;
    bool enabled = false;
    std::mutex mu;
    FrameDecoder dec;
    Carry carry;
    bool carry_crossed = false;
    int trace_count = 0;

    void reset_stream() {
        carry.reset();
        carry_crossed = false;
        dec.flush();
    }

    static void swap_input_buffer(XmaContextData& d) {
        if (d.current_buffer == 0) d.input_buffer_0_valid = 0;
        else d.input_buffer_1_valid = 0;
        d.current_buffer ^= 1;
        d.input_buffer_read_offset = 0;
    }

    static bool try_loop(XmaContextData& d, bool ignore_offset) {
        if (d.loop_count > 0 && d.loop_start < d.loop_end &&
            (ignore_offset || d.input_buffer_read_offset >= d.loop_end)) {
            d.input_buffer_read_offset = d.loop_start;
            if (d.loop_count < 255) d.loop_count--;
            return true;
        }
        return false;
    }

    void decode(XmaContextData& d, const GuestHooks& hooks) {
        if (!d.output_buffer_valid) return;
        if (!d.input_buffer_0_valid && !d.input_buffer_1_valid) {
            d.output_buffer_valid = 0;
            return;
        }
        const int channels = d.is_stereo ? 2 : 1;
        if (!dec.configure(sample_rate_hz(d.sample_rate), channels)) return;

        const uint32_t cap = d.output_buffer_block_count * kBytesPerBlock;
        if (cap == 0) return;
        uint8_t* out = hooks.phys(d.output_buffer_ptr);
        uint32_t rd = d.output_buffer_read_offset * kBytesPerBlock % cap;
        uint32_t wr = d.output_buffer_write_offset * kBytesPerBlock % cap;
        uint32_t used = wr >= rd ? wr - rd : cap - rd + wr;
        uint32_t space = cap - used;
        const uint32_t frame_bytes = kSamplesPerFrame * 2 * uint32_t(channels);

        float pcm0[kSamplesPerFrame], pcm1[kSamplesPerFrame];
        float* pcm[2] = {pcm0, pcm1};
        uint8_t be[kSamplesPerFrame * 4];

        int guard = 0;
        while (space >= frame_bytes && guard++ < 256) {
            if (!d.input_buffer_0_valid && !d.input_buffer_1_valid) break;
            bool cur_valid = d.current_buffer ? d.input_buffer_1_valid : d.input_buffer_0_valid;
            if (!cur_valid) {  // the game refilled the other buffer only
                d.current_buffer ^= 1;
                d.input_buffer_read_offset = 0;
                continue;
            }
            const uint32_t count = d.current_buffer ? d.input_buffer_1_packet_count : d.input_buffer_0_packet_count;
            const uint8_t* in = hooks.phys(d.current_buffer ? d.input_buffer_1_ptr : d.input_buffer_0_ptr);
            if (count == 0) {
                swap_input_buffer(d);
                continue;
            }

            uint32_t p, b;
            bool crossed = false;
            if (carry.active()) {
                // A frame started in the previous buffer continues here.
                p = 0;
                b = kHeaderBits;
                crossed = true;
            } else {
                bool reuse = try_loop(d, false);
                (void)reuse;
                uint32_t pos = d.input_buffer_read_offset;
                if (pos % kBitsPerPacket < kHeaderBits) {
                    if (!seek_frame_from_packet(in, count, pos / kBitsPerPacket, pos)) {
                        if (!try_loop(d, true)) swap_input_buffer(d);
                        continue;
                    }
                    d.input_buffer_read_offset = pos;
                }
                p = pos / kBitsPerPacket;
                b = pos % kBitsPerPacket;
                if (p >= count) {
                    if (!try_loop(d, true)) swap_input_buffer(d);
                    continue;
                }
            }
            d.packet_metadata = packet_metadata(in + size_t(p) * kBytesPerPacket);

            Extract r = extract_frame(in, count, p, b, carry, crossed);
            if (r == Extract::NeedMore) {
                // Rest of the frame is in the other buffer (if the game gives us one).
                if (!try_loop(d, true)) swap_input_buffer(d);
                else carry.reset();  // looping back: drop the partial frame
                continue;
            }
            if (r == Extract::Invalid) {
                st_bad_frames_.fetch_add(1, std::memory_order_relaxed);
                carry.reset();
                uint32_t nb;
                if (seek_frame_from_packet(in, count, p + 1 + packet_skip(in + size_t(p) * kBytesPerPacket), nb)) {
                    d.input_buffer_read_offset = nb;
                } else if (!try_loop(d, true)) {
                    swap_input_buffer(d);
                }
                continue;
            }

            bool trailer = get_bit(carry.bits, carry.len - 1) != 0;
            if (!dec.decode(carry.bits, carry.len, pcm)) st_decode_errors_.fetch_add(1, std::memory_order_relaxed);
            st_frames_.fetch_add(1, std::memory_order_relaxed);
            carry.reset();
            to_be16(pcm, channels, be);
            for (uint32_t i = 0; i < frame_bytes; ++i) out[(wr + i) % cap] = be[i];
            wr = (wr + frame_bytes) % cap;
            space -= frame_bytes;
            d.output_buffer_write_offset = wr / kBytesPerBlock;

            uint32_t next;
            if (next_frame_position(in, count, p, b, crossed, trailer, next)) {
                d.input_buffer_read_offset = next;
            } else if (!try_loop(d, true)) {
                swap_input_buffer(d);
            }
        }
        // A full ring reads as read == write; mark it invalid until the game
        // consumes some output and validates it again.
        if (wr == rd) d.output_buffer_valid = 0;
    }
};

// ------------------------------------------------------------ decoder

std::atomic<uint64_t> XmaDecoder::st_frames_{0}, XmaDecoder::st_bad_frames_{0}, XmaDecoder::st_decode_errors_{0};

XmaDecoder::XmaDecoder() = default;

XmaDecoder::Stats XmaDecoder::stats() {
    Stats s;
    s.kicks = st_kicks_.load(std::memory_order_relaxed);
    s.passes = st_passes_.load(std::memory_order_relaxed);
    s.loops = st_loops_.load(std::memory_order_relaxed);
    s.frames = st_frames_.load(std::memory_order_relaxed);
    s.bad_frames = st_bad_frames_.load(std::memory_order_relaxed);
    s.decode_errors = st_decode_errors_.load(std::memory_order_relaxed);
    std::lock_guard lk(alloc_mu_);
    for (auto& c : contexts_) s.allocated += c->allocated ? 1 : 0;
    return s;
}
XmaDecoder::~XmaDecoder() { shutdown(); }

bool XmaDecoder::init(const GuestHooks& hooks, bool start_thread) {
    hooks_ = hooks;
    av_log_set_level(AV_LOG_ERROR);
    array_ptr_ = hooks_.alloc ? hooks_.alloc(kContextCount * kContextSize, 256) : 0;
    if (!array_ptr_) return false;
    contexts_.clear();
    for (int i = 0; i < kContextCount; ++i) {
        auto c = std::make_unique<Context>();
        c->guest_ptr = array_ptr_ + uint32_t(i) * kContextSize;
        contexts_.push_back(std::move(c));
    }
    uint8_t* regs = hooks_.host(kRegisterBase);
    put_be32(regs + kRegContextArray, array_ptr_ & 0x1FFFFFFF);
    put_be32(regs + kRegNextContext, 1);
    if (start_thread) {
        running_ = true;
        worker_ = std::thread([this] { worker_main(); });
    }
    return true;
}

void XmaDecoder::shutdown() {
    if (running_.exchange(false)) {
        {
            std::lock_guard lk(wake_mu_);
            wake_ = true;
        }
        wake_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    if (array_ptr_ && hooks_.free) hooks_.free(array_ptr_);
    array_ptr_ = 0;
    contexts_.clear();
}

int XmaDecoder::context_index(uint32_t ptr) const {
    if (!array_ptr_ || ptr < array_ptr_) return -1;
    uint32_t off = ptr - array_ptr_;
    if (off >= kContextCount * kContextSize || (off % kContextSize)) return -1;
    return int(off / kContextSize);
}

uint32_t XmaDecoder::create_context() {
    std::lock_guard lk(alloc_mu_);
    for (auto& c : contexts_) {
        if (c->allocated) continue;
        std::lock_guard cl(c->mu);
        c->allocated = true;
        c->enabled = false;
        c->reset_stream();
        std::memset(hooks_.host(c->guest_ptr), 0, kContextSize);
        return c->guest_ptr;
    }
    return 0;
}

void XmaDecoder::release_context(uint32_t ptr) {
    int i = context_index(ptr);
    if (i < 0) return;
    std::lock_guard lk(alloc_mu_);
    Context& c = *contexts_[size_t(i)];
    std::lock_guard cl(c.mu);
    c.allocated = false;
    c.enabled = false;
    c.reset_stream();
    std::memset(hooks_.host(ptr), 0, kContextSize);
}

void XmaDecoder::enable_context(uint32_t ptr) {
    int i = context_index(ptr);
    if (i < 0) return;
    uint64_t nk = st_kicks_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard cl(contexts_[size_t(i)]->mu);
        if (trace_ctx_ && nk < 20)
            std::fprintf(stderr, "[xma] kick #%llu ctx %d allocated=%d\n", (unsigned long long)nk, i,
                         int(contexts_[size_t(i)]->allocated));
        contexts_[size_t(i)]->enabled = true;
    }
    {
        std::lock_guard lk(wake_mu_);
        wake_ = true;
    }
    wake_cv_.notify_one();
}

bool XmaDecoder::disable_context(uint32_t ptr, bool wait) {
    int i = context_index(ptr);
    if (i < 0) return false;
    Context& c = *contexts_[size_t(i)];
    std::unique_lock cl(c.mu, std::defer_lock);
    if (wait) cl.lock();
    else if (!cl.try_lock()) return false;  // busy decoding right now
    c.enabled = false;
    return true;
}

void XmaDecoder::clear_context(uint32_t ptr) {
    int i = context_index(ptr);
    if (i < 0) return;
    Context& c = *contexts_[size_t(i)];
    std::lock_guard cl(c.mu);
    uint8_t* g = hooks_.host(ptr);
    XmaContextData d;
    d.load(g);
    d.input_buffer_0_valid = d.input_buffer_1_valid = d.output_buffer_valid = 0;
    d.output_buffer_read_offset = d.output_buffer_write_offset = 0;
    d.store(g);
    c.reset_stream();
}

bool XmaDecoder::read_context(uint32_t ptr, XmaContextData& out) {
    int i = context_index(ptr);
    if (i < 0) return false;
    std::lock_guard cl(contexts_[size_t(i)]->mu);
    out.load(hooks_.host(ptr));
    return true;
}

bool XmaDecoder::modify_context(uint32_t ptr, const std::function<void(XmaContextData&)>& fn) {
    int i = context_index(ptr);
    if (i < 0) return false;
    std::lock_guard cl(contexts_[size_t(i)]->mu);
    XmaContextData d;
    d.load(hooks_.host(ptr));
    fn(d);
    d.store(hooks_.host(ptr));
    return true;
}

void XmaDecoder::write_register(uint32_t offset, uint32_t value) {
    auto for_bits = [&](uint32_t base_reg, auto fn) {
        uint32_t base = (offset - base_reg) / 4 * 32;
        for (int b = 0; b < 32; ++b)
            if (value & (1u << b)) {
                uint32_t idx = base + uint32_t(b);
                if (idx < kContextCount) fn(array_ptr_ + idx * kContextSize);
            }
    };
    if (offset >= kRegKick && offset < kRegKick + 40) for_bits(kRegKick, [&](uint32_t p) { enable_context(p); });
    else if (offset >= kRegLock && offset < kRegLock + 40) for_bits(kRegLock, [&](uint32_t p) { disable_context(p, true); });
    else if (offset >= kRegClear && offset < kRegClear + 40) for_bits(kRegClear, [&](uint32_t p) { clear_context(p); });
}

uint32_t XmaDecoder::read_register(uint32_t offset) {
    uint8_t* regs = hooks_.host(kRegisterBase);
    if (offset == kRegCurrentContext) {
        uint32_t next = be32(regs + kRegNextContext);
        put_be32(regs + kRegCurrentContext, next);
        put_be32(regs + kRegNextContext, (next + 1) % kContextCount);
    }
    return be32(regs + offset);
}

void XmaDecoder::poll_registers() {
    // The guest writes the kick/lock/clear words as plain memory; take and
    // clear them atomically, then act on the bits.
    uint8_t* regs = hooks_.host(kRegisterBase);
    for (uint32_t base : {kRegClear, kRegLock, kRegKick}) {
        for (uint32_t r = 0; r < 10; ++r) {
            auto* w = reinterpret_cast<uint32_t*>(regs + base + r * 4);
            if (__atomic_load_n(w, __ATOMIC_RELAXED) == 0) continue;
            uint32_t raw = __atomic_exchange_n(w, 0u, __ATOMIC_ACQ_REL);
            if (raw) write_register(base + r * 4, __builtin_bswap32(raw));
        }
    }
}

bool XmaDecoder::work_once() {
    if (hooks_.membase) poll_registers();
    bool did = false;
    for (auto& cp : contexts_) {
        Context& c = *cp;
        std::lock_guard cl(c.mu);
        if (!c.allocated || !c.enabled) continue;
        c.enabled = false;
        uint8_t* g = hooks_.host(c.guest_ptr);
        XmaContextData d;
        d.load(g);
        uint64_t frames_before = st_frames_.load(std::memory_order_relaxed);
        XmaContextData before = d;
        c.decode(d, hooks_);
        d.store(g);
        st_passes_.fetch_add(1, std::memory_order_relaxed);
        if (trace_ctx_ && st_frames_.load(std::memory_order_relaxed) == frames_before && c.trace_count++ < 12) {
            // FA_XMA_DEBUG: a kicked context that decoded nothing; show why.
            std::fprintf(stderr,
                "[xma] ctx %d kicked, no frame: in0 valid=%u ptr=%08x pk=%u | in1 valid=%u ptr=%08x pk=%u | "
                "cur=%u rd=%u | out valid=%u ptr=%08x blocks=%u wr=%u rd=%u | stereo=%u rate=%u loop=%u\n",
                int((c.guest_ptr - array_ptr_) / kContextSize), before.input_buffer_0_valid, before.input_buffer_0_ptr,
                before.input_buffer_0_packet_count, before.input_buffer_1_valid, before.input_buffer_1_ptr,
                before.input_buffer_1_packet_count, before.current_buffer, before.input_buffer_read_offset,
                before.output_buffer_valid, before.output_buffer_ptr, before.output_buffer_block_count,
                before.output_buffer_write_offset, before.output_buffer_read_offset, before.is_stereo,
                before.sample_rate, before.loop_count);
        }
        did = true;
    }
    return did;
}

void XmaDecoder::worker_main() {
    int idle = 0;
    while (running_) {
        st_loops_.fetch_add(1, std::memory_order_relaxed);
        bool did = work_once();
        idle = did ? 0 : idle + 1;
        if (idle > 200) {
            std::unique_lock lk(wake_mu_);
            wake_cv_.wait_for(lk, std::chrono::milliseconds(2), [&] { return wake_; });
            wake_ = false;
        } else if (!did) {
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
    }
}

// ------------------------------------------------------------ offline decode

bool XmaDecoder::decode_stream(const uint8_t* data, size_t size, int channels, int sample_rate,
                               std::vector<int16_t>& pcm_out, std::string& err) {
    if (channels < 1 || channels > 2) {
        err = "only 1 or 2 channel XMA streams are supported";
        return false;
    }
    uint32_t count = uint32_t(size / kBytesPerPacket);
    if (count == 0) {
        err = "stream is shorter than one packet";
        return false;
    }
    int rate_id = sample_rate >= 48000 ? 3 : sample_rate >= 44100 ? 2 : sample_rate >= 32000 ? 1 : 0;
    FrameDecoder dec;
    if (!dec.configure(sample_rate_hz(uint32_t(rate_id)), channels)) {
        err = "FFmpeg WMA Pro decoder unavailable";
        return false;
    }
    auto carry = std::make_unique<Carry>();
    float pcm0[kSamplesPerFrame], pcm1[kSamplesPerFrame];
    float* pcm[2] = {pcm0, pcm1};
    uint32_t pos;
    if (!seek_frame_from_packet(data, count, 0, pos)) {
        err = "no frames found";
        return false;
    }
    size_t frames = 0;
    for (;;) {
        uint32_t p = pos / kBitsPerPacket, b = pos % kBitsPerPacket;
        bool crossed = false;
        Extract r = extract_frame(data, count, p, b, *carry, crossed);
        if (r == Extract::NeedMore) break;
        if (r == Extract::Invalid) {
            carry->reset();
            if (!seek_frame_from_packet(data, count, p + 1 + packet_skip(data + size_t(p) * kBytesPerPacket), pos)) break;
            continue;
        }
        bool trailer = get_bit(carry->bits, carry->len - 1) != 0;
        if (std::getenv("FA_XMA_TRACE")) std::fprintf(stderr, "frame %u len %u end %u:%u crossed %d trailer %d\n", pos, carry->len, p, b, crossed, trailer);
        dec.decode(carry->bits, carry->len, pcm);
        carry->reset();
        for (uint32_t i = 0; i < kSamplesPerFrame; ++i)
            for (int c = 0; c < channels; ++c)
                pcm_out.push_back(int16_t(std::clamp(pcm[c][i], -1.0f, 1.0f) * 32767.0f));
        ++frames;
        if (!next_frame_position(data, count, p, b, crossed, trailer, pos)) break;
    }
    if (!frames) {
        err = "no frames decoded";
        return false;
    }
    return true;
}

}  // namespace fa::hle
