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

#include "tensorflow/lite/delegates/gpu/gl/kernels/quantize_and_dequantize.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "tensorflow/lite/delegates/gpu/common/operations.h"
#include "tensorflow/lite/delegates/gpu/gl/kernels/test_util.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"

using ::testing::FloatNear;
using ::testing::Pointwise;

namespace tflite {
namespace gpu {
namespace gl {
namespace {

TEST(QuantizeAndDequantizeTest, Dim2Bits8) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 3, 2, 1);

  // Unlike TFLite's FakeQuant kernel, we assume that the incoming values are
  // pre-nudged, since this should be done during model conversion.
  const int num_bits = 8;
  const int quant_min = 0;
  const int quant_max = (1 << num_bits) - 1;
  QuantizeAndDequantizeAttributes attr;
  NudgeQuantizationRange(/**original_min**/ 0.0, /**original_max**/ 1.0,
                         quant_min, quant_max, &attr.min, &attr.max,
                         &attr.scale);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 3, 2, 1);

  SingleOpModel model({ToString(OperationType::QUANTIZE_AND_DEQUANTIZE), attr},
                      {input}, {output});
  ASSERT_TRUE(
      model.PopulateTensor(0, {0.0, 1.0, 0.25, 0.50, 0.4444444, 0.00001}));
  ASSERT_OK(model.Invoke(*NewQuantizeAndDequantizeNodeShader()));
  EXPECT_THAT(model.GetOutput(0),
              Pointwise(FloatNear(1e-6),
                        {0.0f, 1.0f, 0.25098f, 0.498039f, 0.443137f, 0.0f}));
}

TEST(QuantizeAndDequantizeTest, Dim3Bits8_NegativeRange) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 3, 1, 2);

  // Unlike TFLite's FakeQuant kernel, we assume that the incoming values are
  // pre-nudged, since this should be done during model conversion.
  const int num_bits = 8;
  const int quant_min = 0;
  const int quant_max = (1 << num_bits) - 1;
  QuantizeAndDequantizeAttributes attr;
  NudgeQuantizationRange(/**original_min**/ -0.9, /**original_max**/ 0.9,
                         quant_min, quant_max, &attr.min, &attr.max,
                         &attr.scale);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 3, 1, 2);

  SingleOpModel model({ToString(OperationType::QUANTIZE_AND_DEQUANTIZE), attr},
                      {input}, {output});
  ASSERT_TRUE(
      model.PopulateTensor(0, {0.0, -0.9, 0.25, 0.50, 0.4444444, -0.00001}));
  ASSERT_OK(model.Invoke(*NewQuantizeAndDequantizeNodeShader()));
  EXPECT_THAT(model.GetOutput(0),
              Pointwise(FloatNear(1e-6), {0.0f, -0.896471f, 0.247059f,
                                          0.501176f, 0.444706f, 0.0f}));
}

TEST(QuantizeAndDequantizeTest, Dim3Bits16) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 3, 1, 2);

  // Unlike TFLite's FakeQuant kernel, we assume that the incoming values are
  // pre-nudged, since this should be done during model conversion.
  const int num_bits = 16;
  const int quant_min = 0;
  const int quant_max = (1 << num_bits) - 1;
  QuantizeAndDequantizeAttributes attr;
  NudgeQuantizationRange(/**original_min**/ 0.0, /**original_max**/ 1.0,
                         quant_min, quant_max, &attr.min, &attr.max,
                         &attr.scale);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 3, 1, 2);

  SingleOpModel model({ToString(OperationType::QUANTIZE_AND_DEQUANTIZE), attr},
                      {input}, {output});
  ASSERT_TRUE(
      model.PopulateTensor(0, {0.0, 1.0, 0.25, 0.50, 0.4444444, 0.00001}));
  ASSERT_OK(model.Invoke(*NewQuantizeAndDequantizeNodeShader()));
  EXPECT_THAT(model.GetOutput(0),
              Pointwise(FloatNear(1e-6), {0.0f, 1.0f, 0.250004f, 0.500008f,
                                          0.44445f, 1.5259e-05f}));
}

TEST(QuantizeAndDequantizeTest, Dim2Bits16_NegativeRange) {
  TensorRef<BHWC> input;
  input.type = DataType::FLOAT32;
  input.ref = 0;
  input.shape = BHWC(1, 3, 2, 1);

  // Unlike TFLite's FakeQuant kernel, we assume that the incoming values are
  // pre-nudged, since this should be done during model conversion.
  const int num_bits = 16;
  const int quant_min = 0;
  const int quant_max = (1 << num_bits) - 1;
  QuantizeAndDequantizeAttributes attr;
  NudgeQuantizationRange(/**original_min**/ -0.9, /**original_max**/ 0.9,
                         quant_min, quant_max, &attr.min, &attr.max,
                         &attr.scale);

  TensorRef<BHWC> output;
  output.type = DataType::FLOAT32;
  output.ref = 1;
  output.shape = BHWC(1, 3, 2, 1);

  SingleOpModel model({ToString(OperationType::QUANTIZE_AND_DEQUANTIZE), attr},
                      {input}, {output});
  ASSERT_TRUE(
      model.PopulateTensor(0, {0.0, -0.9, 0.25, 0.50, 0.4444444, -0.00001}));
  ASSERT_OK(model.Invoke(*NewQuantizeAndDequantizeNodeShader()));
  EXPECT_THAT(model.GetOutput(0),
              Pointwise(FloatNear(1e-6), {0.0f, -0.900014f, 0.249998f,
                                          0.499995f, 0.444431f, 0.0f}));
}

}  // namespace
}  // namespace gl
}  // namespace gpu
}  // namespace tflite
