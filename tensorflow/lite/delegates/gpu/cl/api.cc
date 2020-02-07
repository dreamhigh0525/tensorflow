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

#include "tensorflow/lite/delegates/gpu/cl/api.h"

#include <algorithm>
#include <cstring>

#include <EGL/eglext.h>
#include "absl/memory/memory.h"
#include "absl/types/span.h"
#include "tensorflow/lite/delegates/gpu/cl/cl_command_queue.h"
#include "tensorflow/lite/delegates/gpu/cl/cl_errors.h"
#include "tensorflow/lite/delegates/gpu/cl/cl_event.h"
#include "tensorflow/lite/delegates/gpu/cl/egl_sync.h"
#include "tensorflow/lite/delegates/gpu/cl/environment.h"
#include "tensorflow/lite/delegates/gpu/cl/gl_interop.h"
#include "tensorflow/lite/delegates/gpu/cl/inference_context.h"
#include "tensorflow/lite/delegates/gpu/cl/kernels/converter.h"
#include "tensorflow/lite/delegates/gpu/cl/opencl_wrapper.h"
#include "tensorflow/lite/delegates/gpu/cl/precision.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type.h"
#include "tensorflow/lite/delegates/gpu/cl/tensor_type_util.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/tensor.h"

namespace tflite {
namespace gpu {
namespace cl {
namespace {

// Both internal and external defs are identical, therefore nothing to connect
// here.
class NoopTensorTie : public TensorTie {
 public:
  NoopTensorTie(const TensorTieDef& def, TensorObject obj)
      : TensorTie(def), obj_(obj) {}

  static bool IsSupported(const TensorTieDef& def) {
    return def.external_def == def.internal_def;
  }

  Status SetExternalObject(TensorObject obj) final {
    if (!def().external_def.object_def.user_provided) {
      return InvalidArgumentError("Tensor object is readonly.");
    }
    if (!IsValid(def().external_def, obj)) {
      return InvalidArgumentError("Given object is not valid");
    }
    obj_ = obj;
    return OkStatus();
  }

  TensorObject GetExternalObject() final { return obj_; }

  Status CopyToExternalObject() final { return OkStatus(); }

  Status CopyFromExternalObject() final { return OkStatus(); }

 private:
  TensorObject obj_;
};

// Does one-step conversion between internal and external objects.
// It may also allocate external objects if requested.
class DefaultTensorTie : public TensorTie {
 public:
  DefaultTensorTie(const TensorTieDef& def, TensorObject internal_obj)
      : TensorTie(def), internal_obj_(internal_obj) {}

  static bool IsSupported(
      const TensorTieDef& def,
      const TensorObjectConverterBuilder& converter_builder) {
    auto object_type = def.external_def.object_def.object_type;
    return (object_type == ObjectType::OPENCL_BUFFER ||
            object_type == ObjectType::OPENCL_TEXTURE ||
            object_type == ObjectType::CPU_MEMORY) &&
           converter_builder.IsSupported(def.internal_def, def.external_def) &&
           converter_builder.IsSupported(def.external_def, def.internal_def);
  }

  static Status New(const TensorTieDef& def, TensorObject internal_object,
                    TensorObjectConverterBuilder* converter_builder,
                    Environment* env, std::unique_ptr<TensorTie>* tie) {
    auto tie_impl = absl::make_unique<DefaultTensorTie>(def, internal_object);
    RETURN_IF_ERROR(tie_impl->Init(converter_builder, env));
    *tie = std::move(tie_impl);
    return OkStatus();
  }

  Status CopyToExternalObject() final {
    if (!converter_to_) {
      return UnavailableError("Conversion is not available");
    }
    return converter_to_->Convert(internal_obj_, GetExternalObject());
  }

  Status CopyFromExternalObject() final {
    if (!converter_from_) {
      return UnavailableError("Conversion is not available");
    }
    return converter_from_->Convert(GetExternalObject(), internal_obj_);
  }

  Status SetExternalObject(TensorObject obj) final {
    if (!def().external_def.object_def.user_provided) {
      return InvalidArgumentError("External object is read-only");
    }
    if (!IsValid(def().external_def, obj)) {
      return InvalidArgumentError("Given object is not valid");
    }
    external_obj_ = obj;
    return OkStatus();
  }

  TensorObject GetExternalObject() final { return external_obj_; }

