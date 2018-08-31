/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENTE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONT OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#define EIGEN_USE_CUSTOM_THREAD_POOL
#define EIGEN_USE_THREADS

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/kernels/eigen_benchmark.h"
#include "tensorflow/core/platform/test_benchmark.h"

#define CREATE_THREAD_POOL(threads) \
  Eigen::ThreadPool tp(threads);    \
  Eigen::ThreadPoolDevice device(&tp, threads)

void SpatialConvolution(int iters, int num_threads,
                        /* Input dimensions: */
                        int input_batches, int input_height, int input_width,
                        int input_depth,
                        /* Filter (kernel) dimensions: */
                        int filter_count, int filter_height, int filter_width) {
  ::tensorflow::testing::StopTiming();

  CREATE_THREAD_POOL(num_threads);

  using Benchmark =
      SpatialConvolutionBenchmarksSuite<float, Eigen::ThreadPoolDevice>;
  auto benchmark = Benchmark(iters, device);

  typename Benchmark::Dimensions input_dims(input_batches, input_height,
                                            input_width, input_depth);
  typename Benchmark::Dimensions filter_dims(filter_height, filter_width,
                                             input_depth, filter_count);

  benchmark.SpatialConvolution(input_dims, filter_dims);

  auto output_size = input_dims.TotalSize();
  auto flops = output_size * (input_depth * filter_height * filter_width);
  ::tensorflow::testing::ItemsProcessed(flops * iters);
}

void SpatialConvolutionBackwardInput(int iters, int num_threads,
                                     /* Input dimensions: */
                                     int input_batches, int input_height,
                                     int input_width, int input_depth,
                                     /* Filter (kernel) dimensions: */
                                     int filter_count, int filter_height,
                                     int filter_width) {
  ::tensorflow::testing::StopTiming();

  CREATE_THREAD_POOL(num_threads);

  using Benchmark =
      SpatialConvolutionBenchmarksSuite<float, Eigen::ThreadPoolDevice>;
  auto benchmark = Benchmark(iters, device);

  typename Benchmark::Dimensions input_dims(input_batches, input_height,
                                            input_width, input_depth);
  typename Benchmark::Dimensions filter_dims(filter_height, filter_width,
                                             input_depth, filter_count);

  benchmark.SpatialConvolutionBackwardInput(input_dims, filter_dims);

  auto output_size = input_dims.TotalSize();
  auto flops = output_size * (input_depth * filter_height * filter_width);
  ::tensorflow::testing::ItemsProcessed(flops * iters);
}

// Macro arguments names: --------------------------------------------------- //
//   NT: num threads
//    N: batch size
//    H: height
//    W: width
//    C: channels
//   FC: filter count
//   FH: filter height
//   FW: filter width

#define BM_NAME(prefix, NT, N, H, W, C, FC, FH, FW) \
  BM_##prefix##_CPU_##NT##T_in_##N##_##H##_##W##_##C##_f_##FC##_##FH##_##FW

#define BM_SpatialConvolution(NT, N, H, W, C, FC, FH, FW, LABEL)  \
  static void BM_NAME(SpatialConvolution, NT, N, H, W, C, FC, FH, \
                      FW)(int iters) {                            \
    SpatialConvolution(iters, NT, N, H, W, C, FC, FH, FW);        \
  }                                                               \
  BENCHMARK(BM_NAME(SpatialConvolution, NT, N, H, W, C, FC, FH, FW))

#define BM_SpatialConvolutionBwdInput(NT, N, H, W, C, FC, FH, FW, LABEL)  \
  static void BM_NAME(SpatialConvolutionBwdInput, NT, N, H, W, C, FC, FH, \
                      FW)(int iters) {                                    \
    SpatialConvolutionBackwardInput(iters, NT, N, H, W, C, FC, FH, FW);   \
  }                                                                       \
  BENCHMARK(BM_NAME(SpatialConvolutionBwdInput, NT, N, H, W, C, FC, FH, FW))

#define BM_SpatialConvolutions(N, H, W, C, FC, FH, FW, LABEL) \
  BM_SpatialConvolution(2, N, H, W, C, FC, FH, FW, LABEL);    \
  BM_SpatialConvolution(4, N, H, W, C, FC, FH, FW, LABEL);    \
  BM_SpatialConvolution(8, N, H, W, C, FC, FH, FW, LABEL);    \
  BM_SpatialConvolution(16, N, H, W, C, FC, FH, FW, LABEL);

