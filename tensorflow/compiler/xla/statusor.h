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
#ifndef TENSORFLOW_COMPILER_XLA_STATUSOR_H_
#define TENSORFLOW_COMPILER_XLA_STATUSOR_H_

#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace xla {

// Use steam_executor's StatusOr so we don't duplicate code.
using tsl::StatusOr;  // TENSORFLOW_STATUS_OK

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_STATUSOR_H_
