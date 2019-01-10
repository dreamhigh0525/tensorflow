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
#include "tensorflow/lite/tools/optimize/logging_op_resolver.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "tensorflow/lite/mutable_op_resolver.h"

namespace tflite {
namespace optimize {
namespace calibration {
namespace {

TfLiteStatus ConvPrepare(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

TfLiteStatus ConvEval(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

TfLiteStatus AddPrepare(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

TfLiteStatus AddEval(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

TfLiteStatus WrappingInvoke(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

TEST(LoggingOpResolverTest, KernelInvokesAreReplaced) {
  MutableOpResolver base_resolver;
  TfLiteRegistration conv_registration = {
      .prepare = ConvPrepare,
      .invoke = ConvEval,
  };
  base_resolver.AddBuiltin(BuiltinOperator_CONV_2D, &conv_registration);

  TfLiteRegistration add_registration = {
      .prepare = AddPrepare,
      .invoke = AddEval,
  };
  base_resolver.AddBuiltin(BuiltinOperator_ADD, &add_registration);
  BuiltinOpsSet ops_to_replace = {
      {BuiltinOperator_CONV_2D, /*version*/ 1},
      {BuiltinOperator_ADD, /*version*/ 1},
  };

  LoggingOpResolver resolver(ops_to_replace, base_resolver, WrappingInvoke);

  auto reg = resolver.FindOp(BuiltinOperator_CONV_2D, 1);

  EXPECT_EQ(reg->builtin_code, BuiltinOperator_CONV_2D);
  EXPECT_TRUE(reg->prepare == ConvPrepare);
  EXPECT_TRUE(reg->invoke == WrappingInvoke);

  reg = resolver.FindOp(BuiltinOperator_ADD, 1);

  EXPECT_EQ(reg->builtin_code, BuiltinOperator_ADD);
  EXPECT_TRUE(reg->prepare == AddPrepare);
  EXPECT_TRUE(reg->invoke == WrappingInvoke);
}

TEST(LoggingOpResolverTest, OriginalKernelInvokesAreRetained) {
  MutableOpResolver base_resolver;
  TfLiteRegistration conv_registration = {
      .prepare = ConvPrepare,
      .invoke = ConvEval,
  };
  base_resolver.AddBuiltin(BuiltinOperator_CONV_2D, &conv_registration);

  TfLiteRegistration add_registration = {
      .prepare = AddPrepare,
      .invoke = AddEval,
  };
  base_resolver.AddBuiltin(BuiltinOperator_ADD, &add_registration);
  BuiltinOpsSet ops_to_replace = {
      {BuiltinOperator_CONV_2D, /*version*/ 1},
      {BuiltinOperator_ADD, /*version*/ 1},
  };

  LoggingOpResolver resolver(ops_to_replace, base_resolver, WrappingInvoke);
  auto kernel_invoke =
      resolver.GetWrappedKernelInvoke(BuiltinOperator_CONV_2D, 1);
  EXPECT_TRUE(kernel_invoke == ConvEval);
  kernel_invoke = resolver.GetWrappedKernelInvoke(BuiltinOperator_ADD, 1);
  EXPECT_TRUE(kernel_invoke == AddEval);
}

TEST(LoggingOpResolverTest, OnlyOpsInReplacementSetAreReplaces) {
  MutableOpResolver base_resolver;
  TfLiteRegistration conv_registration = {
      .prepare = ConvPrepare,
      .invoke = ConvEval,
  };
  base_resolver.AddBuiltin(BuiltinOperator_CONV_2D, &conv_registration);

  TfLiteRegistration add_registration = {
      .prepare = AddPrepare,
      .invoke = AddEval,
  };
  base_resolver.AddBuiltin(BuiltinOperator_ADD, &add_registration);
  // Only replace conv2d
  BuiltinOpsSet ops_to_replace = {
      {BuiltinOperator_CONV_2D, /*version*/ 1},
  };

  LoggingOpResolver resolver(ops_to_replace, base_resolver, WrappingInvoke);
  auto reg = resolver.FindOp(BuiltinOperator_CONV_2D, 1);
  EXPECT_EQ(reg->builtin_code, BuiltinOperator_CONV_2D);
  EXPECT_TRUE(reg->prepare == ConvPrepare);
  EXPECT_TRUE(reg->invoke == WrappingInvoke);

  reg = resolver.FindOp(BuiltinOperator_ADD, 1);
  EXPECT_EQ(nullptr, reg);
}

}  // namespace
}  // namespace calibration
}  // namespace optimize
}  // namespace tflite

int main(int argc, char** argv) {
  // On Linux, add: FLAGS_logtostderr = true;
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
