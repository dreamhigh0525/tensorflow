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

#include "tensorflow/lite/delegates/gpu/cl/kernels/gpu_operation.h"

#include "absl/strings/substitute.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/util.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/work_group_picking.h"
#include "tensorflow/lite/delegates/gpu/common/access_type.h"

namespace tflite {
namespace gpu {
namespace cl {
namespace {

std::string GetElementWiseCode(const OperationDef& op_def,
                               bool check_src_slices) {
  std::string c = GetCommonDefines(op_def.precision);

  c += "__kernel void main_function(\n";
  c += "$0) {\n";
  c += "  int X = get_global_id(0);\n";
  c += "  int Y = get_global_id(1);\n";
  c += "  int Z = get_global_id(2);\n";
  c += "  if (X >= args.dst_tensor.Width() || Y >= args.dst_tensor.Height() || "
       "Z >= args.dst_tensor.Slices()) return; \n";
  if (check_src_slices) {
    c += "  FLT4 src = (FLT4)(0.0f);\n";
    c += "  if (Z < args.src_tensor.Slices()) {\n";
    c += "    src = args.src_tensor.Read(X, Y, Z);\n";
    c += "  }\n";
  } else {
    c += "  FLT4 src = args.src_tensor.Read(X, Y, Z);\n";
  }
  c += "  args.dst_tensor.Write(src, X, Y, Z);\n";
  c += "} \n";
  return c;
}

absl::Status MergeOperations(const std::vector<GPUOperation*>& linked_ops,
                             Arguments* merged_args, std::string* merged_code) {
  for (int i = 0; i < linked_ops.size(); ++i) {
    std::string code = linked_ops[i]->code_;
    std::string unique_postfix = absl::StrCat("_link", i + 1);
    linked_ops[i]->args_.RenameArgs(unique_postfix, &code);
    *merged_code += "{\n" + code + "\n}\n";
    RETURN_IF_ERROR(
        merged_args->Merge(std::move(linked_ops[i]->args_), unique_postfix));
    linked_ops[i]->AddUniquePostfix(unique_postfix);
  }
  return absl::OkStatus();
}

}  // namespace

DataType OperationDef::GetDataType() const {
  return DeduceDataTypeFromPrecision(precision);
}

DataType OperationDef::GetPrimaryDataType() const {
  return src_tensors[0].data_type;
}
TensorStorageType OperationDef::GetPrimaryStorageType() const {
  return src_tensors[0].storage_type;
}

bool OperationDef::HasAllTensorsOfType(TensorStorageType storage_type) const {
  for (const auto& src : src_tensors) {
    if (src.storage_type != storage_type) {
      return false;
    }
  }
  for (const auto& dst : dst_tensors) {
    if (dst.storage_type != storage_type) {
      return false;
    }
  }
  return true;
}

bool OperationDef::IsBatchSupported() const {
  for (const auto& src : src_tensors) {
    if (HasAxis(src.layout, Axis::BATCH)) {
      return true;
    }
  }
  for (const auto& dst : dst_tensors) {
    if (HasAxis(dst.layout, Axis::BATCH)) {
      return true;
    }
  }
  return false;
}

GPUOperation::GPUOperation(const OperationDef& definition)
    : definition_(definition) {}

void GPUOperation::SetSrc(Tensor* ptr, int index) {
  if (index >= src_.size()) {
    src_.resize(index + 1, nullptr);
  }
  src_[index] = ptr;
}

void GPUOperation::SetDst(Tensor* ptr, int index) {
  if (index >= dst_.size()) {
    dst_.resize(index + 1, nullptr);
  }
  dst_[index] = ptr;
}

GPUOperation::GPUOperation(GPUOperation&& operation)
    : args_(std::move(operation.args_)),
      code_(std::move(operation.code_)),
      work_group_size_(operation.work_group_size_),
      compiler_options_(std::move(operation.compiler_options_)),
      tensor_to_grid_(operation.tensor_to_grid_),
      elementwise_(operation.elementwise_),
      linkable_(operation.linkable_),
      check_src_channels_size_(operation.check_src_channels_size_),
      definition_(std::move(operation.definition_)),
      src_(std::move(operation.src_)),
      dst_(std::move(operation.dst_)),
      kernel_(std::move(operation.kernel_)),
      grid_size_(operation.grid_size_),
      src_tensors_names_(std::move(operation.src_tensors_names_)),
      dst_tensors_names_(std::move(operation.dst_tensors_names_)),
      linked_operations_(std::move(operation.linked_operations_)) {}

GPUOperation& GPUOperation::operator=(GPUOperation&& operation) {
  if (this != &operation) {
    args_ = std::move(operation.args_);
    code_ = std::move(operation.code_);
    std::swap(work_group_size_, operation.work_group_size_);
    compiler_options_ = std::move(operation.compiler_options_);
    tensor_to_grid_ = operation.tensor_to_grid_;
    elementwise_ = operation.elementwise_;
    linkable_ = operation.linkable_;
    check_src_channels_size_ = operation.check_src_channels_size_;
    definition_ = std::move(operation.definition_);
    src_ = std::move(operation.src_);
    dst_ = std::move(operation.dst_);
    kernel_ = std::move(operation.kernel_);
    std::swap(grid_size_, operation.grid_size_);
    src_tensors_names_ = std::move(operation.src_tensors_names_);
    dst_tensors_names_ = std::move(operation.dst_tensors_names_);
    linked_operations_ = std::move(operation.linked_operations_);
  }
  return *this;
}

void GPUOperation::AddOperation(GPUOperation* operation) {
  linked_operations_.push_back(operation);
}

void GPUOperation::AddSrcTensor(const std::string& tensor_name,
                                const TensorDescriptor& desc) {
  src_tensors_names_.push_back(tensor_name);
  auto desc_new = absl::make_unique<TensorDescriptor>(desc);
  args_.AddObjectRef(tensor_name, AccessType::READ, std::move(desc_new));
}

void GPUOperation::AddSrcBuffer(const std::string& buffer_name,
                                const BufferDescriptor& desc) {
  src_tensors_names_.push_back(buffer_name);
  auto desc_new = absl::make_unique<BufferDescriptor>(desc);
  args_.AddObjectRef(buffer_name, AccessType::READ, std::move(desc_new));
}

void GPUOperation::AddDstTensor(const std::string& tensor_name,
                                const TensorDescriptor& desc) {
  dst_tensors_names_.push_back(tensor_name);
  auto desc_new = absl::make_unique<TensorDescriptor>(desc);
  args_.AddObjectRef(tensor_name, AccessType::WRITE, std::move(desc_new));
}

absl::Status GPUOperation::UpdateParams() {
  for (int i = 0; i < src_tensors_names_.size(); ++i) {
    RETURN_IF_ERROR(args_.SetObjectRef(src_tensors_names_[i], src_[i]));
  }
  for (int i = 0; i < dst_tensors_names_.size(); ++i) {
    RETURN_IF_ERROR(args_.SetObjectRef(dst_tensors_names_[i], dst_[i]));
  }
  for (const auto linked_op : linked_operations_) {
    for (int i = 0; i < linked_op->src_tensors_names_.size(); ++i) {
      RETURN_IF_ERROR(args_.SetObjectRef(linked_op->src_tensors_names_[i],
                                         linked_op->src_[i + 1]));
    }
  }
  RETURN_IF_ERROR(BindArguments());
  grid_size_ = GetGridSize();
  return absl::OkStatus();
}

absl::Status GPUOperation::Compile(const CreationContext& creation_context) {
  if (elementwise_) {
    auto src_desc =
        absl::make_unique<TensorDescriptor>(definition_.src_tensors[0]);
    if (definition_.IsBatchSupported()) {
      src_desc->SetStateVar("BatchedWidth", "true");
    }
    src_tensors_names_.insert(src_tensors_names_.begin(), "src_tensor");
    args_.AddObjectRef("src_tensor", AccessType::READ, std::move(src_desc));

    auto dst_desc =
        absl::make_unique<TensorDescriptor>(definition_.dst_tensors[0]);
    if (definition_.IsBatchSupported()) {
      dst_desc->SetStateVar("BatchedWidth", "true");
    }
    dst_tensors_names_.insert(dst_tensors_names_.begin(), "dst_tensor");
    args_.AddObjectRef("dst_tensor", AccessType::WRITE, std::move(dst_desc));

    std::string code =
        GetElementWiseCode(definition_, check_src_channels_size_);
    std::string element_wise_code;
    element_wise_code += "{\n" + code_ + "\n}\n";
    RETURN_IF_ERROR(
        MergeOperations(linked_operations_, &args_, &element_wise_code));
    RETURN_IF_ERROR(args_.TransformToCLCode(
        creation_context.device->info_,
        {{dst_tensors_names_[0], element_wise_code}}, &code));
    code = absl::Substitute(code, args_.GetListOfArgs());
    RETURN_IF_ERROR(creation_context.cache->GetOrCreateCLKernel(
        code, "main_function", *creation_context.context,
        *creation_context.device, &kernel_));
  } else {
    std::string element_wise_code;
    RETURN_IF_ERROR(
        MergeOperations(linked_operations_, &args_, &element_wise_code));
    RETURN_IF_ERROR(args_.TransformToCLCode(
        creation_context.device->info_,
        {{dst_tensors_names_[0], element_wise_code}}, &code_));
    RETURN_IF_ERROR(creation_context.cache->GetOrCreateCLKernel(
        code_, "main_function", compiler_options_, *creation_context.context,
        *creation_context.device, &kernel_));
  }
  return PostCompileCheck(creation_context.device->info_, kernel_.info_);
}

void GPUOperation::GetPossibleKernelWorkGroups(
    TuningType tuning_type, const DeviceInfo& device_info,
    const KernelInfo& kernel_info, std::vector<int3>* work_groups) const {
  GetPossibleWorkGroups(tuning_type, device_info, kernel_info, grid_size_,
                        work_groups);
}

absl::Status GPUOperation::Tune(const TuningParameters& params) {
  std::vector<int3> possible_work_groups;
  GetPossibleKernelWorkGroups(params.tuning_type, *params.info, kernel_.info_,
                              &possible_work_groups);
  if (possible_work_groups.empty()) {
    return absl::NotFoundError(
        "Can not found work_group size to launch kernel");
  }
  if (possible_work_groups.size() == 1) {
    work_group_size_ = possible_work_groups[0];
    return absl::OkStatus();
  } else {
    RETURN_IF_ERROR(args_.Bind(kernel_.kernel()));
    int best_work_group_index;
    RETURN_IF_ERROR(params.queue->GetBestWorkGroupIndex(
        kernel_, *params.info, grid_size_, possible_work_groups,
        &best_work_group_index));
    work_group_size_ = possible_work_groups[best_work_group_index];
    return absl::OkStatus();
  }
}

int3 GPUOperation::GetGridSize() const {
  if (elementwise_ || tensor_to_grid_ == TensorToGrid::kWBToX_HDToY_SToZ) {
    const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
    const int grid_y = dst_[0]->Height() * dst_[0]->Depth();
    const int grid_z = dst_[0]->Slices();
    return int3(grid_x, grid_y, grid_z);
  }
  if (tensor_to_grid_ == TensorToGrid::kWBToX_HDToY_ZIs1) {
    const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
    const int grid_y = dst_[0]->Height() * dst_[0]->Depth();
    const int grid_z = 1;
    return int3(grid_x, grid_y, grid_z);
  }
  if (tensor_to_grid_ == TensorToGrid::kWBToX_HToY_DToZ) {
    const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
    const int grid_y = dst_[0]->Height();
    const int grid_z = dst_[0]->Depth();
    return int3(grid_x, grid_y, grid_z);
  }
  if (tensor_to_grid_ == TensorToGrid::kBToX_YIs1_ZIs1) {
    const int grid_x = dst_[0]->Batch();
    const int grid_y = 1;
    const int grid_z = 1;
    return int3(grid_x, grid_y, grid_z);
  }
  return int3(0, 0, 0);
}

void GPUOperation::AddUniquePostfix(const std::string& unique_postfix) {
  for (int i = 0; i < src_tensors_names_.size(); ++i) {
    src_tensors_names_[i] += unique_postfix;
  }
  for (int i = 0; i < dst_tensors_names_.size(); ++i) {
    dst_tensors_names_[i] += unique_postfix;
  }
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
