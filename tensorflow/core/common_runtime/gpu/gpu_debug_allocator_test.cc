/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#if (defined(GOOGLE_CUDA) && GOOGLE_CUDA) || \
    (defined(TENSORFLOW_USE_ROCM) && TENSORFLOW_USE_ROCM)

#include "tensorflow/core/common_runtime/gpu/gpu_debug_allocator.h"

#include <algorithm>
#include <vector>

#include "tensorflow/compiler/xla/stream_executor/device_id_utils.h"
#include "tensorflow/compiler/xla/stream_executor/gpu/gpu_init.h"
#include "tensorflow/compiler/xla/stream_executor/platform.h"
#include "tensorflow/compiler/xla/stream_executor/stream_executor.h"
#include "tensorflow/core/common_runtime/device/device_mem_allocator.h"
#include "tensorflow/core/common_runtime/gpu/gpu_bfc_allocator.h"
#include "tensorflow/core/framework/typed_allocator.h"
#include "tensorflow/tsl/framework/device_id.h"
#include "tensorflow/tsl/lib/gtl/inlined_vector.h"
#include "tensorflow/tsl/platform/logging.h"
#include "tensorflow/tsl/platform/test.h"
#include "tensorflow/tsl/platform/types.h"

namespace tensorflow {
namespace {

se::StreamExecutor* ExecutorForPlatformDeviceId(
    tsl::PlatformDeviceId platform_device_id) {
  return se::DeviceIdUtil::ExecutorForPlatformDeviceId(se::GPUMachineManager(),
                                                       platform_device_id)
      .value();
}

TEST(GPUDebugAllocatorTest, OverwriteDetection_None) {
  const tsl::PlatformDeviceId platform_device_id(0);
  auto stream_exec = ExecutorForPlatformDeviceId(platform_device_id);
  GPUDebugAllocator a(
      new GPUBFCAllocator(absl::WrapUnique(new DeviceMemAllocator(
                              stream_exec, platform_device_id,
                              false /*use_unified_memory*/, {}, {})),
                          1 << 30, "", {}),
      platform_device_id);

  for (int s : {8}) {
    std::vector<int64_t> cpu_array(s);
    memset(&cpu_array[0], 0, cpu_array.size() * sizeof(int64_t));
    int64_t* gpu_array =
        TypedAllocator::Allocate<int64_t>(&a, cpu_array.size(), {});
    se::DeviceMemory<int64_t> gpu_array_ptr{se::DeviceMemoryBase{gpu_array}};
    ASSERT_TRUE(stream_exec->SynchronousMemcpy(&gpu_array_ptr, &cpu_array[0],
                                               s * sizeof(int64_t)));
    EXPECT_TRUE(a.CheckHeader(gpu_array));
    EXPECT_TRUE(a.CheckFooter(gpu_array));

    // Confirm no error on free.
    a.DeallocateRaw(gpu_array);
  }
}

TEST(GPUDebugAllocatorTest, OverwriteDetection_Header) {
  for (int s : {8, 211}) {
    EXPECT_DEATH(
        {
          const tsl::PlatformDeviceId platform_device_id(0);
          auto stream_exec = ExecutorForPlatformDeviceId(platform_device_id);
          GPUDebugAllocator a(
              new GPUBFCAllocator(absl::WrapUnique(new DeviceMemAllocator(
                                      stream_exec, platform_device_id,
                                      false /*use_unified_memory*/, {}, {})),
                                  1 << 30, "", {}),
              platform_device_id);

          std::vector<int64_t> cpu_array(s);
          memset(&cpu_array[0], 0, cpu_array.size() * sizeof(int64_t));
          int64_t* gpu_array =
              TypedAllocator::Allocate<int64_t>(&a, cpu_array.size(), {});

          se::DeviceMemory<int64_t> gpu_array_ptr{
              se::DeviceMemoryBase{gpu_array}};
          ASSERT_TRUE(stream_exec->SynchronousMemcpy(
              &gpu_array_ptr, &cpu_array[0],
              cpu_array.size() * sizeof(int64_t)));

          se::DeviceMemory<int64_t> gpu_hdr_ptr{
              se::DeviceMemoryBase{gpu_array - 1}};
          // Clobber first word of the header.
          float pi = 3.1417;
          ASSERT_TRUE(
              stream_exec->SynchronousMemcpy(&gpu_hdr_ptr, &pi, sizeof(float)));

          // Expect error on free.
          a.DeallocateRaw(gpu_array);
        },
        "");
  }
}

TEST(GPUDebugAllocatorTest, OverwriteDetection_Footer) {
  for (int s : {8, 22}) {
    EXPECT_DEATH(
        {
          const tsl::PlatformDeviceId platform_device_id(0);
          auto stream_exec = ExecutorForPlatformDeviceId(platform_device_id);
          GPUDebugAllocator a(
              new GPUBFCAllocator(absl::WrapUnique(new DeviceMemAllocator(
                                      stream_exec, platform_device_id,
                                      false /*use_unified_memory*/, {}, {})),
                                  1 << 30, "", {}),
              platform_device_id);

          std::vector<int64_t> cpu_array(s);
          memset(&cpu_array[0], 0, cpu_array.size() * sizeof(int64_t));
          int64_t* gpu_array =
              TypedAllocator::Allocate<int64_t>(&a, cpu_array.size(), {});

          se::DeviceMemory<int64_t> gpu_array_ptr{
              se::DeviceMemoryBase{gpu_array}};
          ASSERT_TRUE(stream_exec->SynchronousMemcpy(
              &gpu_array_ptr, &cpu_array[0],
              cpu_array.size() * sizeof(int64_t)));

          // Clobber word of the footer.
          se::DeviceMemory<int64_t> gpu_ftr_ptr{
              se::DeviceMemoryBase{gpu_array + s}};
          float pi = 3.1417;
          ASSERT_TRUE(
              stream_exec->SynchronousMemcpy(&gpu_ftr_ptr, &pi, sizeof(float)));

          // Expect error on free.
          a.DeallocateRaw(gpu_array);
        },
        "");
  }
}

TEST(GPUDebugAllocatorTest, ResetToNan) {
  const tsl::PlatformDeviceId platform_device_id(0);
  auto stream_exec = ExecutorForPlatformDeviceId(platform_device_id);
  GPUNanResetAllocator a(
      new GPUBFCAllocator(absl::WrapUnique(new DeviceMemAllocator(
                              stream_exec, platform_device_id,
                              false /*use_unified_memory*/, {}, {})),
                          1 << 30, "", {}),
      platform_device_id);

  std::vector<float> cpu_array(1024);
  std::vector<float> cpu_array_result(1024);

  // Allocate 1024 floats
  float* gpu_array = TypedAllocator::Allocate<float>(&a, cpu_array.size(), {});
  se::DeviceMemory<float> gpu_array_ptr{se::DeviceMemoryBase{gpu_array}};
  ASSERT_TRUE(stream_exec->SynchronousMemcpy(&cpu_array[0], gpu_array_ptr,
                                             cpu_array.size() * sizeof(float)));
  for (float f : cpu_array) {
    ASSERT_FALSE(std::isfinite(f));
  }

  // Set one of the fields to 1.0.
  cpu_array[0] = 1.0;
  ASSERT_TRUE(stream_exec->SynchronousMemcpy(&gpu_array_ptr, &cpu_array[0],
                                             cpu_array.size() * sizeof(float)));
  // Copy the data back and verify.
  ASSERT_TRUE(
      stream_exec->SynchronousMemcpy(&cpu_array_result[0], gpu_array_ptr,
                                     cpu_array_result.size() * sizeof(float)));
  ASSERT_EQ(1.0, cpu_array_result[0]);

  // Free the array
  a.DeallocateRaw(gpu_array);

  // All values should be reset to nan.
  ASSERT_TRUE(
      stream_exec->SynchronousMemcpy(&cpu_array_result[0], gpu_array_ptr,
                                     cpu_array_result.size() * sizeof(float)));
  for (float f : cpu_array_result) {
    ASSERT_FALSE(std::isfinite(f));
  }
}

TEST(GPUDebugAllocatorTest, ResetToNanWithHeaderFooter) {
  const tsl::PlatformDeviceId platform_device_id(0);
  auto stream_exec = ExecutorForPlatformDeviceId(platform_device_id);
  // NaN reset must be the outer-most allocator.
  GPUNanResetAllocator a(
      new GPUBFCAllocator(absl::WrapUnique(new DeviceMemAllocator(
                              stream_exec, platform_device_id,
                              false /*use_unified_memory*/, {}, {})),
                          1 << 30, "", {}),
      platform_device_id);

  std::vector<float> cpu_array(1024);
  std::vector<float> cpu_array_result(1024);

  // Allocate 1024 floats
  float* gpu_array = TypedAllocator::Allocate<float>(&a, cpu_array.size(), {});
  se::DeviceMemory<float> gpu_array_ptr{se::DeviceMemoryBase{gpu_array}};
  ASSERT_TRUE(stream_exec->SynchronousMemcpy(&cpu_array[0], gpu_array_ptr,
                                             cpu_array.size() * sizeof(float)));
  for (float f : cpu_array) {
    ASSERT_FALSE(std::isfinite(f));
  }

  // Set one of the fields to 1.0.
  cpu_array[0] = 1.0;
  ASSERT_TRUE(stream_exec->SynchronousMemcpy(&gpu_array_ptr, &cpu_array[0],
                                             cpu_array.size() * sizeof(float)));
  // Copy the data back and verify.
  ASSERT_TRUE(
      stream_exec->SynchronousMemcpy(&cpu_array_result[0], gpu_array_ptr,
                                     cpu_array_result.size() * sizeof(float)));
  ASSERT_EQ(1.0, cpu_array_result[0]);

  // Free the array
  a.DeallocateRaw(gpu_array);

  // All values should be reset to nan.
  ASSERT_TRUE(
      stream_exec->SynchronousMemcpy(&cpu_array_result[0], gpu_array_ptr,
                                     cpu_array_result.size() * sizeof(float)));
  for (float f : cpu_array_result) {
    ASSERT_FALSE(std::isfinite(f));
  }
}

TEST(GPUDebugAllocatorTest, TracksSizes) {
  const tsl::PlatformDeviceId platform_device_id(0);
  auto stream_exec = ExecutorForPlatformDeviceId(platform_device_id);
  GPUDebugAllocator a(
      new GPUBFCAllocator(absl::WrapUnique(new DeviceMemAllocator(
                              stream_exec, platform_device_id,
                              false /*use_unified_memory*/, {}, {})),
                          1 << 30, "", {}),
      platform_device_id);
  EXPECT_EQ(true, a.TracksAllocationSizes());
}

TEST(GPUDebugAllocatorTest, AllocatedVsRequested) {
  const tsl::PlatformDeviceId platform_device_id(0);
  auto stream_exec = ExecutorForPlatformDeviceId(platform_device_id);
  GPUDebugAllocator a(
      new GPUBFCAllocator(absl::WrapUnique(new DeviceMemAllocator(
                              stream_exec, platform_device_id,
                              false /*use_unified_memory*/, {}, {})),
                          1 << 30, "", {}),
      platform_device_id);
  float* t1 = TypedAllocator::Allocate<float>(&a, 1, {});
  EXPECT_EQ(4, a.RequestedSize(t1));
  EXPECT_EQ(256, a.AllocatedSize(t1));
  a.DeallocateRaw(t1);
}

}  // namespace
}  // namespace tensorflow

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
