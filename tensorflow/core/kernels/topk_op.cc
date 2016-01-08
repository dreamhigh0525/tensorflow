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

// See docs in ../ops/nn_ops.cc.

#define EIGEN_USE_THREADS

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/lib/gtl/top_n.h"
#include "tensorflow/core/public/tensor.h"
#include "tensorflow/core/public/tensor_shape.h"

namespace tensorflow {

template <typename T>
class TopK : public OpKernel {
 public:
  explicit TopK(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("k", &k_));
    OP_REQUIRES_OK(context, context->GetAttr("sorted", &sorted_));

    if (k_ == 1) {
      sorted_ = false;
    }
  }

  void Compute(OpKernelContext* context) override {
    const auto& input_in = context->input(0);
    OP_REQUIRES(context, input_in.dims() == 2,
                errors::InvalidArgument("input must be 2-dimensional"));
    OP_REQUIRES(context, input_in.dim_size(1) >= k_,
                errors::InvalidArgument("input must have at least k columns"));

    const auto& input = input_in.matrix<T>();

    const auto num_rows = input_in.dim_size(0);  // generally batch_size
    const auto num_cols = input_in.dim_size(1);

    Tensor* values_out = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(
                                0, TensorShape({num_rows, k_}), &values_out));
    Tensor* indices_out = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(
                                1, TensorShape({num_rows, k_}), &indices_out));
    auto values = values_out->matrix<T>();
    auto indices = indices_out->matrix<int32>();

    gtl::TopN<std::pair<T, int32>> filter(k_);
    for (int r = 0; r < num_rows; r++) {
      for (int32 c = 0; c < num_cols; ++c) {
        // The second element is the negated index, so that lower-index elements
        // are considered larger than higher-index elements in case of ties.
        filter.push(std::make_pair(input(r, c), -c));
      }

      int32 i = 0;
      if (sorted_) {
        std::unique_ptr<std::vector<std::pair<T, int32>>> top_k(
            filter.Extract());
        for (auto top_k_it = top_k->begin(); top_k_it != top_k->end();
             ++top_k_it, ++i) {
          values(r, i) = top_k_it->first;
          indices(r, i) = -top_k_it->second;
        }
      } else {
        for (auto top_k_it = filter.unsorted_begin();
             top_k_it != filter.unsorted_end(); ++top_k_it, ++i) {
          values(r, i) = top_k_it->first;
          indices(r, i) = -top_k_it->second;
        }
      }
      filter.Reset();
    }
  }

 private:
  int k_;
  bool sorted_;
};

#define REGISTER_KERNELS(type) \
  REGISTER_KERNEL_BUILDER(     \
      Name("TopK").Device(DEVICE_CPU).TypeConstraint<type>("T"), TopK<type>)

TF_CALL_REAL_NUMBER_TYPES(REGISTER_KERNELS);
#undef REGISTER_KERNELS

}  // namespace tensorflow
