/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/delegates/gpu/common/tasks/mean_stddev_normalization.h"

#include <algorithm>
#include <map>
#include <string>

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/common/util.h"

namespace tflite {
namespace gpu {

namespace {

std::string GetReduceCode(const std::string& src_value,
                          const std::string& dst_value, int3 work_group_size,
                          bool two_step) {
  int reduction_size = work_group_size.z;
  std::string mem_name = work_group_size.x * work_group_size.y != 1
                             ? "shared_mem[LOCAL_ID_1][LOCAL_ID_0]"
                             : "shared_mem";
  if (reduction_size <= 8) {
    std::string result;
    result += "  {  // reduction\n";
    result += "    " + mem_name + "[local_id] = " + src_value + ";\n";
    result += "    LOCAL_MEM_BARRIER;\n";
    result += "    " + dst_value + " = " + mem_name + "[0];\n";
    for (int i = 1; i < reduction_size; ++i) {
      result += "    " + dst_value + " += " + mem_name + "[" +
                std::to_string(i) + "];\n";
    }
    if (two_step) {
      result += "    LOCAL_MEM_BARRIER;\n";
    }
    result += "  }\n";
    return result;
  } else {
    // In the reduction step add upper half of the still-to-be-summed vector to
    // the lower half, while taking care of odd sizes and rounding. E.g.:
    // Number of items still to be summed before: 5
    // Local memory before: [a, b, c, d, e];
    // Local memory after: [a+d, b+e, c, d, e];
    // Threads doing work: id < 2 = floor(5/2)
    // Offset to the added items: 3 = ceil(5/2)
    // Number of items still to be summed after: 3 = ceil(5/2)
    return absl::Substitute(R"(
  {  // reduction, all threads inside workgroup must execute this code
    $3[local_id] = $1;
    LOCAL_MEM_BARRIER;
    // The number of items still need to be summed
    int reduction_size = $0;
    while (reduction_size > 1) {
      int active_thread_limit = reduction_size / 2;
      int offset = (reduction_size + 1) / 2;
      if (local_id < active_thread_limit) {
        $1 += $3[local_id + offset];
        $3[local_id] = $1;
      }
      LOCAL_MEM_BARRIER;
      reduction_size = offset;
    }
    $2 = $3[0];
  }
)",
                            reduction_size, src_value, dst_value, mem_name);
  }
}

std::string ZeroClampVec4Code(const std::string& slice_name,
                              const std::string& channels_name,
                              const std::string& value_name) {
  return absl::Substitute(R"(
    // no need to check first element, always valid
    if ($0 * 4 + 1 >= $1) { $2.y = 0.0f; }
    if ($0 * 4 + 2 >= $1) { $2.z = 0.0f; }
    if ($0 * 4 + 3 >= $1) { $2.w = 0.0f; }
)",
                          slice_name, channels_name, value_name);
}
}  // namespace

