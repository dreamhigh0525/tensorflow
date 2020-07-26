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

#include "tensorflow/stream_executor/platform/initialize.h"
#include "tensorflow/stream_executor/tpu/tpu_platform.h"

#if defined(PLATFORM_GOOGLE)
REGISTER_MODULE_INITIALIZER(tpu_platform, tensorflow::RegisterTpuPlatform());

DECLARE_MODULE_INITIALIZER(multi_platform_manager);
DECLARE_MODULE_INITIALIZER(multi_platform_manager_listener);

// Note that module initialization sequencing is not supported in the
// open-source project, so this will be a no-op there.
REGISTER_MODULE_INITIALIZER_SEQUENCE(tpu_platform, multi_platform_manager);
REGISTER_MODULE_INITIALIZER_SEQUENCE(multi_platform_manager_listener,
                                     tpu_platform);
#endif
