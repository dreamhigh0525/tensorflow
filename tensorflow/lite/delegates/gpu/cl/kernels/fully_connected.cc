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

#include "tensorflow/lite/delegates/gpu/cl/kernels/fully_connected.h"

#include <string>
#include <utility>

#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"

namespace tflite {
namespace gpu {
namespace cl {

FullyConnected::FullyConnected(const OperationDef& definition,
                               const DeviceInfo& device_info)
    : GPUOperation(definition) {
  if (device_info.IsAdreno()) {
    if (device_info.IsAdreno3xx()) {
      work_group_size_ = int3(8, 4, 1);
    } else if (device_info.IsAdreno4xx()) {
      work_group_size_ = int3(16, 4, 1);
    } else {
      work_group_size_ = int3(32, 4, 1);
    }
  } else if (device_info.IsIntel()) {
    work_group_size_ = int3(8, 4, 1);
  } else {
    work_group_size_ = int3(16, 4, 1);
  }
  code_ = GetFullyConnectedKernelCode(definition_, work_group_size_);
}

FullyConnected::FullyConnected(FullyConnected&& kernel)
    : GPUOperation(std::move(kernel)) {}

FullyConnected& FullyConnected::operator=(FullyConnected&& kernel) {
  if (this != &kernel) {
    GPUOperation::operator=(std::move(kernel));
  }
  return *this;
}

// We split vec vec dot (every thread do vec vec dot product in basic
// vec mat mult) on 4 parts to create more threads
// tid.y thread process every 4-th element in vec vec dot
// Good results for ~1024 x 1024 sizes, for other can be written more
// optimized shaders

std::string FullyConnected::GetFullyConnectedKernelCode(
    const OperationDef& op_def, const int3& work_group_size) {
  AddSrcTensor("src_tensor", op_def.src_tensors[0]);
  AddDstTensor("dst_tensor", op_def.dst_tensors[0]);

  std::string c = GetCommonDefines(op_def.precision);
  switch (op_def.precision) {
    case CalculationsPrecision::F32:
      c += "#define accumulate(a, b, c) c = mad(a, b, c)\n";
      c += "#define FLT16 float16\n";
      break;
    case CalculationsPrecision::F32_F16:
      c += "#define accumulate(a, b, c) c += convert_float4(a * b)\n";
      c += "#define FLT16 half16\n";
      break;
    case CalculationsPrecision::F16:
      c += "#define accumulate(a, b, c) c = mad(a, b, c)\n";
      c += "#define FLT16 half16\n";
      break;
  }

  c += "#define WG_X " + std::to_string(work_group_size.x) + "\n";
  c += "#define WG_Y " + std::to_string(work_group_size.y) + "\n";

  c += R"(__kernel void main_function($0) {
  int gid = get_global_id(0);
  int2 tid = (int2)(get_local_id(0), get_local_id(1));
  ACCUM_FLT4 s = (ACCUM_FLT4)(0.0f);
  if (gid < args.dst_tensor.Slices()) {
    for (int c = tid.y; c < args.src_tensor.Slices(); c += WG_Y) {
      FLT4 v = args.src_tensor.Read(0, 0, c);
      FLT16 w = args.weights.Read(c * args.dst_tensor.Slices() + gid);
      accumulate(v.s0, w.s0123, s);
      accumulate(v.s1, w.s4567, s);
      accumulate(v.s2, w.s89ab, s);
      accumulate(v.s3, w.scdef, s);
    }
  }
  __local ACCUM_FLT4 temp[WG_X][WG_Y];
  temp[tid.x][tid.y] = s;
  barrier(CLK_LOCAL_MEM_FENCE);
  if (gid >= args.dst_tensor.Slices()) {
    return;
  }
  if (tid.y == 0) {
)";
  for (int i = 1; i < work_group_size.y; ++i) {
    c += "    s += temp[tid.x][" + std::to_string(i) + "];\n";
  }
  c += R"(    FLT4 r0 = TO_FLT4(s) + args.biases.Read(gid);
    args.dst_tensor.Write(r0, 0, 0, gid);
  }
})";

  return c;
}

int3 FullyConnected::GetGridSize() const {
  return int3(dst_[0]->Slices(), 1, 1);
}

FullyConnected CreateFullyConnected(const DeviceInfo& device_info,
                                    const OperationDef& definition,
                                    const FullyConnectedAttributes& attr) {
  FullyConnected result(definition, device_info);
  result.UploadWeights(attr.weights);

  TensorLinearDescriptor desc;
  desc.storage_type = LinearStorageType::TEXTURE_2D;
  desc.element_type = definition.GetDataType();
  desc.UploadLinearData(attr.bias);
  result.args_.AddObject(
      "biases", absl::make_unique<TensorLinearDescriptor>(std::move(desc)));

  return result;
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
