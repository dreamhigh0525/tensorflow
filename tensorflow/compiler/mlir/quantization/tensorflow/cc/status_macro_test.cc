/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/compiler/mlir/quantization/tensorflow/cc/status_macro.h"

#include "absl/status/status.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace quantization {
namespace {

using ::testing::Eq;

TEST(TfQuantReturnIfErrorTest, DoesNotReturnIfOk) {
  const auto returned_status = []() -> absl::Status {
    TF_QUANT_RETURN_IF_ERROR(absl::OkStatus());
    return absl::InternalError("Expected");
  }();

  EXPECT_THAT(returned_status.message(), Eq("Expected"));
}

TEST(TfQuantReturnIfErrorTest, ReturnsIfOk) {
  const auto returned_status = []() -> absl::Status {
    TF_QUANT_RETURN_IF_ERROR(absl::InternalError("Expected"));
    return absl::OkStatus();
  }();

  EXPECT_THAT(returned_status.message(), Eq("Expected"));
}

}  // namespace
}  // namespace quantization
}  // namespace tensorflow