 private:
  Status Init(TensorObjectConverterBuilder* converter_builder,
              Environment* env) {
    RETURN_IF_ERROR(converter_builder->MakeConverter(
        def().internal_def, def().external_def, &converter_to_));
    RETURN_IF_ERROR(converter_builder->MakeConverter(
        def().external_def, def().internal_def, &converter_from_));
    return MaybeAllocateExternalObject(env);
  }

  Status MaybeAllocateExternalObject(Environment* env) {
    const TensorObjectDef& d = def().external_def;
    if (d.object_def.user_provided) {
      return OkStatus();
    }
    switch (d.object_def.object_type) {
      case ObjectType::CPU_MEMORY: {
        size_t bytes_size = NumElements(d) * SizeOf(d.object_def.data_type);
        cpu_memory_.resize(bytes_size);
        external_obj_ = CpuMemory{cpu_memory_.data(), cpu_memory_.size()};
        break;
      }
      case ObjectType::OPENCL_TEXTURE:
      case ObjectType::OPENCL_BUFFER: {
        auto& dims = d.dimensions;
        const BHWC shape(dims.b, dims.h, dims.w, dims.c);
        const TensorDescriptor desc{
            d.object_def.data_type,
            ToTensorStorageType(d.object_def.object_type,
                                d.object_def.data_layout),
            Layout::BHWC};
        RETURN_IF_ERROR(AllocateTensorMemory(env->context(), env->device(),
                                             shape, desc, &cl_memory_));
        if (d.object_def.object_type == ObjectType::OPENCL_TEXTURE) {
          external_obj_ = OpenClTexture{cl_memory_.memory()};
        } else {
          external_obj_ = OpenClBuffer{cl_memory_.memory()};
        }
        break;
      }
      default:
        return InternalError("Unexpected object type");
    }
    return OkStatus();
  }

  const TensorObject internal_obj_;
  TensorObject external_obj_;
  CLMemory cl_memory_;
  std::vector<uint8_t> cpu_memory_;
  std::unique_ptr<TensorObjectConverter> converter_to_;
  std::unique_ptr<TensorObjectConverter> converter_from_;
};

// Copies data to intermediate OpenCL buffer and then does two step conversion.
// It drives the following cases were one-step conversion is not supported:
//   - CPU BHWC -> CL buffer BHWC -> CL texture DHWC4.
class TwoStepTensorTie : public TensorTie {
 public:
  explicit TwoStepTensorTie(const TensorTieDef& def) : TensorTie(def) {}

  static bool IsSupported(
      const TensorTieDef& def,
      const TensorObjectConverterBuilder& converter_builder) {
    auto defs = MakeOuterInnerDefs(def);
    return DefaultTensorTie::IsSupported(defs.first, converter_builder) &&
           DefaultTensorTie::IsSupported(defs.second, converter_builder);
  }

  static Status New(const TensorTieDef& def, TensorObject internal_object,
                    TensorObjectConverterBuilder* converter_builder,
                    Environment* env, std::unique_ptr<TensorTie>* tie) {
    auto tie_impl = absl::make_unique<TwoStepTensorTie>(def);
    RETURN_IF_ERROR(tie_impl->Init(internal_object, converter_builder, env));
    *tie = std::move(tie_impl);
    return OkStatus();
  }

  Status CopyToExternalObject() final {
    RETURN_IF_ERROR(inner_tie_->CopyToExternalObject());
    return outer_tie_->CopyToExternalObject();
  }

  Status CopyFromExternalObject() final {
    RETURN_IF_ERROR(outer_tie_->CopyFromExternalObject());
    return inner_tie_->CopyFromExternalObject();
  }

  Status SetExternalObject(TensorObject obj) final {
    return outer_tie_->SetExternalObject(obj);
  }

  TensorObject GetExternalObject() final {
    return outer_tie_->GetExternalObject();
  }

 private:
  static std::pair<TensorTieDef, TensorTieDef> MakeOuterInnerDefs(
      const TensorTieDef& def) {
    TensorTieDef outer_def;
    outer_def.external_def = def.external_def;
    outer_def.internal_def = def.external_def;
    outer_def.internal_def.object_def.object_type = ObjectType::OPENCL_BUFFER;
    outer_def.internal_def.object_def.user_provided = true;

    TensorTieDef inner_def;
    inner_def.external_def = outer_def.internal_def;
    inner_def.external_def.object_def.user_provided = false;
    inner_def.internal_def = def.internal_def;
    return std::make_pair(outer_def, inner_def);
  }

