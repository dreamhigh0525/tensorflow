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

#include "tensorflow/core/kernels/tensor_map.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_shape.pb.h"
#include "tensorflow/core/framework/variant_op_registry.h"
#include "tensorflow/core/lib/core/coding.h"

namespace tensorflow {

TensorMap::~TensorMap() {
  if (tensors_) tensors_->Unref();
}

void TensorMap::Encode(VariantTensorData* data) const {
  data->set_type_name(TypeName());

  absl::flat_hash_map<TensorKey,Tensor>::const_iterator map_it = tensors().begin();
  while (map_it != tensors().end()) {
    Tensor k = map_it->first;
    Tensor v = map_it->second;
    // TODO: k should also not be DT_RESOURCE or DT_VARIANT
    if(k.dtype() != DT_INVALID && v.dtype() != DT_INVALID) {
      *data->add_tensors() = k;
      *data->add_tensors() = v;
    }
  }
  string metadata;
  // TODO(b/118838800): Add a proto for storing the metadata.
  // Metadata format:
  // <element_dtype><element_shape_proto>
  core::PutVarint64(&metadata, static_cast<uint64>(element_dtype));
  core::PutVarint64(&metadata, static_cast<uint64>(max_num_elements));
  TensorShapeProto element_shape_proto;
  element_shape.AsProto(&element_shape_proto);
  element_shape_proto.AppendToString(&metadata);
  data->set_metadata(metadata);
}

static Status TensorMapDeviceCopy(
    const TensorMap& from, TensorMap* to,
    const UnaryVariantOpRegistry::AsyncTensorDeviceCopyFn& copy) {
  to->element_shape = from.element_shape;
  to->element_dtype = from.element_dtype;
  to->max_num_elements = from.max_num_elements;
  for (const std::pair<TensorKey,Tensor>& p : from.tensors()) {
    to->tensors().emplace(p); //TODO: check valid dtype
    //if (t.dtype() != DT_INVALID) {
      //TF_RETURN_IF_ERROR(copy(p, &to->tensors().back()));
    //}
  }
  return Status::OK();
}

#define REGISTER_LIST_COPY(DIRECTION)                                         \
  INTERNAL_REGISTER_UNARY_VARIANT_DEVICE_COPY_FUNCTION(TensorMap, DIRECTION, \
                                                       TensorMapDeviceCopy)

REGISTER_LIST_COPY(VariantDeviceCopyDirection::HOST_TO_DEVICE);
REGISTER_LIST_COPY(VariantDeviceCopyDirection::DEVICE_TO_HOST);
REGISTER_LIST_COPY(VariantDeviceCopyDirection::DEVICE_TO_DEVICE);

REGISTER_UNARY_VARIANT_DECODE_FUNCTION(TensorMap, TensorMap::kTypeName);

bool TensorMap::Decode(const VariantTensorData& data) {
  // TODO(srbs): Change the signature to Decode(VariantTensorData data) so
  // that we do not have to copy each tensor individually below. This would
  // require changing VariantTensorData::tensors() as well.
  string metadata;
  data.get_metadata(&metadata);
  uint64 scratch;
  StringPiece iter(metadata);

  std::vector<Tensor>::const_iterator tensors_it = data.tensors().begin();

  while (tensors_it != data.tensors().end())
  {
    // should assert that tensors_it + 1 is also not the end
    /*if (*std::next(tensors_it) == data.tensors().end()) {
      return false;
    }*/
    TensorKey k = TensorKey(*tensors_it); // copy inefficient?
    tensors().emplace(k,*++tensors_it);
    tensors_it++;
  }

  core::GetVarint64(&iter, &scratch);
  element_dtype = static_cast<DataType>(scratch);
  core::GetVarint64(&iter, &scratch);
  max_num_elements = static_cast<int>(scratch);
  TensorShapeProto element_shape_proto;
  element_shape_proto.ParseFromString(string(iter.data(), iter.size()));
  element_shape = PartialTensorShape(element_shape_proto);
  return true;
}

const char TensorMap::kTypeName[] = "tensorflow::TensorMap";

}  // namespace tensorflow
