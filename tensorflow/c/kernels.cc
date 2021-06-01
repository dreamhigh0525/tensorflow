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

#include "tensorflow/c/kernels.h"

#include <memory>

#include "tensorflow/c/c_api_internal.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/c/tf_tensor_internal.h"
#include "tensorflow/core/framework/kernel_def_builder.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/framework/resource_var.h"
// Required for IS_MOBILE_PLATFORM definition
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/platform/platform.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/platform/mutex.h"
#if !defined(IS_MOBILE_PLATFORM) && !defined(IS_SLIM_BUILD)
#include "tensorflow/c/experimental/stream_executor/stream_executor_internal.h"
#include "tensorflow/stream_executor/stream.h"
#endif  // !defined(IS_MOBILE_PLATFORM) && !defined(IS_SLIM_BUILD)

using tensorflow::errors::InvalidArgument;
// This file forms the basis of a stable ABI for third-party kernel
// implementations. It is crucial that changes to this file are made cautiously
// and with a focus on maintaining both source and binary compatibility.

struct TF_KernelBuilder {
  ::tensorflow::KernelDefBuilder* cc_builder;

  void* (*create_function)(TF_OpKernelConstruction*);
  void (*compute_function)(void*, TF_OpKernelContext*);
  void (*delete_function)(void*);
};

TF_KernelBuilder* TF_NewKernelBuilder(
    const char* op_name, const char* device_name,
    void* (*create_func)(TF_OpKernelConstruction*),
    void (*compute_func)(void*, TF_OpKernelContext*),
    void (*delete_func)(void*)) {
  TF_KernelBuilder* result = new TF_KernelBuilder;
  result->cc_builder = new ::tensorflow::KernelDefBuilder(op_name);
  result->cc_builder->Device(device_name);
  result->create_function = create_func;
  result->compute_function = compute_func;
  result->delete_function = delete_func;
  return result;
}

void TF_DeleteKernelBuilder(TF_KernelBuilder* builder) {
  if (builder != nullptr) {
    delete builder->cc_builder;
    delete builder;
  }
}

namespace tensorflow {
namespace {

#define CASE(type)                                               \
  case DataTypeToEnum<type>::value: {                            \
    kernel_builder->cc_builder->TypeConstraint<type>(attr_name); \
    break;                                                       \
  }

void AddTypeConstraint(TF_KernelBuilder* kernel_builder, const char* attr_name,
                       const DataType dtype, TF_Status* status) {
  // This needs to be under tensorflow:: namespace so that
  // TF_CALL_ALL_TYPES macro can find tensorflow::string as string.
  switch (dtype) {
    TF_CALL_ALL_TYPES(CASE);
    TF_CALL_QUANTIZED_TYPES(CASE);
    TF_CALL_quint16(CASE);
    TF_CALL_qint16(CASE);
    default:
      status->status = errors::Unimplemented("Unexpected type ", dtype);
      return;
  }
  TF_SetStatus(status, TF_OK, "");
}
#undef CASE

}  // namespace
}  // namespace tensorflow

namespace {
const tensorflow::AttrValue* GetAttrValue(TF_OpKernelConstruction* ctx,
                                          const char* attr_name,
                                          TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelConstruction*>(ctx);
  const tensorflow::AttrValue* attr =
      ::tensorflow::AttrSlice(cc_ctx->def()).Find(attr_name);
  if (attr == nullptr) {
    status->status = InvalidArgument("Operation '", cc_ctx->def().name(),
                                     "' has no attr named '", attr_name, "'.");
  }
  return attr;
}
}  // namespace

void TF_KernelBuilder_TypeConstraint(TF_KernelBuilder* kernel_builder,
                                     const char* attr_name,
                                     const TF_DataType type,
                                     TF_Status* status) {
  tensorflow::DataType dtype = static_cast<tensorflow::DataType>(type);
  tensorflow::AddTypeConstraint(kernel_builder, attr_name, dtype, status);
}

void TF_KernelBuilder_HostMemory(TF_KernelBuilder* kernel_builder,
                                 const char* arg_name) {
  kernel_builder->cc_builder->HostMemory(arg_name);
}

