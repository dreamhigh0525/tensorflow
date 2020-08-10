/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/stream_executor/tpu/tpu_platform_interface.h"

#include <atomic>

#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/stream_executor/multi_platform_manager.h"

namespace tensorflow {
namespace tpu {

namespace {
TpuPlatformInterface* GetRegisteredPlatformStatic(bool initialize_platform) {
  // Prefer TpuPlatform if it's registered.
  auto status_or_tpu_platform =
      stream_executor::MultiPlatformManager::PlatformWithName(
          "TPU", initialize_platform);
  if (status_or_tpu_platform.ok()) {
    return static_cast<TpuPlatformInterface*>(
        status_or_tpu_platform.ValueOrDie());
  }
  if (status_or_tpu_platform.status().code() != error::NOT_FOUND) {
    LOG(WARNING) << "Error when getting the TPU platform: "
                 << status_or_tpu_platform.status();
    return nullptr;
  }

  // Use any other registered TPU platform.
  auto status_or_other_tpu_platforms =
      stream_executor::MultiPlatformManager::PlatformsWithFilter(
          [](const stream_executor::Platform* platform) {
            return dynamic_cast<const TpuPlatformInterface*>(platform) !=
                   nullptr;
          },
          initialize_platform);
  if (!status_or_other_tpu_platforms.ok()) {
    LOG(WARNING) << "Error when getting other TPU platforms: "
                 << status_or_tpu_platform.status();
    return nullptr;
  }
  auto other_tpu_platforms = status_or_other_tpu_platforms.ValueOrDie();
  if (!other_tpu_platforms.empty()) {
    LOG(WARNING) << other_tpu_platforms.size()
                 << " TPU platforms registered, selecting "
                 << other_tpu_platforms[0]->Name();
    return static_cast<TpuPlatformInterface*>(other_tpu_platforms[0]);
  }

  LOG(WARNING) << "No TPU platform registered";
  return nullptr;
}
}  // namespace

/* static */
TpuPlatformInterface* TpuPlatformInterface::GetRegisteredPlatform() {
  return GetRegisteredPlatform(/*initialize_platform=*/true);
}

/* static */
TpuPlatformInterface* TpuPlatformInterface::GetRegisteredPlatform(
    bool initialize_platform) {
  static auto* mu = new mutex;
  static bool requested_initialize_platform = initialize_platform;
  static TpuPlatformInterface* tpu_registered_platform =
      GetRegisteredPlatformStatic(initialize_platform);

  mutex_lock lock(*mu);
  if (!requested_initialize_platform && initialize_platform) {
    // If the first time this function is called, we did not request
    // initializing the platform, but the next caller wants the platform
    // initialized, we will call GetRegisteredPlatformStatic again to initialize
    // the platform.
    tpu_registered_platform = GetRegisteredPlatformStatic(initialize_platform);
    requested_initialize_platform = true;
  }

  return tpu_registered_platform;
}

}  // namespace tpu
}  // namespace tensorflow
