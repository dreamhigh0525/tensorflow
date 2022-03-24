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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_CPU_RUNTIME_FFT_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_CPU_RUNTIME_FFT_H_

#include <stdint.h>

extern "C" {

extern void __xla_cpu_runtime_EigenFft(
    const void* /* xla::ExecutableRunOptions* */ run_options_ptr, void* out,
    void* operand, int32_t fft_type, int32_t double_precision, int32_t fft_rank,
    int64_t input_batch, int64_t fft_length0, int64_t fft_length1,
    int64_t fft_length2);

}  // extern "C"

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_CPU_RUNTIME_FFT_H_
