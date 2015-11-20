/* Copyright 2015 Google Inc. All Rights Reserved.

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

#ifndef TENSORFLOW_KERNELS_RELU_OP_H_
#define TENSORFLOW_KERNELS_RELU_OP_H_
// Functor definition for ReluOp and ReluGradOp, must be compilable by nvcc.

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/tensor_types.h"

namespace tensorflow {
namespace functor {

// Functor used by ReluOp to do the computations.
template <typename Device, typename T>
struct Relu {
  // Computes Relu activation.
  //
  // features: any shape.
  // activations: same shape as "features".
  void operator()(const Device& d, typename TTypes<T>::ConstTensor features,
                  typename TTypes<T>::Tensor activations) {
    activations.device(d) = features.cwiseMax(static_cast<T>(0));
  }
};

// Functor used by ReluGradOp to do the computations.
template <typename Device, typename T>
struct ReluGrad {
  // Computes ReluGrad backprops.
  //
  // gradients: gradients backpropagated to the Relu op.
  // features: inputs that where passed to the Relu op.
  // backprops: gradients to backpropagate to the Relu inputs.
  void operator()(const Device& d, typename TTypes<T>::ConstTensor gradients,
                  typename TTypes<T>::ConstTensor features,
                  typename TTypes<T>::Tensor backprops) {
    // NOTE: When the activation is exactly zero, we arbitrarily choose to not
    // propagate the associated gradient value.
    backprops.device(d) =
        gradients * (features > features.constant(static_cast<T>(0)));
  }
};

// Functor used by Relu6Op to do the computations.
template <typename Device, typename T>
struct Relu6 {
  // Computes Relu6 activation.
  //
  // features: any shape.
  // activations: same shape as "features".
  void operator()(const Device& d, typename TTypes<T>::ConstTensor features,
                  typename TTypes<T>::Tensor activations) {
    activations.device(d) =
        features.cwiseMax(static_cast<T>(0)).cwiseMin(static_cast<T>(6));
  }
};

// Functor used by ReluGradOp to do the computations.
template <typename Device, typename T>
struct Relu6Grad {
  // Computes Relu6Grad backprops.
  //
  // gradients: gradients backpropagated to the Relu6 op.
  // features: inputs that where passed to the Relu6 op.
  // backprops: gradients to backpropagate to the Relu6 inputs.
  void operator()(const Device& d, typename TTypes<T>::ConstTensor gradients,
                  typename TTypes<T>::ConstTensor features,
                  typename TTypes<T>::Tensor backprops) {
    // NOTE: When the activation is exactly zero or six, we
    // arbitrarily choose to not propagate the associated gradient
    // value.
    backprops.device(d) = gradients *
                          (features > features.constant(static_cast<T>(0))) *
                          (features < features.constant(static_cast<T>(6)));
  }
};

}  // namespace functor
}  // namespace tensorflow

#endif  // TENSORFLOW_KERNELS_RELU_OP_H_
