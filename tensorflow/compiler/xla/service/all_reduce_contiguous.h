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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_ALL_REDUCE_CONTIGUOUS_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_ALL_REDUCE_CONTIGUOUS_H_

#include "tensorflow/compiler/xla/hlo/ir/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {

// Concatenates all-reduce operands together, so the all-reduce is performed
// over a single, contiguous buffer.
class AllReduceContiguous : public HloModulePass {
 public:
  absl::string_view name() const override { return "all-reduce-contiguous"; }

  using HloPassInterface::Run;
  StatusOr<bool> Run(
      HloModule* module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) override;
};

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_ALL_REDUCE_CONTIGUOUS_H_
