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

// Small helper library to access "data" dependencies defined in BUILD files.
// Requires the relative paths starting from tensorflow/...
// For example, to get this file, a user would call:
// GetDataDependencyFilepath("tensorflow/core/platform/resource_loadder.h")

#ifndef TENSORFLOW_CORE_PLATFORM_RESOURCE_LOADER_H_
#define TENSORFLOW_CORE_PLATFORM_RESOURCE_LOADER_H_

#include "tensorflow/tsl/platform/resource_loader.h"

namespace tensorflow {

using tsl::GetDataDependencyFilepath;

}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_PLATFORM_RESOURCE_LOADER_H_
