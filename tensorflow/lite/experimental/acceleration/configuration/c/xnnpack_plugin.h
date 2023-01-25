/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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
#ifndef TENSORFLOW_LITE_EXPERIMENTAL_ACCELERATION_CONFIGURATION_C_XNNPACK_PLUGIN_H_
#define TENSORFLOW_LITE_EXPERIMENTAL_ACCELERATION_CONFIGURATION_C_XNNPACK_PLUGIN_H_

// This header file is for the delegate plugin for XNNPACK.
//
// For the C++ delegate plugin interface, the XNNPACK delegate plugin is added
// to the DelegatePluginRegistry by the side effect of a constructor for a
// static object, so there's no public API needed for this plugin, other than
// the API of tflite::delegates::DelegatePluginRegistry, which is declared in
// delegate_registry.h.
//
// But to provide a C API to access the XNNPACK delegate plugin, we do expose
// some functions, which are declared below.

// NOLINTBEGIN(whitespace/line_length)
/// For documentation, see
/// third_party/tensorflow/lite/core/experimental/acceleration/configuration/c/xnnpack_plugin.h.
// NOLINTEND(whitespace/line_length)
#include "tensorflow/lite/core/experimental/acceleration/configuration/c/xnnpack_plugin.h"  // IWYU pragma: export

#endif  // TENSORFLOW_LITE_EXPERIMENTAL_ACCELERATION_CONFIGURATION_C_XNNPACK_PLUGIN_H_
