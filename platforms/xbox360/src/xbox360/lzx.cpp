#include "xbox360/lzx.h"

#include <cstring>

namespace recomp::xbox360 {
namespace {

constexpr int kMinMatch = 2;
constexpr int kNumChars = 256;
constexpr int kPrimaryLengths = 7;
constexpr int kLengthSymbols = 249;
constexpr int kPretreeSymbols = 20;
constexpr int kAlignedSymbols = 8;
constexpr int kMaxMainSymbols = kNumChars + 50 * 8;
constexpr size_t kFrameSize = 32768;

enum BlockType { kVerbatim = 1, kAligned = 2, kUncompressed = 3 };

// Extra bits and base of each position slot.
struct Slots {
  uint8_t extra[51];
  uint32_t base[51];
  Slots() {
    for (int i = 0, j = 0; i < 51; i += 2) {
      extra[i] = extra[i + 1] = uint8_t(j);
      if (i != 0 && j < 17) j++;
    }
    for (uint32_t i = 0, j = 0; i < 51; i++) {
      base[i] = j;
      j += 1u << extra[i];
    }
  }
};
const Slots kSlots;

// Bits come from 16-bit little-endian words, most significant bit first.
class Bits {
 public:
  Bits(const uint8_t* p, size_t n) : p_(p), n_(n) {}

  uint32_t Peek(int n) {
    Fill();
    return n ? uint32_t(buf_ >> (64 - n)) : 0;
  }
  void Drop(int n) {
    buf_ <<= n;
    have_ -= n;
  }
  uint32_t Read(int n) {
    uint32_t v = Peek(n);
    Drop(n);
    return v;
  }
  // The realignment after each frame: the rest of a partly used word.
  void Align() {
    if (have_ > 0) Drop(have_ % 16);
  }
  // Byte position of uncompressed data: after the partly used word, or
  // after one whole padding word when the stream is already aligned.
  // Discards the buffer.
  size_t RawStart() {
    size_t pos = pos_ - 2 * size_t(have_ / 16) + (have_ % 16 == 0 ? 2 : 0);
    buf_ = 0;
    have_ = 0;
    return pos;
  }
  void Restart(size_t pos) {
    pos_ = pos;
    buf_ = 0;
    have_ = 0;
  }
  // Consumed bits past the end of the input (which reads as zero words).
  bool Overrun() const { return pos_ - 2 * size_t(have_ / 16) > n_ + 2; }
  const uint8_t* data() const { return p_; }
  size_t size() const { return n_; }

 private:
  void Fill() {
    while (have_ <= 48) {
      uint64_t w = 0;
      if (pos_ + 1 < n_) w = uint64_t(p_[pos_]) | uint64_t(p_[pos_ + 1]) << 8;
      pos_ += 2;
      buf_ |= w << (48 - have_);
      have_ += 16;
    }
  }

  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  uint64_t buf_ = 0;
  int have_ = 0;
};

// A canonical Huffman code over code lengths of up to 16 bits.
class Huffman {
 public:
  bool Build(const uint8_t* lens, int n) {
    int count[17] = {};
    for (int s = 0; s < n; s++) count[lens[s]]++;
    count[0] = 0;
    int code = 0, index = 0;
    empty_ = true;
    for (int l = 1; l <= 16; l++) {
      first_[l] = code;
      offset_[l] = index;
      count_[l] = count[l];
      index += count[l];
      code = (code + count[l]) << 1;
      if (count[l]) empty_ = false;
    }
    // Oversubscribed codes are malformed; incomplete ones are allowed
    // (a tree with one used symbol).
    if (code > (1 << 17)) return false;
    sorted_.assign(index, 0);
    int next[17];
    std::memcpy(next, offset_, sizeof next);
    for (int s = 0; s < n; s++)
      if (lens[s]) sorted_[next[lens[s]]++] = uint16_t(s);
    return true;
  }
  int Decode(Bits& b) const {
    if (empty_) return -1;
    uint32_t bits = b.Peek(16);
    int code = 0;
    for (int l = 1; l <= 16; l++) {
      code = code << 1 | int((bits >> (16 - l)) & 1);
      if (unsigned(code - first_[l]) < unsigned(count_[l])) {
        b.Drop(l);
        return sorted_[offset_[l] + code - first_[l]];
      }
    }
    return -1;
  }

