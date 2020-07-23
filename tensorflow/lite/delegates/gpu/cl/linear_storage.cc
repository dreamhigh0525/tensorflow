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

#include "tensorflow/lite/delegates/gpu/cl/linear_storage.h"

#include "absl/strings/str_cat.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"

namespace tflite {
namespace gpu {
namespace cl {

GPUResources TensorLinearDescriptor::GetGPUResources() const {
  GPUResources resources;
  resources.ints.push_back("length");
  if (storage_type == LinearStorageType::BUFFER) {
    GPUBufferDescriptor desc;
    desc.data_type = element_type;
    desc.access_type = access_type_;
    desc.element_size = 4;
    desc.memory_type = memory_type;
    resources.buffers.push_back({"buffer", desc});
  } else {
    GPUImage2DDescriptor desc;
    desc.data_type = element_type;
    desc.access_type = access_type_;
    resources.images2d.push_back({"tex2d", desc});
  }
  return resources;
}

absl::Status TensorLinearDescriptor::PerformSelector(
    const std::string& selector, const std::vector<std::string>& args,
    const std::vector<std::string>& template_args, std::string* result) const {
  if (selector == "Length") {
    *result = "length";
    return absl::OkStatus();
  } else if (selector == "Read") {
    return PerformReadSelector(args, result);
  } else if (selector == "GetPtr") {
    if (storage_type != LinearStorageType::BUFFER) {
      return absl::InvalidArgumentError(
          "GetPtr selector supported for LinearStorageType::BUFFER only.");
    }
    *result = "buffer";
    return absl::OkStatus();
  } else {
    return absl::NotFoundError(absl::StrCat(
        "TensorLinearDescriptor don't have selector with name - ", selector));
  }
}

absl::Status TensorLinearDescriptor::PerformReadSelector(
    const std::vector<std::string>& args, std::string* result) const {
  if (args.size() != 1) {
    return absl::NotFoundError(
        absl::StrCat("TensorLinearDescriptor Read require one argument, but ",
                     args.size(), " was passed"));
  }
  if (storage_type == LinearStorageType::BUFFER) {
    *result = absl::StrCat("buffer[", args[0], "]");
    return absl::OkStatus();
  } else {
    const std::string read =
        element_type == DataType::FLOAT16 ? "read_imageh" : "read_imagef";
    *result = absl::StrCat(read, "(tex2d, smp_none, (int2)(", args[0], ", 0))");
    return absl::OkStatus();
  }
}

LinearStorage::LinearStorage(int depth, LinearStorageType storage_type,
                             DataType data_type)
    : depth_(depth), storage_type_(storage_type), data_type_(data_type) {}

LinearStorage::LinearStorage(LinearStorage&& storage)
    : GPUObject(std::move(storage)),
      texture_storage_(std::move(storage.texture_storage_)),
      buffer_storage_(std::move(storage.buffer_storage_)),
      memory_(storage.memory_),
      depth_(storage.depth_),
      name_(std::move(storage.name_)),
      storage_type_(storage.storage_type_),
      data_type_(storage.data_type_) {
  storage.memory_ = nullptr;
}

LinearStorage& LinearStorage::operator=(LinearStorage&& storage) {
  if (this != &storage) {
    texture_storage_ = std::move(storage.texture_storage_);
    buffer_storage_ = std::move(storage.buffer_storage_);
    std::swap(memory_, storage.memory_);
    std::swap(depth_, storage.depth_);
    name_ = std::move(storage.name_);
    std::swap(storage_type_, storage.storage_type_);
    std::swap(data_type_, storage.data_type_);
    GPUObject::operator=(std::move(storage));
  }
  return *this;
}

absl::Status LinearStorage::GetGPUResources(
    const GPUObjectDescriptor* obj_ptr,
    GPUResourcesWithValue* resources) const {
  const auto* linear_desc =
      dynamic_cast<const TensorLinearDescriptor*>(obj_ptr);
  if (!linear_desc) {
    return absl::InvalidArgumentError(
        "Expected TensorLinearDescriptor on input.");
  }

  resources->ints.push_back({"length", depth_});

  if (storage_type_ == LinearStorageType::BUFFER) {
    resources->buffers.push_back({"buffer", memory_});
  } else {
    resources->images2d.push_back({"tex2d", memory_});
  }

  return absl::OkStatus();
}

LinearStorageType DeduceLinearStorageType(
    TensorStorageType tensor_storage_type) {
  if (tensor_storage_type == TensorStorageType::BUFFER) {
    return LinearStorageType::BUFFER;
  } else {
    return LinearStorageType::TEXTURE_2D;
  }
}

absl::Status CreateBufferLinearStorage(int size, DataType data_type, void* data,
                                       CLContext* context,
                                       LinearStorage* result) {
  const int float4_size =
      data_type == DataType::FLOAT32 ? sizeof(float4) : sizeof(half4);
  *result = LinearStorage(size, LinearStorageType::BUFFER, data_type);
  RETURN_IF_ERROR(CreateReadOnlyBuffer(float4_size * size, data, context,
                                       &result->buffer_storage_));
  result->memory_ = result->buffer_storage_.GetMemoryPtr();
  return absl::OkStatus();
}

absl::Status CreateTextureLinearStorage(int size, DataType data_type,
                                        void* data, CLContext* context,
                                        LinearStorage* result) {
  *result = LinearStorage(size, LinearStorageType::TEXTURE_2D, data_type);
  RETURN_IF_ERROR(CreateTexture2DRGBA(data_type, size, 1, data, context,
                                      &result->texture_storage_));
  result->memory_ = result->texture_storage_.GetMemoryPtr();
  return absl::OkStatus();
}

absl::Status CreateLinearStorage(const LinearStorageCreateInfo& creation_info,
                                 int size, void* data, CLContext* context,
                                 LinearStorage* result) {
  if (creation_info.storage_type == LinearStorageType::BUFFER) {
    return CreateBufferLinearStorage(size, creation_info.data_type, data,
                                     context, result);
  } else {
    return CreateTextureLinearStorage(size, creation_info.data_type, data,
                                      context, result);
  }
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