  Status Init(TensorObject internal_object,
              TensorObjectConverterBuilder* converter_builder,
              Environment* env) {
    auto defs = MakeOuterInnerDefs(def());
    RETURN_IF_ERROR(DefaultTensorTie::New(defs.second, internal_object,
                                          converter_builder, env, &inner_tie_));
    return DefaultTensorTie::New(defs.first, inner_tie_->GetExternalObject(),
                                 converter_builder, env, &outer_tie_);
  }

  std::unique_ptr<TensorTie> inner_tie_;
  std::unique_ptr<TensorTie> outer_tie_;
};

// Captures GL object into CL context before performing a conversion.
class GlBufferHolder : public TensorTie {
 public:
  GlBufferHolder(const TensorTieDef& def, GlInteropFabric* gl_interop_fabric,
                 Environment* env)
      : TensorTie(def),
        gl_interop_fabric_(gl_interop_fabric),
        environment_(env) {}

  static bool IsSupported(
      const TensorTieDef& def,
      const TensorObjectConverterBuilder& converter_builder) {
    if (!def.external_def.object_def.user_provided ||
        def.external_def.object_def.object_type != ObjectType::OPENGL_SSBO) {
      return false;
    }
    return DefaultTensorTie::IsSupported(MakeClDef(def), converter_builder);
  }

  static Status New(const TensorTieDef& def, TensorObject internal_object,
                    TensorObjectConverterBuilder* converter_builder,
                    GlInteropFabric* gl_interop_fabric, Environment* env,
                    std::unique_ptr<TensorTie>* tie) {
    auto tie_impl =
        absl::make_unique<GlBufferHolder>(def, gl_interop_fabric, env);
    RETURN_IF_ERROR(DefaultTensorTie::New(MakeClDef(def), internal_object,
                                          converter_builder, env,
                                          &tie_impl->tie_));
    *tie = std::move(tie_impl);
    return OkStatus();
  }

  Status SetExternalObject(TensorObject obj) final {
    auto ssbo = absl::get_if<OpenGlBuffer>(&obj);
    if (!ssbo) {
      return InvalidArgumentError("Missing OpenGL SSBO");
    }
    auto old_ssbo = absl::get_if<OpenGlBuffer>(&external_obj_);
    if (old_ssbo && ssbo->id == old_ssbo->id) {
      return OkStatus();
    }
    if (cl_object_.memory()) {
      gl_interop_fabric_->UnregisterMemory(cl_object_.memory());
    }
    RETURN_IF_ERROR(CreateClMemoryFromGlBuffer(
        ssbo->id, def().access_type, &environment_->context(), &cl_object_));
    external_obj_ = obj;
    RETURN_IF_ERROR(tie_->SetExternalObject(OpenClBuffer{cl_object_.memory()}));
    gl_interop_fabric_->RegisterMemory(cl_object_.memory());
    return OkStatus();
  }

  TensorObject GetExternalObject() final { return external_obj_; }

  Status CopyFromExternalObject() final {
    return tie_->CopyFromExternalObject();
  }

  Status CopyToExternalObject() final { return tie_->CopyToExternalObject(); }

 private:
  static TensorTieDef MakeClDef(const TensorTieDef& def) {
    auto cl_def = def;
    cl_def.external_def.object_def.object_type = ObjectType::OPENCL_BUFFER;
    cl_def.external_def.object_def.user_provided = true;
    return cl_def;
  }

  CLMemory cl_object_;
  GlInteropFabric* gl_interop_fabric_;
  Environment* environment_;
  std::unique_ptr<TensorTie> tie_;
  TensorObject external_obj_;
};

TensorObject TensorToObj(const Tensor& tensor) {
  if (tensor.StorageType() == TensorStorageType::BUFFER) {
    return OpenClBuffer{tensor.GetMemoryPtr()};
  }
  if (tensor.StorageType() == TensorStorageType::IMAGE_BUFFER) {
    return OpenClBuffer{tensor.GetMemoryPtrForWriting()};
  }
  return OpenClTexture{tensor.GetMemoryPtr()};
}

// Responsible for creating new tensor objects.
class TensorTieFactory {
 public:
  TensorTieFactory(Environment* env, InferenceContext* context,
                   GlInteropFabric* gl_interop_fabric)
      : env_(*env),
        context_(*context),
        gl_interop_fabric_(gl_interop_fabric),
        converter_builder_(NewConverterBuilder(env)) {}

