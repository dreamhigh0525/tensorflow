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
==============================================================================
*/

// This header declares the class CudaSolver, which contains wrappers of linear
// algebra solvers in the cuBlas and cuSolverDN libraries for use in TensorFlow
// kernels.

#ifdef GOOGLE_CUDA

#include <functional>
#include <vector>

#include "cuda/include/cublas_v2.h"
#include "cuda/include/cusolverDn.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/stream_executor.h"

namespace tensorflow {

// Type traits to get CUDA complex types from std::complex<T>.
template <typename T>
struct CUDAComplexT {
  typedef T type;
};
template <>
struct CUDAComplexT<std::complex<float>> {
  typedef cuComplex type;
};
template <>
struct CUDAComplexT<std::complex<double>> {
  typedef cuDoubleComplex type;
};
// Converts pointers of std::complex<> to pointers of
// cuComplex/cuDoubleComplex. No type conversion for non-complex types.
template <typename T>
inline const typename CUDAComplexT<T>::type* CUDAComplex(const T* p) {
  return reinterpret_cast<const typename CUDAComplexT<T>::type*>(p);
}
template <typename T>
inline typename CUDAComplexT<T>::type* CUDAComplex(T* p) {
  return reinterpret_cast<typename CUDAComplexT<T>::type*>(p);
}

// Template to give the Cublas adjoint operation for real and complex types.
template <typename T>
cublasOperation_t CublasAdjointOp() {
  return Eigen::NumTraits<T>::IsComplex ? CUBLAS_OP_C : CUBLAS_OP_T;
}

// Container of LAPACK info data (an array of int) generated on-device by
// a CudaSolver call. One or more such objects can be passed to
// CudaSolver::CopyLapackInfoToHostAsync() along with a callback to
// check the LAPACK info data after the corresponding kernels
// finish and LAPACK info has been copied from the device to the host.
class DeviceLapackInfo;

// Host-side copy of LAPACK info.
class HostLapackInfo;

// The CudaSolver class provides a simplified templated API for the dense linear
// solvers implemented in cuSolverDN (http://docs.nvidia.com/cuda/cusolver) and
// cuBlas (http://docs.nvidia.com/cuda/cublas/#blas-like-extension/).
// An object of this class wraps static cuSolver and cuBlas instances,
// and will launch Cuda kernels on the stream wrapped by the GPU device
// in the OpKernelContext provided to the constructor.
//
// Notice: All the computational member functions are asynchronous and simply
// launch one or more Cuda kernels on the Cuda stream wrapped by the CudaSolver
// object. To check the final status of the kernels run, call
// CopyLapackInfoToHostAsync() on the CudaSolver object to set a callback that
// will be invoked with the status of the kernels launched thus far as
// arguments.
//
// Example of an asynchronous TensorFlow kernel using CudaSolver:
//
// template <typename Scalar>
// class SymmetricPositiveDefiniteSolveOpGpu : public AsyncOpKernel {
//  public:
//   explicit SymmetricPositiveDefiniteSolveOpGpu(OpKernelConstruction* context)
//       : AsyncOpKernel(context) { }
//   void ComputeAsync(OpKernelContext* context, DoneCallback done) final {
//     // 1. Set up input and output device ptrs. See, e.g.,
//     // matrix_inverse_op.cc for a full example.
//     ...
//
//     // 2. Initialize the solver object.
//     CudaSolver solver(context);
//
//     // 3. Launch the two compute kernels back to back on the stream without
//     // synchronizing.
//     std::vector<DeviceLapackInfo> dev_info;
//     const int batch_size = 1;
//     dev_info.emplace_back(context, batch_size, "potrf");
//     // Compute the Cholesky decomposition of the input matrix.
//     OP_REQUIRES_OK_ASYNC(context,
//                          solver.Potrf(uplo, n, dev_matrix_ptrs, n,
//                                       dev_info.back().mutable_data()),
//                          done);
//     dev_info.emplace_back(context, batch_size, "potrs");
//     // Use the Cholesky decomposition of the input matrix to solve A X = RHS.
//     OP_REQUIRES_OK_ASYNC(context,
//                          solver.Potrs(uplo, n, nrhs, dev_matrix_ptrs, n,
//                                       dev_output_ptrs, ldrhs,
//                                       dev_info.back().mutable_data()),
//                          done);
//
//     // 4. Check the status after the computation finishes and call done.
//     // Capture dev_info so the underlying buffers don't get deallocated
//     // before the kernels run.
//     auto check_status = [context, done, dev_info](const Status& status,
//       const std::vector<HostLapackInfo>& /* unused */) {
//           // In this example we don't care about the exact cause of
//           // death, so just check status.
//           OP_REQUIRES_OK_ASYNC(context, status, done);
//           done();
//     };
//     OP_REQUIRES_OK_ASYNC(context,
//                          solver.CopyLapackInfoToHostAsync(
//                            dev_info, std::move(check_status));
//                          done);
//   }
// };

class CudaSolver {
 public:
  // This object stores a pointer to context, which must outlive it.
  explicit CudaSolver(OpKernelContext* context);
  virtual ~CudaSolver() {}

