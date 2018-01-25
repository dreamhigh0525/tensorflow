/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CORE_KERNELS_DETERMINANT_OP_H_
#define TENSORFLOW_CORE_KERNELS_DETERMINANT_OP_H_

#include "tensorflow/core/framework/tensor_types.h"

namespace tensorflow {
namespace functor {

// Helper functor to compute Determinant from a partially pivoted LU
// factorization.
template <typename Device, typename Scalar>
struct DeterminantFromPivotedLUFunctor {
  void operator()(const Device& device,
                  typename TTypes<Scalar, 3>::ConstTensor lu_factor,
                  const int* pivots, typename TTypes<Scalar, 1>::Tensor output,
                  int* info);
};

// Helper functor to compute sign and log of the absolute value of the
// determinant from a partially pivoted LU factorization.
template <typename Device, typename Scalar>
struct LogDeterminantFromPivotedLUFunctor {
  void operator()(const Device& device,
                  typename TTypes<Scalar, 3>::ConstTensor lu_factor,
                  const int* pivots, typename TTypes<Scalar, 1>::Tensor sign,
                  typename TTypes<Scalar, 1>::Tensor log_abs_det);
};

}  // namespace functor
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_KERNELS_DETERMINANT_OP_H_