  bool IsSupported(const TensorTieDef& def) const {
    return IsValid(def.external_def.object_def) &&
           (NoopTensorTie::IsSupported(def) ||
            DefaultTensorTie::IsSupported(def, *converter_builder_) ||
            GlBufferHolder::IsSupported(def, *converter_builder_) ||
            TwoStepTensorTie::IsSupported(def, *converter_builder_));
  }

  Status NewTensorTie(const TensorTieDef& def,
                      std::unique_ptr<TensorTie>* tie) {
    TensorObject internal_object = TensorToObj(*context_.GetTensor(def.id));
    auto converter = converter_builder_.get();
    if (NoopTensorTie::IsSupported(def)) {
      *tie = absl::make_unique<NoopTensorTie>(def, internal_object);
      return OkStatus();
    }
    if (DefaultTensorTie::IsSupported(def, *converter)) {
      return DefaultTensorTie::New(def, internal_object, converter, &env_, tie);
    }
    if (GlBufferHolder::IsSupported(def, *converter)) {
      if (!gl_interop_fabric_) {
        return InvalidArgumentError(
            "GL object is used but InferenceEnvironmentOptions does not have "
            "EGL display and context set.");
      }
      return GlBufferHolder::New(def, internal_object, converter,
                                 gl_interop_fabric_, &env_, tie);
    }
    if (TwoStepTensorTie::IsSupported(def, *converter)) {
      return TwoStepTensorTie::New(def, internal_object, converter, &env_, tie);
    }
    return UnimplementedError("Unsupported tensor tie definition.");
  }

 private:
  Environment& env_;
  InferenceContext& context_;
  GlInteropFabric* gl_interop_fabric_;
  std::unique_ptr<TensorObjectConverterBuilder> converter_builder_;
};

class InferenceRunnerImpl : public InferenceRunner {
 public:
  InferenceRunnerImpl(Environment* environment,
                      std::unique_ptr<InferenceContext> context,
                      std::unique_ptr<GlInteropFabric> gl_interop_fabric)
      : queue_(environment->queue()),
        context_(std::move(context)),
        gl_interop_fabric_(std::move(gl_interop_fabric)) {}

  Status Initialize(const std::vector<TensorTieDef>& inputs,
                    const std::vector<TensorTieDef>& outputs,
                    TensorTieFactory* factory) {
    RETURN_IF_ERROR(LinkTensors(inputs, factory, &inputs_));
    return LinkTensors(outputs, factory, &outputs_);
  }

  std::vector<TensorObjectDef> inputs() const override {
    return GetExternalDefinitions(inputs_);
  }

  std::vector<TensorObjectDef> outputs() const override {
    return GetExternalDefinitions(outputs_);
  }

  Status GetInputObject(int index, TensorObject* object) override {
    if (index < 0 || index >= inputs_.size()) {
      return OutOfRangeError("Index is out of range");
    }
    *object = inputs_[index]->GetExternalObject();
    return OkStatus();
  }

  Status GetOutputObject(int index, TensorObject* object) override {
    if (index < 0 || index >= outputs_.size()) {
      return OutOfRangeError("Index is out of range");
    }
    *object = outputs_[index]->GetExternalObject();
    return OkStatus();
  }

  Status SetInputObject(int index, TensorObject object) override {
    if (index < 0 || index >= inputs_.size()) {
      return OutOfRangeError("Index is out of range");
    }
    return inputs_[index]->SetExternalObject(object);
  }

  Status SetOutputObject(int index, TensorObject object) override {
    if (index < 0 || index >= outputs_.size()) {
      return OutOfRangeError("Index is out of range");
    }
    return outputs_[index]->SetExternalObject(object);
  }

  Status Run() override {
    if (gl_interop_fabric_) {
      RETURN_IF_ERROR(gl_interop_fabric_->Start());
    }
    for (auto& obj : inputs_) {
      RETURN_IF_ERROR(obj->CopyFromExternalObject());
    }
    RETURN_IF_ERROR(context_->AddToQueue(queue_));
    clFlush(queue_->queue());
    for (auto& obj : outputs_) {
      RETURN_IF_ERROR(obj->CopyToExternalObject());
    }
    if (gl_interop_fabric_) {
      RETURN_IF_ERROR(gl_interop_fabric_->Finish());
    }
    return OkStatus();
  }

