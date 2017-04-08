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

// Declarations for 1D FFT routines in third_party/fft2d/fft.

#ifndef THIRD_PARTY_FFT2D_FFT_H__
#define THIRD_PARTY_FFT2D_FFT_H__

#ifdef __cplusplus
extern "C" {
#endif

extern void cdft(int, int, double *, int *, double *);
extern void rdft(int, int, double *, int *, double *);
extern void ddct(int, int, double *, int *, double *);
extern void ddst(int, int, double *, int *, double *);
extern void dfct(int, double *, double *, int *, double *);
extern void dfst(int, double *, double *, int *, double *);

#ifdef __cplusplus
}
#endif

#endif  // THIRD_PARTY_FFT2D_FFT_H__
