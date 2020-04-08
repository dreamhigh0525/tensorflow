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

#include "tensorflow/lite/delegates/gpu/cl/kernels/strided_slice.h"

#include <string>

#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"

namespace tflite {
namespace gpu {
namespace cl {
namespace {

std::string GetStridedSliceCode(
    const OperationDef& op_def, bool alignedx4,
    const std::vector<ElementwiseOperation*>& linked_operations) {
  TensorCodeGenerator src_tensor(
      "src_data",
      WHSBPoint{"src_size.x", "src_size.y", "src_size.z", "src_size.w"},
      op_def.src_tensors[0]);
  TensorCodeGenerator dst_tensor(
      "dst_data",
      WHSBPoint{"dst_size.x", "dst_size.y", "dst_size.z", "dst_size.w"},
      op_def.dst_tensors[0]);

  const std::string dst_batch = op_def.IsBatchSupported() ? "B" : "";
  std::string c = GetCommonDefines(op_def.precision);
  c += "__kernel void main_function(\n";
  c += src_tensor.GetDeclaration(AccessType::READ);
  c += GetArgsDeclaration(linked_operations);
  c += dst_tensor.GetDeclaration(AccessType::WRITE) + ",\n";
  c += "    int4 offset,            \n";
  c += "    int4 stride,            \n";
  c += "    int4 src_size,             \n";
  c += "    int4 dst_size              \n";
  c += ") {\n";
  if (op_def.IsBatchSupported()) {
    c += "  int linear_id = get_global_id(0);\n";
    c += "  int X = linear_id / dst_size.w;\n";
    c += "  int B = linear_id % dst_size.w;\n";
  } else {
    c += "  int X = get_global_id(0);\n";
  }
  c += "  int Y = get_global_id(1);\n";
  c += "  int Z = get_global_id(2);\n";
  c += "  if (X >= dst_size.x || Y >= dst_size.y || Z >= dst_size.z) { \n";
  c += "    return; \n";
  c += "  } \n";
  c += "  int s_x = X * stride.x + offset.x;\n";
  c += "  int s_y = Y * stride.y + offset.y;\n";
  if (op_def.IsBatchSupported()) {
    c += "  int s_b = B * stride.w + offset.w;\n";
  }
  const std::string src_batch = op_def.IsBatchSupported() ? "s_b" : "";
  if (alignedx4) {
    c += "  int s_z = Z + offset.z;\n";
    c += "  FLT4 result = " +
         src_tensor.ReadWHSB("s_x", "s_y", "s_z", src_batch) + ";\n";
  } else {
    c += "  FLT4 result;\n";
    const std::string postfixes[] = {"x", "y", "z", "w"};
    for (int i = 0; i < 4; ++i) {
      c += "  {\n";
      const std::string channel = "(Z * 4 + " + std::to_string(i) + ")";
      c += "    int s_ch = " + channel + " * stride.z + offset.z;\n";
      c += "    int s_z = min(s_ch >> 2, src_size.z - 1);\n";
      c += "    int s_z_rem = s_ch & 3;\n";
      c += "    FLT4 t = " +
           src_tensor.ReadWHSB("s_x", "s_y", "s_z", src_batch) + ";\n";
      c += "    FLT t_ar[4] = {t.x, t.y, t.z, t.w};\n";
      c += "    result." + postfixes[i] + " = t_ar[s_z_rem];\n";
      c += "  }\n";
    }
  }
  std::string x_3dcoord =
      op_def.IsBatchSupported() ? "X * dst_size.w + B" : "X";
  const LinkingContext context{"result", x_3dcoord, "Y", "Z"};
  c += PostProcess(linked_operations, context);
  c += "  " + dst_tensor.WriteWHSB("result", "X", "Y", "Z", dst_batch);
  c += "}\n";
  return c;
}

bool Is4Aligned(const SliceAttributes& attr) {
  return attr.strides.c == 1 && attr.starts.c % 4 == 0;
}

int4 GetOffset(const SliceAttributes& attr, int src_width, int src_height,
               int src_channels, int src_batch) {
  int4 offset;
  if (attr.strides.w > 0) {
    offset.x = attr.starts.w;
  } else {
    if (attr.ends.w > 0) {
      offset.x = attr.ends.w;
    } else {
      offset.x = src_width + attr.ends.w;
    }
  }
  if (attr.strides.h > 0) {
    offset.y = attr.starts.h;
  } else {
    if (attr.ends.h > 0) {
      offset.y = attr.ends.h;
    } else {
      offset.y = src_height + attr.ends.h;
    }
  }
  if (attr.strides.c > 0) {
    offset.z = attr.starts.c;
  } else {
    if (attr.ends.c > 0) {
      offset.z = attr.ends.c;
    } else {
      offset.z = src_channels + attr.ends.c;
    }
  }
  if (Is4Aligned(attr)) {
    offset.z /= 4;
  }
  if (attr.strides.b > 0) {
    offset.w = attr.starts.b;
  } else {
    if (attr.ends.b > 0) {
      offset.w = attr.ends.b;
    } else {
      offset.w = src_batch + attr.ends.b;
    }
  }
  return offset;
}

}  // namespace

StridedSlice::StridedSlice(const OperationDef& definition,
                           const SliceAttributes& attr)
    : GPUOperation(definition), attributes_(attr), work_group_size_(8, 4, 1) {}

StridedSlice::StridedSlice(StridedSlice&& operation)
    : GPUOperation(std::move(operation)),
      attributes_(operation.attributes_),
      kernel_(std::move(operation.kernel_)),
      work_group_size_(operation.work_group_size_) {}

StridedSlice& StridedSlice::operator=(StridedSlice&& operation) {
  if (this != &operation) {
    attributes_ = operation.attributes_;
    kernel_ = std::move(operation.kernel_);
    std::swap(work_group_size_, operation.work_group_size_);
    GPUOperation::operator=(std::move(operation));
  }
  return *this;
}

absl::Status StridedSlice::Compile(const CreationContext& creation_context) {
  const auto code = GetStridedSliceCode(definition_, Is4Aligned(attributes_),
                                        linked_operations_);
  return creation_context.cache->GetOrCreateCLKernel(
      code, "main_function", *creation_context.context,
      *creation_context.device, &kernel_);
}

absl::Status StridedSlice::BindArguments() {
  kernel_.ResetBindingCounter();
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(src_[0]->GetMemoryPtr()));
  RETURN_IF_ERROR(BindArgs(&kernel_, linked_operations_));
  RETURN_IF_ERROR(kernel_.SetMemoryAuto(dst_[0]->GetMemoryPtrForWriting()));
  int4 offset = GetOffset(attributes_, src_[0]->Width(), src_[0]->Height(),
                          src_[0]->Channels(), src_[0]->Batch());
  RETURN_IF_ERROR(kernel_.SetBytesAuto(offset));
  RETURN_IF_ERROR(
      kernel_.SetBytesAuto(int4(attributes_.strides.w, attributes_.strides.h,
                                attributes_.strides.c, attributes_.strides.b)));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(src_[0]->GetWHSB()));
  RETURN_IF_ERROR(kernel_.SetBytesAuto(dst_[0]->GetWHSB()));
  return absl::OkStatus();
}

int3 StridedSlice::GetGridSize() const {
  const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
  const int grid_y = dst_[0]->Height();
  const int grid_z = dst_[0]->Slices();
  return int3(grid_x, grid_y, grid_z);
}

absl::Status StridedSlice::Tune(const TuningParameters& params) {
  RETURN_IF_ERROR(BindArguments());
  return GetBestWorkGroup(params, kernel_, GetGridSize(), &work_group_size_);
}

absl::Status StridedSlice::AddToQueue(CLCommandQueue* queue) {
  RETURN_IF_ERROR(BindArguments());
  return queue->DispatchImplicit(kernel_, GetGridSize(), work_group_size_);
}

StridedSlice CreateStridedSlice(const OperationDef& definition,
                                const SliceAttributes& attr) {
  return StridedSlice(definition, attr);
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
