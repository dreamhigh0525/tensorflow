/* Copyright 2016 Google Inc. All Rights Reserved.

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

#include "tensorflow/core/kernels/mirror_pad_op.h"

#include <string>

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/mirror_pad_mode.h"

namespace tensorflow {

template <typename Device, typename T>
class MirrorPadOp : public OpKernel {
 public:
  explicit MirrorPadOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("mode", &mode_));
  }

  ~MirrorPadOp() override = default;

  void Compute(OpKernelContext* context) override {
    const Tensor& in0 = context->input(0);
    const Tensor& in1 = context->input(1);
    const int dims = in0.dims();
    constexpr int kMinDims = 0;
    constexpr int kMaxDims = 5;
    OP_REQUIRES(context, kMinDims <= dims && dims <= kMaxDims,
                errors::Unimplemented("inputs rank not in [", kMinDims, ",",
                                      kMaxDims, "]: ", dims));
    OP_REQUIRES(
        context,
        TensorShapeUtils::IsMatrix(in1.shape()) && in1.dim_size(1) == 2,
        errors::InvalidArgument("paddings must be a matrix with 2 columns: ",
                                in1.shape().DebugString()));
    OP_REQUIRES(
        context, dims == in1.dim_size(0),
        errors::InvalidArgument(
            "The first dimension of paddings must be the rank of inputs",
            in1.shape().DebugString(), " ", in0.shape().DebugString()));

    // Compute the shape of the output tensor, and allocate it.
    TensorShape output_shape;
    TTypes<int32>::ConstMatrix paddings = in1.matrix<int32>();
    for (int d = 0; d < dims; ++d) {
      const int32 before_d = paddings(d, 0);  // Pad before existing elements.
      const int32 after_d = paddings(d, 1);   // Pad after exisitng elements.
      OP_REQUIRES(context, before_d >= 0 && after_d >= 0,
                  errors::InvalidArgument("paddings must be non-negative: ",
                                          before_d, " ", after_d));
      int32 max_padding = in0.dim_size(d);
      if (mode_ == MirrorPadMode::REFLECT) {
        max_padding = (in0.dim_size(d) > 0) ? in0.dim_size(d) - 1 : 0;
      }
      OP_REQUIRES(context, before_d <= max_padding && after_d <= max_padding,
                  errors::InvalidArgument(
                      "paddings must be smaller than the dimension size: ",
                      before_d, ", ", after_d, " not less than ", max_padding));

      output_shape.AddDim(before_d + in0.dim_size(d) + after_d);
    }

    if (dims == 0) {
      context->set_output(0, in0);
      return;
    }

    Tensor* output = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &output));

#define MIRROR_PAD_CASE(i)                                                \
  case i: {                                                               \
    functor::MirrorPad<Device, T, i>()(                                   \
        context->eigen_device<Device>(), To32Bit(output->tensor<T, i>()), \
        To32Bit(in0.tensor<T, i>()), paddings, mode_);                    \
    break;                                                                \
  }

    // Invoke the dims-specific implementation.
    switch (dims) {
      MIRROR_PAD_CASE(1)
      MIRROR_PAD_CASE(2)
      MIRROR_PAD_CASE(3)
      MIRROR_PAD_CASE(4)
      MIRROR_PAD_CASE(5)
      default:
        OP_REQUIRES(context, false,
                    errors::InvalidArgument("Unsupported rank: ",
                                            in0.shape().DebugString()));
    }

#undef MIRROR_PAD_CASE
  }

 private:
  MirrorPadMode mode_;
};

using CpuDevice = Eigen::ThreadPoolDevice;
using GpuDevice = Eigen::GpuDevice;

#define REGISTER_KERNEL(type)                            \
  REGISTER_KERNEL_BUILDER(Name("MirrorPad")              \
                              .Device(DEVICE_CPU)        \
                              .TypeConstraint<type>("T") \
                              .HostMemory("paddings"),   \
                          MirrorPadOp<CpuDevice, type>);

TF_CALL_ALL_TYPES(REGISTER_KERNEL);
#undef REGISTER_KERNEL

#if GOOGLE_CUDA
namespace functor {
// Forward declarations of the functor specializations for GPU.
#define DECLARE_GPU_SPEC(T, i)                                               \
  template <>                                                                \
  void MirrorPad<GpuDevice, T, i>::operator()(                               \
      const GpuDevice&, typename TTypes<T, i, int32>::Tensor,                \
      typename TTypes<T, i, int32>::ConstTensor, TTypes<int32>::ConstMatrix, \
      MirrorPadMode);                                                        \
  extern template struct MirrorPad<GpuDevice, T, i>;

#define DECLARE_GPU_SPECS(T) \
  DECLARE_GPU_SPEC(T, 1);    \
  DECLARE_GPU_SPEC(T, 2);    \
  DECLARE_GPU_SPEC(T, 3);    \
  DECLARE_GPU_SPEC(T, 4);    \
  DECLARE_GPU_SPEC(T, 5);

TF_CALL_GPU_NUMBER_TYPES(DECLARE_GPU_SPECS);
#undef DECLARE_GPU_SPECS
#undef DECLARE_GPU_SPEC
}  // namespace functor

// Registration of the GPU implementations.
#define REGISTER_GPU_KERNEL(T)                         \
  REGISTER_KERNEL_BUILDER(Name("MirrorPad")            \
                              .Device(DEVICE_GPU)      \
                              .TypeConstraint<T>("T")  \
                              .HostMemory("paddings"), \
                          MirrorPadOp<GpuDevice, T>)

TF_CALL_GPU_NUMBER_TYPES(REGISTER_GPU_KERNEL);
#undef REGISTER_GPU_KERNEL
#endif  // GOOGLE_CUDA

// Gradient op.
template <typename Device, typename T>
class MirrorPadGradOp : public OpKernel {
 public:
  explicit MirrorPadGradOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("mode", &mode_));
  }

  ~MirrorPadGradOp() override = default;

  void Compute(OpKernelContext* context) override {
    const Tensor& in0 = context->input(0);
    const Tensor& in1 = context->input(1);
    const int dims = in0.dims();
    constexpr int kMinDims = 0;
    constexpr int kMaxDims = 5;
    OP_REQUIRES(context, kMinDims <= dims && dims <= kMaxDims,
                errors::Unimplemented("inputs rank not in [", kMinDims, ",",
                                      kMaxDims, "]: ", dims));
    OP_REQUIRES(
        context,
        TensorShapeUtils::IsMatrix(in1.shape()) && in1.dim_size(1) == 2,
        errors::InvalidArgument("paddings must be a matrix with 2 columns: ",
                                in1.shape().DebugString()));
    OP_REQUIRES(
        context, dims == in1.dim_size(0),
        errors::InvalidArgument(
            "The first dimension of paddings must be the rank of inputs",
            in1.shape().DebugString(), " ", in0.shape().DebugString()));

    // Compute the shape of the output tensor, and allocate it.
    TensorShape output_shape;
    TTypes<int32>::ConstMatrix paddings = in1.matrix<int32>();
    for (int d = 0; d < dims; ++d) {
      const int32 before_d = paddings(d, 0);  // Pad before existing elements.
      const int32 after_d = paddings(d, 1);   // Pad after exisitng elements.
      OP_REQUIRES(context, before_d >= 0 && after_d >= 0,
                  errors::InvalidArgument("Paddings must be non-negative: ",
                                          before_d, ", ", after_d));

      const int32 out_size = in0.dim_size(d) - (before_d + after_d);
      int32 max_padding = out_size;
      if (mode_ == MirrorPadMode::REFLECT) {
        max_padding = (out_size > 0) ? out_size - 1 : 0;
      }

      OP_REQUIRES(
          context, before_d <= max_padding && after_d <= max_padding,
          errors::InvalidArgument(
              "Paddings must be no larger than the output dimension size: ",
              before_d, ", ", after_d, " not less than ", max_padding));

      output_shape.AddDim(out_size);
    }

    if (dims == 0) {
      context->set_output(0, in0);
      return;
    }

    int offset = 0;
    if (mode_ == MirrorPadMode::REFLECT) {
      offset = 1;
    } else {
      OP_REQUIRES(
          context, mode_ == MirrorPadMode::SYMMETRIC,
          errors::InvalidArgument("mode must be either REFLECT or SYMMETRIC."));
    }

    Tensor scratch;
    OP_REQUIRES_OK(context, context->allocate_temp(DataTypeToEnum<T>::value,
                                                   in0.shape(), &scratch));

    Tensor* output = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &output));

#define OP_CASE(k)                                                        \
  case k: {                                                               \
    functor::MirrorPadGrad<Device, T, k>()(                               \
        context->eigen_device<Device>(), To32Bit(output->tensor<T, k>()), \
        To32Bit(in0.tensor<T, k>()), paddings, offset,                    \
        To32Bit(scratch.tensor<T, k>()));                                 \
    break;                                                                \
  }

    // Invoke the dims-specific implementation.
    switch (dims) {
      OP_CASE(1);
      OP_CASE(2);
      OP_CASE(3);
      OP_CASE(4);
      OP_CASE(5);
      default:
        OP_REQUIRES(context, false,
                    errors::InvalidArgument("Unsupported rank: ",
                                            in0.shape().DebugString()));
    }
  }

 private:
  MirrorPadMode mode_;
};

#define REGISTER_KERNEL(type)                            \
  REGISTER_KERNEL_BUILDER(Name("MirrorPadGrad")          \
                              .Device(DEVICE_CPU)        \
                              .TypeConstraint<type>("T") \
                              .HostMemory("paddings"),   \
                          MirrorPadGradOp<CpuDevice, type>);

TF_CALL_ALL_TYPES(REGISTER_KERNEL);
#undef REGISTER_KERNEL

#if GOOGLE_CUDA
namespace functor {
// Forward declarations of the functor specializations for GPU.
#define DECLARE_GPU_SPEC(T, k)                                               \
  template <>                                                                \
  void MirrorPadGrad<GpuDevice, T, k>::operator()(                           \
      const GpuDevice&, typename TTypes<T, k, int32>::Tensor,                \
      typename TTypes<T, k, int32>::ConstTensor, TTypes<int32>::ConstMatrix, \
      int32 offset, typename TTypes<T, k, int32>::Tensor);                   \
  extern template struct MirrorPadGrad<GpuDevice, T, k>;

#define DECLARE_GPU_SPECS(T) \
  DECLARE_GPU_SPEC(T, 1);    \
  DECLARE_GPU_SPEC(T, 2);    \
  DECLARE_GPU_SPEC(T, 3);    \
  DECLARE_GPU_SPEC(T, 4);    \
  DECLARE_GPU_SPEC(T, 5);

TF_CALL_GPU_NUMBER_TYPES(DECLARE_GPU_SPECS);
#undef DECLARE_GPU_SPECS
#undef DECLARE_GPU_SPEC
}  // namespace functor

// Registration of the GPU implementations.
#define REGISTER_GPU_KERNEL(T)                         \
  REGISTER_KERNEL_BUILDER(Name("MirrorPadGrad")        \
                              .Device(DEVICE_GPU)      \
                              .TypeConstraint<T>("T")  \
                              .HostMemory("paddings"), \
                          MirrorPadGradOp<GpuDevice, T>)

TF_CALL_GPU_NUMBER_TYPES(REGISTER_GPU_KERNEL);
#undef REGISTER_GPU_KERNEL
#endif  // GOOGLE_CUDA

}  // namespace tensorflow