 private:
  int first_[17] = {}, offset_[17] = {}, count_[17] = {};
  std::vector<uint16_t> sorted_;
  bool empty_ = true;
};

// Decodes straight into the output: the whole of it is the history, so
// the window size only sets the number of position slots.
class Decoder {
 public:
  Decoder(Bits& bits, int window_bits, uint32_t window_size) : b_(bits), window_(window_size) {
    static const int kSlotCount[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       30, 32, 34, 36, 38, 42, 50};
    main_symbols_ = kNumChars + kSlotCount[window_bits] * 8;
  }

  bool Run(size_t out_size, std::vector<uint8_t>& out, std::string& err) {
    out.assign(out_size, 0);
    out_ = &out;
    // The stream header: whether calls were translated, and the size the
    // translation assumed.
    if (b_.Read(1)) e8_size_ = int32_t(b_.Read(16) << 16 | b_.Read(16));
    for (size_t frame_end = 0; frame_end < out_size;) {
      frame_end = std::min(frame_end + kFrameSize, out_size);
      // A match may run on past the frame's end, into the next frame.
      while (pos_ < frame_end) {
        if (!block_left_ && !BlockHeader(err)) return false;
        size_t made = 0;
        if (!Block(std::min<size_t>(frame_end - pos_, block_left_), made, err)) return false;
        if (made > block_left_) {
          err = "LZX: match runs past the end of its block";
          return false;
        }
        block_left_ -= made;
      }
      if (b_.Overrun()) {
        err = "LZX: input ends early";
        return false;
      }
      b_.Align();
    }
    if (e8_size_)
      for (size_t f = 0; f * kFrameSize < out_size && f < 32768; f++) {
        size_t n = std::min(kFrameSize, out_size - f * kFrameSize);
        if (n > 10) Untranslate(out.data() + f * kFrameSize, n, f * kFrameSize);
      }
    return true;
  }

 private:
  // Code lengths are sent as deltas from the previous block's, through the
  // pretree.
  bool ReadLengths(uint8_t* lens, int first, int last) {
    uint8_t pre[kPretreeSymbols];
    for (uint8_t& l : pre) l = uint8_t(b_.Read(4));
    Huffman pretree;
    if (!pretree.Build(pre, kPretreeSymbols)) return false;
    for (int x = first; x < last;) {
      int z = pretree.Decode(b_);
      if (z < 0) return false;
      if (z == 17 || z == 18) {
        int run = z == 17 ? int(b_.Read(4)) + 4 : int(b_.Read(5)) + 20;
        for (; run && x < last; run--) lens[x++] = 0;
      } else if (z == 19) {
        int run = int(b_.Read(1)) + 4;
        z = pretree.Decode(b_);
        if (z < 0 || z > 16) return false;
        int v = (lens[x] - z + 17) % 17;
        for (; run && x < last; run--) lens[x++] = uint8_t(v);
      } else {
        lens[x] = uint8_t((lens[x] - z + 17) % 17);
        x++;
      }
    }
    return true;
  }

  bool BlockHeader(std::string& err) {
    type_ = int(b_.Read(3));
    block_left_ = b_.Read(16) << 8;
    block_left_ |= b_.Read(8);
    block_size_ = block_left_;
    switch (type_) {
      case kAligned: {
        uint8_t lens[kAlignedSymbols];
        for (uint8_t& l : lens) l = uint8_t(b_.Read(3));
        if (!aligned_.Build(lens, kAlignedSymbols)) break;
        [[fallthrough]];
      }
      case kVerbatim:
        if (!ReadLengths(main_lens_, 0, kNumChars) ||
            !ReadLengths(main_lens_, kNumChars, main_symbols_) ||
            !main_.Build(main_lens_, main_symbols_) ||
            !ReadLengths(length_lens_, 0, kLengthSymbols) ||
            !length_.Build(length_lens_, kLengthSymbols))
          break;
        return true;
      case kUncompressed: {
        size_t pos = b_.RawStart();
        if (pos + 12 > b_.size()) break;
        const uint8_t* p = b_.data() + pos;
        for (int k = 0; k < 3; k++)
          r_[k] = uint32_t(p[4 * k]) | uint32_t(p[4 * k + 1]) << 8 | uint32_t(p[4 * k + 2]) << 16 |
                  uint32_t(p[4 * k + 3]) << 24;
        raw_ = pos + 12;
        return true;
      }
      default:
        break;
    }
    err = "LZX: malformed block header";
    return false;
  }