void TF_KernelBuilder_Priority(TF_KernelBuilder* kernel_builder,
                               int32_t priority_number) {
  kernel_builder->cc_builder->Priority(priority_number);
}

namespace tensorflow {
namespace {

// An OpKernel whose methods delegate to C function pointers.
class COpKernel : public OpKernel {
 public:
  explicit COpKernel(OpKernelConstruction* ctx,
                     void* (*create_func)(TF_OpKernelConstruction*),
                     void (*compute_func)(void*, TF_OpKernelContext*),
                     void (*delete_func)(void*))
      : OpKernel(ctx), compute_func_(compute_func), delete_func_(delete_func) {
    if (create_func != nullptr) {
      c_kernel_ =
          (*create_func)(reinterpret_cast<TF_OpKernelConstruction*>(ctx));
    } else {
      c_kernel_ = nullptr;
    }
  }

  void Compute(OpKernelContext* ctx) override {
    (*compute_func_)(c_kernel_, reinterpret_cast<TF_OpKernelContext*>(ctx));
  }

  ~COpKernel() override {
    if (delete_func_ != nullptr) {
      (*delete_func_)(c_kernel_);
    }
  }

 private:
  void (*compute_func_)(void*, TF_OpKernelContext* context);
  void (*delete_func_)(void*);
  void* c_kernel_;
};

// A KernelFactory that returns COpKernel instances.
class KernelBuilderFactory
    : public ::tensorflow::kernel_factory::OpKernelFactory {
 public:
  explicit KernelBuilderFactory(TF_KernelBuilder* builder)
      : builder_(builder) {}
  ::tensorflow::OpKernel* Create(
      ::tensorflow::OpKernelConstruction* context) override {
    return new ::tensorflow::COpKernel(context, builder_->create_function,
                                       builder_->compute_function,
                                       builder_->delete_function);
  }
  ~KernelBuilderFactory() override { TF_DeleteKernelBuilder(builder_); }

 private:
  TF_KernelBuilder* builder_;
};
}  // namespace
}  // namespace tensorflow

void TF_RegisterKernelBuilder(const char* name, TF_KernelBuilder* builder,
                              TF_Status* status) {
  using tensorflow::register_kernel::Name;

  tensorflow::kernel_factory::OpKernelRegistrar(
      builder->cc_builder->Build(), name,
      absl::make_unique<tensorflow::KernelBuilderFactory>(builder));

  TF_SetStatus(status, TF_OK, "");
}

// This function is only for pluggable device.
// It will return nullptr in all other cases.
// This function is experimental and subject to change.
SP_Stream TF_GetStream(TF_OpKernelContext* ctx, TF_Status* status) {
#if defined(IS_MOBILE_PLATFORM) || defined(IS_SLIM_BUILD)
  status->status = tensorflow::errors::Unimplemented(
      "Accessing device stream is not supported on mobile. File a bug at "
      "https://github.com/tensorflow/tensorflow/issues if this feature is "
      "important to you");
  return nullptr;
#else
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  if (cc_ctx->op_device_context() == nullptr) {  // CPU Device
    status->status = tensorflow::errors::FailedPrecondition(
        "Accessing device stream is not supported for a CPU device.");
    return nullptr;
  } else if (!cc_ctx->op_device_context()->IsPluggableDevice()) {
    status->status = tensorflow::errors::FailedPrecondition(
        "Accessing device stream is only supported for pluggable devices.");
    return nullptr;
  } else {  // Is a PluggableDevice
    TF_SetStatus(status, TF_OK, "");
    auto c_stream = static_cast<stream_executor::CStream*>(
        cc_ctx->op_device_context()->stream()->implementation());
    return c_stream->Handle();
  }
#endif  // defined(IS_MOBILE_PLATFORM) || defined(IS_SLIM_BUILD)
}

int TF_NumInputs(TF_OpKernelContext* ctx) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  return cc_ctx->num_inputs();
}

int TF_NumOutputs(TF_OpKernelContext* ctx) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  return cc_ctx->num_outputs();
}

