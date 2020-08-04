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
      c += "#define FLT16 float16\n";
      break;
    case CalculationsPrecision::F32_F16:
    case CalculationsPrecision::F16:
      c += "#define FLT16 half16\n";
      break;
  }

  const std::string wg_x = std::to_string(work_group_size.x);
  const std::string wg_y = std::to_string(work_group_size.y);
  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  c += "  int gid = get_global_id(0);\n";
  c += "  bool inside = gid < args.dst_tensor.Slices();\n";
  c += "  gid = min(gid, args.dst_tensor.Slices() - 1);\n";
  c += "  int2 tid = (int2)(get_local_id(0), get_local_id(1));\n";
  c += "  ACCUM_FLT4 s = (ACCUM_FLT4)(0.0f);\n";
  c += "  for (uint c = tid.y; c < args.src_tensor.Slices(); c += " + wg_y +
       ") {\n";
  c += "    FLT4 v = args.src_tensor.Read(0, 0, c);\n";
  c += "    FLT16 w = args.weights.Read(c * args.dst_tensor.Slices() + gid);\n";
  c += "    s.x += dot(v, w.s0123);\n";
  c += "    s.y += dot(v, w.s4567);\n";
  c += "    s.z += dot(v, w.s89ab);\n";
  c += "    s.w += dot(v, w.scdef);\n";
  c += "  }\n";
  c += "  __local ACCUM_FLT4 temp[" + wg_x + "][" + wg_y + "];\n";
  c += "  temp[tid.x][tid.y] = s;\n";
  c += "  barrier(CLK_LOCAL_MEM_FENCE);\n";
  c += "  if (tid.y == 0 && inside) {\n";
  for (int i = 1; i < work_group_size.y; ++i) {
    c += "    s += temp[tid.x][" + std::to_string(i) + "];\n";
  }
  c += "    FLT4 r0 = TO_FLT4(s) + args.biases.Read(gid);\n";
  c += "    args.dst_tensor.Write(r0, 0, 0, gid);\n";
  c += "  }\n";
  c += "}\n";

  return c;
}

int3 FullyConnected::GetGridSize() const {
  return int3(dst_[0]->Slices(), 1, 1);
}

absl::Status CreateFullyConnected(const CreationContext& creation_context,
                                  const OperationDef& definition,
                                  const FullyConnectedAttributes& attr,
                                  FullyConnected* result) {
  *result = FullyConnected(definition, creation_context.device->GetInfo());
  RETURN_IF_ERROR(
      result->UploadWeights(attr.weights, creation_context.context));

  TensorLinearDescriptor desc;
  desc.storage_type = LinearStorageType::TEXTURE_2D;
  desc.element_type = definition.GetDataType();

  LinearStorage lt;
  RETURN_IF_ERROR(
      CreateLinearStorage(desc, attr.bias, creation_context.context, &lt));
  result->args_.AddObject("biases", AccessType::READ,
                          absl::make_unique<LinearStorage>(std::move(lt)),
                          absl::make_unique<TensorLinearDescriptor>(desc));

  return absl::OkStatus();
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
