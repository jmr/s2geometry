// Copyright 2000 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

//
//
// This holds the encoding/decoding routines that used to live in netutil

#ifndef S2_UTIL_CODING_CODER_H_
#define S2_UTIL_CODING_CODER_H_

#include <cstring>

#include <cstdint>
#include <cstring>
#include <utility>

// Avoid adding expensive includes here.
#include "absl/base/attributes.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/log/absl_check.h"
#include "absl/meta/type_traits.h"
#include "absl/numeric/int128.h"
#include "absl/strings/string_view.h"

#include "s2/util/coding/varint.h"
#include "s2/util/endian/endian.h"

/* Class for encoding data into a memory buffer */
class Decoder;
class Encoder {
 public:
  // Creates an empty Encoder with no room that is enlarged
  // (if necessary) when "Encoder::Ensure(N)" is called.
  Encoder() = default;
  void reset();

  // Movable.
  Encoder(Encoder&& other);
  Encoder& operator=(Encoder&& other);

  ~Encoder();

  // Initialize encoder to encode into "buf"
  Encoder(void* buf, size_t maxn);
  void reset(void* buf, size_t maxn);
  void clear();

  // Encoding routines. Callers must know that the remaining buffer has
  // sufficient size.
  void put8(unsigned char v) {
    ABSL_HARDENING_ASSERT(avail() >= sizeof(v));
    writer().put8(v);
  }
  void put16(uint16_t v) {
    ABSL_HARDENING_ASSERT(avail() >= sizeof(v));
    writer().put16(v);
  }
  void put32(uint32_t v) {
    ABSL_HARDENING_ASSERT(avail() >= sizeof(v));
    writer().put32(v);
  }
  void put64(uint64_t v) {
    ABSL_HARDENING_ASSERT(avail() >= sizeof(v));
    writer().put64(v);
  }
  void put128(absl::uint128 v) {
    ABSL_HARDENING_ASSERT(avail() >= sizeof(v));
    writer().put128(v);
  }
  void putn(const void* mem, size_t n) {
    ABSL_HARDENING_ASSERT(avail() >= n);
    writer().putn(mem, n);
  }

  // Put no more than n bytes, stopping when c is put.
  void putcn(const void* mem, int c, size_t n) {
    ABSL_HARDENING_ASSERT(avail() >= n);
    writer().putcn(mem, c, n);
  }

  // Put a c-string including \0.
  void puts(const void* mem) {
    // No ABSL_HARDENING_ASSERT. This eventually calls memccpy which has size
    // parameter.
    writer().puts(mem);
  }

  // Put the contents of c-string up to but not including the terminating NUL
  // character.
  // `mem` must not be nullptr.
  // `mem` must be a NUL-terminated string.
  void puts_without_null(const char* mem) {
    // No ABSL_HARDENING_ASSERT. The implementation stops writing at the end of
    // the buffer.
    writer().puts_no_null(mem);
  }

  void putfloat(float f) {
    ABSL_HARDENING_ASSERT(avail() >= sizeof(uint32_t));
    writer().put32(absl::bit_cast<uint32_t>(f));
  }
  void putdouble(double d) {
    ABSL_HARDENING_ASSERT(avail() >= sizeof(uint64_t));
    writer().put64(absl::bit_cast<uint64_t>(d));
  }

  // Support for variable length encoding with 7 bits per byte
  // (these are just simple wrappers around the Varint module)
  static constexpr int kVarintMax32 = Varint::kMax32;
  static constexpr int kVarintMax64 = Varint::kMax64;