void TF_GetInput(TF_OpKernelContext* ctx, int i, TF_Tensor** tensor,
                 TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  if (i < 0 || i >= cc_ctx->num_inputs()) {
    TF_SetStatus(status, TF_OUT_OF_RANGE, "input index out of range");
    return;
  }
  const ::tensorflow::Tensor& cc_tensor(cc_ctx->input(i));
  TF_Tensor* result =
      ::tensorflow::TF_TensorFromTensor(cc_tensor, &status->status);
  if (TF_GetCode(status) == TF_OK) {
    *tensor = result;
  }
}

void TF_SetOutput(TF_OpKernelContext* ctx, int i, const TF_Tensor* tensor,
                  TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  if (i < 0 || i >= cc_ctx->num_outputs()) {
    TF_SetStatus(status, TF_OUT_OF_RANGE, "output index out of range");
    return;
  }
  ::tensorflow::Tensor cc_tensor;
  ::tensorflow::Status s = ::tensorflow::TF_TensorToTensor(tensor, &cc_tensor);
  TF_SetStatus(status, TF_OK, "");
  ::tensorflow::Set_TF_Status_from_Status(status, s);
  if (s.ok()) {
    cc_ctx->set_output(i, cc_tensor);
  }
}

void TF_OpKernelConstruction_Failure(TF_OpKernelConstruction* ctx,
                                     TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelConstruction*>(ctx);
  ::tensorflow::Status s(::tensorflow::StatusFromTF_Status(status));
  cc_ctx->CtxFailure(s);
}

void TF_OpKernelContext_Failure(TF_OpKernelContext* ctx, TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  ::tensorflow::Status s(::tensorflow::StatusFromTF_Status(status));
  cc_ctx->CtxFailure(s);
}

void TF_OpKernelConstruction_GetAttrSize(TF_OpKernelConstruction* ctx,
                                         const char* attr_name,
                                         int32_t* list_size,
                                         int32_t* total_size,
                                         TF_Status* status) {
  const tensorflow::AttrValue* attr = GetAttrValue(ctx, attr_name, status);
  if (!status->status.ok()) {
    *list_size = -1;
    *total_size = -1;
    return;
  }
  switch (attr->value_case()) {
#define SINGLE_CASE(kK, attr_type, size_expr) \
  case tensorflow::AttrValue::kK:             \
    *list_size = -1;                          \
    *total_size = size_expr;                  \
    break;

    SINGLE_CASE(kS, TF_ATTR_STRING, attr->s().length());
    SINGLE_CASE(kI, TF_ATTR_INT, -1);
    SINGLE_CASE(kF, TF_ATTR_FLOAT, -1);
    SINGLE_CASE(kB, TF_ATTR_BOOL, -1);
    SINGLE_CASE(kType, TF_ATTR_TYPE, -1);
    SINGLE_CASE(kShape, TF_ATTR_SHAPE,
                attr->shape().unknown_rank() ? -1 : attr->shape().dim_size());
    SINGLE_CASE(kTensor, TF_ATTR_TENSOR, -1);
#undef SINGLE_CASE

    case tensorflow::AttrValue::kList:
      *list_size = 0;
      *total_size = -1;
#define LIST_CASE(field, attr_type, ...)      \
  if (attr->list().field##_size() > 0) {      \
    *list_size = attr->list().field##_size(); \
    __VA_ARGS__;                              \
    break;                                    \
  }

      LIST_CASE(
          s, TF_ATTR_STRING, *total_size = 0;
          for (int i = 0; i < attr->list().s_size();
               ++i) { *total_size += attr->list().s(i).size(); });
      LIST_CASE(i, TF_ATTR_INT);
      LIST_CASE(f, TF_ATTR_FLOAT);
      LIST_CASE(b, TF_ATTR_BOOL);
      LIST_CASE(type, TF_ATTR_TYPE);
      LIST_CASE(
          shape, TF_ATTR_SHAPE, *total_size = 0;
          for (int i = 0; i < attr->list().shape_size(); ++i) {
            const auto& s = attr->list().shape(i);
            *total_size += s.unknown_rank() ? 0 : s.dim_size();
          });
      LIST_CASE(tensor, TF_ATTR_TENSOR);
      LIST_CASE(tensor, TF_ATTR_FUNC);
#undef LIST_CASE
      break;

    case tensorflow::AttrValue::kPlaceholder:
      *list_size = -1;
      *total_size = -1;
      break;

    case tensorflow::AttrValue::kFunc:
      *list_size = -1;
      *total_size = -1;
      break;

    case tensorflow::AttrValue::VALUE_NOT_SET:
      status->status =
          InvalidArgument("Attribute '", attr_name, "' has no value set");
      break;
  }
}

