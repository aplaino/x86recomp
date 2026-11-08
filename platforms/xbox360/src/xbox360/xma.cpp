// The XMA decoder (xma.h): the context array, its registers, commands and
// decoding.
//
// The registers', the commands' and the context's fields' meaning is
// Xenia's XMA decoder's (BSD-3); what xam's and titles' code shows is
// noted in docs/xboxkrnl.md, "XMA". XMA's frames are WMA Pro's: ffmpeg's
// wmapro decoder (LGPL, linked dynamically) decodes them, one frame to a
// packet built for it, while the walk through the context's packets, its
// buffers and loops, and the output ring is the decoder's own here.

#include "xbox360/xma.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}

#include "ppc/fatal.h"
#include "xbox360/hle.h"
#include "xbox360/kernel.h"
#include "xbox360/memory.h"

namespace recomp::xbox360 {
namespace {

using ppc::Fatal;

constexpr uint32_t kCommandWords = (kXmaContexts + 31) / 32;

// Input: packets of 2 KiB, each a 32-bit header and frames' bits; a frame
// starts with its length in bits (15 bits) and may run on into the
// stream's next packet. A context's input read offset is in bits from its
// current buffer's start.
constexpr uint32_t kPacketBytes = 2048, kPacketBits = kPacketBytes * 8, kHeaderBits = 32,
                   kFrameLengthBits = 15;
// A length that is no frame: the rest of the packet is padding.
constexpr uint32_t kNoFrame = 0x7FFF;
// Output: 512 samples a channel per frame, 16-bit, in blocks of 256 bytes
// (a subframe of 128 samples is a block a channel).
constexpr uint32_t kSamplesPerFrame = 512, kBlockBytes = 256;

// The context's words (big-endian in guest memory), as Xenia lays them
// out: the bit of each field and its width.
struct Field {
  uint32_t word, shift, width;
};
constexpr Field kInputPackets0{0, 0, 12}, kLoopCount{0, 12, 8}, kInputValid0{0, 20, 1},
    kInputValid1{0, 21, 1}, kOutputBlocks{0, 22, 5}, kOutputWrite{0, 27, 5}, kInputPackets1{1, 0, 12},
    kLoopSubframeEnd{1, 14, 3}, kLoopSubframeSkip{1, 17, 3}, kSubframeDecodeCount{1, 20, 4},
    kOutputPadding{1, 24, 3}, kSampleRate{1, 27, 2}, kStereo{1, 29, 1}, kOutputValid{1, 31, 1},
    kInputRead{2, 0, 26}, kLoopStart{3, 0, 26}, kLoopEnd{4, 0, 26}, kCurrentBuffer{4, 31, 1},
    kInput0{5, 0, 32}, kInput1{6, 0, 32}, kOutput{7, 0, 32}, kOutputRead{9, 0, 5};
constexpr uint32_t kRates[4] = {24000, 32000, 44100, 48000};

uint32_t Mask(Field f) { return f.width == 32 ? ~0u : ((1u << f.width) - 1) << f.shift; }

// The packet ffmpeg's wmapro decoder takes a frame in: a WMA Pro packet
// header (a 4-bit sequence number, 2 bits, the bits of a frame begun in
// the packet before, 15 bits, here 0), the frame and nothing after it.
// block_align says how long a packet is: 2048 to 4095 bytes give frame
// lengths of 15 bits, as XMA's; 4095 holds the longest frame.
constexpr uint32_t kWmaHeaderBits = 21, kWmaPacketBytes = 4095,
                   kMaxFrameBits = kWmaPacketBytes * 8 - kWmaHeaderBits;
// The wmapro decoder's extradata: 16 bits a sample, no channel mask (the
// channels are the codec context's), and the decode flags XMA's frames
// are coded with (ffmpeg's xma decoders use the same).
constexpr uint8_t kWmaExtradata[18] = {16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xD6, 0x10, 0, 0};

// A context's decoding on the host: ffmpeg's decoder, and the frame being
// written out.
struct Decoding {
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  uint32_t rate = 0, channels = 0;
  uint8_t sequence = 0;
  // The frame ffmpeg decodes into (Planes), whatever it trims.
  float planes[2][kSamplesPerFrame];
  alignas(16) uint8_t bits[kWmaPacketBytes + AV_INPUT_BUFFER_PADDING_SIZE];
  // The frame decoded, big-endian, channels interleaved; the next block to
  // write and how many to write in all.
  uint8_t pcm[2 * kSamplesPerFrame * 2];
  uint32_t next = 0, end = 0;
  // Subframes of the next frame to leave out (a loop's start).
  uint32_t skip = 0;
};

std::mutex g_lock;
uint32_t g_array;  // the context array's guest address
std::bitset<kXmaContexts> g_used;
std::unique_ptr<Decoding> g_decoding[kXmaContexts];

uint32_t Bswap(uint32_t v) { return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24; }
uint32_t Guest(uint32_t v) { return std::endian::native == std::endian::little ? Bswap(v) : v; }
uint32_t RegisterVa(uint32_t r) { return kXmaRegisters + 4 * r; }
uint32_t ContextVa(uint32_t index) { return g_array + index * kXmaContextSize; }

// Takes the bits written to a command register, leaving it 0 for the next.
// The word's bytes are the register's, little-endian.
uint32_t TakeCommand(uint32_t r) {
  auto* word = reinterpret_cast<uint32_t*>(Mem() + RegisterVa(r));
  uint32_t bytes = std::atomic_ref<uint32_t>(*word).exchange(0);
  return std::endian::native == std::endian::little ? bytes : Bswap(bytes);
}

// Sets fields of a context word, leaving the others as the title last
// wrote them: titles write a context while it decodes (the other input
// buffer, the output read offset).
void Update(uint32_t va, uint32_t word, uint32_t mask, uint32_t value) {
  std::atomic_ref<uint32_t> ref(*reinterpret_cast<uint32_t*>(Mem() + va + 4 * word));
  uint32_t raw = ref.load();
  while (!ref.compare_exchange_weak(raw, Guest((Guest(raw) & ~mask) | (value & mask)))) {
  }
}

// ffmpeg's frames for a context are its planes (get_buffer2), so that a
// frame's trim (which only moves the data pointers) leaves all 512
// samples: the console's decoder writes every frame whole. Console check.
void NoFree(void*, uint8_t*) {}
int Planes(AVCodecContext* codec, AVFrame* frame, int) {
  auto* d = static_cast<Decoding*>(codec->opaque);
  if (frame->nb_samples > int(kSamplesPerFrame) || frame->ch_layout.nb_channels > 2) return AVERROR(EINVAL);
  for (int c = 0; c < frame->ch_layout.nb_channels; c++) {
    frame->buf[c] = av_buffer_create(reinterpret_cast<uint8_t*>(d->planes[c]), sizeof d->planes[c], NoFree,
                                     nullptr, 0);
    if (!frame->buf[c]) return AVERROR(ENOMEM);
    frame->data[c] = frame->buf[c]->data;
  }
  frame->extended_data = frame->data;
  frame->linesize[0] = sizeof d->planes[0];
  return 0;
}

// A decoder for the rate and channels, a new one when they change.
void Open(Decoding& d, uint32_t index, uint32_t rate, uint32_t channels) {
  if (d.codec && d.rate == rate && d.channels == channels) return;
  avcodec_free_context(&d.codec);
  const AVCodec* wmapro = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
  if (!wmapro) Fatal(nullptr, "XMA: ffmpeg has no wmapro decoder");
  d.codec = avcodec_alloc_context3(wmapro);
  if (!d.codec) Fatal(nullptr, "XMA: context %u: no memory for a decoder", index);
  d.codec->sample_rate = int(rate);
  av_channel_layout_default(&d.codec->ch_layout, int(channels));
  d.codec->block_align = kWmaPacketBytes;
  d.codec->extradata = static_cast<uint8_t*>(av_mallocz(sizeof kWmaExtradata + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!d.codec->extradata) Fatal(nullptr, "XMA: context %u: no memory for a decoder", index);
  std::memcpy(d.codec->extradata, kWmaExtradata, sizeof kWmaExtradata);
  d.codec->extradata_size = sizeof kWmaExtradata;
  d.codec->opaque = &d;
  d.codec->get_buffer2 = Planes;
  if (avcodec_open2(d.codec, wmapro, nullptr) < 0)
    Fatal(nullptr, "XMA: context %u: the wmapro decoder does not open (%u Hz, %u channels)", index, rate, channels);
  d.rate = rate;
  d.channels = channels;
}

// A context's host state as after a clear: no frame being written, and the
// decoder's own (the overlap with the frame before) reset, so that the
// first frame decoded gives no samples (ffmpeg's decoder's, and Xenia's).
// Console check.
void Reset(uint32_t index) {
  Decoding* d = g_decoding[index].get();
  if (!d) return;
  d->next = d->end = d->skip = 0;
  if (d->codec) avcodec_flush_buffers(d->codec);
}

// Clear: no buffer valid, the read and write offsets at their start
// (Xenia's). The context's words are big-endian.
void ClearContext(uint32_t index) {
  uint32_t va = ContextVa(index);
  Wr32(va, Rd32(va) & ~(0x3u << 20 | 0x1Fu << 27));       // input buffers valid, output write offset
  Wr32(va + 4, Rd32(va + 4) & ~(1u << 31));                // output buffer valid
  Wr32(va + 8, (Rd32(va + 8) & ~0x3FFFFFFu) | kHeaderBits);  // input read offset
  Wr32(va + 36, Rd32(va + 36) & ~0x1Fu);                   // output read offset
  Reset(index);
}

// Where decoding is in a context's input: a buffer, a packet of it and a
// bit of that packet.
struct Position {
  uint32_t buffer, packet, bit;
};

// A context as a kick finds it, and what decoding changes of it.
struct Context {
  uint32_t index, va;
  uint32_t words[10];
  uint32_t input[2], packets[2];
  bool valid[2];
  bool released[2] = {false, false};
  uint32_t current;
  Position at;  // the read position
  uint32_t loop_count;
  bool looped = false;

  uint32_t Get(Field f) const { return f.width == 32 ? words[f.word] : (words[f.word] & Mask(f)) >> f.shift; }
  bool Available(const Position& p) const { return valid[p.buffer] && p.packet < packets[p.buffer]; }
  const uint8_t* Packet(const Position& p) const {
    return Mem() + PhysicalToVirtual(input[p.buffer]) + p.packet * kPacketBytes;
  }
  // The stream's packet after p's: the header's skip count is the packets
  // of other streams in between (0xFF, the next packet: Xenia's). Past the
  // end of p's buffer, into the other.
  Position NextPacket(const Position& p) const {
    uint8_t skip = Packet(p)[3];
    Position q{p.buffer, p.packet + (skip == 0xFF ? 1 : skip + 1u), kHeaderBits};
    if (q.packet >= packets[q.buffer]) {
      q.packet -= packets[q.buffer];
      q.buffer ^= 1;
    }
    return q;
  }
  // Reads a bit at p and moves p past it, on into the stream's next packet
  // at a packet's end. False if p's packet is not available.
  bool ReadBit(Position& p, uint32_t& bit) const {
    if (!Available(p)) return false;
    bit = Packet(p)[p.bit >> 3] >> (7 - (p.bit & 7)) & 1;
    if (++p.bit == kPacketBits) p = NextPacket(p);
    return true;
  }
  bool Read(Position& p, uint32_t n, uint32_t& value) const {
    value = 0;
    for (uint32_t k = 0, bit; k < n; k++) {
      if (!ReadBit(p, bit)) return false;
      value = value << 1 | bit;
    }
    return true;
  }
  // Moves the read position to p; a buffer it leaves is consumed.
  void MoveTo(const Position& p) {
    if (p.buffer != current) {
      valid[current] = false;
      released[current] = true;
      current = p.buffer;
    }
    at = p;
  }
};

Context Load(uint32_t index) {
  Context c;
  c.index = index;
  c.va = ContextVa(index);
  for (uint32_t w = 0; w < 10; w++) c.words[w] = Rd32(c.va + 4 * w);
  c.input[0] = c.Get(kInput0);
  c.input[1] = c.Get(kInput1);
  c.packets[0] = c.Get(kInputPackets0);
  c.packets[1] = c.Get(kInputPackets1);
  c.valid[0] = c.Get(kInputValid0);
  c.valid[1] = c.Get(kInputValid1);
  c.current = c.Get(kCurrentBuffer);
  uint32_t read = c.Get(kInputRead);
  c.at = {c.current, read / kPacketBits, read % kPacketBits};
  c.loop_count = c.Get(kLoopCount);
  return c;
}

// The first bit of the first frame that begins in a packet: past the
// header and the end of a frame begun in an earlier packet. kPacketBits or
// more when no frame begins in it.
uint32_t FirstFrame(const uint8_t* packet) {
  return ((packet[0] & 3u) << 13 | packet[1] << 5 | packet[2] >> 3) + kHeaderBits;
}

// Moves p to where the next frame begins, from p on: a packet's first
// frame when p is before it, the next packet when no frame (or padding)
// follows. False, with p where it got to, when a packet needed is not
// available.
bool FrameStart(const Context& c, Position& p, uint32_t& length) {
  for (;;) {
    if (!c.Available(p)) return false;
    p.bit = std::max(p.bit, FirstFrame(c.Packet(p)));
    if (p.bit < kPacketBits) {
      Position q = p;
      if (!c.Read(q, kFrameLengthBits, length)) return false;
      // Xenia's frame scan stops at a length of 0, too.
      if (length != kNoFrame && length != 0) return true;
    }
    p = c.NextPacket(p);
  }
}

// Decodes the frame at the read position into d's PCM (none for the first
// after a reset) and moves the read position past it. False when no frame
// is available.
bool DecodeFrame(Context& c, Decoding& d) {
  Position p = c.at;
  uint32_t length;
  bool found = FrameStart(c, p, length);
  c.MoveTo(p);
  if (!found) return false;
  if (length > kMaxFrameBits) Fatal(nullptr, "XMA: context %u: a frame of %u bits", c.index, length);
  // The frame at the loop's end (when loops are left) is the loop's last;
  // decoding goes on at its start after it (Xenia's). Offsets below the
  // first packet's header are its end.
  uint32_t offset = p.packet * kPacketBits + p.bit;
  bool loop_end = c.loop_count && offset == std::max(kHeaderBits, c.Get(kLoopEnd));

  // The frame into ffmpeg's packet, its last bit (whether another frame
  // follows in the packet) cleared, so that the decoder takes no more.
  std::memset(d.bits, 0, sizeof d.bits);
  Position q = p, last = p;
  uint32_t bit = 0;
  for (uint32_t k = 0; k < length; k++) {
    last = q;
    if (!c.ReadBit(q, bit)) return false;
    if (k + 1 < length && bit) {
      uint32_t at = kWmaHeaderBits + k;
      d.bits[at >> 3] |= uint8_t(0x80 >> (at & 7));
    }
  }
  bool more = bit;

  uint32_t channels = c.Get(kStereo) + 1;
  Open(d, c.index, kRates[c.Get(kSampleRate)], channels);
  d.bits[0] = uint8_t(d.sequence++ << 4);
  d.packet->data = d.bits;
  d.packet->size = kWmaPacketBytes;
  if (int r = avcodec_send_packet(d.codec, d.packet); r < 0)
    Fatal(nullptr, "XMA: context %u: the frame at bit %u does not decode (%d)", c.index, offset, r);
  int r = avcodec_receive_frame(d.codec, d.frame);
  d.next = d.end = 0;
  if (r >= 0) {
    // To 16 bits, saturated (ffmpeg's samples may pass 1.0) and rounded.
    auto* out = d.pcm;
    for (uint32_t s = 0; s < kSamplesPerFrame; s++)
      for (uint32_t ch = 0; ch < channels; ch++) {
        auto v = int16_t(std::lrint(std::clamp(d.planes[ch][s], -1.0f, 1.0f) * 32767.0f));
        *out++ = uint8_t(uint16_t(v) >> 8);
        *out++ = uint8_t(v);
      }
    av_frame_unref(d.frame);
    // The loop's start leaves out its first subframes; its end, those
    // after its last (Xenia's).
    uint32_t blocks = kSamplesPerFrame * 2 * channels / kBlockBytes;
    d.next = std::min(d.skip * channels, blocks);
    d.end = loop_end ? std::min((c.Get(kLoopSubframeEnd) + 1) * channels, blocks) : blocks;
    d.next = std::min(d.next, d.end);
    d.skip = 0;
  } else if (r != AVERROR(EAGAIN)) {
    Fatal(nullptr, "XMA: context %u: the frame at bit %u does not decode (%d)", c.index, offset, r);
  }

  if (loop_end) {
    uint32_t start = std::max(kHeaderBits, c.Get(kLoopStart));
    q = {c.current, start / kPacketBits, start % kPacketBits};
    if (c.loop_count != 0xFF) c.loop_count--;
    c.looped = true;
    d.skip = c.Get(kLoopSubframeSkip);
  } else if (!more && last.buffer == p.buffer && last.packet == p.packet) {
    // The rest of the packet is padding. A frame that runs on into the
    // next packet is followed by that packet's first frame, whatever its
    // last bit (ffmpeg's xma decoders and Xenia's ignore it).
    q = c.NextPacket(last);
  }
  c.MoveTo(q);
  return true;
}

// A kick: decodes frames into the output ring until it has no room for
// another subframe_decode_count blocks (at least one) and the padding, or
// no frame is available. The title reads the ring from its read offset to
// the write offset, and sets the output buffer valid again; the decoder
// clears it when the ring is full (the write offset back at the read
// offset). Xenia's.
void Kick(uint32_t index) {
  Context c = Load(index);
  uint32_t blocks = c.Get(kOutputBlocks);
  if (!c.Get(kOutputValid) || !blocks) return;
  auto& slot = g_decoding[index];
  if (!slot) {
    slot = std::make_unique<Decoding>();
    slot->packet = av_packet_alloc();
    slot->frame = av_frame_alloc();
    if (!slot->packet || !slot->frame) Fatal(nullptr, "XMA: context %u: no memory for a decoder", index);
  }
  Decoding& d = *slot;
  uint32_t write = c.Get(kOutputWrite) % blocks, read = c.Get(kOutputRead) % blocks;
  int room = int(read == write ? blocks : (read + blocks - write) % blocks);
  uint32_t step = std::max(1u, c.Get(kSubframeDecodeCount)), padding = c.Get(kOutputPadding);
  uint8_t* out = Mem() + PhysicalToVirtual(c.Get(kOutput));
  bool wrote = false;
  while (room >= int(step + padding)) {
    if (d.next == d.end) {
      if (!DecodeFrame(c, d)) break;
      continue;
    }
    uint32_t n = std::min(d.end - d.next, step);
    for (uint32_t k = 0; k < n; k++) {
      std::memcpy(out + write * kBlockBytes, d.pcm + d.next++ * kBlockBytes, kBlockBytes);
      write = (write + 1) % blocks;
    }
    room -= int(n);
    if (d.next == d.end) room -= int(padding);
    wrote = true;
  }

  // The fields decoding owns; the output buffer's valid bit last, which
  // titles wait on.
  uint32_t offset = c.at.packet * kPacketBits + c.at.bit;
  Update(c.va, kInputRead.word, Mask(kInputRead), offset);
  Update(c.va, kCurrentBuffer.word, Mask(kCurrentBuffer), c.current << kCurrentBuffer.shift);
  uint32_t mask = Mask(kOutputWrite) | (c.released[0] ? Mask(kInputValid0) : 0) |
                  (c.released[1] ? Mask(kInputValid1) : 0) | (c.looped ? Mask(kLoopCount) : 0);
  Update(c.va, 0, mask, write << kOutputWrite.shift | c.loop_count << kLoopCount.shift);
  if (wrote && write == read) Update(c.va, kOutputValid.word, Mask(kOutputValid), 0);
}

void CheckContexts(uint32_t bits, uint32_t k, const char* command) {
  for (uint32_t n = 0; n < 32; n++)
    if (bits >> n & 1 && 32 * k + n >= kXmaContexts)
      Fatal(nullptr, "XMA: context %u %s: no such context", 32 * k + n, command);
}

// Takes the commands written to the registers, which nothing signals, so
// the decoder polls them: a lock, a clear, then a kick, so that a context
// cleared and kicked between two polls is cleared first. A kick decodes
// what the output ring has room for, here before the next poll; a lock
// stops a context's decoding (Xenia's), and with decoding done within the
// poll there is none to stop: it is taken and changes nothing.
void Decoder(void*) {
  for (unsigned idle = 0;; idle++) {
    bool worked = false;
    for (uint32_t k = 0; k < kCommandWords; k++) {
      if (uint32_t bits = TakeCommand(kXmaRegLock + k)) {
        CheckContexts(bits, k, "locked");
        worked = true;
      }
      uint32_t clear = TakeCommand(kXmaRegClear + k), kick = TakeCommand(kXmaRegKick + k);
      CheckContexts(clear, k, "cleared");
      CheckContexts(kick, k, "kicked");
      std::lock_guard<std::mutex> g(g_lock);
      for (uint32_t n = 0; n < 32; n++) {
        if (clear >> n & 1) ClearContext(32 * k + n);
        if (kick >> n & 1) Kick(32 * k + n);
      }
      worked |= clear || kick;
    }
    if (worked) {
      idle = 0;
    } else if (idle < 1000) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
}

}  // namespace

// The kernel keeps the array in uncached physical memory and writes its
// physical address to the register (Xenia's); xam and titles read it there.
void StartXma() {
  uint32_t size = kXmaContexts * kXmaContextSize;
  g_array = ReservePhysical(Heap::kPhysical4k, size, 0x1000, 0, kPhysicalSize - 1, false);
  if (!g_array || !Commit(g_array, size, kPageReadWrite | kPageNoCache))
    Fatal(nullptr, "XMA: no physical memory for the context array");
  uint32_t pa;
  PhysicalAddress(g_array, pa);
  Wr32(RegisterVa(kXmaRegContextArray), Bswap(pa));
  CreateSystemThread(Decoder, nullptr);
}

uint32_t AllocateXmaContext() {
  std::lock_guard<std::mutex> g(g_lock);
  for (uint32_t k = 0; k < kXmaContexts; k++)
    if (!g_used[k]) {
      g_used[k] = true;
      return ContextVa(k);
    }
  return 0;
}

bool ReleaseXmaContext(uint32_t va) {
  std::lock_guard<std::mutex> g(g_lock);
  uint32_t offset = va - g_array, k = offset / kXmaContextSize;
  if (offset % kXmaContextSize || k >= kXmaContexts || !g_used[k]) return false;
  std::memset(Mem() + va, 0, kXmaContextSize);
  Reset(k);
  g_used[k] = false;
  return true;
}

}  // namespace recomp::xbox360
