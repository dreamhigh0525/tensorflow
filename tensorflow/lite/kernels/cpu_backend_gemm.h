/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_LITE_KERNELS_CPU_BACKEND_GEMM_H_
#define TENSORFLOW_LITE_KERNELS_CPU_BACKEND_GEMM_H_

#include <cstdint>

#include "tensorflow/lite/kernels/cpu_backend_context.h"
#include "tensorflow/lite/kernels/cpu_backend_gemm_params.h"
#include "tensorflow/lite/kernels/cpu_backend_gemm_ruy.h"

#ifndef TFLITE_WITH_RUY
#include "tensorflow/lite/kernels/cpu_backend_gemm_eigen.h"
#include "tensorflow/lite/kernels/cpu_backend_gemm_gemmlowp.h"
#endif

namespace tflite {

namespace cpu_backend_gemm {

/* Generic implementation using ruy.
 * Non-ruy implementation will be partial specializations of this template.
 */

template <typename LhsScalar, typename RhsScalar, typename AccumScalar,
          typename DstScalar>
struct GemmImpl
    : detail::GemmImplUsingRuy<LhsScalar, RhsScalar, AccumScalar, DstScalar> {};

#ifndef TFLITE_WITH_RUY

/* Specializations using gemmlowp */

template <typename SrcScalar, typename DstScalar>
struct GemmImpl<SrcScalar, SrcScalar, std::int32_t, DstScalar>
    : detail::GemmImplUsingGemmlowp<SrcScalar, SrcScalar, std::int32_t,
                                    DstScalar> {};

// When SrcScalar=int8 or DstScalar=int8, gemmlowp fails to compile
// outside of NEON. We avoid the compilation failure by subspecializing these
// cases, rerouting it back to ruy.
#ifndef GEMMLOWP_NEON
template <typename SrcScalar>
struct GemmImpl<SrcScalar, SrcScalar, std::int32_t, std::int8_t>
    : detail::GemmImplUsingRuy<SrcScalar, SrcScalar, std::int32_t,
                               std::int8_t> {};

template <typename DstScalar>
struct GemmImpl<std::int8_t, std::int8_t, std::int32_t, DstScalar>
    : detail::GemmImplUsingRuy<std::int8_t, std::int8_t, std::int32_t,
                               DstScalar> {};

template <>
struct GemmImpl<std::int8_t, std::int8_t, std::int32_t, std::int8_t>
    : detail::GemmImplUsingRuy<std::int8_t, std::int8_t, std::int32_t,
                               std::int8_t> {};
#endif  // not GEMMLOWP_NEON

/* Specializations using Eigen */

template <>
struct GemmImpl<float, float, float, float>
    : detail::GemmImplUsingEigen<float, float, float, float> {};

#endif  // not TFLITE_WITH_RUY

/* Public entry point */

template <typename LhsScalar, typename RhsScalar, typename AccumScalar,
          typename DstScalar>
void Gemm(const MatrixParams<LhsScalar>& lhs_params, const LhsScalar* lhs_data,
          const MatrixParams<RhsScalar>& rhs_params, const RhsScalar* rhs_data,
          const MatrixParams<DstScalar>& dst_params, DstScalar* dst_data,
          const GemmParams<AccumScalar, DstScalar>& params,
          CpuBackendContext* context) {
  ValidateParams(lhs_params, rhs_params, dst_params, params);
  GemmImpl<LhsScalar, RhsScalar, AccumScalar, DstScalar>::Run(
      lhs_params, lhs_data, rhs_params, rhs_data, dst_params, dst_data, params,
      context);
}

}  // namespace cpu_backend_gemm

}  // namespace tflite

#endif  // TENSORFLOW_LITE_KERNELS_CPU_BACKEND_GEMM_H_
