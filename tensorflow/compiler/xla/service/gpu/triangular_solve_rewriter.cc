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

#include "tensorflow/compiler/xla/service/gpu/triangular_solve_rewriter.h"

#include <numeric>

#include "tensorflow/compiler/xla/service/gpu/cublas_cudnn.h"
#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"

namespace xla {
namespace gpu {

StatusOr<bool> TriangularSolveRewriter::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  bool changed = false;
  for (HloComputation* comp :
       module->MakeNonfusionComputations(execution_threads)) {
    std::vector<HloInstruction*> to_rewrite;
    for (HloInstruction* instr : comp->instructions()) {
      if (instr->opcode() == HloOpcode::kTriangularSolve) {
        to_rewrite.push_back(instr);
      }
    }

    for (HloInstruction* instr : to_rewrite) {
      const Shape& b_shape = instr->operand(1)->shape();
      int64_t batch_size = std::accumulate(
          b_shape.dimensions().begin(), b_shape.dimensions().end() - 2,
          int64_t{1}, [](int64_t a, int64_t b) { return a * b; });

      // batch 1 triangular solves get 0 temp bytes, because unbatched trsm()
      // doesn't require temp memory.
      int64_t temp_bytes = batch_size == 1 ? 0 : 2 * sizeof(void*) * batch_size;
      Shape new_shape = ShapeUtil::MakeTupleShape({
          instr->shape(),
          ShapeUtil::MakeShape(S8, {temp_bytes}),
      });

      HloInstruction* custom_call =
          comp->AddInstruction(HloInstruction::CreateCustomCall(
              new_shape, instr->operands(), kTriangularSolveCallTarget));
      module->SetAndUniquifyInstrName(custom_call, "triangular-solve");
      TF_RETURN_IF_ERROR(
          custom_call->set_backend_config(instr->triangular_solve_options()));

      // Preserve metadata from `instr`.
      custom_call->set_metadata(instr->metadata());
      custom_call->set_frontend_attributes(instr->frontend_attributes());

      // Get the actual result out of the custom call's tuple.
      TF_ASSIGN_OR_RETURN(HloInstruction * gte,
                          MakeGetTupleElementHlo(custom_call, 0));
      TF_RETURN_IF_ERROR(comp->ReplaceInstruction(instr, gte));
    }
  }
  return changed;
}

}  // namespace gpu
}  // namespace xla
