/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CORE_TRANSFORMS_REGION_TO_FUNCTIONAL_PASS_H_
#define TENSORFLOW_CORE_TRANSFORMS_REGION_TO_FUNCTIONAL_PASS_H_

#include <memory>

#include "mlir/Pass/Pass.h"  // from @llvm-project

namespace mlir {
namespace tfg {
// Creates a conversion pass from region control-flow to functional
// control-flow. If `force_control_capture` is set, then all region control-flow
// ops are guaranteed to be converted to functional form by capturing implicit
// control tokens using a `Const` node.
std::unique_ptr<Pass> CreateRegionToFunctionalPass(
    bool force_control_capture = false);
}  // namespace tfg
}  // namespace mlir

#endif  // TENSORFLOW_CORE_TRANSFORMS_REGION_TO_FUNCTIONAL_PASS_H_