#define DEFINE_TF_GETATTR(func, c_type, cc_type, attr_type, list_field)        \
  void TF_OpKernelConstruction_GetAttr##func(TF_OpKernelConstruction* ctx,     \
                                             const char* attr_name,            \
                                             c_type* val, TF_Status* status) { \
    TF_SetStatus(status, TF_OK, "");                                           \
    cc_type v;                                                                 \
    auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelConstruction*>(ctx); \
    ::tensorflow::Status s = cc_ctx->GetAttr(attr_name, &v);                   \
    ::tensorflow::Set_TF_Status_from_Status(status, s);                        \
    if (s.ok()) {                                                              \
      *val = static_cast<c_type>(v);                                           \
    }                                                                          \
  }                                                                            \
  void TF_OpKernelConstruction_GetAttr##func##List(                            \
      TF_OpKernelConstruction* ctx, const char* attr_name, c_type* vals,       \
      int max_vals, TF_Status* status) {                                       \
    TF_SetStatus(status, TF_OK, "");                                           \
    const tensorflow::AttrValue* attr = GetAttrValue(ctx, attr_name, status);  \
    if (!status->status.ok()) return;                                          \
    if (attr->value_case() != tensorflow::AttrValue::kList) {                  \
      status->status =                                                         \
          InvalidArgument("Value for '", attr_name, "' is not a list.");       \
      return;                                                                  \
    }                                                                          \
    status->status =                                                           \
        tensorflow::AttrValueHasType(*attr, "list(" attr_type ")");            \
    if (!status->status.ok()) return;                                          \
    const auto len = std::min(max_vals, attr->list().list_field##_size());     \
    for (int i = 0; i < len; ++i) {                                            \
      vals[i] = static_cast<c_type>(attr->list().list_field(i));               \
    }                                                                          \
  }

DEFINE_TF_GETATTR(Type, TF_DataType, tensorflow::DataType, "type", type)
DEFINE_TF_GETATTR(Int32, int32_t, tensorflow::int32, "int", i)
DEFINE_TF_GETATTR(Int64, int64_t, tensorflow::int64, "int", i)
DEFINE_TF_GETATTR(Float, float, float, "float", f)
DEFINE_TF_GETATTR(Bool, TF_Bool, bool, "bool", b)

void TF_OpKernelConstruction_GetAttrString(TF_OpKernelConstruction* ctx,
                                           const char* attr_name, char* value,
                                           size_t max_length,
                                           TF_Status* status) {
  std::string v;
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelConstruction*>(ctx);
  ::tensorflow::Status s = cc_ctx->GetAttr(attr_name, &v);
  ::tensorflow::Set_TF_Status_from_Status(status, s);

  if (!status->status.ok()) return;

  if (max_length <= 0) {
    return;
  }
  std::memcpy(value, v.data(), std::min<size_t>(v.length(), max_length));
}

void TF_OpKernelConstruction_GetAttrStringList(TF_OpKernelConstruction* ctx,
                                               const char* attr_name,
                                               char** values, size_t* lengths,
                                               int max_values, void* storage,
                                               size_t storage_size,
                                               TF_Status* status) {
  std::vector<std::string> v;
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelConstruction*>(ctx);
  ::tensorflow::Status s = cc_ctx->GetAttr(attr_name, &v);
  ::tensorflow::Set_TF_Status_from_Status(status, s);

  if (!status->status.ok()) return;

  const auto len = std::min(max_values, static_cast<int>(v.size()));
  char* p = static_cast<char*>(storage);
  for (int i = 0; i < len; ++i) {
    const std::string& s = v[i];
    values[i] = p;
    lengths[i] = s.size();
    if ((p + s.size()) > (static_cast<char*>(storage) + storage_size)) {
      status->status = InvalidArgument(
          "Not enough storage to hold the requested list of strings");
      return;
    }
    memcpy(values[i], s.data(), s.size());
    p += s.size();
  }
}

bool TF_OpKernelConstruction_HasAttr(TF_OpKernelConstruction* ctx,
                                     const char* attr_name, TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelConstruction*>(ctx);
  return cc_ctx->HasAttr(attr_name);
}

TF_StringView TF_OpKernelConstruction_GetName(TF_OpKernelConstruction* ctx) {
  auto* cc_ctx = reinterpret_cast<tensorflow::OpKernelConstruction*>(ctx);
  TF_StringView string_view_of_name;
  string_view_of_name.data = cc_ctx->def().name().data();
  string_view_of_name.len = cc_ctx->def().name().length();
  return string_view_of_name;
}

TF_DataType TF_ExpectedOutputDataType(TF_OpKernelContext* ctx, int i) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  return static_cast<TF_DataType>(cc_ctx->expected_output_dtype(i));
}