  void put_varint32(uint32_t v) {
    // The majority of the time we can avoid computing Length32().
    ABSL_HARDENING_ASSERT(avail() >= static_cast<size_t>(Varint::kMax32) ||
                          avail() >= static_cast<size_t>(Varint::Length32(v)));
    writer().put_varint32(v);
  }
  void put_varint32_inline(uint32_t v) {
    // The majority of the time we can avoid computing Length32().
    ABSL_HARDENING_ASSERT(avail() >= static_cast<size_t>(Varint::kMax32) ||
                          avail() >= static_cast<size_t>(Varint::Length32(v)));
    writer().put_varint32_inline(v);
  }
  void put_varint64(uint64_t v) {
    // The majority of the time we can avoid computing Length64().
    ABSL_HARDENING_ASSERT(avail() >= static_cast<size_t>(Varint::kMax64) ||
                          avail() >= static_cast<size_t>(Varint::Length64(v)));
    writer().put_varint64(v);
  }
  static int varint32_length(uint32_t v);  // Length of var encoding of "v"
  static int varint64_length(uint64_t v);  // Length of var encoding of "v"

  // The fast implementation of the code below with boundary checks.
  //   uint64 val;
  //   if (!dec->get_varint64(&val))
  //     return false;
  //   enc->put_varint64(val);
  //   return true;
  // We assume that the encoder and decoder point to different buffers.
  // If the decoder has invalid value, i.e., dec->get_varint64(&val)
  // returns false, the decoder is not updated, which is different from
  // dec->get_varint64(&val).
  bool put_varint64_from_decoder(Decoder* dec);

  // Return number of bytes encoded so far
  size_t length() const;

  // Return number of bytes of space remaining in buffer
  size_t avail() const;

  // Return capacity of buffer.
  size_t capacity() const { return limit_ - orig_; }

  // REQUIRES: Encoder was created with the 0-argument constructor or 0-argument
  // reset().
  //
  // This interface ensures that at least "N" more bytes are available
  // in the underlying buffer by resizing the buffer (if necessary).
  //
  // Note that no bounds checking is done on any of the put routines,
  // so it is the client's responsibility to call Ensure() at
  // appropriate intervals to ensure that enough space is available
  // for the data being added.
  void Ensure(size_t N);

  // Returns true if Ensure is allowed to be called on "this"
  bool ensure_allowed() const { return orig_ == underlying_buffer_; }

  // Return ptr to start of encoded data.  This pointer remains valid
  // until reset or Ensure is called.
  const char* base() const { return reinterpret_cast<const char*>(orig_); }

  // Advances the write pointer by "N" bytes. It returns the position of the
  // pointer before the skip (in other words start of the skipped bytes).
  char* skip(ptrdiff_t N) {
    // Negative values of N are allowed. Ensure the pointer lands within the
    // original bounds.
    ABSL_HARDENING_ASSERT(static_cast<ptrdiff_t>(length()) + N >= 0 &&
                          static_cast<ptrdiff_t>(avail()) >= N);
    return writer().skip(N); }

  // REQUIRES: length() >= N
  // Removes the last N bytes out of the encoded buffer
  void RemoveLast(size_t N) {
    ABSL_HARDENING_ASSERT(length() >= N);
    writer().skip(-static_cast<ptrdiff_t>(N));
  }

  // REQUIRES: length() >= N
  // Removes the last length()-N bytes to make the encoded buffer have length N
  void Resize(size_t N);

 private:
  // All encoding operations are done through the Writer. This avoids aliasing
  // between `buf_` and `this` which allows the compiler to avoid reloading
  // `buf_` repeatedly. See https://godbolt.org/z/zM36s3ded.
  struct Writer {
    Encoder* enc;
    char* p;

    explicit Writer(Encoder* e)
        : enc(e), p(reinterpret_cast<char*>(enc->buf_)) {}

    ~Writer() {
      enc->buf_ = reinterpret_cast<unsigned char*>(p);
      ABSL_DCHECK_GE(enc->buf_, enc->orig_);
      ABSL_DCHECK_LE(enc->buf_, enc->limit_);
    }

    char* skip(ptrdiff_t N) { return std::exchange(p, p + N); }

