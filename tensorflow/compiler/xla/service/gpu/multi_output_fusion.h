/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_MULTI_OUTPUT_FUSION_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_MULTI_OUTPUT_FUSION_H_

#include <queue>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"
#include "tensorflow/compiler/xla/service/hlo_reachability.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {
namespace gpu {

// Multi-output fusion of sibling and producer-consumer instructions for the
// GPU backend.
class GpuMultiOutputFusion : public HloModulePass {
 public:
  GpuMultiOutputFusion() = default;

  absl::string_view name() const override { return "multi_output_fusion"; }

  StatusOr<bool> Run(HloModule* module) override;

 private:
  bool FuseSiblings(HloInstruction* parent);

  bool DoMultiOutputFusion();

  // Recompute reachability for the current computation.
  void RecomputeReachability();

  // Computation for the pass.
  HloComputation* computation_;

  // The reachability map of current computation.
  std::unique_ptr<HloReachabilityMap> reachability_;
};

}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_MULTI_OUTPUT_FUSION_H_
