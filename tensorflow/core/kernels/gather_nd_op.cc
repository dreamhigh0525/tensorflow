/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#include <atomic>

#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/kernels/gather_nd_op.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mem.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/util.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

template <typename Device, typename T, typename Index>
class GatherNdOp : public OpKernel {
 public:
  explicit GatherNdOp(OpKernelConstruction* c) : OpKernel(c) {
    const DataType dt = DataTypeToEnum<T>::v();
    const DataType index_t = DataTypeToEnum<Index>::v();
    OP_REQUIRES_OK(c, c->MatchSignature({dt, index_t}, {dt}));
  }

  void Compute(OpKernelContext* c) override {
    const Tensor& params = c->input(0);
    const Tensor& indices = c->input(1);
    OP_REQUIRES(c, TensorShapeUtils::IsVectorOrHigher(params.shape()),
                errors::InvalidArgument("params must be at least a vector"));
    OP_REQUIRES(c, TensorShapeUtils::IsVectorOrHigher(indices.shape()),
                errors::InvalidArgument("indices must be at least a vector"));
    OP_REQUIRES(
        c, indices.dim_size(indices.dims() - 1) <= params.dims(),
        errors::InvalidArgument(
            "index innermost dimension length must be <= params rank; saw: ",
            indices.dim_size(indices.dims() - 1), " vs. ", params.dims()));

    TensorShape indices_shape(indices.shape());
    const int64 indices_nd = indices_shape.dim_size(indices_shape.dims() - 1);

    // Check that we have enough index space
    int64 N_big = 1;
    for (int i = 0; i < indices_shape.dims() - 1; ++i) {
      N_big *= indices_shape.dim_size(i);
    }
    OP_REQUIRES(c, N_big <= std::numeric_limits<int>::max(),
                errors::InvalidArgument(
                    "indices has too many elements for int indexing: ", N_big,
                    " > ", std::numeric_limits<int>::max()));
    OP_REQUIRES(
        c, params.NumElements() <= std::numeric_limits<Index>::max(),
        errors::InvalidArgument("params.NumElements() too large for ",
                                DataTypeString(DataTypeToEnum<Index>::v()),
                                " indexing: ", params.NumElements(), " > ",
                                std::numeric_limits<Index>::max()));

    // The result shape is
    //   indices.shape[:-1] + params.shape[indices.shape[-1]:]
    Index N_result = 1;
    for (int i = 0; i < indices_shape.dims() - 1; ++i) {
      N_result *= indices_shape.dim_size(i);
    }

    TensorShape params_shape(params.shape());
    Index total_nd = params_shape.dims();

    TensorShape result_shape(indices_shape);
    result_shape.RemoveDim(result_shape.dims() - 1);

    int64 slice_size_big = 1;
    for (Index i = indices_nd; i < total_nd; ++i) {
      slice_size_big *= params_shape.dim_size(i);
      result_shape.AddDim(params_shape.dim_size(i));
    }

    OP_REQUIRES(c, slice_size_big <= std::numeric_limits<Index>::max(),
                errors::InvalidArgument(
                    "slice size is too large for indexing: ", slice_size_big,
                    " > ", std::numeric_limits<Index>::max()));

    const Index slice_size = static_cast<Index>(slice_size_big);

    Tensor* out = nullptr;
    OP_REQUIRES_OK(c, c->allocate_output(0, result_shape, &out));
    if (N_result > 0) {
      OP_REQUIRES(c, params_shape.num_elements() > 0,
                  errors::InvalidArgument("Requested more than 0 entries, but "
                                          "params is empty.  Params shape: ",
                                          params_shape.DebugString()));

      auto indices_mat = indices.flat_inner_dims<Index>();

      Index bad_i = -1;

      // Request to copy slices / subtensors
      // Make out a matrix with the slices the col size.
      auto out_mat = out->shaped<T, 2>({N_result, slice_size});
      Tensor scratch;
      OP_REQUIRES_OK(c, c->allocate_temp(DT_INT32, TensorShape(), &scratch));
      auto scratch_scalar = scratch.scalar<int32>();

      switch (indices_nd) {
#define PARAMS_CASE(IXDIM)                                              \
  case IXDIM: {                                                         \
    functor::GatherNdSlice<Device, T, Index, IXDIM> func;               \
    auto params_flat = params.flat_outer_dims<T, IXDIM + 1>();          \
    bad_i = func(c->eigen_device<Device>(), slice_size, scratch_scalar, \
                 params_flat, indices_mat, out_mat);                    \
  } break
        PARAMS_CASE(0);
        PARAMS_CASE(1);
        PARAMS_CASE(2);
        PARAMS_CASE(3);
        PARAMS_CASE(4);
        PARAMS_CASE(5);
#undef PARAMS_CASE
        default:
          OP_REQUIRES(c, false,
                      errors::InvalidArgument(
                          "Only indices.shape[-1] values between 1 and 5 "
                          "are currently supported.  Requested rank: ",
                          indices_nd));
      }

      // bad_i will only return >= 0 on CPUs right now.
      OP_REQUIRES(c, bad_i < 0,
                  errors::InvalidArgument(
                      "flat indices[", bad_i, ", :] = [",
                      str_util::Join(gtl::ArraySlice<Index>(
                                         &indices_mat(bad_i, 0), indices_nd),
                                     ", "),
                      "] does not index into param (shape: ",
                      params.shape().DebugString(), ")."));
    }
  }
};