    void put8(unsigned char v) { *p++ = v; }
    void put16(uint16_t v) { LittleEndian::Store16(skip(2), v); }
    void put32(uint32_t v) { LittleEndian::Store32(skip(4), v); }
    void put64(uint64_t v) { LittleEndian::Store64(skip(8), v); }
    void put128(absl::uint128 v) { LittleEndian::Store128(skip(16), v); }
    void putn(const void* src, size_t n) { memcpy(skip(n), src, n); }
    void putcn(const void* src, int c, size_t n) {
      auto* o = p;
      p = static_cast<char*>(memccpy(p, src, c, n));
      if (p == nullptr) p = o + n;
    }
    void puts(const void* src) { putcn(src, '\0', enc->avail()); }
    void puts_no_null(const char* mem) {
      auto* l = reinterpret_cast<char*>(enc->limit_);
      while (*mem != '\0' && p < l) *p++ = *mem++;
    }
    ABSL_ATTRIBUTE_ALWAYS_INLINE void put_varint32(uint32_t v) {
      p = Varint::Encode32(p, v);
    }
    ABSL_ATTRIBUTE_ALWAYS_INLINE void put_varint32_inline(uint32_t v) {
      p = Varint::Encode32Inline(p, v);
    }
    ABSL_ATTRIBUTE_ALWAYS_INLINE void put_varint64(uint64_t v) {
      p = Varint::Encode64(p, v);
    }
  };

  Writer writer() { return Writer(this); }

  static std::pair<unsigned char*, size_t> NewBuffer(size_t size);
  static void DeleteBuffer(unsigned char* buf, size_t size);

  void EnsureSlowPath(size_t N);

  // Puts varint64 from decoder for varint64 sizes from 3 ~ 10. This is less
  // common cases compared to 1 - 2 byte varint64. Returns false if either the
  // encoder or the decoder fails the boundary check, or varint64 size exceeds
  // the maximum size (kVarintMax64).
  bool PutVarint64FromDecoderLessCommonSizes(Decoder* dec);

  // buf_ points into the orig_ buffer, just past the last encoded byte.
  unsigned char* buf_ = nullptr;

  // limits_ points just past the last allocated byte in the orig_ buffer.
  unsigned char* limit_ = nullptr;

  // If this Encoder owns its buffer, underlying_buffer_ == orig_ (note this is
  // also the case when both are nullptr). The Encoder is allowed to resize it
  // when Ensure() is called.
  unsigned char* underlying_buffer_ = nullptr;

  // orig_ points to the start of the encoding buffer, whether or not the
  // Encoder owns it.
  unsigned char* orig_ = nullptr;
};

/* Class for decoding data from a memory buffer */
class Decoder {
 public:
  // Empty constructor to create uninitialized decoder
  Decoder() = default;

  // NOTE: for efficiency reasons, this is not virtual.  so don't add
  // any members that really need to be destructed, and be careful about
  // inheritance.
  // The defaulted destructor is not explicitly written to avoid confusing SWIG.
  // ~Decoder() = default;

  // Initialize decoder to decode from "buf"
  explicit Decoder(absl::string_view buf) : Decoder(buf.data(), buf.size()) {}
  Decoder(const void* buf, size_t maxn);
  void reset(const void* buf, size_t maxn);

  // Decoding routines.  Callers must know that the remaining buffer has
  // sufficient size.
  unsigned char get8();
  uint16_t get16();
  uint32_t get32();
  uint64_t get64();
  absl::uint128 get128();
  float  getfloat();
  double getdouble();
  void   getn(void* mem, size_t n);
  void   getcn(void* mem, int c, size_t n);    // get no more than n bytes,
                                               // stopping after c is got
  void   gets(void* mem, size_t n);            // get a c-string no more than
                                               // n bytes. always appends '\0'
  const char* skip(ptrdiff_t n);
  ABSL_DEPRECATED("use skip(0) instead")
  unsigned char const* ptr() const;  // Return ptr to current position in buffer

  // "get_varint" actually checks bounds
  bool get_varint32(uint32_t* v);
  bool get_varint64(uint64_t* v);

