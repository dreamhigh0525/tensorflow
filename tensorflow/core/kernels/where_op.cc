/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

// See docs in ../ops/array_ops.cc.

#define EIGEN_USE_THREADS

#if GOOGLE_CUDA
#define EIGEN_USE_GPU
#endif  // GOOGLE_CUDA

#include "tensorflow/core/kernels/where_op.h"

#include <memory>
#include <numeric>
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/types.h"

#if GOOGLE_CUDA
#include "tensorflow/core/common_runtime/gpu/gpu_event_mgr.h"
#include "tensorflow/core/kernels/cuda_solvers.h"
#include "tensorflow/core/platform/cuda.h"

using ::perftools::gputools::cuda::ScopedActivateExecutorContext;
#endif  // GOOGLE_CUDA

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

namespace functor {

template <>
struct NumTrue<CPUDevice, int64> {
  static Status Compute(OpKernelContext* ctx, const CPUDevice& d,
                        TTypes<bool>::ConstFlat input,
                        TTypes<int64>::Scalar num_true) {
    *num_true.data() =
        std::accumulate(input.data(), input.data() + input.size(), 0);
    return Status::OK();
  }
};

template <int DIMS, typename TIndex>
struct Where<CPUDevice, DIMS, TIndex> {
  EIGEN_ALWAYS_INLINE static void WriteIndexRowMajor(
      typename TTypes<int64>::Matrix output,
      const typename Eigen::DSizes<TIndex, DIMS>& strides, TIndex true_n,
      TIndex index) {
    for (int i = 0; i < DIMS; ++i) {
      output(true_n, i) = index / strides[i];
      index -= output(true_n, i) * strides[i];
    }
  }

  EIGEN_ALWAYS_INLINE static Status Compute(
      OpKernelContext* ctx, const CPUDevice& d,
      typename TTypes<bool, DIMS>::ConstTensor input,
      typename TTypes<int64>::Matrix output, TIndex* found_true) {
    Eigen::DSizes<Eigen::DenseIndex, DIMS> dims = input.dimensions();
    Eigen::DSizes<TIndex, DIMS> strides;

    EIGEN_STATIC_ASSERT((static_cast<int>(decltype(input)::Layout) ==
                         static_cast<int>(Eigen::RowMajor)),
                        INTERNAL_ERROR_INPUT_SHOULD_BE_ROWMAJOR);

    strides[DIMS - 1] = 1;
    for (int i = DIMS - 2; i >= 0; --i) {
      strides[i] = strides[i + 1] * dims[i + 1];
    }

    Eigen::DenseIndex output_size = output.dimension(0);
    for (Eigen::DenseIndex n = 0; n < input.size(); ++n) {
      if (input.data()[n]) {
        if (FastBoundsCheck(*found_true, output_size)) {
          WriteIndexRowMajor(output, strides, *found_true, n);
        }
        ++*found_true;
      }
    }
    return Status::OK();
  }
};

}  // namespace functor

class WhereCPUOp : public OpKernel {
 public:
  explicit WhereCPUOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* context) override {
    const Tensor& input = context->input(0);

    const int input_dims = input.dims();

    Tensor num_true;
    OP_REQUIRES_OK(
        context, context->allocate_temp(DT_INT64, TensorShape({}), &num_true));
    auto num_true_t = num_true.scalar<int64>();

    Status s = functor::NumTrue<CPUDevice, int64>::Compute(
        context, context->eigen_device<CPUDevice>(), input.flat<bool>(),
        num_true_t);
    OP_REQUIRES_OK(context, s);
    TensorShape output_shape({num_true_t(), input_dims});
    Tensor* output = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &output));

    // TODO(ebrevdo): Replace single-threaded copy with a
    // multithreaded block copy by getting block counts above instead
    // of a global NumTrue, then having each block filled in in
    // separate threads below.
    int64 found_true = 0;

#define HANDLE_DIM(NDIM)                                                   \
  case NDIM: {                                                             \
    Status s = functor::Where<CPUDevice, NDIM, int64>::Compute(            \
        context, context->eigen_device<CPUDevice>(),                       \
        input.tensor<bool, NDIM>(), output->matrix<int64>(), &found_true); \
    OP_REQUIRES_OK(context, s);                                            \
  } break;

    switch (input_dims) {
      HANDLE_DIM(1);
      HANDLE_DIM(2);
      HANDLE_DIM(3);
      HANDLE_DIM(4);
      HANDLE_DIM(5);

      default:
        OP_REQUIRES(context, false,
                    errors::InvalidArgument(
                        "WhereOp : Unhandled input dimensions: ", input_dims));
    }
#undef HANDLE_DIM

    OP_REQUIRES(
        context, found_true == num_true_t(),
        errors::InvalidArgument(
            "WhereOp: Race condition between counting the number of true "
            "elements and writing them.  When counting, saw ",
            num_true_t(), " elements; but when writing their indices, saw ",
            found_true, " elements."));
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(WhereCPUOp);
};

REGISTER_KERNEL_BUILDER(Name("Where").Device(DEVICE_CPU), WhereCPUOp);

#if GOOGLE_CUDA

