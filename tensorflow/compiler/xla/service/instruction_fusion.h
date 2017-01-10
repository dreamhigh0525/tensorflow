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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_INSTRUCTION_FUSION_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_INSTRUCTION_FUSION_H_

#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_pass.h"
#include "tensorflow/core/platform/macros.h"

namespace xla {

// Returns true if the computation of the given instruction is significantly
// more expensive than just writing all the values of the instructions' result
// array. Expensive operations should not be duplicated.
bool IsExpensive(const HloInstruction& instruction);

// Returns true if fusing producer into consumer would cause producer to be
// duplicated. This is the case if producer has uses other than consumer.
bool FusionWouldDuplicate(HloInstruction* producer, HloInstruction* consumer);

// HLO pass which performs instruction fusion. Instructions are fused
// "vertically", meaning producing instructions are fused into their consumers
// with the intent that the loops which compute their values will be fused in
// code generation. Derived classes define ShouldFuse method to select which
// instructions to fuse.
class InstructionFusion : public HloPass {
 public:
  explicit InstructionFusion(bool may_duplicate = true)
      : HloPass("fusion"), may_duplicate_(may_duplicate) {}
  ~InstructionFusion() override {}

  // Run instruction fusion on the given computation. Returns whether the
  // computation was changed (instructions were fused).
  StatusOr<bool> Run(HloModule* module) override;

 protected:
  // Returns whether the given producer instruction should be fused into the
  // given consumer instruction. producer is necessarily an operand of consumer.
  // Derived classes should define this method to specify which instructions
  // should be fused. `operand_index` is which operand of the consumer the
  // producer is.
  //
  // Instructions are traversed in reverse post order (computation root to
  // leaves). This method is called for each operand of the instruction (where
  // the operand is 'producer' and the instruction is 'consumer')
  //
  // Subtypes can override this with target-specific heuristics.
  virtual bool ShouldFuse(HloInstruction* consumer, int64 operand_index);

  // Chooses a fusion kind for `producer` and `consumer`.
  // Default method chooses `kLoop`.
  virtual HloInstruction::FusionKind ChooseKind(const HloInstruction* producer,
                                                const HloInstruction* consumer);

  // Current HloComputation instance the loop fuser is traversing.
  HloComputation* computation_;

 private:
  HloInstruction* Fuse(HloInstruction* producer, HloInstruction* consumer);

  // Returns whether we may duplicate an instruction if we want to fuse it.
  bool may_duplicate_;

  TF_DISALLOW_COPY_AND_ASSIGN(InstructionFusion);
};

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_INSTRUCTION_FUSION_H_