  size_t pos() const;
  // Return number of bytes decoded so far

  size_t avail() const;
  // Return number of available bytes to read

 private:
  friend class Encoder;
  friend class IndexBlockDecoder;
  const unsigned char* orig_;
  const unsigned char* buf_;
  const unsigned char* limit_;
};

// TODO(user): Remove when LLVM detects and optimizes this case.
class DecoderExtensions {
 private:
  friend class Untranspose;  // In net/proto/transpose.cc.
  friend void TestFillArray();
  // Fills an array of num_decoders decoders with Decoder(nullptr, 0) instances.
  // This is much more efficient than using the stl.
  static void FillArray(Decoder* array, int num_decoders);
};

/***** Implementation details.  Clients should ignore them. *****/

inline Encoder::Encoder(void* b, size_t maxn) :
    buf_(reinterpret_cast<unsigned char*>(b)),
    limit_(reinterpret_cast<unsigned char*>(b) + maxn),
    orig_(reinterpret_cast<unsigned char*>(b)) { }

inline void Encoder::reset() {
  if (ensure_allowed()) DeleteBuffer(underlying_buffer_, capacity());
  orig_ = underlying_buffer_ = limit_ = buf_ = nullptr;
}

inline void Encoder::reset(void* b, size_t maxn) {
  // Can't use the underlying buffer anymore
  if (ensure_allowed()) DeleteBuffer(underlying_buffer_, capacity());
  underlying_buffer_ = nullptr;
  orig_ = buf_ = reinterpret_cast<unsigned char*>(b);
  limit_ = orig_ + maxn;
}

inline void Encoder::clear() {
  buf_ = orig_;
}

inline void Encoder::Ensure(size_t N) {
  ABSL_DCHECK(ensure_allowed());
  if (avail() < N) {
    EnsureSlowPath(N);
  }
}

inline size_t Encoder::length() const {
  ABSL_DCHECK_GE(buf_, orig_);
  ABSL_DCHECK_LE(buf_, limit_);
  return buf_ - orig_;
}

inline size_t Encoder::avail() const {
  ABSL_DCHECK_GE(limit_, buf_);
  return limit_ - buf_;
}

// Copies N bytes from *src to *dst then advances both pointers by N bytes.
// Template parameter N specifies the number of bytes to copy. Passing
// constant size results in optimized code from memcpy for the size.
template <size_t N>
void CopyAndAdvance(const uint8_t** src, uint8_t** dst) {
  memcpy(*dst, *src, N);
  *dst += N;
  *src += N;
}

// Tries a fast path if both the decoder and the encoder have enough room for
// max varint64 (10 bytes). With enough room, we don't need boundary checks at
// every iterations. Also, memcpy with known size is faster than copying a byte
// at a time (e.g. one movq vs. eight movb's).
//
// If either the decoder or the encoder doesn't have enough room, it falls back
// to previous example where copy and boundary check happen at every byte.
inline bool Encoder::PutVarint64FromDecoderLessCommonSizes(Decoder* dec) {
  const unsigned char* dec_ptr = dec->buf_;
  const unsigned char* dec_limit = dec->limit_;

  // Check once if both the encoder and the decoder have enough room for
  // maximum varint64 (kVarintMax64) instead of checking at every bytes.
  if (ABSL_PREDICT_TRUE(dec_ptr <= dec_limit - kVarintMax64 &&
                        buf_ <= limit_ - kVarintMax64)) {
    if (dec_ptr[2] < 128) {
      CopyAndAdvance<3>(&dec->buf_, &buf_);
    } else if (dec_ptr[3] < 128) {
      CopyAndAdvance<4>(&dec->buf_, &buf_);
    } else if (dec_ptr[4] < 128) {
      CopyAndAdvance<5>(&dec->buf_, &buf_);
    } else if (dec_ptr[5] < 128) {
      CopyAndAdvance<6>(&dec->buf_, &buf_);
    } else if (dec_ptr[6] < 128) {
      CopyAndAdvance<7>(&dec->buf_, &buf_);
    } else if (dec_ptr[7] < 128) {
      CopyAndAdvance<8>(&dec->buf_, &buf_);
    } else if (dec_ptr[8] < 128) {
      CopyAndAdvance<9>(&dec->buf_, &buf_);
    } else if (dec_ptr[9] < 2) {
      // 10th byte stores at most 1 bit for varint64.
      CopyAndAdvance<10>(&dec->buf_, &buf_);
    } else {
      return false;
    }
    return true;
  }

  unsigned char c;
  unsigned char* enc_ptr = buf_;

  // The loop executes at most (kVarintMax64 - 1) iterations because either the
  // decoder or the encoder has less availability than kVarintMax64. We must be
  // careful about the cost of moving any computation out of the loop.
  // Xref cl/133546957 for more details of various implementations we explored.
  do {
    if (dec_ptr >= dec_limit) return false;
    if (enc_ptr >= limit_) return false;
    c = *dec_ptr;
    *enc_ptr = c;
    ++dec_ptr;
    ++enc_ptr;
  } while (c >= 128);

  dec->buf_ = dec_ptr;
  buf_ = enc_ptr;
  return true;
}