int64_t TF_StepId(TF_OpKernelContext* ctx) {
  return reinterpret_cast<::tensorflow::OpKernelContext*>(ctx)->step_id();
}

TF_Tensor* TF_AllocateOutput(TF_OpKernelContext* context, int index,
                             TF_DataType dtype, int64_t* dims, int num_dims,
                             size_t len, TF_Status* status) {
  TF_SetStatus(status, TF_OK, "");
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(context);
  static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                "64-bit int types should match in size");
  tensorflow::gtl::ArraySlice<tensorflow::int64> dimarray(
      reinterpret_cast<tensorflow::int64*>(dims), num_dims);
  tensorflow::Tensor* tensor;
  tensorflow::Status s = cc_ctx->allocate_output(
      index, tensorflow::TensorShape(dimarray), &tensor);
  if (!s.ok()) {
    ::tensorflow::Set_TF_Status_from_Status(status, s);
    return nullptr;
  }
  TF_Tensor* tf_tensor = TF_TensorFromTensor(*tensor, &s);
  if (!s.ok()) {
    ::tensorflow::Set_TF_Status_from_Status(status, s);
    return nullptr;
  }
  return tf_tensor;
}

TF_Tensor* TF_ForwardInputOrAllocateOutput(
    TF_OpKernelContext* context, int* candidate_input_indices,
    int num_candidate_input_indices, int output_index, int64_t* output_dims,
    int output_num_dims, int* forwarded_input, TF_Status* status) {
  TF_SetStatus(status, TF_OK, "");
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(context);

  static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                "64-bit int types should match in size");
  tensorflow::gtl::ArraySlice<int> input_indices_array(
      candidate_input_indices, num_candidate_input_indices);
  tensorflow::gtl::ArraySlice<tensorflow::int64> output_dimarray(
      reinterpret_cast<tensorflow::int64*>(output_dims), output_num_dims);
  tensorflow::Tensor* output_tensor_pointer;
  tensorflow::Status s = cc_ctx->forward_input_or_allocate_output(
      input_indices_array, output_index,
      tensorflow::TensorShape(output_dimarray), &output_tensor_pointer,
      forwarded_input);
  if (!s.ok()) {
    ::tensorflow::Set_TF_Status_from_Status(status, s);
    return nullptr;
  }
  TF_Tensor* tf_tensor_output = TF_TensorFromTensor(*output_tensor_pointer, &s);
  if (!s.ok()) {
    ::tensorflow::Set_TF_Status_from_Status(status, s);
    return nullptr;
  }
  return tf_tensor_output;
}