#define BM_SpatialConvolutionsBwdInput(N, H, W, C, FC, FH, FW, LABEL) \
  BM_SpatialConvolutionBwdInput(2, N, H, W, C, FC, FH, FW, LABEL);    \
  BM_SpatialConvolutionBwdInput(4, N, H, W, C, FC, FH, FW, LABEL);    \
  BM_SpatialConvolutionBwdInput(8, N, H, W, C, FC, FH, FW, LABEL);    \
  BM_SpatialConvolutionBwdInput(16, N, H, W, C, FC, FH, FW, LABEL);

// ImageNet Forward Convolutions -------------------------------------------- //

BM_SpatialConvolutions(32,          // batch size
                       56, 56, 64,  // input: height, width, depth
                       192, 3, 3,   // filter: height, width, count
                       "conv2_00");

BM_SpatialConvolutions(32, 28, 28, 96, 128, 3, 3, "conv3a_00_3x3");
BM_SpatialConvolutions(32, 28, 28, 16, 32, 5, 5, "conv3a_00_5x5");
BM_SpatialConvolutions(32, 28, 28, 128, 192, 3, 3, "conv3_00_3x3");
BM_SpatialConvolutions(32, 28, 28, 32, 96, 5, 5, "conv3_00_5x5");
BM_SpatialConvolutions(32, 14, 14, 96, 204, 3, 3, "conv4a_00_3x3");
BM_SpatialConvolutions(32, 14, 14, 16, 48, 5, 5, "conv4a_00_5x5");
BM_SpatialConvolutions(32, 14, 14, 112, 224, 3, 3, "conv4b_00_3x3");
BM_SpatialConvolutions(32, 14, 14, 24, 64, 5, 5,
                       "conv4b_00_5x5 / conv4c_00_5x5");
BM_SpatialConvolutions(32, 14, 14, 128, 256, 3, 3, "conv4c_00_3x3");
BM_SpatialConvolutions(32, 14, 14, 144, 288, 3, 3, "conv4d_00_3x3");
BM_SpatialConvolutions(32, 14, 14, 32, 64, 5, 5, "conv4d_00_5x5");
BM_SpatialConvolutions(32, 14, 14, 160, 320, 3, 3, "conv4_00_3x3");
BM_SpatialConvolutions(32, 14, 14, 32, 128, 5, 5, "conv4_00_5x5");
BM_SpatialConvolutions(32, 7, 7, 160, 320, 3, 3, "conv5a_00_3x3");
BM_SpatialConvolutions(32, 7, 7, 48, 128, 5, 5, "conv5a_00_5x5 / conv5_00_5x5");
BM_SpatialConvolutions(32, 7, 7, 192, 384, 3, 3, "conv5_00_3x3");

// Benchmarks from https://github.com/soumith/convnet-benchmarks
BM_SpatialConvolutions(128, 128, 128, 3, 96, 11, 11, "convnet-layer1");
BM_SpatialConvolutions(128, 64, 64, 64, 128, 9, 9, "convnet-layer2");
BM_SpatialConvolutions(128, 32, 32, 128, 128, 9, 9, "convnet-layer3");
BM_SpatialConvolutions(128, 16, 16, 128, 128, 7, 7, "convnet-layer4");
BM_SpatialConvolutions(128, 13, 13, 384, 384, 3, 3, "convnet-layer5");

// ImageNet BackwardInput Convolutions -------------------------------------- //

BM_SpatialConvolutionsBwdInput(32, 56, 56, 64, 192, 3, 3, "conv2_00");
BM_SpatialConvolutionsBwdInput(32, 28, 28, 96, 128, 3, 3, "conv3a_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 28, 28, 16, 32, 5, 5, "conv3a_00_5x5");
BM_SpatialConvolutionsBwdInput(32, 28, 28, 128, 192, 3, 3, "conv3_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 28, 28, 32, 96, 5, 5, "conv3_00_5x5");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 96, 204, 3, 3, "conv4a_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 16, 48, 5, 5, "conv4a_00_5x5");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 112, 224, 3, 3, "conv4b_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 24, 64, 5, 5,
                               "conv4b_00_5x5 / conv4c_00_5x5");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 128, 256, 3, 3, "conv4c_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 144, 288, 3, 3, "conv4d_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 32, 64, 5, 5, "conv4d_00_5x5");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 160, 320, 3, 3, "conv4_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 14, 14, 32, 128, 5, 5, "conv4_00_5x5");
BM_SpatialConvolutionsBwdInput(32, 7, 7, 160, 320, 3, 3, "conv5a_00_3x3");
BM_SpatialConvolutionsBwdInput(32, 7, 7, 48, 128, 5, 5,
                               "conv5a_00_5x5 / conv5_00_5x5");
BM_SpatialConvolutionsBwdInput(32, 7, 7, 192, 384, 3, 3, "conv5_00_3x3");
