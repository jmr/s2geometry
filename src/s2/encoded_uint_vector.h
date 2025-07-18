// Copyright 2018 Google Inc. All Rights Reserved.
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

// Author: ericv@google.com (Eric Veach)

#ifndef S2_ENCODED_UINT_VECTOR_H_
#define S2_ENCODED_UINT_VECTOR_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/log/absl_check.h"
#include "absl/types/span.h"

#include "s2/base/port.h"
#include "s2/util/coding/coder.h"
#include "s2/util/coding/varint.h"
#include "s2/_fp_contract_off.h"  // IWYU pragma: keep

namespace s2coding {

// Encodes a vector of unsigned integers in a format that can later be
// decoded as an EncodedUintVector.
//
// REQUIRES: T is an unsigned integer type.
// REQUIRES: 2 <= sizeof(T) <= 8
// REQUIRES: "encoder" uses the default constructor, so that its buffer
//           can be enlarged as necessary by calling Ensure(int).
template <class T>
void EncodeUintVector(absl::Span<const T> v, Encoder* encoder);

// This class represents an encoded vector of unsigned integers of type T.
// Values are decoded only when they are accessed.  This allows for very fast
// initialization and no additional memory use beyond the encoded data.
// The encoded data is not owned by this class; typically it points into a
// large contiguous buffer that contains other encoded data as well.
//
// This is one of several helper classes that allow complex data structures to
// be initialized from an encoded format in constant time and then decoded on
// demand.  This can be a big performance advantage when only a small part of
// the data structure is actually used.
//
// Values are encoded using a fixed number of bytes per value, where the
// number of bytes depends on the largest value present.
//
// REQUIRES: T is an unsigned integer type.
// REQUIRES: 2 <= sizeof(T) <= 8
template <class T>
class EncodedUintVector {
 public:
  static_assert(std::is_unsigned<T>::value, "Unsupported signed integer");
  static_assert(sizeof(T) & 0xe, "Unsupported integer length");

  // Constructs an uninitialized object; requires Init() to be called.
  EncodedUintVector() = default;

  // Initializes the EncodedUintVector.  Returns false on errors, leaving the
  // vector in an unspecified state.
  //
  // REQUIRES: The Decoder data buffer must outlive this object.
  bool Init(Decoder* decoder);

  // Resets the vector to be empty.
  void Clear();

  // Returns the size of the original vector.
  size_t size() const;

  // Returns the element at the given index.
  T operator[](int i) const;

  // Returns the index of the first element x such that (x >= target), or
  // size() if no such element exists.
  //
  // REQUIRES: The vector elements are sorted in non-decreasing order.
  size_t lower_bound(T target) const;

  // Decodes and returns the entire original vector.
  std::vector<T> Decode() const;

  void Encode(Encoder* encoder) const;

 private:
  template <int length> size_t lower_bound(T target) const;

