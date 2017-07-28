/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef THIRD_PARTY_TENSORFLOW_COMPILER_XLA_EXECUTION_OPTIONS_UTIL_H_
#define THIRD_PARTY_TENSORFLOW_COMPILER_XLA_EXECUTION_OPTIONS_UTIL_H_

#include "tensorflow/compiler/xla/xla.pb.h"

namespace xla {

// Create a default ExecutionOptions proto; this proto has its debug options
// popupated to the default values taken from flags.
ExecutionOptions CreateDefaultExecutionOptions();

}  // namespace xla

#endif  // THIRD_PARTY_TENSORFLOW_COMPILER_XLA_EXECUTION_OPTIONS_UTIL_H_