MeanStdDevNormalization::MeanStdDevNormalization(const OperationDef& definition,
                                                 const GpuInfo& gpu_info,
                                                 const BHWC& shape,
                                                 float variance_bias,
                                                 bool two_step)
    : GPUOperation(definition) {
  const int tensor_slices = DivideRoundUp(shape.c, 4);
  int desired_work_group_size = gpu_info.GetMaxWorkGroupSizeForZ();
  if (gpu_info.IsMali()) {
    // Don't use more than 64 work items per work group on ARM Mali. They
    // implement local memory using the global memory, larger workgroups have
    // severe performance penalty.
    desired_work_group_size = 64;
  }
  if (gpu_info.IsAdreno()) {
    AdrenoInfo info = gpu_info.adreno_info;
    desired_work_group_size = 256;
    if (info.IsAdreno3xx()) {
      if (info.adreno_gpu == AdrenoGpu::kAdreno320 ||
          info.adreno_gpu == AdrenoGpu::kAdreno330) {
        desired_work_group_size = 128;
      } else {
        desired_work_group_size = 64;
      }
    } else if (info.IsAdreno4xx()) {
      if (info.adreno_gpu == AdrenoGpu::kAdreno430) {
        desired_work_group_size = 256;
      } else {
        desired_work_group_size = 128;
      }
    } else if (info.IsAdreno5xx()) {
      if (info.adreno_gpu == AdrenoGpu::kAdreno530 ||
          info.adreno_gpu == AdrenoGpu::kAdreno540) {
        desired_work_group_size = 256;
      } else {
        desired_work_group_size = 128;
      }
    }
  }
  if (gpu_info.IsPowerVR()) {
    desired_work_group_size = 64;
  }
  if (gpu_info.IsApple()) {
    desired_work_group_size = 64;
  }
  if (gpu_info.IsAMD()) {
    desired_work_group_size = 512;
  }
  if (shape.w * shape.h == 1) {
    desired_work_group_size =
        std::min(desired_work_group_size, gpu_info.GetMaxWorkGroupSizeForZ());
    while (desired_work_group_size >= tensor_slices * 2) {
      desired_work_group_size /= 2;
    }
    work_group_size_.x = 1;
    work_group_size_.y = 1;
    work_group_size_.z = desired_work_group_size;
  } else {
    if (tensor_slices >= 16) {
      work_group_size_.z = 8;
    } else if (tensor_slices >= 10) {
      work_group_size_.z = 4;
    } else {
      std::map<int, int> slices_to_group_size = {
          {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 3},
          {6, 3}, {7, 4}, {8, 4}, {9, 3},
      };
      work_group_size_.z = slices_to_group_size[tensor_slices];
    }
    desired_work_group_size =
        std::min(desired_work_group_size, gpu_info.GetMaxWorkGroupTotalSize());
    work_group_size_.x = 1;
    work_group_size_.y =
        desired_work_group_size / AlignByN(work_group_size_.z, 4);
    while (work_group_size_.y > work_group_size_.x) {
      work_group_size_.y /= 2;
      work_group_size_.x *= 2;
    }
  }
  args_.AddFloat("variance_bias", variance_bias);
  args_.AddFloat("inv_ch_count", 1.0f / shape.c);
  code_ = GetNormalizationCode(gpu_info, shape.c % 4 == 0, two_step);
}

