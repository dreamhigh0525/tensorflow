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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_ALL_REDUCE_SCATTER_CREATOR_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_ALL_REDUCE_SCATTER_CREATOR_H_

#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"

namespace xla {
namespace gpu {

// Transforms dynamic-slice(all-reduce) to a all-reduce-scatter custom call.
class AllReduceScatterCreator : public HloModulePass {
 public:
  AllReduceScatterCreator() = default;
  absl::string_view name() const override {
    return "all-reduce-scatter-creator";
  }

  StatusOr<bool> Run(HloModule* module) override;
};

}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_ALL_REDUCE_SCATTER_CREATOR_H_