  bool Put(uint8_t v) {
    if (pos_ >= out_->size()) return false;
    (*out_)[pos_++] = v;
    return true;
  }

  // Up to `n` bytes of the current block; a match may run over, and
  // `made` says how many were produced.
  bool Block(size_t n, size_t& made, std::string& err) {
    if (type_ == kUncompressed) {
      if (raw_ + n > b_.size() || pos_ + n > out_->size()) {
        err = "LZX: uncompressed block runs past the input";
        return false;
      }
      std::memcpy(out_->data() + pos_, b_.data() + raw_, n);
      pos_ += n;
      raw_ += n;
      made = n;
      // An odd-sized uncompressed block is followed by a padding byte.
      if (block_left_ == n) b_.Restart(raw_ + (block_size_ & 1));
      return true;
    }
    while (made < n) {
      int sym = main_.Decode(b_);
      if (sym < 0) {
        err = "LZX: bad main tree symbol";
        return false;
      }
      if (sym < kNumChars) {
        Put(uint8_t(sym));
        made++;
        continue;
      }
      sym -= kNumChars;
      int length = sym & 7;
      if (length == kPrimaryLengths) {
        int footer = length_.Decode(b_);
        if (footer < 0) {
          err = "LZX: bad length tree symbol";
          return false;
        }
        length += footer;
      }
      length += kMinMatch;
      uint32_t slot = uint32_t(sym >> 3), offset;
      if (slot > 2) {
        int extra = kSlots.extra[slot];
        offset = kSlots.base[slot] - 2;
        if (type_ == kAligned && extra >= 3) {
          if (extra > 3) offset += b_.Read(extra - 3) << 3;
          int a = aligned_.Decode(b_);
          if (a < 0) {
            err = "LZX: bad aligned tree symbol";
            return false;
          }
          offset += uint32_t(a);
        } else if (extra) {
          offset += b_.Read(extra);
        }
        r_[2] = r_[1];
        r_[1] = r_[0];
        r_[0] = offset;
      } else {
        offset = r_[slot];
        r_[slot] = r_[0];
        r_[0] = offset;
      }
      if (offset == 0 || offset > pos_ || offset > window_) {
        err = "LZX: match offset outside the window";
        return false;
      }
      for (int k = 0; k < length; k++)
        if (!Put((*out_)[pos_ - offset])) {
          err = "LZX: output longer than expected";
          return false;
        }
      made += size_t(length);
    }
    return true;
  }

  // Undoes the E8 translation: absolute call targets back to relative.
  void Untranslate(uint8_t* p, size_t n, size_t frame_pos) {
    int32_t cur = int32_t(frame_pos);
    for (size_t k = 0; k + 10 <= n;) {
      if (p[k] != 0xE8) {
        k++;
        cur++;
        continue;
      }
      int32_t abs = int32_t(uint32_t(p[k + 1]) | uint32_t(p[k + 2]) << 8 | uint32_t(p[k + 3]) << 16 |
                            uint32_t(p[k + 4]) << 24);
      if (abs >= -cur && abs < e8_size_) {
        int32_t rel = abs >= 0 ? abs - cur : abs + e8_size_;
        for (int b = 0; b < 4; b++) p[k + 1 + b] = uint8_t(uint32_t(rel) >> (8 * b));
      }
      k += 5;
      cur += 5;
    }
  }

  Bits& b_;
  uint32_t window_;
  std::vector<uint8_t>* out_ = nullptr;
  size_t pos_ = 0;
  int main_symbols_ = 0;
  int type_ = 0;
  size_t block_left_ = 0, block_size_ = 0;
  size_t raw_ = 0;
  uint32_t r_[3] = {1, 1, 1};
  int32_t e8_size_ = 0;
  uint8_t main_lens_[kMaxMainSymbols] = {};
  uint8_t length_lens_[kLengthSymbols] = {};
  Huffman main_, length_, aligned_;
};

}  // namespace

bool LzxDecompress(const uint8_t* in, size_t in_size, uint32_t window_size, size_t out_size,
                   std::vector<uint8_t>& out, std::string& err) {
  int bits = 0;
  while ((1u << bits) < window_size) bits++;
  if (window_size != 1u << bits || bits < 15 || bits > 21) {
    err = "LZX: unsupported window size";
    return false;
  }
  Bits b(in, in_size);
  Decoder d(b, bits, window_size);
  return d.Run(out_size, out, err);
}

}  // namespace recomp::xbox360
