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

#include "tensorflow/core/kernels/cwise_ops_common.h"

namespace tensorflow {
REGISTER8(BinaryOp, CPU, "BitwiseXor", functor::bitwise_xor, int8, int16, int32,
          int64, uint8, uint16, uint32, uint64);


#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
#if !defined(MLIR_GENERATED_GPU_KERNELS_ENABLED) || \
    !defined(MLIR_GENERATED_EXPERIMENTAL_KERNELS_ENABLED)
REGISTER8(BinaryOp, GPU, "BitwiseXor", functor::bitwise_xor, int8, int16, int32,
          int64, uint8, uint16, uint32, uint64);
#else
// TODO(b/172804967): We do not generate unsigned kernels for GPU via mlir.
REGISTER4(BinaryOp, GPU, "BitwiseXor", functor::bitwise_xor, uint8, uint16,
          uint32, uint64);
#endif  // !MLIR_GENERATED_GPU_KERNELS_ENABLED ||
        // !MLIR_GENERATED_EXPERIMENTAL_KERNELS_ENABLED
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

}  // namespace tensorflow