  // Launches a memcpy of solver status data specified by dev_lapack_info from
  // device to the host, and asynchronously invokes the given callback when the
  // copy is complete. The first Status argument to the callback will be
  // Status::OK if all lapack infos retrieved are zero, otherwise an error
  // status is given. The second argument contains a host-side copy of the
  // entire set of infos retrieved, and can be used for generating detailed
  // error messages.
  Status CopyLapackInfoToHostAsync(
      const std::vector<DeviceLapackInfo>& dev_lapack_info,
      std::function<void(const Status&, const std::vector<HostLapackInfo>&)>
          info_checker_callback) const TF_MUST_USE_RESULT;

  // ====================================================================
  // Wrappers for cuSolverDN and cuBlas solvers start here.
  //
  // Apart from capitalization of the first letter, the method names below map
  // to those in cuSolverDN and cuBlas, which follow the naming convention in
  // LAPACK see, e.g., http://docs.nvidia.com/cuda/cusolver/#naming-convention

  // This function performs the matrix-matrix addition/transposition
  //   C = alpha * op(A) + beta * op(B).
  // Returns Status::OK() if the kernel was launched successfully.  See:
  // http://docs.nvidia.com/cuda/cublas/index.html#cublas-lt-t-gt-geam
  // NOTE(ebrevdo): Does not support in-place transpose of non-square matrices.
  template <typename Scalar>
  Status Geam(cublasOperation_t transa, cublasOperation_t transb, int m, int n,
              const Scalar* alpha, /* host or device pointer */
              const Scalar* A, int lda,
              const Scalar* beta, /* host or device pointer */
              const Scalar* B, int ldb, Scalar* C,
              int ldc) const TF_MUST_USE_RESULT;

  // Computes the Cholesky factorization A = L * L^T for a single matrix.
  // Returns Status::OK() if the kernel was launched successfully. See:
  // http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-potrf
  template <typename Scalar>
  Status Potrf(cublasFillMode_t uplo, int n, Scalar* dev_A, int lda,
               int* dev_lapack_info) const TF_MUST_USE_RESULT;

  // LU factorization.
  // Computes LU factorization with partial pivoting P * A = L * U.
  // See: http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-getrf
  template <typename Scalar>
  Status Getrf(int m, int n, Scalar* dev_A, int lda, int* dev_pivots,
               int* dev_lapack_info) const TF_MUST_USE_RESULT;

  // Uses LU factorization to solve A * X = B.
  // See: http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-getrs
  template <typename Scalar>
  Status Getrs(cublasOperation_t trans, int n, int nrhs, const Scalar* A,
               int lda, const int* pivots, Scalar* B, int ldb,
               int* dev_lapack_info) const TF_MUST_USE_RESULT;

  // Computes partially pivoted LU factorizations for a batch of small matrices.
  // Returns Status::OK() if the kernel was launched successfully.See:
  // http://docs.nvidia.com/cuda/cublas/index.html#cublas-lt-t-gt-getrfbatched
  template <typename Scalar>
  Status GetrfBatched(int n, const Scalar* host_a_dev_ptrs[], int lda,
                      int* dev_pivots, DeviceLapackInfo* dev_lapack_info,
                      int batch_size) const TF_MUST_USE_RESULT;

  // Batched linear solver using LU factorization from getrfBatched.
  // See:
  // http://docs.nvidia.com/cuda/cublas/index.html#cublas-lt-t-gt-getrsbatched
  template <typename Scalar>
  Status GetrsBatched(cublasOperation_t trans, int n, int nrhs,
                      const Scalar* dev_Aarray[], int lda, const int* devIpiv,
                      const Scalar* dev_Barray[], int ldb,
                      DeviceLapackInfo* dev_lapack_info,
                      int batch_size) const TF_MUST_USE_RESULT;