namespace functor {

#define DECLARE_GPU_NUMTRUE(Tindex)                                            \
  template <>                                                                  \
  Status NumTrue<GPUDevice, Tindex>::Compute(                                  \
      OpKernelContext* ctx, const GPUDevice& d, TTypes<bool>::ConstFlat input, \
      TTypes<Tindex>::Scalar num_true);                                        \
  extern template struct NumTrue<GPUDevice, Tindex>

DECLARE_GPU_NUMTRUE(int32);
DECLARE_GPU_NUMTRUE(int64);
#undef DECLARE_GPU_NUMTRUE

#define DECLARE_GPU_WHERE_INDEX(Dims, Tindex)                     \
  template <>                                                     \
  Status Where<GPUDevice, Dims, Tindex>::Compute(                 \
      OpKernelContext* ctx, const GPUDevice& d,                   \
      typename TTypes<bool, Dims>::ConstTensor input,             \
      typename TTypes<int64>::Matrix output, Tindex* found_true); \
  extern template struct Where<GPUDevice, Dims, Tindex>;
#define DECLARE_GPU_WHERE(Dims)         \
  DECLARE_GPU_WHERE_INDEX(Dims, int32); \
  DECLARE_GPU_WHERE_INDEX(Dims, int64);

DECLARE_GPU_WHERE(1);
DECLARE_GPU_WHERE(2);
DECLARE_GPU_WHERE(3);
DECLARE_GPU_WHERE(4);
DECLARE_GPU_WHERE(5);
#undef DECLARE_GPU_WHERE
#undef DECLARE_GPU_WHERE_INDEX

}  // namespace functor

class WhereGPUOp : public AsyncOpKernel {
 public:
  explicit WhereGPUOp(OpKernelConstruction* context) : AsyncOpKernel(context) {}

  void ComputeAsync(OpKernelContext* context, DoneCallback done) override {
    const Tensor& input = context->input(0);
    const int input_dims = input.dims();

    if (input.NumElements() < std::numeric_limits<int32>::max()) {
      ComputeAsyncType<int32>(input, input_dims, context, done);
    } else {
      ComputeAsyncType<int64>(input, input_dims, context, done);
    }
  }

  template <typename Tindex>
  void ComputeAsyncType(const Tensor& input, const int input_dims,
                        OpKernelContext* context, DoneCallback done) {
    // Step 0: alloc nnz
    // Step 1: call nnz kernel
    // Step 2: copy nnz to host
    // Step 3: call create_output
    // Step 4: call where kernel
    Tensor num_true;
    OP_REQUIRES_OK_ASYNC(context,
                         context->allocate_temp(DataTypeToEnum<Tindex>::v(),
                                                TensorShape({}), &num_true),
                         done);

    auto num_true_t = num_true.scalar<Tindex>();

    perftools::gputools::DeviceMemoryBase num_true_ptr(
        static_cast<void*>(num_true_t.data()));
    // Push kernel to stream to get number of true elements.
    const GPUDevice& d = context->eigen_device<GPUDevice>();
    Status s = functor::NumTrue<GPUDevice, Tindex>::Compute(
        context, d, input.flat<bool>(), num_true_t);
    OP_REQUIRES_OK_ASYNC(context, s, done);

    // Copy num_true to host;
    ScratchSpace<Tindex> num_true_host(context, 1, /* on_host */ true);

    auto stream = context->op_device_context()->stream();
    OP_REQUIRES_ASYNC(
        context,
        stream
            ->ThenMemcpy(num_true_host.mutable_data(), num_true_ptr,
                         sizeof(Tindex))
            .ok(),
        errors::Internal("WhereOp: failed to copy num_true from device"), done);

    auto create_and_check_output = [context, &d, &input, input_dims,
                                    num_true_host, done]() {
      // Ensure that within the callback, the proper GPU settings are
      // configured.
      auto stream = context->op_device_context()->stream();
      ScopedActivateExecutorContext scoped_activation{stream->parent()};

      Tindex num_true = *num_true_host.data();

      // TODO(ebrevdo): Properly copy back found_true value to CPU for
      // validation checking.  Currently Where<GPUDevice>::Compute()
      // does not perform this copy back to CPU.
      Tindex found_true = -1;

      // Step 1: Allocate the output and perform the selection/copy.
      Tensor* output;
      OP_REQUIRES_OK_ASYNC(context,
                           context->allocate_output(
                               0, TensorShape({num_true, input_dims}), &output),
                           done);

#define HANDLE_DIM(NDIM)                                                 \
  case NDIM: {                                                           \
    Status s = functor::Where<GPUDevice, NDIM, Tindex>::Compute(         \
        context, d, input.tensor<bool, NDIM>(), output->matrix<int64>(), \
        &found_true);                                                    \
    OP_REQUIRES_OK_ASYNC(context, s, done);                              \
  } break;

      switch (input_dims) {
        HANDLE_DIM(1);
        HANDLE_DIM(2);
        HANDLE_DIM(3);
        HANDLE_DIM(4);
        HANDLE_DIM(5);

        default:
          OP_REQUIRES_ASYNC(
              context, false,
              errors::InvalidArgument("WhereOp: Unhandled input dimensions: ",
                                      input_dims),
              done);
      }
#undef HANDLE_DIM

      // TODO(ebrevdo): Fix the copy back to host.

      // OP_REQUIRES_ASYNC(
      //     context, found_true == num_true,
      //     errors::InvalidArgument(
      //         "WhereOp: Race condition between counting the number of true "
      //         "elements and writing them.  When counting, saw ",
      //         num_true, " elements; but when writing their indices, saw ",
      //         found_true, " elements."),
      //     done);

      done();
    };
    context->device()->tensorflow_gpu_device_info()->event_mgr->ThenExecute(
        stream, create_and_check_output);
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(WhereGPUOp);
};

REGISTER_KERNEL_BUILDER(Name("Where").Device(DEVICE_GPU), WhereGPUOp);

#endif  // GOOGLE_CUDA

}  // namespace tensorflow