TF_Tensor* TF_AllocateTemp(TF_OpKernelContext* context, TF_DataType dtype,
                           int64_t* dims, int num_dims,
                           TF_AllocatorAttributes* attributes,
                           TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(context);
  TF_SetStatus(status, TF_OK, "");
  static_assert(sizeof(int64_t) == sizeof(tensorflow::int64),
                "64-bit int types should match in size");
  tensorflow::gtl::ArraySlice<tensorflow::int64> dimarray(
      reinterpret_cast<tensorflow::int64*>(dims), num_dims);
  if (attributes && !attributes->struct_size) {
    TF_SetStatus(
        status, TF_INVALID_ARGUMENT,
        "TF_AllocatorAttributes struct "
        "size member must be set to TF_ALLOCATOR_ATTRIBUTES_STRUCT_SIZE");
    return nullptr;
  }
  tensorflow::AllocatorAttributes allocator_attr;
  if (attributes && attributes->on_host) {
    allocator_attr.set_on_host(true);
  }
  tensorflow::Status s;
  tensorflow::Tensor tensor;
  s = cc_ctx->allocate_temp(static_cast<tensorflow::DataType>(dtype),
                            tensorflow::TensorShape(dimarray), &tensor,
                            allocator_attr);
  if (!s.ok()) {
    ::tensorflow::Set_TF_Status_from_Status(status, s);
    return nullptr;
  }
  TF_Tensor* tf_tensor;
  tf_tensor = TF_TensorFromTensor(tensor, &s);
  if (!s.ok()) {
    ::tensorflow::Set_TF_Status_from_Status(status, s);
    return nullptr;
  }
  return tf_tensor;
}

tensorflow::Status EnsureSparseVariableAccess(TF_OpKernelContext* ctx,
                                      bool variantType,
                                      void (*copyFunc)(TF_OpKernelContext * ctx,
                                                       TF_Tensor *source,
                                                       TF_Tensor *dest),
                                      tensorflow::Var* var) {
  using namespace tensorflow;
  auto* context = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  if (var->copy_on_read_mode.load()) {
    return Status::OK();
  }
  mutex_lock ml(*var->mu());
  // Once copy-on-read mode is True the refcount is guaranteed to be 1. This can
  // also happen if there are no concurrent reads of the variable and
  // copy-on-read mode is false.
  if (var->tensor()->RefCountIsOne()) {
    var->copy_on_read_mode.store(true);
    return Status::OK();
  }
  Tensor tmp;
  if (variantType) {
    AllocatorAttributes attr;
    attr.set_on_host(true);
    TF_RETURN_IF_ERROR(context->allocate_temp(var->tensor()->dtype(),
                                          var->tensor()->shape(), &tmp, attr));

    const auto elements_in = var->tensor()->flat<Variant>();
    auto elements_out = tmp.flat<Variant>();
    for (int64 i = 0; i < elements_in.size(); ++i) {
      elements_out(i) = elements_in(i);
    }
  } else {
    AllocatorAttributes attr;
    attr.set_gpu_compatible(true);
    attr.set_nic_compatible(true);
    TF_RETURN_IF_ERROR(context->allocate_temp(var->tensor()->dtype(),
                                          var->tensor()->shape(), &tmp, attr));
    tensorflow::Status s;
    TF_Tensor *tf_tmp = TF_TensorFromTensor(tmp, &s);
    TF_Tensor *tf_tensor = TF_TensorFromTensor(*var->tensor(), &s);
    copyFunc(ctx, tf_tensor, tf_tmp);
  }
  *var->tensor() = tmp;
  var->copy_on_read_mode.store(true);
  return Status::OK();
}

tensorflow::Status PrepareToUpdateVariable(TF_OpKernelContext* ctx, tensorflow::Tensor* tensor,
                               bool copy_on_read_mode,
                               bool variantType,
                               void (*copyFunc)(TF_OpKernelContext * ctx, TF_Tensor *source, TF_Tensor *dest)) {

  using namespace tensorflow;
  auto* context = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  if (copy_on_read_mode || !tensor->RefCountIsOne()) {
    // Tensor's buffer is in use by some read, so we need to copy before
    // updating.
    Tensor tmp;
    if(variantType) {
      AllocatorAttributes attr;
      attr.set_on_host(true);
      TF_RETURN_IF_ERROR(
          context->allocate_temp(tensor->dtype(), tensor->shape(), &tmp, attr));

      const auto elements_in = tensor->flat<Variant>();
      auto elements_out = tmp.flat<Variant>();
      for (int64 i = 0; i < elements_in.size(); ++i) {
        elements_out(i) = elements_in(i);
      }
    } else {
      AllocatorAttributes attr;
      attr.set_gpu_compatible(true);
      attr.set_nic_compatible(true);
      TF_RETURN_IF_ERROR(
          context->allocate_temp(tensor->dtype(), tensor->shape(), &tmp, attr));
      tensorflow::Status s;
      TF_Tensor *tf_tmp = TF_TensorFromTensor(tmp, &s);
      TF_Tensor *tf_tensor = TF_TensorFromTensor(*tensor, &s);
      copyFunc(ctx, tf_tensor, tf_tmp);
    }
    *tensor = tmp;
  }
  return Status::OK();
}

