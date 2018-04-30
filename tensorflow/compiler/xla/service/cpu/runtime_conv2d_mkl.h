/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_CPU_RUNTIME_CONV2D_MKL_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_CPU_RUNTIME_CONV2D_MKL_H_

#include <iostream>
#include "tensorflow/core/platform/types.h"

extern "C" {

extern void __xla_cpu_runtime_MKLConvF32(
    const void* /* xla::ExecutableRunOptions* */ run_options_ptr, float* out,
    float* lhs, float* rhs, tensorflow::int64 input_batch,
    tensorflow::int64 input_rows, tensorflow::int64 input_cols,
    tensorflow::int64 input_channels, tensorflow::int64 kernel_rows,
    tensorflow::int64 kernel_cols, tensorflow::int64 kernel_channels,
    tensorflow::int64 kernel_filters, tensorflow::int64 output_rows,
    tensorflow::int64 output_cols, tensorflow::int64 row_stride,
    tensorflow::int64 col_stride, tensorflow::int64 padding_top,
    tensorflow::int64 padding_bottom, tensorflow::int64 padding_left,
    tensorflow::int64 padding_right, tensorflow::int64 lhs_row_dilation,
    tensorflow::int64 lhs_col_dilation, tensorflow::int64 rhs_row_dilation,
    tensorflow::int64 rhs_col_dilation);
}

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_CPU_RUNTIME_CONV2D_MKL_H_