 private:
  static Status LinkTensors(const std::vector<TensorTieDef>& defs,
                            TensorTieFactory* factory,
                            std::vector<std::unique_ptr<TensorTie>>* objects) {
    objects->reserve(defs.size());
    for (auto& def : defs) {
      std::unique_ptr<TensorTie> object;
      RETURN_IF_ERROR(factory->NewTensorTie(def, &object));
      objects->push_back(std::move(object));
    }
    return OkStatus();
  }

  static std::vector<TensorObjectDef> GetExternalDefinitions(
      const std::vector<std::unique_ptr<TensorTie>>& objects) {
    std::vector<TensorObjectDef> defs;
    defs.reserve(objects.size());
    for (auto& obj : objects) {
      defs.push_back(obj->def().external_def);
    }
    return defs;
  }

  CLCommandQueue* queue_;
  std::unique_ptr<InferenceContext> context_;
  std::unique_ptr<GlInteropFabric> gl_interop_fabric_;
  std::vector<std::unique_ptr<TensorTie>> inputs_;
  std::vector<std::unique_ptr<TensorTie>> outputs_;
};

TensorObjectDef TensorToDef(const Tensor& tensor) {
  TensorObjectDef def;
  def.dimensions.b = 1;
  def.dimensions.h = tensor.Height();
  def.dimensions.w = tensor.Width();
  def.dimensions.c = tensor.Channels();
  def.object_def.data_layout = ToDataLayout(tensor.StorageType());
  def.object_def.data_type = tensor.DataType();
  def.object_def.object_type = ToObjectType(tensor.StorageType());
  def.object_def.user_provided = false;
  return def;
}

class InferenceBuilderImpl : public InferenceBuilder {
 public:
  explicit InferenceBuilderImpl(Environment* environment)
      : environment_(environment) {}

  Status Initialize(const InferenceOptions& options,
                    const InferenceEnvironmentOptions& env_options,
                    const GraphFloat32& graph) {
    context_ = absl::make_unique<InferenceContext>();
    InferenceContext::CreateInferenceInfo create_info;
    create_info.precision = GetPrecision(options);
    create_info.storage_type = GetStorageType(options);
    if (options.usage == InferenceUsage::FAST_SINGLE_ANSWER) {
      create_info.hints.Add(ModelHints::kReduceKernelsCount);
      create_info.hints.Add(ModelHints::kFastTuning);
    }
    RETURN_IF_ERROR(context_->InitFromGraph(create_info, graph, environment_));

    if (env_options.IsGlAware()) {
      gl_interop_fabric_ = absl::make_unique<GlInteropFabric>(
          env_options.egl_display, environment_);
    }
    tie_factory_ = absl::make_unique<TensorTieFactory>(
        environment_, context_.get(), gl_interop_fabric_.get());

    inputs_ = LinkTensors(graph, graph.inputs());
    outputs_ = LinkTensors(graph, graph.outputs());
    return OkStatus();
  }

  std::vector<TensorObjectDef> inputs() const override {
    return GetExternalDefinitions(inputs_);
  }

  std::vector<TensorObjectDef> outputs() const override {
    return GetExternalDefinitions(outputs_);
  }

  Status SetInputShape(int index, const Dimensions& dimensions) override {
    if (index < 0 || index >= inputs_.size()) {
      return OutOfRangeError("Index is out of range");
    }
    return UnimplementedError("Changing input shapes is not supported");
  }

  Status SetInputObjectDef(int index, ObjectDef new_def) override {
    if (index < 0 || index >= inputs_.size()) {
      return OutOfRangeError("Index is out of range");
    }
    auto def = inputs_[index];
    def.external_def.object_def = new_def;
    if (!tie_factory_->IsSupported(def)) {
      return InvalidArgumentError("New object definition is not supported.");
    }
    inputs_[index] = def;
    return OkStatus();
  }

