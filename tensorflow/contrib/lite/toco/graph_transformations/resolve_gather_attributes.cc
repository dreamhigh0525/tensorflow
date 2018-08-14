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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tensorflow/contrib/lite/toco/graph_transformations/graph_transformations.h"
#include "tensorflow/contrib/lite/toco/model.h"
#include "tensorflow/contrib/lite/toco/tooling_util.h"
#include "tensorflow/core/platform/logging.h"

namespace toco {

bool ResolveGatherAttributes::Run(Model* model, std::size_t op_index) {
  auto* gather_op = model->operators[op_index].get();
  if (gather_op->type != OperatorType::kGather) return false;
  auto* op = static_cast<GatherOperator*>(gather_op);

  if (op->axis) {
    // Attributes already resolved
    return false;
  }
  if (op->inputs.size() != 3) return false;
  if (!IsConstantParameterArray(*model, op->inputs[2])) return false;

  const auto& indices_array = model->GetArray(op->inputs[2]);
  if (!indices_array.has_shape()) return false;
  const auto& axis_data = indices_array.GetBuffer<ArrayDataType::kInt32>().data;
  CHECK_EQ(axis_data.size(), 1)
      << "Multidimensional gather not supported on " << LogName(*op);
  op->axis = {axis_data[0]};

  // Drop the axis array as we no longer need it.
  DeleteArrayIfUsedOnce(op->inputs[2], model);
  op->inputs.resize(2);

  return true;
}

}  // namespace toco
