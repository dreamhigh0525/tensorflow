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

#ifndef TENSORFLOW_SECURITY_FUZZING_CC_CORE_FRAMEWORK_TENSOR_DOMAINS_H_
#define TENSORFLOW_SECURITY_FUZZING_CC_CORE_FRAMEWORK_TENSOR_DOMAINS_H_

#include "fuzztest/fuzztest.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.pb.h"

namespace tensorflow::fuzzing {

/// Returns a fuzztest domain of tensors of the specified shape and datatype
fuzztest::Domain<Tensor> AnyValidTensorOfShapeAndType(const TensorShape& shape,
                                                      DataType datatype);

/// Returns a fuzztest domain of tensors with shape and datatype
/// that live in the given corresponding domains.
fuzztest::Domain<Tensor> AnyValidTensor(
    fuzztest::Domain<TensorShape> tensor_shape_domain,
    fuzztest::Domain<DataType> datatype_domain);

}  // namespace tensorflow::fuzzing

#endif  // TENSORFLOW_SECURITY_FUZZING_CC_CORE_FRAMEWORK_TENSOR_DOMAINS_H_