void TF_AssignVariable(TF_OpKernelContext* ctx,
                       int input_index,
                       int value_index,
                       void (*copyFunc)(TF_OpKernelContext * ctx, TF_Tensor *source, TF_Tensor *dest),
                       TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  tensorflow::AllocatorAttributes allocator_attr;
  tensorflow::core::RefCountPtr<tensorflow::Var> variable;
  const tensorflow::Tensor& value = cc_ctx->input(value_index);
  auto c_stream = static_cast<stream_executor::CStream*>(
      cc_ctx->op_device_context()->stream()->implementation());
  OP_REQUIRES_OK(cc_ctx, LookupOrCreateResource<tensorflow::Var>(
                                cc_ctx, HandleFromInput(cc_ctx, input_index), &variable,
                                [&value](tensorflow::Var** ptr) {
                                  *ptr = new tensorflow::Var(value.dtype());
                                  *(*ptr)->tensor() = value;
                                  (*ptr)->is_initialized = true;
                                  return tensorflow::Status::OK();
                                }));
  tensorflow::mutex_lock ml(*variable->mu());

  if (variable->copy_on_read_mode.load()) {
    tensorflow::Tensor* tmp;
    tensorflow::AllocatorAttributes attr;
    attr.set_gpu_compatible(true);
    attr.set_nic_compatible(true);
    OP_REQUIRES_OK(cc_ctx,
                     cc_ctx->allocate_temp(value.dtype(), value.shape(),
                                            variable->tensor(), attr));
    tensorflow::Status s;
    TF_Tensor *tf_tmp = TF_TensorFromTensor(*tmp, &s);
    TF_Tensor *tf_value = TF_TensorFromTensor(value, &s);
    copyFunc(ctx, tf_value, tf_tmp);
    *variable->tensor() = *tmp;
  } else {
    *variable->tensor() = value;
  }
  variable->is_initialized = true;
  TF_SetStatus(status, TF_OK, "");
}

tensorflow::mutex* TF_GetTrainingVariableMutex(tensorflow::OpKernelContext* ctx,
                                               int32_t input,
                                               bool sparse,
                                               tensorflow::Var** maybe_resource) {
  *maybe_resource = nullptr;
  if (ctx->input_dtype(input) == tensorflow::DT_RESOURCE) {
    if (LookupResource(ctx, HandleFromInput(ctx, input), maybe_resource).ok()) {
      return (*maybe_resource)->mu();
    } else {
      ctx->CtxFailureWithWarning(
          tensorflow::errors::Internal("Invalid variable reference."));
      return nullptr;
    }
  }
  return ctx->input_ref_mutex(input);
}

void TF_MaybeLockVariableInputMutexesInOrder(
    TF_OpKernelContext* ctx, bool do_lock, bool sparse,
    const int* const inputs,
    size_t len,
    TF_VariableInputLockHolder** lockHolder,
    TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  bool any_resource = false;
  std::vector<int> input_ids(inputs, inputs+len);
  for (auto i : input_ids) {
    if (cc_ctx->input_dtype(i) == tensorflow::DT_RESOURCE) {
      any_resource = true;
      break;
    }
  }
  if (!do_lock && !any_resource) {
    *lockHolder =  new TF_VariableInputLockHolder({}, {}, {});
    TF_SetStatus(status, TF_OK, "");
    return;

  }
  std::vector<tensorflow::Var*> vars;
  std::vector<tensorflow::mutex*> mutexes;
  std::vector<int32_t> acquire_order;
  for (auto input : input_ids) {
    tensorflow::Var* var;
    tensorflow::mutex* mutex =
        TF_GetTrainingVariableMutex(cc_ctx, input, sparse, &var);
    if (var) vars.push_back(var);
    // Only lock each mutex once if duplicates exist (n^2 but n is 2 or 3).
    if (std::find(mutexes.begin(), mutexes.end(), mutex) == mutexes.end()) {
      acquire_order.push_back(mutexes.size());
      mutexes.push_back(mutex);
    }
  }
  std::sort(acquire_order.begin(), acquire_order.end(),
            [&mutexes](int a, int b) { return mutexes[a] < mutexes[b]; });

  auto locks = absl::make_unique<std::vector<tensorflow::mutex_lock>>();
  auto shared_locks = absl::make_unique<std::vector<tensorflow::tf_shared_lock>>();
  locks->reserve(acquire_order.size());

  for (auto input : acquire_order) {
    tensorflow::Var* var;
    tensorflow::mutex* mu = TF_GetTrainingVariableMutex(cc_ctx, input, sparse, &var);
    tensorflow::core::ScopedUnref scoped_unref(var);
    if (mu != nullptr) {
      if (do_lock) {
        locks->emplace_back(*mu);
      } else {
        shared_locks->emplace_back(*mu);
      }
    }
  }
  *lockHolder =  new TF_VariableInputLockHolder(std::move(vars), std::move(locks),
                                    std::move(shared_locks));
  TF_SetStatus(status, TF_OK, "");
  return;
}

