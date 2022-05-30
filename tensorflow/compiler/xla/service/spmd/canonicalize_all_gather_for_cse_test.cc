/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/service/spmd/canonicalize_all_gather_for_cse.h"

#include "tensorflow/compiler/xla/service/hlo_matchers.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/service/hlo_pass_pipeline.h"
#include "tensorflow/compiler/xla/service/hlo_verifier.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace xla {
namespace spmd {
namespace {

using ::testing::_;
using ::testing::AllOf;
namespace op = xla::testing::opcode_matchers;

class AllGatherCanonicalizeTest : public HloTestBase {
 public:
  StatusOr<std::unique_ptr<HloModule>> RunPass(absl::string_view hlo_module) {
    TF_ASSIGN_OR_RETURN(auto module, ParseAndReturnVerifiedModule(
                                         hlo_module, GetModuleConfigForTest()));
    HloPassPipeline pipeline("all-gather-cse");
    pipeline.AddPass<CanonicalizeAllGatherForCSE>();
    TF_RETURN_IF_ERROR(pipeline.Run(module.get()).status());
    return StatusOr<std::unique_ptr<HloModule>>(std::move(module));
  }
  Status RunPassOnModule(HloModule* module, int64_t distance_threshold = 100) {
    HloPassPipeline pipeline("all-gather-cse");
    pipeline.AddPass<CanonicalizeAllGatherForCSE>();
    TF_RETURN_IF_ERROR(pipeline.Run(module).status());
    return Status::OK();
  }
};

TEST_F(AllGatherCanonicalizeTest, SimpleReshape) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[8]{0} parameter(0)
  resh = s32[1,8]{1,0} reshape(param0)
  ROOT ag = s32[2,8]{1,0} all-gather(resh), replica_groups={{0,1}},
    dimensions={0}, channel_id=0, use_global_device_ids=true
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  const HloInstruction* const reshape =
      module->entry_computation()->root_instruction();
  EXPECT_THAT(reshape,
              AllOf(op::Reshape(op::AllGather(_)), op::Shape("s32[2,8]")));
}

TEST_F(AllGatherCanonicalizeTest, MultipleDegenerateReshapes) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[8]{0} parameter(0)
  resh = s32[1,8]{1,0} reshape(param0)
  resh2 = s32[1,8,1,1]{3,2,1,0} reshape(resh)
  ROOT ag = s32[2,8,1,1]{3,2,1,0} all-gather(resh2), replica_groups={{0,1}},
    dimensions={0}, channel_id=0, use_global_device_ids=true
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  const HloInstruction* const reshape =
      module->entry_computation()->root_instruction();
  EXPECT_THAT(reshape, op::Reshape(op::AllGather(op::Parameter())));
}

TEST_F(AllGatherCanonicalizeTest, MultipleDegenerateReshapes2) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[8]{0} parameter(0)
  resh = s32[8,1,1]{2,1,0} reshape(param0)
  resh2 = s32[1,8,1,1]{3,2,1,0} reshape(resh)
  ROOT ag = s32[2,8,1,1]{3,2,1,0} all-gather(resh2), replica_groups={{0,1}},
    dimensions={0}, channel_id=0, use_global_device_ids=true
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  const HloInstruction* const reshape =
      module->entry_computation()->root_instruction();
  EXPECT_THAT(reshape, op::Reshape(op::AllGather(op::Parameter())));
}

TEST_F(AllGatherCanonicalizeTest, MultipleDegenerateReshapesNoDim0) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[8]{0} parameter(0)
  resh = s32[8,1,1]{2,1,0} reshape(param0)
  resh2 = s32[1,8,1,1]{3,2,1,0} reshape(resh)
  ROOT ag = s32[1,16,1,1]{3,2,1,0} all-gather(resh2), replica_groups={{0,1}},
    dimensions={1}, channel_id=0, use_global_device_ids=true
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  const HloInstruction* const reshape =
      module->entry_computation()->root_instruction();
  EXPECT_THAT(reshape, op::Reshape(op::AllGather(op::Parameter())));
}

TEST_F(AllGatherCanonicalizeTest, NonDegenerateReshape) {
  absl::string_view hlo_string = R"(
HloModule module

ENTRY entry {
  param0 = s32[8]{0} parameter(0)
  resh = s32[8,1,1]{2,1,0} reshape(param0)
  resh2 = s32[1,4,2,1,1]{4,3,2,1,0} reshape(resh)
  ROOT ag = s32[2,4,2,1,1]{4,3,2,1,0} all-gather(resh2), replica_groups={{0,1}},
    dimensions={0}, channel_id=0, use_global_device_ids=true
})";
  auto module_status = RunPass(hlo_string);
  EXPECT_TRUE(module_status.status().ok());
  auto module = module_status.ConsumeValueOrDie();
  const HloInstruction* const reshape =
      module->entry_computation()->root_instruction();
  EXPECT_THAT(reshape, AllOf(op::AllGather(op::Reshape(op::Reshape(_))),
                             op::Shape("s32[2,4,2,1,1]")));
}

}  // namespace
}  // namespace spmd
}  // namespace xla
