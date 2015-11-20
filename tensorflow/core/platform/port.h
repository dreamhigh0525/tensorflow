/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_PLATFORM_PORT_H_
#define TENSORFLOW_PLATFORM_PORT_H_

#include <string>
#include <vector>

#if !defined(PLATFORM_POSIX) && !defined(PLATFORM_GOOGLE) && \
    !defined(PLATFORM_POSIX_ANDROID) && !defined(PLATFORM_GOOGLE_ANDROID)

// Choose which platform we are on.
#if defined(ANDROID) || defined(__ANDROID__)
#define PLATFORM_POSIX_ANDROID
#elif defined(__APPLE__)
#define PLATFORM_POSIX
#else
// If no platform specified, use:
#define PLATFORM_POSIX
#endif

#endif

// Define tensorflow::string to refer to appropriate platform specific type.
namespace tensorflow {
#if defined(PLATFORM_GOOGLE)
using ::string;
#else
using std::string;
#endif
}  // namespace tensorflow

namespace tensorflow {
enum ConditionResult { kCond_Timeout, kCond_MaybeNotified };
}  // namespace tensorflow

// Include appropriate platform-dependent implementations of mutex etc.
#if defined(PLATFORM_GOOGLE)
#include "tensorflow/core/platform/google/dynamic_annotations.h"
#include "tensorflow/core/platform/google/integral_types.h"
#include "tensorflow/core/platform/google/mutex.h"
#elif defined(PLATFORM_POSIX) || defined(PLATFORM_POSIX_ANDROID) || \
    defined(PLATFORM_GOOGLE_ANDROID)
#include "tensorflow/core/platform/default/dynamic_annotations.h"
#include "tensorflow/core/platform/default/integral_types.h"
#include "tensorflow/core/platform/default/mutex.h"
#else
#error Define the appropriate PLATFORM_<foo> macro for this platform
#endif

namespace tensorflow {

static const uint8 kuint8max = ((uint8)0xFF);
static const uint16 kuint16max = ((uint16)0xFFFF);
static const uint32 kuint32max = ((uint32)0xFFFFFFFF);
static const uint64 kuint64max = ((uint64)0xFFFFFFFFFFFFFFFFull);
static const int8 kint8min = ((int8)~0x7F);
static const int8 kint8max = ((int8)0x7F);
static const int16 kint16min = ((int16)~0x7FFF);
static const int16 kint16max = ((int16)0x7FFF);
static const int32 kint32min = ((int32)~0x7FFFFFFF);
static const int32 kint32max = ((int32)0x7FFFFFFF);
static const int64 kint64min = ((int64)~0x7FFFFFFFFFFFFFFFll);
static const int64 kint64max = ((int64)0x7FFFFFFFFFFFFFFFll);

// A typedef for a uint64 used as a short fingerprint.
typedef uint64 Fprint;

// The mutex library included above defines:
//   class mutex;
//   class mutex_lock;
//   class condition_variable;
// It also defines the following:

// Like "cv->wait(*mu)", except that it only waits for up to "ms" milliseconds.
//
// Returns kCond_Timeout if the timeout expired without this
// thread noticing a signal on the condition variable.  Otherwise may
// return either kCond_Timeout or kCond_MaybeNotified
ConditionResult WaitForMilliseconds(mutex_lock* mu, condition_variable* cv,
                                    int64 ms);
}  // namespace tensorflow