  // Computes matrix inverses for a batch of small matrices. Uses the outputs
  // from GetrfBatched. Returns Status::OK() if the kernel was launched
  // successfully. See:
  // http://docs.nvidia.com/cuda/cublas/index.html#cublas-lt-t-gt-getribatched
  template <typename Scalar>
  Status GetriBatched(int n, const Scalar* host_a_dev_ptrs[], int lda,
                      const int* dev_pivots,
                      const Scalar* host_a_inverse_dev_ptrs[], int ldainv,
                      DeviceLapackInfo* dev_lapack_info,
                      int batch_size) const TF_MUST_USE_RESULT;

  // Computes matrix inverses for a batch of small matrices with size n < 32.
  // Returns Status::OK() if the kernel was launched successfully. See:
  // http://docs.nvidia.com/cuda/cublas/index.html#cublas-lt-t-gt-matinvbatched
  template <typename Scalar>
  Status MatInvBatched(int n, const Scalar* host_a_dev_ptrs[], int lda,
                       const Scalar* host_a_inverse_dev_ptrs[], int ldainv,
                       DeviceLapackInfo* dev_lapack_info,
                       int batch_size) const TF_MUST_USE_RESULT;

  // QR factorization.
  // Computes QR factorization A = Q * R.
  // Returns Status::OK() if the kernel was launched successfully.
  // See: http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-geqrf
  template <typename Scalar>
  Status Geqrf(int m, int n, Scalar* dev_A, int lda, Scalar* dev_tau,
               int* dev_lapack_info) const TF_MUST_USE_RESULT;

  // Overwrite matrix C by product of C and Householder matrix Q. The
  // Householder matrix Q is represented by the output from Geqrf in dev_a and
  // dev_tau.
  // Notice: If Scalar is real, only trans=CUBLAS_OP_N or trans=CUBLAS_OP_T is
  // supported. If Scalar is complex, trans=CUBLAS_OP_N or trans=CUBLAS_OP_C is
  // supported.
  // Returns Status::OK() if the kernel was launched successfully.
  // See: http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-ormqr
  template <typename Scalar>
  Status Ormqr(cublasSideMode_t side, cublasOperation_t trans, int m, int n,
               int k, const Scalar* dev_a, int lda, const Scalar* dev_tau,
               Scalar* dev_c, int ldc,
               int* dev_lapack_info) const TF_MUST_USE_RESULT;

  // Overwrites QR factorization produced by Geqrf by Householder matrix Q.
  // On input, the Householder matrix Q is represented by the output from Geqrf
  // in dev_a and dev_tau. On output, dev_a is overwritten with the first n
  // columns of Q.
  // Requires m >= n >= 0.
  // Returns Status::OK() if the kernel was launched successfully.
  // See: http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-orgqr
  template <typename Scalar>
  Status Orgqr(int m, int n, int k, Scalar* dev_a, int lda,
               const Scalar* dev_tau,
               int* dev_lapack_info) const TF_MUST_USE_RESULT;

  // Singular value decomposition.
  // Returns Status::OK() if the kernel was launched successfully.
  // TODO(rmlarsen, volunteers): Add support for complex types.
  // See: http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-gesvd
  template <typename Scalar>
  Status Gesvd(signed char jobu, signed char jobvt, int m, int n, Scalar* dev_A,
               int lda, Scalar* dev_S, Scalar* dev_U, int ldu, Scalar* dev_VT,
               int ldvt, int* dev_lapack_info) const TF_MUST_USE_RESULT;

  /*
  TODO(rmlarsen, volunteers): Implement the kernels below.

  // Symmetric/Hermitian Eigen decomposition.
  // See: http://docs.nvidia.com/cuda/cusolver/#cuds-lt-t-gt-syevd
  template <typename Scalar>
  Status Syevd(cusolverEigMode_t jobz, cublasFillMode_t uplo, int n, Scalar*
  dev_A, int lda, Scalar* dev_W, int* dev_lapack_info) const TF_MUST_USE_RESULT;
  */

 private:
  OpKernelContext* context_;  // not owned.
  cudaStream_t cuda_stream_;
  cusolverDnHandle_t cusolver_dn_handle_;
  cublasHandle_t cublas_handle_;