  const char* data_;
  uint32_t size_;
  uint8_t len_;
};

// Encodes an unsigned integer in little-endian format using "length" bytes.
// (The client must ensure that the encoder's buffer is large enough.)
//
// REQUIRES: T is an unsigned integer type.
// REQUIRES: 2 <= sizeof(T) <= 8
// REQUIRES: 0 <= length <= sizeof(T)
// REQUIRES: value < 256 ** length
// REQUIRES: encoder->avail() >= length
template <class T>
void EncodeUintWithLength(T value, int length, Encoder* encoder);

// Decodes a variable-length integer consisting of "length" bytes starting at
// "ptr" in little-endian format.
//
// REQUIRES: T is an unsigned integer type.
// REQUIRES: 2 <= sizeof(T) <= 8
// REQUIRES: 0 <= length <= sizeof(T)
template <class T>
T GetUintWithLength(const char* ptr, int length);

// Decodes and consumes a variable-length integer consisting of "length" bytes
// in little-endian format.  Returns false if not enough bytes are available.
//
// REQUIRES: T is an unsigned integer type.
// REQUIRES: 2 <= sizeof(T) <= 8
// REQUIRES: 0 <= length <= sizeof(T)
template <class T>
bool DecodeUintWithLength(int length, Decoder* decoder, T* result);


//////////////////   Implementation details follow   ////////////////////


template <class T>
inline void EncodeUintWithLength(T value, int length, Encoder* encoder) {
  static_assert(std::is_unsigned<T>::value, "Unsupported signed integer");
  static_assert(sizeof(T) & 0xe, "Unsupported integer length");
  ABSL_DCHECK(length >= 0 && length <= sizeof(T));
  ABSL_DCHECK_GE(encoder->avail(), length);

  while (--length >= 0) {
    encoder->put8(value);
    value >>= 8;
  }
  ABSL_DCHECK_EQ(value, 0);
}

template <class T>
inline T GetUintWithLength(const char* ptr, int length) {
  static_assert(std::is_unsigned<T>::value, "Unsupported signed integer");
  static_assert(sizeof(T) & 0xe, "Unsupported integer length");
  ABSL_DCHECK(length >= 0 && length <= sizeof(T));

  // Note that the following code is faster than any of the following:
  //
  //  - A loop that repeatedly loads and shifts one byte.
  //  - memcpying "length" bytes to a local variable of type T.
  //  - A switch statement that handles each length optimally.
  //
  // The following code is slightly faster:
  //
  //   T mask = (length == 0) ? 0 : ~T{0} >> 8 * (sizeof(T) - length);
  //   return *reinterpret_cast<const T*>(ptr) & mask;
  //
  // However this technique is unsafe because in extremely rare cases it might
  // access out-of-bounds heap memory.  (This can only happen if "ptr" is
  // within (sizeof(T) - length) bytes of the end of a memory page and the
  // following page in the address space is unmapped.)

  if (length & sizeof(T)) {
    // There is also a `gtl::UnalignedLoad<T>()`, but it has slightly heavier
    // dependencies.  We could use it with an open-source shim, or switch
    // to `LittleEndian::Load*()` if we want to handle big-endian architectures.
    if (sizeof(T) == 8) return UNALIGNED_LOAD64(ptr);
    if (sizeof(T) == 4) return UNALIGNED_LOAD32(ptr);
    if (sizeof(T) == 2) return UNALIGNED_LOAD16(ptr);
    ABSL_DCHECK_EQ(sizeof(T), 1);
    return *ptr;
  }
  T x = 0;
  ptr += length;
  if (sizeof(T) > 4 && (length & 4)) {
    x = UNALIGNED_LOAD32(ptr -= sizeof(uint32_t));
  }
  if (sizeof(T) > 2 && (length & 2)) {
    x = (x << 16) + UNALIGNED_LOAD16(ptr -= sizeof(uint16_t));
  }
  if (sizeof(T) > 1 && (length & 1)) {
    x = (x << 8) + static_cast<uint8_t>(*--ptr);
  }
  return x;
}

template <class T>
bool DecodeUintWithLength(int length, Decoder* decoder, T* result) {
  if (decoder->avail() < static_cast<size_t>(length)) return false;
  const char* ptr = decoder->skip(0);
  *result = GetUintWithLength<T>(ptr, length);
  decoder->skip(length);
  return true;
}

template <class T>
void EncodeUintVector(absl::Span<const T> v, Encoder* encoder) {
  // The encoding is as follows:
  //
  //   varint64: (v.size() * sizeof(T)) | (len - 1)
  //   array of v.size() elements ["len" bytes each]
  //
  // Note that we don't allow (len == 0) since this would require an extra bit
  // to encode the length.

  T one_bits = 1;  // Ensures len >= 1.
  for (auto x : v) one_bits |= x;
  ABSL_ASSUME(one_bits != 0);
  int len = ((absl::bit_width(one_bits) - 1) >> 3) + 1;
  ABSL_DCHECK(len >= 1 && len <= 8);

  // Note that the multiplication is optimized into a bit shift.
  encoder->Ensure(Varint::kMax64 + v.size() * len);
  uint64_t size_len = (uint64_t{v.size()} * sizeof(T)) | (len - 1);
  encoder->put_varint64(size_len);
  for (auto x : v) {
    EncodeUintWithLength(x, len, encoder);
  }
}

template <class T>
bool EncodedUintVector<T>::Init(Decoder* decoder) {
  uint64_t size_len;
  if (!decoder->get_varint64(&size_len)) return false;
  size_ = size_len / sizeof(T);  // Optimized into bit shift.
  len_ = (size_len & (sizeof(T) - 1)) + 1;
  if (size_ > std::numeric_limits<size_t>::max() / sizeof(T)) return false;
  size_t bytes = static_cast<size_t>(size_) * static_cast<size_t>(len_);
  if (decoder->avail() < bytes) return false;
  data_ = decoder->skip(0);
  decoder->skip(bytes);
  return true;
}

template <class T>
void EncodedUintVector<T>::Clear() {
  size_ = 0;
  data_ = nullptr;
}

template <class T>
inline size_t EncodedUintVector<T>::size() const {
  return size_;
}

template <class T>
inline T EncodedUintVector<T>::operator[](int i) const {
  ABSL_DCHECK(i >= 0 && i < size_);
  return GetUintWithLength<T>(data_ + i * len_, len_);
}

template <class T>
size_t EncodedUintVector<T>::lower_bound(T target) const {
  static_assert(sizeof(T) & 0xe, "Unsupported integer length");
  ABSL_DCHECK(len_ >= 1 && len_ <= sizeof(T));

  // TODO(ericv): Consider using the unused 28 bits of "len_" to store the
  // last result of lower_bound() to be used as a hint.  This should help in
  // common situation where the same element is looked up repeatedly.  This
  // would require declaring the new field (length_lower_bound_hint_) as
  // mutable std::atomic<uint32_t> (accessed using std::memory_order_relaxed)
  // with a custom copy constructor that resets the hint component to zero.
  switch (len_) {
    case 1: return lower_bound<1>(target);
    case 2: return lower_bound<2>(target);
    case 3: return lower_bound<3>(target);
    case 4: return lower_bound<4>(target);
    case 5: return lower_bound<5>(target);
    case 6: return lower_bound<6>(target);
    case 7: return lower_bound<7>(target);
    default: return lower_bound<8>(target);
  }
}

template <class T> template <int length>
inline size_t EncodedUintVector<T>::lower_bound(T target) const {
  size_t lo = 0, hi = size_;
  while (lo < hi) {
    size_t mid = (lo + hi) >> 1;
    T value = GetUintWithLength<T>(data_ + mid * length, length);
    if (value < target) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

template <class T>
std::vector<T> EncodedUintVector<T>::Decode() const {
  std::vector<T> result(size_);
  for (int i = 0; i < size_; ++i) {
    result[i] = (*this)[i];
  }
  return result;
}

template <class T>
// The encoding must be identical to StringVectorEncoder::Encode().
void EncodedUintVector<T>::Encode(Encoder* encoder) const {
  uint64_t size_len = (uint64_t{size_} * sizeof(T)) | (len_ - 1);

  encoder->Ensure(Varint::kMax64 + size_len);
  encoder->put_varint64(size_len);
  encoder->putn(data_, size_ * len_);
}

}  // namespace s2coding

#endif  // S2_ENCODED_UINT_VECTOR_H_
