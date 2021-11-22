/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/mlir/xla/transforms/utils.h"

#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/utils/hlo_utils.h"

namespace mlir {
namespace mhlo {

ConstOp GetScalarConstOfType(Type ty, Location loc, int64_t raw_value,
                             OpBuilder* builder) {
  return builder->create<ConstOp>(loc, hlo::GetScalarOfType(ty, raw_value));
}

DenseIntElementsAttr GetI64ElementsAttr(ArrayAttr attr) {
  RankedTensorType ty =
      RankedTensorType::get(static_cast<int64_t>(attr.size()),
                            IntegerType::get(attr.getContext(), 64));
  return DenseIntElementsAttr::get(ty, attr.getValue());
}

DenseIntElementsAttr GetI64ElementsAttr(ArrayRef<int64_t> values,
                                        Builder* builder) {
  RankedTensorType ty = RankedTensorType::get(
      {static_cast<int64_t>(values.size())}, builder->getIntegerType(64));
  return DenseIntElementsAttr::get(ty, values);
}

}  // namespace mhlo
}  // namespace mlir