std::string MeanStdDevNormalization::GetNormalizationCode(
    const GpuInfo& gpu_info, bool channels_x4, bool two_step) {
  AddSrcTensor("src_tensor", definition_.src_tensors[0]);
  AddDstTensor("dst_tensor", definition_.dst_tensors[0]);

  std::string c;
  if (gpu_info.IsApiOpenCl()) {
    c += "__attribute__((reqd_work_group_size(" +
         std::to_string(work_group_size_.x) + ", " +
         std::to_string(work_group_size_.y) + ", " +
         std::to_string(work_group_size_.z) + ")))\n";
  }
  c += "MAIN_FUNCTION($0) {\n";
  std::string accum_type = two_step ? "float" : "float2";
  if (work_group_size_.x * work_group_size_.y == 1) {
    c += "__local " + accum_type + " shared_mem[" +
         std::to_string(work_group_size_.z) + "];\n";
  } else {
    c += "__local " + accum_type + " shared_mem[" +
         std::to_string(work_group_size_.x) + "][" +
         std::to_string(work_group_size_.y) + "][" +
         std::to_string(work_group_size_.z) + "];\n";
  }
  if (definition_.dst_tensors[0].HasAxis(Axis::BATCH)) {
    c += "  int linear_id = GLOBAL_ID_0;\n";
    c += "  int X = linear_id / args.dst_tensor.Batch();\n";
    c += "  int B = linear_id % args.dst_tensor.Batch();\n";
    c += "  args.src_tensor.SetBatchRef(B);\n";
    c += "  args.dst_tensor.SetBatchRef(B);\n";
  } else {
    c += "  int X = GLOBAL_ID_0;\n";
  }
  c += "  int Y = GLOBAL_ID_1;\n";
  if (!two_step) {
    c += "  float4 private_sum4_sq = INIT_FLOAT4(0.0f);\n";
  }
  c += R"(
  float4 private_sum4 = INIT_FLOAT4(0.0f);
  int local_id = LOCAL_ID_2;
  int reduction_group_size = GROUP_SIZE_2;
  for (int S = local_id; S < args.src_tensor.Slices(); S += reduction_group_size) {
    int x_clamped = min(X, args.src_tensor.Width() - 1);
    int y_clamped = min(Y, args.src_tensor.Height() - 1);
    float4 t = args.src_tensor.Read<float>(x_clamped, y_clamped, S);)";
  if (!channels_x4) {
    c += ZeroClampVec4Code("S", "args.src_tensor.Channels()", "t");
  }
  if (two_step) {
    c += "    private_sum4 += t;\n";
    c += "  }\n";
    c += "  float private_sum = dot(private_sum4, INIT_FLOAT4(1.0f));\n";
    c += "  float sum;\n";
  } else {
    c += "    private_sum4 += t;\n";
    c += "    private_sum4_sq += t * t;\n";
    c += "  }\n";
    c += "  float2 private_sum;\n";
    c += "  private_sum.x = dot(private_sum4, INIT_FLOAT4(1.0f));\n";
    c += "  private_sum.y = dot(private_sum4_sq, INIT_FLOAT4(1.0f));\n";
    c += "  float2 sum;\n";
  }
  c += GetReduceCode("private_sum", "sum", work_group_size_, two_step);
  if (two_step) {
    c += R"(
  // Calculate the mean
  float mean = sum * args.inv_ch_count;
  // Calculate the squared sum of the difference from the mean.
  float4 private_sum_diff_sq4 = INIT_FLOAT4(0.0f);
  for (int S = local_id; S < args.src_tensor.Slices(); S += reduction_group_size) {
    int x_clamped = min(X, args.src_tensor.Width() - 1);
    int y_clamped = min(Y, args.src_tensor.Height() - 1);
    float4 t = args.src_tensor.Read<float>(x_clamped, y_clamped, S);
    float4 diff = t - mean;)";
    if (!channels_x4) {
      c += ZeroClampVec4Code("S", "args.src_tensor.Channels()", "diff");
    }
    c += R"(
    private_sum_diff_sq4 += diff * diff;
  }
  // Reduce
  float private_sum_diff_sq = dot(private_sum_diff_sq4, INIT_FLOAT4(1.0f));
  float sum_diff_sq;
)";
    c += GetReduceCode("private_sum_diff_sq", "sum_diff_sq", work_group_size_,
                       two_step);
    c += "  float variance = sum_diff_sq * args.inv_ch_count;\n";
  } else {
    c += "  float mean = sum.x * args.inv_ch_count;\n";
    c += "  float mean_sq = sum.y * args.inv_ch_count;\n";
    c += "  float variance = mean_sq - mean * mean;\n";
  }
  c += R"(
  // no more shared memory usage, 'useless' threads can exit now
  if (X >= args.dst_tensor.Width()) { return; }
  if (Y >= args.dst_tensor.Height()) { return; }
  // Calculate 1/stddev (with the 'regulazing constant' as in tensor_utils.cc)
  float stddev_inv = rsqrt(variance + args.variance_bias);
  // Calculate (t-mean)/stddev for each element
  for (int S = local_id; S < args.src_tensor.Slices(); S += reduction_group_size) {
    float4 t = args.src_tensor.Read<float>(X, Y, S);
    FLT4 result = TO_FLT4((t - mean) * stddev_inv);
    args.dst_tensor.Write(result, X, Y, S);
  }
})";
  return c;
}

int3 MeanStdDevNormalization::GetGridSize() const {
  // To avoid dealing with global reductions, we restrict the grid size to the
  // work group size in the first dimension.
  const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
  const int grid_y = dst_[0]->Height();
  const int grid_z = work_group_size_.z;
  return int3(grid_x, grid_y, grid_z);
}

MeanStdDevNormalization CreateMeanStdDevNormalization(
    const OperationDef& definition, const GpuInfo& gpu_info, const BHWC& shape,
    float variance_bias, bool two_step) {
  return MeanStdDevNormalization(definition, gpu_info, shape, variance_bias,
                                 two_step);
}

}  // namespace gpu
}  // namespace tflite
