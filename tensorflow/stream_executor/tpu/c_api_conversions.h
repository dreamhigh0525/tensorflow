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

#ifndef TENSORFLOW_STREAM_EXECUTOR_TPU_C_API_CONVERSIONS_H_
#define TENSORFLOW_STREAM_EXECUTOR_TPU_C_API_CONVERSIONS_H_

#include "absl/container/inlined_vector.h"
#include "tensorflow/compiler/xla/executable_run_options.h"
#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/service/maybe_owning_device_memory.h"
#include "tensorflow/compiler/xla/service/service_executable_run_options.h"
#include "tensorflow/compiler/xla/service/shaped_buffer.h"
#include "tensorflow/compiler/xla/shape.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/stream_executor/device_memory.h"
#include "tensorflow/stream_executor/device_memory_allocator.h"
#include "tensorflow/stream_executor/tpu/c_api_decl.h"
#include "tensorflow/stream_executor/tpu/tpu_executor_c_api.h"

// APIs for converting between internal and external versions of
// XLA/StreamExecutor data structures.
namespace ApiConverter {

// se::DeviceMemoryBase
SE_DeviceMemoryBase ToC(const stream_executor::DeviceMemoryBase& base);
void ToC(const stream_executor::DeviceMemoryBase& base,
         SE_DeviceMemoryBase* se_base);
stream_executor::DeviceMemoryBase FromC(const SE_DeviceMemoryBase& se_base);
void Free(SE_DeviceMemoryBase*);

// xla::Shape
xla::Shape FromC(XLA_Shape* shape);
void ToC(const xla::Shape& xla_shape, XLA_Shape* c_shape);
void Free(XLA_Shape* shape);

// xla::ShapeIndex
XLA_ShapeIndex ToC(const xla::ShapeIndex& xla_shape);
xla::ShapeIndex FromC(XLA_ShapeIndex* c_shape);
void Free(XLA_ShapeIndex*);

// Literal
void ToC(const xla::LiteralSlice& literal, XLA_Literal* c_literal);
xla::MutableBorrowingLiteral FromC(XLA_Literal* c_literal);
void Free(XLA_Literal* c_literal);

// ShapedBuffer
void ToC(const xla::ShapedBuffer& buffer, XLA_ShapedBuffer* c_device_buffer);
xla::ShapedBuffer FromC(XLA_ShapedBuffer* c_buffer);
void Free(XLA_ShapedBuffer* c_buffer);

// se::DeviceMemoryBase
SE_DeviceMemoryBase ToC(const stream_executor::DeviceMemoryBase& base);
stream_executor::DeviceMemoryBase FromC(const SE_DeviceMemoryBase& se_base);
void Free(SE_DeviceMemoryBase*);

// xla::Shape
xla::Shape FromC(XLA_Shape* shape);
void ToC(const xla::Shape& xla_shape, XLA_Shape* c_shape);
void Free(XLA_Shape* shape);

// Literal
void ToC(const xla::LiteralSlice& literal, XLA_Literal* c_literal);
xla::MutableBorrowingLiteral FromC(XLA_Literal* c_literal);
void Free(XLA_Literal* c_literal);

// ShapedBuffer
void ToC(const xla::ShapedBuffer& buffer, XLA_ShapedBuffer* c_device_buffer);
xla::ShapedBuffer FromC(XLA_ShapedBuffer* c_buffer);
void Free(XLA_ShapedBuffer* c_buffer);

xla::MaybeOwningDeviceMemory FromC(
    SE_MaybeOwningDeviceMemory* se_mem,
    stream_executor::DeviceMemoryAllocator* allocator);

// DeviceMemoryAllocator
SE_DeviceMemoryAllocator ToC(stream_executor::DeviceMemoryAllocator* allocator);

// OwningDeviceMemory
SE_MaybeOwningDeviceMemory ToC(stream_executor::OwningDeviceMemory* mem);
SE_MaybeOwningDeviceMemory ToC(xla::MaybeOwningDeviceMemory& mem);

// Helper for managing stack based C -> C++ conversions.
template <class CType>
struct StackHelper {
  explicit StackHelper() {}

  template <class CppType>
  explicit StackHelper(const CppType& t) {
    ::ApiConverter::ToC(t, &value);
  }
  ~StackHelper() { ::ApiConverter::Free(&value); }

  template <class CppType>
  CppType AsCpp() const {
    return ::ApiConverter::FromC(&value);
  }

  mutable CType value;
};

}  // namespace ApiConverter

#endif
