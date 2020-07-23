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

#include "tensorflow/c/experimental/saved_model/public/saved_model_api.h"

#include <string>
#include <vector>

#include "tensorflow/c/eager/c_api.h"
#include "tensorflow/c/eager/c_api_experimental.h"
#include "tensorflow/c/eager/c_api_test_util.h"
#include "tensorflow/c/experimental/saved_model/public/concrete_function.h"
#include "tensorflow/c/experimental/saved_model/public/tensorhandle_list.h"
#include "tensorflow/c/tf_status.h"
#include "tensorflow/c/tf_tensor.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/stringpiece.h"
#include "tensorflow/core/platform/test.h"

namespace {

constexpr char kTestData[] = "cc/saved_model/testdata";
const char* kServeTag[] = {"serve"};

std::string SavedModelPath(tensorflow::StringPiece saved_model_dir) {
  return tensorflow::io::JoinPath(tensorflow::testing::TensorFlowSrcRoot(),
                                  kTestData, saved_model_dir);
}

// This value parameterized test allows us to test both TFRT
// and non TFRT runtimes.
// https://github.com/google/googletest/blob/dcc92d0ab6c4ce022162a23566d44f673251eee4/googletest/docs/advanced.md#value-parameterized-tests
class CSavedModelAPITest : public ::testing::TestWithParam<bool> {};

TEST_P(CSavedModelAPITest, LoadsSavedModelWithTags) {
  TF_Status* status = TF_NewStatus();
  TFE_ContextOptions* opts = TFE_NewContextOptions();
  bool use_tfrt = GetParam();
  if (use_tfrt) {
    TFE_DeleteContextOptions(opts);
    TF_DeleteStatus(status);
    GTEST_SKIP();  // TODO(chky) : Enable this once TFRT is open sourced.
  }

  TFE_ContextOptionsSetTfrt(opts, use_tfrt);

  TFE_Context* ctx = TFE_NewContext(opts, status);
  ASSERT_EQ(TF_OK, TF_GetCode(status)) << TF_Message(status);
  TFE_DeleteContextOptions(opts);

  std::string model_dir = SavedModelPath("VarsAndArithmeticObjectGraph");

  TF_SavedModel* saved_model =
      TF_LoadSavedModelWithTags(model_dir.c_str(), ctx, kServeTag, 1, status);

  // TODO(bmzhao): Change this to expect TF_OK when loading is implemented.
  // That unblocks writing other tests that require a TF_SavedModel*,
  // like loading a ConcreteFunction. This test at least checks that the
  // C API builds and can be minimally run.
  EXPECT_EQ(TF_GetCode(status), TF_UNIMPLEMENTED);

  TF_DeleteSavedModel(saved_model);
  TF_DeleteStatus(status);
  TFE_DeleteContext(ctx);
}

TEST_P(CSavedModelAPITest, LoadsSavedModel) {
  TF_Status* status = TF_NewStatus();
  TFE_ContextOptions* opts = TFE_NewContextOptions();
  bool use_tfrt = GetParam();
  if (use_tfrt) {
    TFE_DeleteContextOptions(opts);
    TF_DeleteStatus(status);
    GTEST_SKIP();  // TODO(chky) : Enable this once TFRT is open sourced.
  }

  TFE_ContextOptionsSetTfrt(opts, use_tfrt);

  TFE_Context* ctx = TFE_NewContext(opts, status);
  ASSERT_EQ(TF_OK, TF_GetCode(status)) << TF_Message(status);
  TFE_DeleteContextOptions(opts);

  std::string model_dir = SavedModelPath("VarsAndArithmeticObjectGraph");

  TF_SavedModel* saved_model =
      TF_LoadSavedModel(model_dir.c_str(), ctx, status);

  EXPECT_EQ(TF_GetCode(status), TF_OK) << TF_Message(status);
  TF_ConcreteFunction* compute_fn =
      TF_GetSavedModelConcreteFunction(saved_model, "compute", status);
  EXPECT_EQ(TF_GetCode(status), TF_OK) << TF_Message(status);

  TFE_Op* compute_fn_op = TF_ConcreteFunctionGetCallOp(compute_fn, status);
  EXPECT_EQ(TF_GetCode(status), TF_OK) << TF_Message(status);

  const TF_TensorHandleList* captures =
      TF_ConcreteFunctionGetCaptures(compute_fn);

  // TODO(bmzhao): Finish API on FunctionMetadata args, so we know how many
  // inputs + outputs a function has.
  std::vector<TFE_TensorHandle*> compute_fn_inputs;
  TFE_TensorHandle* input_a = TestScalarTensorHandle(ctx, 2.0f);
  TFE_TensorHandle* input_b = TestScalarTensorHandle(ctx, 1.0f);
  compute_fn_inputs.reserve(2 + TF_TensorHandleListSize(captures));
  compute_fn_inputs.push_back(input_a);
  compute_fn_inputs.push_back(input_b);
  for (int i = 0; i < TF_TensorHandleListSize(captures); ++i) {
    compute_fn_inputs.push_back(TF_TensorHandleListGet(captures, i));
  }
  TFE_OpAddInputList(compute_fn_op, compute_fn_inputs.data(),
                     compute_fn_inputs.size(), status);
  EXPECT_EQ(TF_GetCode(status), TF_OK) << TF_Message(status);

  TFE_TensorHandle* compute_fn_outputs[1] = {nullptr};
  int num_retvals = 1;

  TFE_Execute(compute_fn_op, &compute_fn_outputs[0], &num_retvals, status);
  EXPECT_EQ(TF_GetCode(status), TF_OK) << TF_Message(status);

  TF_Tensor* result = TFE_TensorHandleResolve(compute_fn_outputs[0], status);
  EXPECT_EQ(TF_GetCode(status), TF_OK) << TF_Message(status);

  EXPECT_EQ(TF_NumDims(result), 0);
  float output_value = *static_cast<float*>(TF_TensorData(result));
  // (1 + 2) * (2 + 1) / 3 + 5 should be 8
  EXPECT_FLOAT_EQ(output_value, 8.0);

  TF_DeleteTensor(result);
  TFE_DeleteTensorHandle(compute_fn_outputs[0]);
  TFE_DeleteTensorHandle(input_a);
  TFE_DeleteTensorHandle(input_b);
  TFE_DeleteOp(compute_fn_op);
  TF_DeleteSavedModel(saved_model);
  TF_DeleteStatus(status);
  TFE_DeleteContext(ctx);
}

INSTANTIATE_TEST_SUITE_P(RuntimeAgnosticSavedModelTests, CSavedModelAPITest,
                         ::testing::Bool());

}  // namespace