// The fast implementation of the code below with boundary checks.
//   uint64 val;
//   if (!dec->get_varint64(&val))
//     return false;
//   enc->put_varint64(val);
//   return true;
// BM_getvarfrom* in coder_unittest.cc are the benchmarks that measure the
// performance of different implementations.
//
// Handles varint64 with one to two bytes separately as a common case.
// PutVarint64FromDecoderLessCommonSizes handles the remaining sizes. To avoid
// over-inlining, PutVarint64FromDecoderLessCommonSizes is defined in coder.cc.
// As Untranspose::DecodeMessage is the only caller, compiler should be able to
// inline all if necessary.
ABSL_ATTRIBUTE_ALWAYS_INLINE inline bool Encoder::put_varint64_from_decoder(
    Decoder* dec) {
  unsigned char* enc_ptr = buf_;
  const unsigned char* dec_ptr = dec->buf_;

  // Common cases to handle varint64 with one to two bytes.
  if (ABSL_PREDICT_TRUE(dec_ptr < dec->limit_ && dec_ptr[0] < 128)) {
    if (ABSL_PREDICT_FALSE(enc_ptr >= limit_)) {
      return false;
    }
    *enc_ptr = *dec_ptr;
    dec->buf_++;
    buf_++;
    return true;
  }

  if (dec_ptr < dec->limit_ - 1 && dec_ptr[1] < 128) {
    if (ABSL_PREDICT_FALSE(enc_ptr >= limit_ - 1)) {
      return false;
    }
    UNALIGNED_STORE16(enc_ptr, UNALIGNED_LOAD16(dec_ptr));
    dec->buf_ += 2;
    buf_ += 2;
    return true;
  }

  // For less common sizes in [3, kVarintMax64].
  return PutVarint64FromDecoderLessCommonSizes(dec);
}

inline Decoder::Decoder(const void* b, size_t maxn) {
  reset(b, maxn);
}

inline void Decoder::reset(const void* b, size_t maxn) {
  orig_ = buf_ = reinterpret_cast<const unsigned char*>(b);
  limit_ = orig_ + maxn;
}

inline size_t Decoder::pos() const {
  ABSL_DCHECK_GE(buf_, orig_);
  return buf_ - orig_;
}

inline size_t Decoder::avail() const {
  ABSL_DCHECK_GE(limit_, buf_);
  return limit_ - buf_;
}

inline void Decoder::getn(void* dst, size_t n) {
  memcpy(dst, buf_, n);
  buf_ += n;
}

inline void Decoder::getcn(void* dst, int c, size_t n) {
  void *ptr;
  ptr = memccpy(dst, buf_, c, n);
  if (ptr == nullptr)
    buf_ = buf_ + n;
  else
    buf_ = buf_ + (reinterpret_cast<unsigned char *>(ptr) -
                   reinterpret_cast<unsigned char *>(dst));
}