  TF_DISALLOW_COPY_AND_ASSIGN(CudaSolver);
};

// Helper class to allocate scratch memory and keep track of debug info.
// Mostly a thin wrapper around Tensor.
template <typename Scalar>
class ScratchSpace {
 public:
  ScratchSpace(OpKernelContext* context, int size, bool on_host)
      : ScratchSpace(context, size, "", on_host) {}

  ScratchSpace(OpKernelContext* context, int size, const string& debug_info,
               bool on_host)
      : context_(context), debug_info_(debug_info), on_host_(on_host) {
    AllocatorAttributes alloc_attr;
    if (on_host) {
      // Allocate pinned memory on the host to avoid unnecessary
      // synchronization.
      alloc_attr.set_on_host(true);
      alloc_attr.set_gpu_compatible(true);
    }
    TF_CHECK_OK(context->allocate_temp(DataTypeToEnum<Scalar>::value,
                                       TensorShape({size}), &scratch_tensor_,
                                       alloc_attr));
  }

  virtual ~ScratchSpace() {}

  Scalar* mutable_data() {
    return scratch_tensor_.template flat<Scalar>().data();
  }
  const Scalar* data() const {
    return scratch_tensor_.template flat<Scalar>().data();
  }
  Scalar operator[](int64 i) const {
    return scratch_tensor_.template flat<Scalar>().data()[i];
  }
  int64 bytes() const { return scratch_tensor_.TotalBytes(); }
  int64 size() const { return scratch_tensor_.NumElements(); }
  const string& debug_info() const { return debug_info_; }

  Tensor& tensor() { return scratch_tensor_; }
  const Tensor& tensor() const { return scratch_tensor_; }

  // Returns true if this ScratchSpace is in host memory.
  bool on_host() const { return on_host_; }

 protected:
  OpKernelContext* context() const { return context_; }

 private:
  OpKernelContext* context_;  // not owned
  const string debug_info_;
  const bool on_host_;
  Tensor scratch_tensor_;
};

class HostLapackInfo : public ScratchSpace<int> {
 public:
  HostLapackInfo(OpKernelContext* context, int size, const string& debug_info)
      : ScratchSpace<int>(context, size, debug_info, /* on_host */ true){};
};

class DeviceLapackInfo : public ScratchSpace<int> {
 public:
  DeviceLapackInfo(OpKernelContext* context, int size, const string& debug_info)
      : ScratchSpace<int>(context, size, debug_info, /* on_host */ false) {}

  // Allocates a new scratch space on the host and launches a copy of the
  // contents of *this to the new scratch space. Sets success to true if
  // the copy kernel was launched successfully.
  HostLapackInfo CopyToHost(bool* success) const {
    CHECK(success != nullptr);
    HostLapackInfo copy(context(), size(), debug_info());
    auto stream = context()->op_device_context()->stream();
    perftools::gputools::DeviceMemoryBase wrapped_src(
        static_cast<void*>(const_cast<int*>(this->data())));
    *success =
        stream->ThenMemcpy(copy.mutable_data(), wrapped_src, this->bytes())
            .ok();
    return copy;
  }
};

namespace functor {
// Helper functor to transpose and conjugate all matrices in a flattened batch.
template <typename Device, typename Scalar>
struct AdjointBatchFunctor {
  // We assume that the tensor sizes are correct.
  void operator()(const Device& device,
                  typename TTypes<Scalar, 3>::ConstTensor input,
                  typename TTypes<Scalar, 3>::Tensor output);
};

template <typename Device, typename Scalar>
struct ConjugateFunctor {
  // We assume that the tensor sizes are correct.
  void operator()(const Device& device, typename TTypes<Scalar>::ConstVec input,
                  typename TTypes<Scalar>::Vec output);
};

// Helper functor to compute the product of diagonal elements in all matrices
// in a flattened batch.
template <typename Device, typename Scalar>
struct DeterminantFromPivotedLUFunctor {
  void operator()(const Device& device,
                  typename TTypes<Scalar, 3>::ConstTensor lu_factor,
                  const int* pivots, typename TTypes<Scalar, 1>::Tensor output,
                  int* info);
};

// Helper functor to set a batch of matrices to the identity.
// TODO(rmlarsen): Use this kernel to replace the horribly inefficient tf.eye
// op.
template <typename Device, typename Scalar>
struct EyeFunctor {
  void operator()(const Device& device,
                  typename TTypes<Scalar, 3>::Tensor matrix_batch);
};

}  // namespace functor

}  // namespace tensorflow

#endif  // GOOGLE_CUDA