void TF_GetInputTensorFromVariable(TF_OpKernelContext* ctx, 
                                  int input,
                                  bool lock_held,
                                  bool isVariantType,
                                  bool sparse,
                                  void (*copyFunc)(TF_OpKernelContext * ctx, TF_Tensor *source, TF_Tensor *dest),
                                  TF_Tensor** out,
                                  TF_Status* status) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  auto c_stream = static_cast<stream_executor::CStream*>(
        cc_ctx->op_device_context()->stream()->implementation());
  tensorflow::Status s;
  if (cc_ctx->input_dtype(input) == tensorflow::DT_RESOURCE) {
    tensorflow::core::RefCountPtr<tensorflow::Var> var;
    LookupResource(cc_ctx, HandleFromInput(cc_ctx, input), &var);
    if (sparse) {
      OP_REQUIRES_OK(cc_ctx, EnsureSparseVariableAccess(ctx, isVariantType, copyFunc, var.get()));
      *out = ::tensorflow::TF_TensorFromTensor(*var->tensor(), &s);
      TF_SetStatus(status, TF_OK, "");
      return;
    }
    OP_REQUIRES_OK(cc_ctx, PrepareToUpdateVariable(
        ctx, var->tensor(), var->copy_on_read_mode.load(), false, copyFunc));
    *out = ::tensorflow::TF_TensorFromTensor(*var->tensor(), &s);
    TF_SetStatus(status, TF_OK, "");
    return;
  }
  *out = ::tensorflow::TF_TensorFromTensor(cc_ctx->mutable_input(input, lock_held),
                                           &s);
  TF_SetStatus(status, TF_OK, "");
}

void TF_OpKernelContext_ForwardRefInputToRefOutput(TF_OpKernelContext* ctx,
                                                   int32_t input_index,
                                                   int32_t output_index) {
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  if (cc_ctx->input_dtype(input_index) != tensorflow::DT_RESOURCE) {
    cc_ctx->forward_ref_input_to_ref_output(input_index, output_index);
  }
}

void TF_ReleaseVariableInputLockHolder(TF_VariableInputLockHolder *v) {
  if (v != nullptr) {
    v->locks_.reset();
    for (tensorflow::Var* var : v->vars_) {
      var->Unref();
    }
    delete v;
  }
}

void TF_GetInputByName(TF_OpKernelContext* ctx, const char *inputName,
                                       TF_Tensor** tensor, TF_Status* status)
{
  auto* cc_ctx = reinterpret_cast<::tensorflow::OpKernelContext*>(ctx);
  const ::tensorflow::Tensor *cc_tensor = nullptr;
  tensorflow::Status s = cc_ctx->input(inputName, &cc_tensor);

  if(!s.ok()) {
    ::tensorflow::Set_TF_Status_from_Status(status, s);
    return;
  }
  TF_Tensor* result =
      ::tensorflow::TF_TensorFromTensor(*cc_tensor, &status->status);
  if (TF_GetCode(status) == TF_OK) {
    *tensor = result;
  }
}

