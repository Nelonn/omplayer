// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// NOLINTBEGIN(bugprone-integer-division,bugprone-unhandled-self-assignment,modernize-use-std-numbers)

#pragma once

#include <memory>
#include <functional>
#include <cstddef>
#include <type_traits>
#include <audiokit/Macro.hpp>

#if defined(_MSC_VER)
#include <malloc.h>  // for _aligned_malloc
#endif

namespace detail {

template<class C, class FreeProc /*= ScopedPtrMallocFree*/>
class scoped_ptr_malloc {
  //MOVE_ONLY_TYPE_FOR_CPP_03(scoped_ptr_malloc, RValue)
  using RValue = scoped_ptr_malloc&;

 public:

  // The element type
  typedef C element_type;

  // Constructor.  Defaults to initializing with NULL.
  // There is no way to create an uninitialized scoped_ptr.
  // The input parameter must be allocated with an allocator that matches the
  // Free functor.  For the default Free functor, this is malloc, calloc, or
  // realloc.
  explicit scoped_ptr_malloc(C* p = NULL): ptr_(p) {}

  // Constructor.  Move constructor for C++03 move emulation of this type.
  scoped_ptr_malloc(RValue& other)
      // The type of the underlying object is scoped_ptr_malloc; we have to
      // reinterpret_cast back to the original type for the call to release to
      // be valid. (See C++11 5.2.10.7)
      : ptr_(reinterpret_cast<scoped_ptr_malloc&>(other).release()) {
  }

  // Destructor.  If there is a C object, call the Free functor.
  ~scoped_ptr_malloc() {
    reset();
  }

  // operator=.  Move operator= for C++03 move emulation of this type.
  scoped_ptr_malloc& operator=(RValue& rhs) {
    swap(rhs);
    return *this;
  }

  // Reset.  Calls the Free functor on the current owned object, if any.
  // Then takes ownership of a new object, if given.
  // this->reset(this->get()) works.
  void reset(C* p = NULL) {
    if (ptr_ != p) {
      FreeProc free_proc;
      free_proc(ptr_);
      ptr_ = p;
    }
  }

  // Get the current object.
  // operator* and operator-> will cause an assert() failure if there is
  // no current object.
  C& operator*() const {
    assert(ptr_ != NULL);
    return *ptr_;
  }

  C* operator->() const {
    assert(ptr_ != NULL);
    return ptr_;
  }

  C* get() const {
    return ptr_;
  }

  // Comparison operators.
  // These return whether a scoped_ptr_malloc and a plain pointer refer
  // to the same object, not just to two different but equal objects.
  // For compatibility with the boost-derived implementation, these
  // take non-const arguments.
  bool operator==(C* p) const {
    return ptr_ == p;
  }

  bool operator!=(C* p) const {
    return ptr_ != p;
  }

  // Swap two scoped pointers.
  void swap(scoped_ptr_malloc & b) {
    C* tmp = b.ptr_;
    b.ptr_ = ptr_;
    ptr_ = tmp;
  }

  // Release a pointer.
  // The return value is the current pointer held by this object.
  // If this object holds a NULL pointer, the return value is NULL.
  // After this operation, this object will hold a NULL pointer,
  // and will not own the object any more.
  C* release() /*WARN_UNUSED_RESULT*/ {
    C* tmp = ptr_;
    ptr_ = NULL;
    return tmp;
  }

 private:
  C* ptr_;

  // no reason to use these: each scoped_ptr_malloc should have its own object
  template <class C2, class GP>
  bool operator==(scoped_ptr_malloc<C2, GP> const& p) const;
  template <class C2, class GP>
  bool operator!=(scoped_ptr_malloc<C2, GP> const& p) const;
};

inline auto AlignedAlloc(size_t size, size_t alignment) -> void* {
#if defined(_MSC_VER) || defined(__MINGW32__)
  return _aligned_malloc(size, alignment);
#else
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0) {
    return nullptr;
  }
  return ptr;
#endif
}

struct ScopedPtrAlignedFree {
  void operator()(void* ptr) const {
    if (ptr) {
#if defined(_MSC_VER)
      _aligned_free(ptr);
#else
      free(ptr);
#endif
    }
  }
};

}

#define FRIEND_TEST_ALL_PREFIXES(...)

namespace audiokit {

// SincResampler is a high-quality single-channel sample-rate converter.
class AUDIOKIT_ABI SincResampler {
 public:
  // Callback type for providing more data into the resampler.  Expects |frames|
  // of data to be rendered into |destination|; zero padded if not enough frames
  // are available to satisfy the request.
  using ReadCB = std::function<void(float* destination, int frames)>;

  // Constructs a SincResampler with the specified |read_cb|, which is used to
  // acquire audio data for resampling.  |io_sample_rate_ratio| is the ratio of
  // input / output sample rates.
  SincResampler(double io_sample_rate_ratio, const ReadCB& read_cb);
  virtual ~SincResampler();

  // Resample |frames| of data from |read_cb_| into |destination|.
  void Resample(float* destination, int frames);

  // The maximum size in frames that guarantees Resample() will only make a
  // single call to |read_cb_| for more data.
  int ChunkSize();

  // Flush all buffered data and reset internal indices.
  void Flush();

 private:
  FRIEND_TEST_ALL_PREFIXES(SincResamplerTest, Convolve);
  FRIEND_TEST_ALL_PREFIXES(SincResamplerTest, ConvolveBenchmark);

  void InitializeKernel();

  // Compute convolution of |k1| and |k2| over |input_ptr|, resultant sums are
  // linearly interpolated using |kernel_interpolation_factor|.  On x86, the
  // underlying implementation is chosen at run time based on SSE support.  On
  // ARM, NEON support is chosen at compile time based on compilation flags.
  static float Convolve(const float* input_ptr, const float* k1,
                        const float* k2, double kernel_interpolation_factor);
  static float Convolve_C(const float* input_ptr, const float* k1,
                          const float* k2, double kernel_interpolation_factor);
  static float Convolve_SSE(const float* input_ptr, const float* k1,
                            const float* k2,
                            double kernel_interpolation_factor);
  static float Convolve_NEON(const float* input_ptr, const float* k1,
                             const float* k2,
                             double kernel_interpolation_factor);

  // The ratio of input / output sample rates.
  double io_sample_rate_ratio_;

  // An index on the source input buffer with sub-sample precision.  It must be
  // double precision to avoid drift.
  double virtual_source_idx_;

  // The buffer is primed once at the very beginning of processing.
  bool buffer_primed_;

  // Source of data for resampling.
  ReadCB read_cb_;

  // Contains kKernelOffsetCount kernels back-to-back, each of size kKernelSize.
  // The kernel offsets are sub-sample shifts of a windowed sinc shifted from
  // 0.0 to 1.0 sample.
  detail::scoped_ptr_malloc<float, detail::ScopedPtrAlignedFree> kernel_storage_;

  // Data from the source is copied into this buffer for each processing pass.
  detail::scoped_ptr_malloc<float, detail::ScopedPtrAlignedFree> input_buffer_;

  // Pointers to the various regions inside |input_buffer_|.  See the diagram at
  // the top of the .cc file for more information.
  float* const r0_;
  float* const r1_;
  float* const r2_;
  float* const r3_;
  float* const r4_;
  float* const r5_;

  //DISALLOW_COPY_AND_ASSIGN(SincResampler);
};

}  // namespace media

// NOLINTEND(bugprone-integer-division,bugprone-unhandled-self-assignment,modernize-use-std-numbers)
