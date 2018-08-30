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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_FUSIBLE_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_FUSIBLE_H_

#include "tensorflow/compiler/xla/service/hlo_instruction.h"

// TODO(b/112957171): Extract logic to determine fusibility of HLO ops from
// GpuInstructionFusion, FusionMerger, and GpuMultiOutputFusion.

namespace xla {
namespace gpu {

// The code emitted for reduce-rooted input fusions (EmitReductionToVector)
// suffers from poor data locality if the layouts of input parameters differ. In
// such situtations it is better not to fuse. Only input params with
// maximum rank are considered. Params with smaller ranks will be broadcasted
// and have not been observed to cause data locality issues.
// TODO(b/111977086): Improve reduce emitters to remove this limitation.
bool LayoutsAreReduceInputFusionFriendly(const HloInstruction& producer,
                                         const HloInstruction& reduce);

}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_GPU_FUSIBLE_H_
