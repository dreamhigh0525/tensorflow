/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/kernels/kernel_runner.h"
#include "tensorflow/lite/micro/testing/micro_test.h"
#include "tensorflow/lite/micro/testing/test_utils.h"

namespace tflite {
namespace testing {
namespace {

void TestNegFloat(std::initializer_list<int> input_dims_data,
                  std::initializer_list<float> input_data,
                  std::initializer_list<float> expected_output_data,
                  std::initializer_list<int> output_dims_data,
                  float* output_data) {
  TfLiteIntArray* input_dims = IntArrayFromInitializer(input_dims_data);
  TfLiteIntArray* output_dims = IntArrayFromInitializer(output_dims_data);
  const int output_dims_count = ElementCount(*output_dims);
  constexpr int inputs_size = 1;
  constexpr int outputs_size = 1;
  constexpr int tensors_size = inputs_size + outputs_size;
  TfLiteTensor tensors[tensors_size] = {
      CreateFloatTensor(input_data, input_dims),
      CreateFloatTensor(output_data, output_dims),
  };

  int inputs_array_data[] = {1, 0};
  TfLiteIntArray* inputs_array = IntArrayFromInts(inputs_array_data);
  int outputs_array_data[] = {1, 1};
  TfLiteIntArray* outputs_array = IntArrayFromInts(outputs_array_data);

  const TfLiteRegistration registration = tflite::ops::micro::Register_NEG();
  micro::KernelRunner runner(registration, tensors, tensors_size, inputs_array,
                             outputs_array,
                             /*builtin_data=*/nullptr, micro_test::reporter);

  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, runner.InitAndPrepare());
  TF_LITE_MICRO_EXPECT_EQ(kTfLiteOk, runner.Invoke());

  TF_LITE_MICRO_EXPECT_EQ(expected_output_data.begin()[0], output_data[0]);
  for (int i = 0; i < output_dims_count; ++i) {
    TF_LITE_MICRO_EXPECT_EQ(expected_output_data.begin()[i], output_data[i]);
  }
}

}  // namespace
}  // namespace testing
}  // namespace tflite

TF_LITE_MICRO_TESTS_BEGIN

TF_LITE_MICRO_TEST(NegOpSingleFloat) {
  float output_data[2];
  tflite::testing::TestNegFloat(/*input_dims_data=*/{1, 2},
                                /*input_data=*/{8.5f, 0.0f},
                                /*expected_output_data=*/{-8.5f, 0.0f},
                                /*output_dims_data*/ {1, 2},
                                /*output_data=*/output_data);
}

TF_LITE_MICRO_TEST(NegOpFloat) {
  float output_data[6];
  tflite::testing::TestNegFloat(/*input_dims_data=*/{2, 2, 3},
                                /*input_data=*/
                                {-2.0f, -1.0f, 0.f, 1.0f, 2.0f, 3.0f},
                                /*expected_output_data=*/
                                {2.0f, 1.0f, -0.f, -1.0f, -2.0f, -3.0f},
                                /*output_dims_data=*/{2, 2, 3},
                                /*output_data=*/output_data);
}

TF_LITE_MICRO_TESTS_END
