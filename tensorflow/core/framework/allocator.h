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

#ifndef TENSORFLOW_FRAMEWORK_ALLOCATOR_H_
#define TENSORFLOW_FRAMEWORK_ALLOCATOR_H_

#include <stdlib.h>
#include <unistd.h>

#include <limits>

#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/port.h"

namespace tensorflow {

// Attributes for a single allocation call. Different calls to the same
// allocator could potentially have different allocation attributes.
struct AllocationAttributes {
  // If the first attempt to allocate the memory fails, the allocation
  // should return immediately without retrying.
  // An example use case is optional scratch spaces where a failure
  // has only performance impact.
  bool no_retry_on_failure = false;
};

// Allocator is an abstract interface for allocating and deallocating
// device memory.
class Allocator {
 public:
  virtual ~Allocator();

  // Return a string identifying this allocator
  virtual string Name() = 0;

  // Return an uninitialized block of memory that is "num_bytes" bytes
  // in size.  The returned pointer is guaranteed to be aligned to a
  // multiple of "alignment" bytes.
  // REQUIRES: "alignment" is a power of 2.
  virtual void* AllocateRaw(size_t alignment, size_t num_bytes) = 0;

  // Return an uninitialized block of memory that is "num_bytes" bytes
  // in size with specified allocation attributes.  The returned pointer is
  // guaranteed to be aligned to a multiple of "alignment" bytes.
  // REQUIRES: "alignment" is a power of 2.
  virtual void* AllocateRaw(size_t alignment, size_t num_bytes,
                            const AllocationAttributes& allocation_attr) {
    // The default behavior is to use the implementation without any allocation
    // attributes.
    return AllocateRaw(alignment, num_bytes);
  }

  // Deallocate a block of memory pointer to by "ptr"
  // REQUIRES: "ptr" was previously returned by a call to AllocateRaw
  virtual void DeallocateRaw(void* ptr) = 0;

  // Convenience functions to do typed allocation.  Note that these functions
  // do not invoke C++ constructors or destructors.  May return NULL if the
  // tensor has too many elements to represent in a single allocation.
  template <typename T>
  T* Allocate(size_t num_elements) {
    return Allocate<T>(num_elements, AllocationAttributes());
  }

  template <typename T>
  T* Allocate(size_t num_elements,
              const AllocationAttributes& allocation_attr) {
    // TODO(jeff): Do we need to allow clients to pass in alignment
    // requirements?

    if (num_elements > (std::numeric_limits<size_t>::max() / sizeof(T))) {
      return NULL;
    }

    void* p = AllocateRaw(32 /* align to 32 byte boundary */,
                          sizeof(T) * num_elements, allocation_attr);
    return reinterpret_cast<T*>(p);
  }

  template <typename T>
  void Deallocate(T* ptr) {
    DeallocateRaw(ptr);
  }

  // Returns true if this allocator tracks the sizes of allocations.
  // RequestedSize and AllocatedSize must be overridden if
  // TracksAlloctionSizes is overridden to return true.
  virtual bool TracksAllocationSizes() { return false; }

  // Returns the user-requested size of the data allocated at
  // 'ptr'.  Note that the actual buffer allocated might be larger
  // than requested, but this function returns the size requested by
  // the user.
  //
  // REQUIRES: TracksAllocationSizes() is true.
  //
  // REQUIRES: 'ptr!=nullptr' and points to a buffer previously
  // allocated by this allocator.
  virtual size_t RequestedSize(void* ptr) {
    CHECK(false) << "allocator doesn't track sizes";
  }

  // Returns the allocated size of the buffer at 'ptr' if known,
  // otherwise returns RequestedSize(ptr). AllocatedSize(ptr) is
  // guaranteed to be >= RequestedSize(ptr).
  //
  // REQUIRES: TracksAllocationSizes() is true.
  //
  // REQUIRES: 'ptr!=nullptr' and points to a buffer previously
  // allocated by this allocator.
  virtual size_t AllocatedSize(void* ptr) { return RequestedSize(ptr); }

  // TODO(jeff): Maybe provide some interface to give info about
  // current allocation state (total number of bytes available for
  // allocation, number of bytes free on device, etc.)
};

// A tensorflow Op may need access to different kinds of memory that
// are not simply a function of the device to which the Op has been
// assigned.  For example, an Op executing on a GPU may still need
// to allocate CPU RAM for some purpose.  Internal to the tensorflow
// runtime we may choose to allocate CPU ram from special regions
// that have been prepared for higher performance in some use
// contexts, e.g. doing DMA with particular devices.  For these
// reasons, the Device interface does not expose just one memory
// Allocator, but instead provides an accessor that takes a
// specification of the desired memory attributes in order to select
// an Allocator.
//
// NOTE: The upper 8 bits of the value are reserved for
// device-specific uses.  Implementors of a device can interpret these
// upper 8 bits in device-specific ways, and ops implemented for those
// devices are responsible for setting those 8 bits appropriately.
//
// Example use:
//  // Allocator for ordinary device memory:
//  Allocator* a = allocator(AllocatorAttributes());
// ...
//  // Allocator for CPU RAM, regardless of where Op is executing:
//  AllocatorAttributes attr;
//  attr.set_on_host(true);
//  Allocator* a = allocator(attr);
struct AllocatorAttributes {
  void set_on_host(bool v) { value |= (static_cast<int>(v)); }
  bool on_host() const { return value & 0x1; }
  void set_nic_compatible(bool v) { value |= (static_cast<int>(v) << 1); }
  bool nic_compatible() const { return value & (0x1 << 1); }
  void set_gpu_compatible(bool v) { value |= (static_cast<int>(v) << 2); }
  bool gpu_compatible() const { return value & (0x1 << 2); }

  void Merge(AllocatorAttributes other) { value |= other.value; }

  uint32 value = 0;
};

// Returns a trivial implementation of Allocator which uses the system
// default malloc.
Allocator* cpu_allocator();

}  // namespace tensorflow

#endif  // TENSORFLOW_FRAMEWORK_ALLOCATOR_H_