namespace tensorflow {
namespace port {

// TODO(jeff,sanjay): Make portable
static const bool kLittleEndian = true;

// TODO(jeff,sanjay): Find appropriate places for all the code below.
// Possible places for any particular item below:
//  (a) Here, so it gets reimplemented on every platform
//  (b) Env
//  (c) config.h (auto-generated by autotools?)
//  (d) macros.h
//  ...

// Return the hostname of the machine on which this process is running
string Hostname();

// Returns an estimate of the number of schedulable CPUs for this
// process.  Usually, it's constant throughout the lifetime of a
// process, but it might change if the underlying cluster management
// software can change it dynamically.
int NumSchedulableCPUs();

// Some platforms require that filenames be of a certain form when
// used for logging.  This function is invoked to allow platforms to
// adjust the filename used for logging appropriately, if necessary
// (most ports can just do nothing).  If any changes are necessary, the
// implementation should mutate "*filename" appropriately.
void AdjustFilenameForLogging(string* filename);

// Aligned allocation/deallocation
void* aligned_malloc(size_t size, int minimum_alignment);
void aligned_free(void* aligned_memory);

// Prefetching support
//
// Defined behavior on some of the uarchs:
// PREFETCH_HINT_T0:
//   prefetch to all levels of the hierarchy (except on p4: prefetch to L2)
// PREFETCH_HINT_NTA:
//   p4: fetch to L2, but limit to 1 way (out of the 8 ways)
//   core: skip L2, go directly to L1
//   k8 rev E and later: skip L2, can go to either of the 2-ways in L1
enum PrefetchHint {
  PREFETCH_HINT_T0 = 3,  // More temporal locality
  PREFETCH_HINT_T1 = 2,
  PREFETCH_HINT_T2 = 1,  // Less temporal locality
  PREFETCH_HINT_NTA = 0  // No temporal locality
};
template <PrefetchHint hint>
void prefetch(const void* x);

// Snappy compression/decompression support
bool Snappy_Compress(const char* input, size_t length, string* output);

bool Snappy_GetUncompressedLength(const char* input, size_t length,
                                  size_t* result);
bool Snappy_Uncompress(const char* input, size_t length, char* output);

#if defined(__GXX_EXPERIMENTAL_CXX0X__) || __cplusplus >= 201103L
// Define this to 1 if the code is compiled in C++11 mode; leave it
// undefined otherwise.  Do NOT define it to 0 -- that causes
// '#ifdef LANG_CXX11' to behave differently from '#if LANG_CXX11'.
#define LANG_CXX11 1
#endif

// Compiler attributes
#if (defined(__GNUC__) || defined(__APPLE__)) && !defined(SWIG)
// Compiler supports GCC-style attributes
#define TF_ATTRIBUTE_NORETURN __attribute__((noreturn))
#define TF_ATTRIBUTE_NOINLINE __attribute__((noinline))
#define TF_ATTRIBUTE_UNUSED __attribute__((unused))
#define TF_ATTRIBUTE_COLD __attribute__((cold))
#define TF_PACKED __attribute__((packed))
#define TF_MUST_USE_RESULT __attribute__((warn_unused_result))
#define TF_PRINTF_ATTRIBUTE(string_index, first_to_check) \
  __attribute__((__format__(__printf__, string_index, first_to_check)))
#define TF_SCANF_ATTRIBUTE(string_index, first_to_check) \
  __attribute__((__format__(__scanf__, string_index, first_to_check)))

#else
// Non-GCC equivalents
#define TF_ATTRIBUTE_NORETURN
#define TF_ATTRIBUTE_NOINLINE
#define TF_ATTRIBUTE_UNUSED
#define TF_ATTRIBUTE_COLD
#define TF_MUST_USE_RESULT
#define TF_PACKED
#define TF_PRINTF_ATTRIBUTE(string_index, first_to_check)
#define TF_SCANF_ATTRIBUTE(string_index, first_to_check)
#endif

// GCC can be told that a certain branch is not likely to be taken (for
// instance, a CHECK failure), and use that information in static analysis.
// Giving it this information can help it optimize for the common case in
// the absence of better information (ie. -fprofile-arcs).
//
#if defined(COMPILER_GCC3)
#define TF_PREDICT_FALSE(x) (__builtin_expect(x, 0))
#define TF_PREDICT_TRUE(x) (__builtin_expect(!!(x), 1))
#else
#define TF_PREDICT_FALSE(x) x
#define TF_PREDICT_TRUE(x) x
#endif

// ---------------------------------------------------------------------------
// Inline implementations of some performance-critical methods
// ---------------------------------------------------------------------------
template <PrefetchHint hint>
inline void prefetch(const void* x) {
#if defined(__llvm__) || defined(COMPILER_GCC)
  __builtin_prefetch(x, 0, hint);
#else
// You get no effect.  Feel free to add more sections above.
#endif
}

// A macro to disallow the copy constructor and operator= functions
// This is usually placed in the private: declarations for a class.
#define TF_DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;         \
  void operator=(const TypeName&) = delete

// The TF_ARRAYSIZE(arr) macro returns the # of elements in an array arr.
//
// The expression TF_ARRAYSIZE(a) is a compile-time constant of type
// size_t.
#define TF_ARRAYSIZE(a)         \
  ((sizeof(a) / sizeof(*(a))) / \
   static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))

#if defined(__clang__) && defined(LANG_CXX11) && defined(__has_warning)
#if __has_feature(cxx_attributes) && __has_warning("-Wimplicit-fallthrough")
#define TF_FALLTHROUGH_INTENDED [[clang::fallthrough]]  // NOLINT
#endif
#endif

#ifndef TF_FALLTHROUGH_INTENDED
#define TF_FALLTHROUGH_INTENDED \
  do {                          \
  } while (0)
#endif

}  // namespace port
}  // namespace tensorflow

#endif  // TENSORFLOW_PLATFORM_PORT_H_