  Status SetOutputObjectDef(int index, ObjectDef new_def) override {
    if (index < 0 || index >= outputs_.size()) {
      return OutOfRangeError("Index is out of range");
    }
    auto def = outputs_[index];
    def.external_def.object_def = new_def;
    if (!tie_factory_->IsSupported(def)) {
      return InvalidArgumentError("New object definition is not supported.");
    }
    outputs_[index] = def;
    return OkStatus();
  }

  Status Build(std::unique_ptr<InferenceRunner>* runner) override {
    if (gl_interop_fabric_ && !HasGlObjects()) {
      // destroy interop layer when there are no GL objects to avoid
      // extra synchronization cost.
      gl_interop_fabric_.reset(nullptr);
    }
    auto runner_impl = absl::make_unique<InferenceRunnerImpl>(
        environment_, std::move(context_), std::move(gl_interop_fabric_));
    RETURN_IF_ERROR(
        runner_impl->Initialize(inputs_, outputs_, tie_factory_.get()));
    *runner = std::move(runner_impl);
    return OkStatus();
  }

 private:
  TensorStorageType GetStorageType(const InferenceOptions& options) const {
    // Fallback to BUFFER that should be supported by default.
    std::vector<TensorStorageType> preferred_storage_types;
    if (GetRelativeImportance(options, InferencePriority::MIN_LATENCY,
                              InferencePriority::MIN_MEMORY_USAGE) ==
        PriorityImportance::HIGHER) {
      preferred_storage_types = {GetFastestStorageType(environment_->device()),
                                 TensorStorageType::BUFFER};
    } else {
      preferred_storage_types = {TensorStorageType::IMAGE_BUFFER,
                                 GetFastestStorageType(environment_->device()),
                                 TensorStorageType::BUFFER};
    }

    for (TensorStorageType storage_type : preferred_storage_types) {
      if (environment_->IsSupported(storage_type)) {
        return storage_type;
      }
    }
    return TensorStorageType::UNKNOWN;
  }

  CalculationsPrecision GetPrecision(const InferenceOptions& options) const {
    CalculationsPrecision precision;
    switch (GetPosition(options, InferencePriority::MAX_PRECISION)) {
      case 1:
        precision = CalculationsPrecision::F32;
        break;
      case 2:
        precision = CalculationsPrecision::F32_F16;
        break;
      case 3:
        precision = CalculationsPrecision::F16;
        break;
      default:
        precision = CalculationsPrecision::F16;
        break;
    }
    // Increase precision if lower precision is not supported.
    if (!environment_->IsSupported(precision)) {
      precision = CalculationsPrecision::F32_F16;
      if (!environment_->IsSupported(precision)) {
        precision = CalculationsPrecision::F32;
      }
    }
    return precision;
  }

  // Links internal tensors with external user-facing objects.
  std::vector<TensorTieDef> LinkTensors(
      const GraphFloat32& graph,
      const std::vector<Value<TensorRef<BHWC>>*>& values) {
    std::vector<TensorTieDef> links;
    links.reserve(values.size());
    for (const auto& value : values) {
      TensorObjectDef def = TensorToDef(*context_->GetTensor(value->id));
      AccessType access =
          graph.IsGraphInput(value->id) ? AccessType::READ : AccessType::WRITE;
      links.push_back({value->id, access, def, def});
    }
    return links;
  }

  bool HasGlObjects() const {
    auto is_gl = [](ObjectType t) {
      return t == ObjectType::OPENGL_SSBO || t == ObjectType::OPENGL_TEXTURE;
    };
    for (const TensorTieDef& def : inputs_) {
      if (is_gl(def.external_def.object_def.object_type)) {
        return true;
      }
    }
    for (const TensorTieDef& def : outputs_) {
      if (is_gl(def.external_def.object_def.object_type)) {
        return true;
      }
    }
    return false;
  }

  static std::vector<TensorObjectDef> GetExternalDefinitions(
      const std::vector<TensorTieDef>& links) {
    std::vector<TensorObjectDef> defs;
    defs.reserve(links.size());
    for (auto& desc : links) {
      defs.push_back(desc.external_def);
    }
    return defs;
  }

  std::unique_ptr<InferenceContext> context_;
  std::unique_ptr<GlInteropFabric> gl_interop_fabric_;
  Environment* environment_;

  std::vector<TensorTieDef> inputs_;
  std::vector<TensorTieDef> outputs_;
  std::unique_ptr<TensorTieFactory> tie_factory_;
};

class InferenceEnvironmentImpl : public InferenceEnvironment {
 public:
  explicit InferenceEnvironmentImpl(const InferenceEnvironmentOptions& options)
      : options_(options) {}