inline void Decoder::gets(void* dst, size_t n) {
  size_t len = n - 1;
  ABSL_DCHECK_GE(limit_, buf_);
  if (n > static_cast<size_t>(1 + limit_ - buf_)) {
    len = limit_ - buf_;
  }
  (reinterpret_cast<char *>(dst))[len] = '\0';
  getcn(dst, '\0', len);
}

inline const char* Decoder::skip(ptrdiff_t n) {
  // Negative values of n are allowed. Ensure the pointer lands within the
  // original bounds.
  ABSL_HARDENING_ASSERT(static_cast<ptrdiff_t>(avail()) >= n &&
                        static_cast<ptrdiff_t>(pos()) + n >= 0);
  auto* start = reinterpret_cast<const char*>(buf_);
  buf_ += n;
  return start;
}

inline unsigned char const* Decoder::ptr() const {
  return buf_;
}

inline void DecoderExtensions::FillArray(Decoder* array, int num_decoders) {
  // This is an optimization based on the fact that Decoder(nullptr, 0) sets all
  // structure bytes to 0. This is valid because Decoder is TriviallyCopyable
  // (https://en.cppreference.com/w/cpp/named_req/TriviallyCopyable).
  static_assert(absl::is_trivially_copy_constructible<Decoder>::value,
                "Decoder must be trivially copy-constructible");
  static_assert(absl::is_trivially_copy_assignable<Decoder>::value,
                "Decoder must be trivially copy-assignable");
  static_assert(absl::is_trivially_destructible<Decoder>::value,
                "Decoder must be trivially destructible");
  std::memset(array, 0, num_decoders * sizeof(Decoder));
}

inline unsigned char Decoder::get8() {
  ABSL_HARDENING_ASSERT(avail() >= sizeof(unsigned char));
  const unsigned char v = *buf_;
  buf_ += sizeof(v);
  return v;
}

inline uint16_t Decoder::get16() {
  ABSL_HARDENING_ASSERT(avail() >= sizeof(uint16_t));
  const uint16_t v = LittleEndian::Load16(buf_);
  buf_ += sizeof(v);
  return v;
}

inline uint32_t Decoder::get32() {
  ABSL_HARDENING_ASSERT(avail() >= sizeof(uint32_t));
  const uint32_t v = LittleEndian::Load32(buf_);
  buf_ += sizeof(v);
  return v;
}

inline uint64_t Decoder::get64() {
  ABSL_HARDENING_ASSERT(avail() >= sizeof(uint64_t));
  const uint64_t v = LittleEndian::Load64(buf_);
  buf_ += sizeof(v);
  return v;
}

inline absl::uint128 Decoder::get128() {
  ABSL_HARDENING_ASSERT(avail() >= sizeof(absl::uint128));
  const absl::uint128 v = LittleEndian::Load128(buf_);
  buf_ += sizeof(v);
  return v;
}

inline float Decoder::getfloat() {
  return absl::bit_cast<float>(get32());
}

inline double Decoder::getdouble() {
  return absl::bit_cast<double>(get64());
}

inline bool Decoder::get_varint32(uint32_t* v) {
  const char* const r =
      Varint::Parse32WithLimit(reinterpret_cast<const char*>(buf_),
                               reinterpret_cast<const char*>(limit_), v);
  if (r == nullptr) {
    return false;
  }
  buf_ = reinterpret_cast<const unsigned char*>(r);
  return true;
}

inline bool Decoder::get_varint64(uint64_t* v) {
  const char* const r =
      Varint::Parse64WithLimit(reinterpret_cast<const char*>(buf_),
                               reinterpret_cast<const char*>(limit_), v);
  if (r == nullptr) {
    return false;
  }
  buf_ = reinterpret_cast<const unsigned char*>(r);
  return true;
}

#endif  // S2_UTIL_CODING_CODER_H_
