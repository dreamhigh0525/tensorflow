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

// This header file defines common utils used when transforming TF ops to
// XLA/Uniform Quantized ops.
#ifndef TENSORFLOW_COMPILER_MLIR_QUANTIZATION_TENSORFLOW_UTILS_TF_TO_XLA_ATTRIBUTE_UTILS_H_
#define TENSORFLOW_COMPILER_MLIR_QUANTIZATION_TENSORFLOW_UTILS_TF_TO_XLA_ATTRIBUTE_UTILS_H_

#include "mlir/IR/Builders.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/quantization/tensorflow/passes/utils.h"

namespace mlir::quant {

// Caclulate padding values for XLA ops.
// Padding values for Uniform Quantized ops can be generated with this method as
// well as it shares the same definition for padding attribute with the XLA ops.
Value CalculatePaddingAndPadIfNeeded(
    OpBuilder &builder, Location loc, Value input, Value filter,
    int8_t input_zp_value, ArrayAttr strides, ArrayAttr dilations,
    StringAttr conv_padding, ArrayAttr explicit_paddings, Value &padding);

}  // namespace mlir::quant

#endif  // TENSORFLOW_COMPILER_MLIR_QUANTIZATION_TENSORFLOW_UTILS_TF_TO_XLA_ATTRIBUTE_UTILS_H_