// Specialization of GatherNdSlice to CPU
namespace generator {

template <typename T, typename Index, int IXDIM>
class GatherNdSliceGenerator {
 public:
  EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE GatherNdSliceGenerator(
      const Index slice_size, typename TTypes<Index>::ConstMatrix Tindices,
      typename TTypes<T, IXDIM + 1>::ConstTensor Tparams,
      typename TTypes<T>::Matrix Tout, std::atomic<Index>* error_loc)
      : slice_size_(slice_size),
        Tindices_(Tindices),
        Tparams_(Tparams),
        Tout_(Tout),
        error_loc_(error_loc) {}

  EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE bool GenerateIndices(
      const Index loc, Eigen::array<Eigen::DenseIndex, IXDIM + 1>* ix) const {
    (*ix)[IXDIM] = 0;
    bool out_of_bounds = false;
    for (int i = 0; i < IXDIM; ++i) {
      const Index ix_i = internal::SubtleMustCopy(Tindices_(loc, i));
      (*ix)[i] = ix_i;
      out_of_bounds |= !FastBoundsCheck(ix_i, Tparams_.dimension(i));
    }
    return out_of_bounds;
  }

  EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE int32
  operator()(const Eigen::array<Eigen::DenseIndex, 1>& loc_array) const {
    const Index loc = loc_array[0];
    Eigen::array<Eigen::DenseIndex, IXDIM + 1> ix;
    Eigen::array<Eigen::DenseIndex, 2> ix_out;
    ix_out[0] = loc;
    ix_out[1] = 0;
    const bool out_of_bounds = GenerateIndices(loc, &ix);
    if (TF_PREDICT_FALSE(out_of_bounds)) {
      error_loc_->store(loc);
      std::fill_n(&Tout_(ix_out), slice_size_, T());
    } else {
      std::copy_n(&Tparams_(ix), slice_size_, &Tout_(ix_out));
    }

    return static_cast<int32>(0);  // Return something...
  }

 private:
  const Index slice_size_;
  const typename TTypes<Index>::ConstMatrix Tindices_;
  const typename TTypes<T, IXDIM + 1>::ConstTensor Tparams_;
  mutable typename TTypes<T>::Matrix Tout_;
  std::atomic<Index>* error_loc_;
};

template <typename T, typename Index, int IXDIM>
class GatherNdElementGenerator {
 public:
  EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE
  GatherNdElementGenerator(typename TTypes<Index>::ConstMatrix Tindices,
                           typename TTypes<T, IXDIM + 1>::ConstTensor Tparams,
                           std::atomic<Index>* error_loc)
      : Tindices_(Tindices), Tparams_(Tparams), error_loc_(error_loc) {}

  EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE bool GenerateIndices(
      const Index loc, Eigen::array<Eigen::DenseIndex, IXDIM + 1>* ix) const {
    (*ix)[IXDIM] = 0;
    bool out_of_bounds = false;
    for (int i = 0; i < IXDIM; ++i) {
      const Index ix_i = internal::SubtleMustCopy(Tindices_(loc, i));
      (*ix)[i] = ix_i;
      out_of_bounds |= !FastBoundsCheck(ix_i, Tparams_.dimension(i));
    }
    return out_of_bounds;
  }

  EIGEN_DEVICE_FUNC EIGEN_ALWAYS_INLINE T
  operator()(const Eigen::array<Eigen::DenseIndex, 2>& loc_array) const {
    const Index loc = loc_array[0];
    Eigen::array<Eigen::DenseIndex, IXDIM + 1> ix;
    const bool out_of_bounds = GenerateIndices(loc, &ix);
    if (TF_PREDICT_FALSE(out_of_bounds)) {
      error_loc_->store(loc);
      return T();
    } else {
      return Tparams_(ix);
    }
  }

 private:
  const typename TTypes<Index>::ConstMatrix Tindices_;
  const typename TTypes<T, IXDIM + 1>::ConstTensor Tparams_;
  std::atomic<Index>* error_loc_;
};

}  // namespace generator