  Status Init() {
    RETURN_IF_ERROR(LoadOpenCL());
    properties_.is_opencl_available = true;

    CLDevice device;
    if (options_.device) {
      cl_platform_id platform;
      RETURN_IF_ERROR(GetDeviceInfo<cl_platform_id>(
          options_.device, CL_DEVICE_PLATFORM, &platform));
      device = CLDevice(options_.device, platform);
    } else {
      RETURN_IF_ERROR(CreateDefaultGPUDevice(&device));
    }

    properties_.is_gl_sharing_supported = IsGlSharingSupported(device);
    properties_.is_gl_to_cl_fast_sync_supported =
        IsClEventFromEglSyncSupported(device);
    properties_.is_cl_to_gl_fast_sync_supported =
        IsEglSyncFromClEventSupported();
    if (options_.IsGlAware() && !properties_.is_gl_sharing_supported) {
      return UnavailableError("GL sharing is not supported");
    }

    CLContext context;
    if (options_.context) {
      if (options_.IsGlAware()) {
        return InvalidArgumentError(
            "OpenCL context and EGL parameters are set in the same time.");
      }
      context = CLContext(options_.context, /* has_ownership = */ false);
    } else {
      if (options_.IsGlAware()) {
        RETURN_IF_ERROR(CreateCLGLContext(
            device,
            reinterpret_cast<cl_context_properties>(options_.egl_context),
            reinterpret_cast<cl_context_properties>(options_.egl_display),
            &context));
      } else {
        RETURN_IF_ERROR(CreateCLContext(device, &context));
      }
    }

    CLCommandQueue queue;
    if (options_.command_queue) {
      queue =
          CLCommandQueue(options_.command_queue, /* has_ownership = */ false);
    } else {
      RETURN_IF_ERROR(CreateCLCommandQueue(device, context, &queue));
    }
    // Profiling queue is used for workgroup size tuning.
    ProfilingCommandQueue profiling_queue;
    RETURN_IF_ERROR(
        CreateProfilingCommandQueue(device, context, &profiling_queue));
    environment_ = Environment(std::move(device), std::move(context),
                               std::move(queue), std::move(profiling_queue));
    return environment_.Init();
  }

  Status NewInferenceBuilder(const InferenceOptions& options,
                             GraphFloat32 model,
                             std::unique_ptr<InferenceBuilder>* builder) final {
    if (!IsValid(options)) {
      return InvalidArgumentError("InferenceOptions are invalid.");
    }
    InferenceOptions resolved_options = options;
    ResolveAutoPriority(&resolved_options);
    if (environment_.program_cache() &&
        !options_.serialized_binary_cache.empty()) {
      // Ignore returned error. Cache is discarded.
      environment_.program_cache()
          ->AddSerializedCache(environment_.context(), environment_.device(),
                               options_.serialized_binary_cache)
          .IgnoreError();
    }

    RETURN_IF_ERROR(RunGraphTransforms(&model));
    auto builder_impl = absl::make_unique<InferenceBuilderImpl>(&environment_);
    RETURN_IF_ERROR(
        builder_impl->Initialize(resolved_options, options_, model));
    *builder = std::move(builder_impl);
    return OkStatus();
  }

  std::vector<uint8_t> GetSerializedBinaryCache() const final {
    std::vector<uint8_t> data;
    // Is there was a problem, data would be empty.
    environment_.program_cache()
        ->GetSerializedCache(environment_.device(), &data)
        .IgnoreError();
    return data;
  }

  const InferenceEnvironmentProperties& properties() const {
    return properties_;
  }

 private:
  const InferenceEnvironmentOptions options_;
  Environment environment_;
  InferenceEnvironmentProperties properties_;
};

}  // namespace

Status NewInferenceEnvironment(
    const InferenceEnvironmentOptions& options,
    std::unique_ptr<InferenceEnvironment>* environment,
    InferenceEnvironmentProperties* properties) {
  auto env_impl = absl::make_unique<InferenceEnvironmentImpl>(options);
  Status status = env_impl->Init();
  if (properties) {
    *properties = env_impl->properties();
  }
  RETURN_IF_ERROR(status);
  *environment = std::move(env_impl);
  return OkStatus();
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
