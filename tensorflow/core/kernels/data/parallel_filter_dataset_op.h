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
#ifndef TENSORFLOW_CORE_KERNELS_DATA_PARALLEL_FILTER_DATASET_OP_H_
#define TENSORFLOW_CORE_KERNELS_DATA_PARALLEL_FILTER_DATASET_OP_H_

#include "tensorflow/core/data/captured_function.h"
#include "tensorflow/core/data/dataset_utils.h"
#include "tensorflow/core/framework/dataset.h"

namespace tensorflow {
namespace data {

class ParallelFilterDatasetOp : public UnaryDatasetOpKernel {
 public:
  static constexpr const char* const kDatasetType = "ParallelFilter";
  static constexpr const char* const kInputDataset = "input_dataset";
  static constexpr const char* const kOtherArguments = "other_arguments";
  static constexpr const char* const kNumParallelCalls = "num_parallel_calls";
  static constexpr const char* const kPredicate = "predicate";
  static constexpr const char* const kDeterministic = "deterministic";
  static constexpr const char* const kTarguments = "Targuments";
  static constexpr const char* const kOutputTypes = "output_types";
  static constexpr const char* const kOutputShapes = "output_shapes";

  explicit ParallelFilterDatasetOp(OpKernelConstruction* ctx);

 protected:
  void MakeDataset(OpKernelContext* ctx, DatasetBase* input,
                   DatasetBase** output) override;

 private:
  class Dataset;
  DeterminismPolicy deterministic_;
  std::shared_ptr<FunctionMetadata> func_metadata_ = nullptr;
};

}  // namespace data
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_KERNELS_DATA_PARALLEL_FILTER_DATASET_OP_H_