namespace functor {

template <typename T, typename Index, int IXDIM>
struct GatherNdSlice<CPUDevice, T, Index, IXDIM> {
  Index operator()(const CPUDevice& d, const Index slice_size,
                   typename TTypes<int32>::Scalar Tscratch,
                   typename TTypes<T, IXDIM + 1>::ConstTensor Tparams,
                   typename TTypes<Index>::ConstMatrix Tindices,
                   typename TTypes<T>::Matrix Tout) {
    std::atomic<Index> error_loc(-1);

    switch (Tout.dimension(1)) {
      case IXDIM: {
        generator::GatherNdElementGenerator<T, Index, IXDIM>
            gather_element_generator(Tindices, Tparams, &error_loc);
        Tout.device(d) = Tout.generate(gather_element_generator);
      } break;
      default: {
        const Eigen::DenseIndex batch_size = Tindices.dimension(0);
#if !defined(EIGEN_HAS_INDEX_LIST)
        Eigen::Tensor<Eigen::DenseIndex, 1>::Dimensions reshape_dims{{ 1 }};
        Eigen::array<Eigen::DenseIndex, 1> broadcast_dims{{ batch_size }};
#else
        Eigen::IndexList<Eigen::type2index<1> > reshape_dims;
        Eigen::IndexList<Eigen::DenseIndex> broadcast_dims;
        broadcast_dims.set(0, batch_size);
#endif
        generator::GatherNdSliceGenerator<T, Index, IXDIM> gather_nd_generator(
            slice_size, Tindices, Tparams, Tout, &error_loc);
        Tscratch.device(d) = Tscratch.reshape(reshape_dims)
                                 .broadcast(broadcast_dims)
                                 .generate(gather_nd_generator)
                                 .sum();
      }
    }

    // error_loc() returns -1 if there's no out-of-bounds index,
    // otherwise it returns the location of an OOB index in Tindices.
    return error_loc.load();
  }
};

}  // namespace functor

#define REGISTER_GATHER_ND_FULL(dev, type, index_type)                 \
  REGISTER_KERNEL_BUILDER(Name("GatherNd")                             \
                              .Device(DEVICE_##dev)                    \
                              .TypeConstraint<type>("Tparams")         \
                              .TypeConstraint<index_type>("Tindices"), \
                          GatherNdOp<dev##Device, type, index_type>)

#define REGISTER_GATHER_ND_ALL_INDICES(dev, type) \
  REGISTER_GATHER_ND_FULL(dev, type, int32);      \
  REGISTER_GATHER_ND_FULL(dev, type, int64)

#define REGISTER_GATHER_ND_CPU(type) REGISTER_GATHER_ND_ALL_INDICES(CPU, type)

TF_CALL_ALL_TYPES(REGISTER_GATHER_ND_CPU);

#undef REGISTER_GATHER_ND_CPU

#if GOOGLE_CUDA
// Forward declarations of the functor specializations for GPU.
namespace functor {
#define DECLARE_GPU_SPECS_INDEX_NDIM(T, Index, NDIM)          \
  template <>                                                 \
  Index GatherNdSlice<GPUDevice, T, Index, NDIM>::operator()( \
      const GPUDevice& d, const Index slice_size,             \
      typename TTypes<int32>::Scalar Tscratch,                \
      typename TTypes<T, NDIM + 1>::ConstTensor Tparams,      \
      typename TTypes<Index>::ConstMatrix Tindices,           \
      typename TTypes<T>::Matrix Tout);                       \
  extern template struct GatherNdSlice<GPUDevice, T, Index, NDIM>;

#define DECLARE_GPU_SPECS_INDEX(T, Index)    \
  DECLARE_GPU_SPECS_INDEX_NDIM(T, Index, 0); \
  DECLARE_GPU_SPECS_INDEX_NDIM(T, Index, 1); \
  DECLARE_GPU_SPECS_INDEX_NDIM(T, Index, 2); \
  DECLARE_GPU_SPECS_INDEX_NDIM(T, Index, 3); \
  DECLARE_GPU_SPECS_INDEX_NDIM(T, Index, 4); \
  DECLARE_GPU_SPECS_INDEX_NDIM(T, Index, 5)

#define DECLARE_GPU_SPECS(T)         \
  DECLARE_GPU_SPECS_INDEX(T, int32); \
  DECLARE_GPU_SPECS_INDEX(T, int64)

TF_CALL_GPU_NUMBER_TYPES(DECLARE_GPU_SPECS);

#undef DECLARE_GPU_SPECS
#undef DECLARE_GPU_SPECS_INDEX
}  // namespace functor

// Registration of the GPU implementations.
#define REGISTER_GATHER_ND_GPU(type) REGISTER_GATHER_ND_ALL_INDICES(GPU, type)

TF_CALL_GPU_NUMBER_TYPES(REGISTER_GATHER_ND_GPU);

#undef REGISTER_GATHER_ND_GPU

#endif  // GOOGLE_CUDA

#undef REGISTER_GATHER_ND_ALL_INDICES
#undef REGISTER_GATHER_ND_FULL

}  // namespace tensorflow
