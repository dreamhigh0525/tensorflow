/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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
#include <algorithm>
#include <memory>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tensorflow/compiler/tf2tensorrt/common/utils.h"
#include "tensorflow/compiler/tf2tensorrt/convert/convert_nodes.h"
#include "tensorflow/compiler/tf2tensorrt/convert/utils.h"
#include "tensorflow/compiler/tf2tensorrt/utils/trt_allocator.h"
#include "tensorflow/compiler/tf2tensorrt/utils/trt_engine_utils.h"
#include "tensorflow/compiler/tf2tensorrt/utils/trt_logger.h"
#include "tensorflow/compiler/tf2tensorrt/utils/trt_lru_cache.h"
#include "tensorflow/compiler/tf2tensorrt/utils/trt_shape_optimization_profiles.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/graph_constructor.h"
#include "tensorflow/core/common_runtime/graph_optimizer.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/graph/algorithm.h"
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/stream_executor.h"
#include "tensorflow/core/platform/thread_annotations.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/env_var.h"
#include "tensorflow/stream_executor/lib/statusor.h"

#if GOOGLE_CUDA && GOOGLE_TENSORRT
#include "third_party/gpus/cuda/include/cuda_runtime_api.h"
#include "third_party/tensorrt/NvInfer.h"

namespace tensorflow {
namespace tensorrt {
static Logger logger;
using absl::StrAppend;
using absl::StrCat;
using ::nvinfer1::IRuntime;
using ::stream_executor::port::StatusOr;

#define LOG_FIRST_FEW_WARNING_WITH_PREFIX \
  LOG_FIRST_N(WARNING, 5) << "TF-TRT Warning: "

// A helper class to call done() when destructed for asynchronous execution.
// Helps simultaneous execution of native and TRT engines.

class AsyncHelper : public core::RefCounted {
 public:
  AsyncHelper(AsyncOpKernel::DoneCallback done) : done_(done) {}

  ~AsyncHelper() override { this->operator()(); }

  void operator()() {
    if (!called_) {
      done_();
      called_ = true;
    }
  }

 private:
  AsyncOpKernel::DoneCallback done_;
  bool called_ = false;  // Has `done_` been called?
};

//  This OP can construct TRTEngine on the fly and if construction of engine
//  fails, executes equivalent subgraph as a TensorFlow function.
class TRTEngineOp : public AsyncOpKernel {
 public:
  explicit TRTEngineOp(OpKernelConstruction* context);

  void ComputeAsync(OpKernelContext* context,
                    AsyncOpKernel::DoneCallback done) override;

 private:
  using CacheType =
      LRUCache<std::vector<TensorShape>, std::unique_ptr<EngineContext>,
               VectorTensorShapeHasher>;

  // Executes calibration.
  void ExecuteCalibration(OpKernelContext* ctx,
                          TRTEngineCacheResource* cache_res,
                          AsyncHelper* helper);

  // Constructs a function handle for the segment of the TRTEngineOp.
  StatusOr<FunctionLibraryRuntime::Handle> ConstructFunctionHandle(
      FunctionLibraryRuntime* lib, const string& device_name,
      bool allow_soft_placement = false, size_t num_inputs = 0,
      size_t num_outputs = 0);

  // Imports the GraphDef for the segment of the TRTEngineOp to
  // segment_graph_def_.
  Status ImportSegmentGraphDef(FunctionLibraryRuntime* lib,
                               const string& device_name);

  // Executes replaced native segment as function Op.
  void ExecuteNativeSegment(OpKernelContext* ctx, AsyncHelper* helper);

  // Executes the tensorrt engine. Returns whether we need to retry by running
  // the native segment.
  Status ExecuteTrtEngine(OpKernelContext* ctx, EngineContext* engine_context,
                          int trt_context_idx);

  // Allocates necessary resources for calibration.
  Status AllocateCalibrationResources(OpKernelContext* ctx,
                                      TRTEngineCacheResource* cache_res);

  Status GetEngineCacheResource(OpKernelContext* ctx,
                                TRTEngineCacheResource** cache_res);

  // Returns a pair of 1) An EngineContext object that is compatible with the
  // input and 2) The index of the IExecutionContext compatible with the input.
  StatusOr<std::pair<EngineContext*, int>> GetEngine(
      const std::vector<TensorShape>& input_concrete_shapes,
      OpKernelContext* ctx, TRTEngineCacheResource* cache_resource);

  // Builds and returns a cuda engine for the input shapes. If building the
  // engine fails, enters a dummy entry into the cache_resource cache so we
  // don't continually try to build the same failing engine.
  StatusOr<TrtUniquePtrType<nvinfer1::ICudaEngine>> BuildEngine(
      const std::vector<TensorShape>& input_concrete_shapes, int batch_size,
      bool use_calibration, TRTInt8Calibrator* calibrator,
      TRTEngineCacheResource* cache_resource);

  // Verify that the input shapes are consistent and can be handled by this op.
  Status VerifyInputShapes(const std::vector<TensorShape>& shapes);

  std::vector<string> input_nodes_;
  std::vector<string> output_nodes_;

  // serialized protobuf segment or trt engine depending on static_engine_ flag.
  string serialized_segment_;

  // The function for TF native execution of the segment.
  NameAttrList func_;

  // GraphDef representation of the segment.
  GraphDef segment_graph_def_;

  // Engine Precision mode.
  TrtPrecisionMode precision_mode_;

  // Whether engine is constructed during the conversion or needs to be
  // constructed from protobuf segment.
  bool static_engine_;

  // Whether to calibrate INT8 engine.
  bool calibration_mode_;

  // Whether to use implicit batch dimension for TensorRT.
  bool use_implicit_batch_;

  // Whether to collect optimization profiles for TensorRT, only used when
  // use_implicit_batch_=false.
  bool profile_generation_mode_;

  // Whether to build TensorRT engines at runtime.
  bool allow_build_at_runtime_;

  // Whether to allow soft placement when the graph is executed with native
  // TensorFlow.
  bool allow_soft_placement_;

  // Maximum number of cached engines.
  int max_cached_engines_;

  int64 workspace_size_;
  mutex engine_mutex_;
  FunctionLibraryRuntime::Handle native_execution_func_handle_;

  // The finalized calibrator for inference.
  std::unique_ptr<TRTInt8Calibrator> calibrator_;

  // If true, create calibration graph for INT8 mode. Otherwise, we are using
  // user-provided quantization ranges.
  bool use_calibration_;

  // Array of all input shapes, collected from the input_shapes attribute when
  // constructing the TRTEngineOp. The input_shapes attribute is set during
  // graph conversion time. This data is used to retrieve which input dimensions
  // could be unknown. During inference time this information is not available
  // otherwise (all shapes are known (concrete) shapes when we run inference).
  std::vector<PartialTensorShape> input_partial_shapes_;
};

#define TYPECASE(dt, X, Y)                                    \
  case dt: {                                                  \
    return (void*)X->flat<EnumToDataType<dt>::Type>().data(); \
  }

void* GetTensorAddress(const Tensor* tensor_ptr) {
  auto tensor_type = tensor_ptr->dtype();
  switch (tensor_type) {
    TYPECASE(DT_FLOAT, tensor_ptr, dest_ptr);
    TYPECASE(DT_HALF, tensor_ptr, dest_ptr);
    TYPECASE(DT_INT8, tensor_ptr, dest_ptr);
    TYPECASE(DT_INT32, tensor_ptr, dest_ptr);
    default: {
      LOG(ERROR) << "Unsupported Data type " << DataTypeString(tensor_type);
      return nullptr;
    }
  }
}

static Status FunctionDefToGraphDef(FunctionLibraryRuntime::Handle handle,
                                    FunctionLibraryRuntime* flib_runtime,
                                    GraphDef* graph_def) {
  const FunctionLibraryDefinition* flib_def =
      flib_runtime->GetFunctionLibraryDefinition();
  const FunctionBody* fbody;
  fbody = flib_runtime->GetFunctionBody(handle);
  if (!fbody) {
    return errors::Internal(
        "Function body is null when converting from FuncDef to GraphDef.");
  }
  std::unique_ptr<Graph> graph(new Graph(flib_def));
  CopyGraph(*fbody->graph, graph.get());

  auto replace_name = [](const char* const prefix, string* name) {
    if (absl::StartsWith(*name, absl::AsciiStrToLower(prefix))) {
      name->replace(0, strlen(prefix), prefix);
      return true;
    }
    return false;
  };
  graph->ToGraphDef(graph_def);
  // GraphToFunctionDef() will convert all the node names to lowercase.
  for (auto& node : *graph_def->mutable_node()) {
    if (!replace_name(IONamePrefixes::kInputPHName, node.mutable_name())) {
      if (replace_name(IONamePrefixes::kOutputPHName, node.mutable_name())) {
        // Instantiation of the function will append _RetVal to the node name,
        // need to remove it for backward compatibility.
        const char* const suffix_to_remove = "_RetVal";
        if (absl::EndsWith(node.name(), suffix_to_remove)) {
          node.mutable_name()->erase(node.name().size() -
                                     strlen(suffix_to_remove));
        }
      }
    }
    for (auto& input : *node.mutable_input()) {
      if (!replace_name(IONamePrefixes::kInputPHName, &input)) {
        replace_name(IONamePrefixes::kOutputPHName, &input);
      }
    }
  }
  return Status::OK();
}

StatusOr<FunctionLibraryRuntime::Handle> TRTEngineOp::ConstructFunctionHandle(
    FunctionLibraryRuntime* lib, const string& device_name,
    bool allow_soft_placement, size_t num_inputs, size_t num_outputs) {
  VLOG(1) << "Constructing function handle";
  if (lib == nullptr) {
    return errors::Internal("Context function library is null");
  }
  FunctionLibraryRuntime::InstantiateOptions inst_ops;
  inst_ops.state_handle = "";
  inst_ops.target = device_name;
  if (allow_soft_placement) {
    const FunctionDef* fdef =
        lib->GetFunctionLibraryDefinition()->Find(func_.name());
    if (!fdef) {
      return errors::Internal(
          StrCat("Cann't find FunctionDef for", func_.name()));
    }
    bool ints_on_device =
        fdef->attr().count(FunctionLibraryDefinition::kIntsOnDeviceAttr) != 0 &&
        fdef->attr().at(FunctionLibraryDefinition::kIntsOnDeviceAttr).b();
    // kIntsOnDeviceAttr is not compatible with is_multi_device_function which
    // is needed to support allow_soft_placement.
    if (ints_on_device) {
      LOG_FIRST_FEW_WARNING_WITH_PREFIX
          << "Function " << name()
          << " has attribute kIntsOnDeviceAttr=true "
             "and will be executed natively with allow_soft_placement=false. "
             "If this is a problem, please re-generate your SavedModel with "
             "the TF-TRT runtime you are using.";
    } else {
      inst_ops.is_multi_device_function = true;
      inst_ops.input_devices.resize(num_inputs, device_name);
      inst_ops.output_devices.resize(num_outputs, device_name);
      inst_ops.config_proto.set_allow_soft_placement(true);
    }
  }
  FunctionLibraryRuntime::Handle func_handle;
  Status status = lib->Instantiate(func_.name(), AttrSlice(&func_.attr()),
                                   inst_ops, &func_handle);
  if (status.ok()) {
    return func_handle;
  }
  return status;
}

Status TRTEngineOp::ImportSegmentGraphDef(FunctionLibraryRuntime* lib,
                                          const string& device_name) {
  TF_ASSIGN_OR_RETURN(FunctionLibraryRuntime::Handle func_handle,
                      ConstructFunctionHandle(lib, device_name));
  return FunctionDefToGraphDef(func_handle, lib, &segment_graph_def_);
}

TRTEngineOp::TRTEngineOp(OpKernelConstruction* context)
    : AsyncOpKernel(context) {
  // read serialized_engine
  OP_REQUIRES_OK(context,
                 context->GetAttr("serialized_segment", &serialized_segment_));
  OP_REQUIRES_OK(context,
                 context->GetAttr("workspace_size_bytes", &workspace_size_));
  OP_REQUIRES_OK(context, context->GetAttr("static_engine", &static_engine_));

  VLOG(1) << "Constructing " << name();
  string precision_string;
  OP_REQUIRES_OK(context,
                 context->GetAttr("precision_mode", &precision_string));
  string calibration_data;
  OP_REQUIRES_OK(context,
                 context->GetAttr("calibration_data", &calibration_data));
  OP_REQUIRES_OK(context, context->GetAttr("segment_func", &func_));
  OP_REQUIRES(context, !func_.name().empty(),
              errors::InvalidArgument(
                  "The TF function for the TRT segment could not be empty"));
  OP_REQUIRES_OK(context,
                 TrtPrecisionModeFromName(precision_string, &precision_mode_));
  OP_REQUIRES_OK(context,
                 context->GetAttr("use_calibration", &use_calibration_));
  OP_REQUIRES_OK(context,
                 context->GetAttr("input_shapes", &input_partial_shapes_));
  auto status =
      context->GetAttr("_allow_build_at_runtime", &allow_build_at_runtime_);
  if (status.code() == tensorflow::error::NOT_FOUND) {
    VLOG(2) << "Not found _allow_build_at_runtime in "
            << context->device()->name()
            << ", thus setting _allow_build_at_runtime=true";
    allow_build_at_runtime_ = true;
  } else {
    OP_REQUIRES_OK(context, status);
  }

  status = context->GetAttr("_allow_soft_placement", &allow_soft_placement_);
  if (status.code() == tensorflow::error::NOT_FOUND) {
    allow_soft_placement_ = true;
  } else {
    OP_REQUIRES_OK(context, status);
  }

  native_execution_func_handle_ = kInvalidHandle;
  if (!static_engine_) {
    OP_REQUIRES_OK(context, ImportSegmentGraphDef(context->function_library(),
                                                  context->device()->name()));
  }
  // TODO(laigd): calibration_data is used in TF v1.x and we keep it only for
  // backward compatibility reasons. Remove it once all known users switch to
  // 2.0.
  calibration_mode_ =
      (use_calibration_ && precision_mode_ == TrtPrecisionMode::INT8 &&
       calibration_data.empty());
  if (!calibration_data.empty()) {
    calibrator_.reset(new TRTInt8Calibrator(calibration_data));
    calibration_data.resize(0);
  }
  OP_REQUIRES_OK(context, context->GetAttr("max_cached_engines_count",
                                           &max_cached_engines_));

  status = context->GetAttr("_use_implicit_batch", &use_implicit_batch_);
  if (status.code() == tensorflow::error::NOT_FOUND) {
    VLOG(2) << "Not found _use_implicit_batch in " << context->device()->name()
            << ", thus setting _use_implicit_batch=true";
    use_implicit_batch_ = true;
  }
#if !IS_TRT_VERSION_GE(6, 0, 0, 0)
  if (!use_implicit_batch_) {
    VLOG(2) << "Need at least TensorRT 6.0 for explicit batch mode. Setting "
            << "_use_implicit_batch=true";
    use_implicit_batch_ = true;
  }
#endif
  status =
      context->GetAttr("_profile_generation_mode", &profile_generation_mode_);
  if (status.code() == tensorflow::error::NOT_FOUND) {
    VLOG(2) << "Not found _profile_generation_mode in "
            << context->device()->name()
            << ", thus setting _profile_generation_mode=false";
    profile_generation_mode_ = false;
  }
  if (use_implicit_batch_) {
    OP_REQUIRES(context, !profile_generation_mode_,
                errors::InvalidArgument(
                    "profile_generation_mode_=true is only supported if "
                    "use_implicit_batch=false"));
    if (input_partial_shapes_.empty()) {
      VLOG(1) << "Attribute input_shapes is not set. This happens probably "
              << "because you are using a model that is already converted "
              << "to TensorRT with a previous version of TF-TRT (i.e. includes "
              << "TRTEngineOp in graph). This is not an error. If you convert "
              << "the original model again to TensorRT, the attributes "
              << "input_shapes will be set automatically.";
    }
  } else {
    OP_REQUIRES(
        context, !input_partial_shapes_.empty(),
        errors::InvalidArgument(
            "Explicit batch mode requires attribute input_shapes to be set."
            "If you are using a model that was converted to TensorRT by a "
            "previous version of TF-TRT, (i.e. includes TRTEngineOp in graph "
            "without the input_shapes attribute), then you need to convert the "
            "original model again to TensorRT in order to set the attribute "
            "input_shapes."));
    OP_REQUIRES(context, !calibration_mode_,
                errors::InvalidArgument(
                    "Explicit batch mode does not support calibration"));
  }
}

void TRTEngineOp::ExecuteNativeSegment(OpKernelContext* ctx,
                                       AsyncHelper* helper) {
  std::vector<Tensor> inputs;
  std::vector<Tensor>* outputs = new std::vector<Tensor>();
  if (native_execution_func_handle_ == kInvalidHandle) {
    StatusOr<FunctionLibraryRuntime::Handle> status_or_handle =
        ConstructFunctionHandle(ctx->function_library(), ctx->device()->name(),
                                allow_soft_placement_, ctx->num_inputs(),
                                ctx->num_outputs());
    OP_REQUIRES_OK_ASYNC(ctx, status_or_handle.status(), *helper);
    native_execution_func_handle_ = status_or_handle.ValueOrDie();
  }
  auto lib = ctx->function_library();
  FunctionLibraryRuntime::Options opts;
  opts.rendezvous = ctx->rendezvous();
  opts.cancellation_manager = ctx->cancellation_manager();
  opts.runner = ctx->runner();
  inputs.reserve(ctx->num_inputs());
  for (int i = 0; i < ctx->num_inputs(); i++) {
    inputs.push_back(ctx->input(i));
  }
  helper->Ref();  // Increment count for calculating native graph
  VLOG(1) << "Executing native segment: " << name();
  lib->Run(opts, native_execution_func_handle_, inputs, outputs,
           [this, ctx, outputs, helper](const Status& s) {
             core::ScopedUnref sc(helper);
             OP_REQUIRES_OK_ASYNC(ctx, s, *helper);
             VLOG(1) << "Native Segment completed";
             for (size_t t = 0; t < outputs->size(); ++t) {
               ctx->set_output(t, outputs->at(t));
             }
             delete outputs;
           });
}

void TRTEngineOp::ExecuteCalibration(OpKernelContext* ctx,
                                     TRTEngineCacheResource* cache_res,
                                     AsyncHelper* helper) {
  VLOG(1) << "Executing TRT calibration: " << name();
  helper->Ref();
  core::ScopedUnref sc(helper);

  CalibrationContext* calib_ctx = cache_res->calib_ctx_.get();
  const int num_inputs = ctx->num_inputs();
  // TODO(laigd): need to check that input shape matches.
  // Pass input data to calibrator
  std::unordered_map<string, void*> input_data;
  for (int i = 0; i < num_inputs; i++) {
    const Tensor& t = ctx->input(i);
    void* data_address = GetTensorAddress(&t);
    OP_REQUIRES_ASYNC(ctx, data_address,
                      errors::InvalidArgument(
                          "Unsupported data type encountered in input ", i),
                      *helper);
    // Check the allocated buffer is sufficient for input
    const auto device_tensor =
        calib_ctx->device_tensors_.at(i).AccessTensor(ctx);
    CHECK_EQ(t.TotalBytes(), device_tensor->TotalBytes());
    input_data.emplace(StrCat(IONamePrefixes::kInputPHName, i), data_address);
  }
  VLOG(2) << "Filled map for sending";
  // copied from cuda_kernel_helper since it seems only valid in *.cu.cc files
  const cudaStream_t* stream = CHECK_NOTNULL(
      reinterpret_cast<const cudaStream_t*>(ctx->op_device_context()
                                                ->stream()
                                                ->implementation()
                                                ->GpuStreamMemberHack()));
  // If calibrator is terminated before, it means an error has occurred.
  //
  // Note: setBatch() will wait until TRTInt8Calibrator::getBatch() is called
  // the first time before proceeding, so if buildCudaEngine() returns an error,
  // it means getBatch() is never called, and the setBatch() here will hang
  // until setDone() is called later by the calibration thread in
  // AllocateCalibrationResources(). In that case, this setBatch() will always
  // be able to detect the error and return false.
  OP_REQUIRES_ASYNC(ctx, calib_ctx->calibrator_->setBatch(input_data, *stream),
                    errors::Internal("Failed to feed calibration data"),
                    *helper);
  VLOG(2) << "Passed calibration data";
  ExecuteNativeSegment(ctx, helper);
}

Status TRTEngineOp::VerifyInputShapes(
    const std::vector<TensorShape>& input_concrete_shapes) {
  if (input_concrete_shapes.empty()) {
    return errors::InvalidArgument("Input shapes are empty, for ", name());
  }

  if (input_partial_shapes_.empty()) {
    if (!use_implicit_batch_) {
      return errors::InvalidArgument(
          "Explicit batch mode requires input_partial_shapes_ ",
          "to contain the dynamic input shapes to TRTEngineOp");
    }
    // If the graph was converted with an earlier version of TF-TRT, it can
    // happen that the input_partial_shapes_ vector is not set (see
    // input_shapes attribute handling in the TRTEngineOp constructor).
    // In implicit batch mode it is allowed to have empty input_partial_shapes_,
    // since it is only required in explicit batch mode (see the input_shapes
    // attribute of ConvertGraphDefToEngine in TRTEngineOp::GetEngine.
  } else {
    // Additional consistency checks if input_partial_shapes_ is present.
    const string error_msg = StrCat(
        "Input shapes do not match input partial shapes stored in graph, for ",
        name(), ": ", DebugString(input_concrete_shapes),
        " != ", DebugString(input_partial_shapes_));
    if (input_concrete_shapes.size() != input_partial_shapes_.size()) {
      return errors::InvalidArgument(error_msg);
    }
    for (int i = 0; i < input_concrete_shapes.size(); i++) {
      if (input_concrete_shapes[i].dims() != input_partial_shapes_[i].dims()) {
        return errors::InvalidArgument(error_msg);
      }
    }
    for (int i = 0; i < input_concrete_shapes.size(); i++) {
      for (int d = 0; d < input_concrete_shapes[i].dims(); d++) {
        if (input_partial_shapes_[i].dim_size(d) != -1) {
          if (input_concrete_shapes[i].dim_size(d) !=
              input_partial_shapes_[i].dim_size(d)) {
            return errors::InvalidArgument(error_msg);
          }
        }
      }
    }
  }

  if (use_implicit_batch_) {
    if (input_concrete_shapes[0].dims() < 1) {
      return errors::InvalidArgument(
          "Input shapes contain scalar, for ", name(), ": ",
          TensorShapeUtils::ShapeListString(input_concrete_shapes));
    }
    const int batch_size = input_concrete_shapes[0].dim_size(0);
    if (batch_size < 1) {
      return errors::InvalidArgument(
          "Incorrect batch dimension, for ", name(), ": ",
          TensorShapeUtils::ShapeListString(input_concrete_shapes));
    }
    for (const TensorShape& shape : input_concrete_shapes) {
      if (batch_size != shape.dim_size(0)) {
        return errors::InvalidArgument(
            "Input shapes are inconsistent on the batch dimension, for ",
            name(), ": ",
            TensorShapeUtils::ShapeListString(input_concrete_shapes));
      }
    }
  }
  return Status::OK();
}

static bool AllowEngineNativeSegmentExecution() {
  bool value;
  Status status =
      ReadBoolFromEnvVar("TF_TRT_ALLOW_ENGINE_NATIVE_SEGMENT_EXECUTION",
                         /*default_value=*/true, &value);
  if (!status.ok()) {
    LOG(ERROR) << status;
  }
  return value;
}

void TRTEngineOp::ComputeAsync(OpKernelContext* ctx,
                               AsyncOpKernel::DoneCallback done) {
  auto helper = new AsyncHelper(done);
  core::ScopedUnref sc(helper);

  // Get TRT resource.
  TRTEngineCacheResource* cache_res = nullptr;
  OP_REQUIRES_OK_ASYNC(ctx, GetEngineCacheResource(ctx, &cache_res), *helper);
  core::ScopedUnref unref_cache_res(cache_res);

  // Run calibration if in int8+calibration mode.
  // * Logic in TF 1.x:
  //   - During conversion: calibration_mode_ is true and cache size is 0, so it
  //     will run calibration.
  //   - During inference: calibration_data will be set, so calibration_mode_ is
  //     false and it won't trigger calibration.
  // * Logic in TF 2.0:
  //   - During conversion: similar to 1.x.
  //   - During inference: calibration_data will still be empty, but cache will
  //     contain the the calibrated engine, so it won't trigger calibration.
  //
  // TODO(laigd): consider the following alternatives:
  // 1. Serialize the state (calibration or inference) using
  //    TRTEngineInstance proto (or a new proto), so we know which mode we're
  //    in and don't run calibration during inference (which is invalid).
  // 2. Reuse the calibration_data attribute or use a new attribute in the
  //    NodeDef to indicate whether it's in calibration mode.
  if (calibration_mode_ && cache_res->cache_.size() == 0) {
    if (!cache_res->calib_ctx_) {
      // TODO(laigd): better encapsulation.
      mutex_lock lock(engine_mutex_);
      if (!cache_res->calib_ctx_) {
        OP_REQUIRES_OK_ASYNC(ctx, AllocateCalibrationResources(ctx, cache_res),
                             *helper);
      }
    }
    // TODO(laigd): check that the input shapes match the shapes of the
    // persistent tensor in the calibration resource.
    ExecuteCalibration(ctx, cache_res, helper);
    return;
  }

  // Get shapes of inputs to engine.
  std::vector<TensorShape> input_concrete_shapes;
  input_concrete_shapes.reserve(ctx->num_inputs());
  for (int i = 0; i < ctx->num_inputs(); ++i) {
    input_concrete_shapes.push_back(ctx->input(i).shape());
  }

  Status verify_input_shape_status = VerifyInputShapes(input_concrete_shapes);
  // TODO(bixia): Fix the segmentation.
  if (!verify_input_shape_status.ok()) {
    LOG_FIRST_FEW_WARNING_WITH_PREFIX
        << "Running native segment for" << name()
        << " due to failure in verifying input shapes: "
        << verify_input_shape_status.error_message();
    ExecuteNativeSegment(ctx, helper);
    return;
  }

  if (!use_implicit_batch_) {
    if (profile_generation_mode_) {
      // Collecting new shapes for profiles can be only done once. After the
      // shapes are converted to TRT profiles, no shapes can be collected
      // anymore.
      OP_REQUIRES(ctx, cache_res->profiles_.GetNumProfiles() == 0,
                  errors::Unimplemented("Cannot collect new shapes when "
                                        "profiles are already created."));
      // Just collect the input shape info and return. The shapes are used to
      // generate optimization profiles during engine creation.
      cache_res->profiles_.AddShape(input_concrete_shapes);
      VLOG(1) << "Native segment is used during collecting shapes for profiles";
      ExecuteNativeSegment(ctx, helper);
      return;
    } else if (cache_res->profiles_.GetNumProfiles() == 0) {
      // Create profiles out of collected shapes during profile generation.
      cache_res->profiles_.InitProfiles();
    }
  }
  StatusOr<std::pair<EngineContext*, int>> status =
      GetEngine(input_concrete_shapes, ctx, cache_res);
  OP_REQUIRES_OK_ASYNC(ctx, status.status(), *helper);

  EngineContext* engine_context = status.ValueOrDie().first;
  int trt_context_idx = status.ValueOrDie().second;
  auto may_execute_native_segment = [&] {
    if (!AllowEngineNativeSegmentExecution()) {
      ctx->CtxFailure(
          errors::Aborted("User disallowed engine native segment execution"));
      return false;
    }
    return true;
  };
  if (!engine_context->cuda_engine) {
    LOG_FIRST_FEW_WARNING_WITH_PREFIX
        << "Engine retrieval for input shapes: "
        << TensorShapeUtils::ShapeListString(input_concrete_shapes)
        << " failed. Running native segment for " << name();
    if (may_execute_native_segment()) {
      ExecuteNativeSegment(ctx, helper);
    }
    return;
  }
  Status stat = ExecuteTrtEngine(ctx, engine_context, trt_context_idx);
  if (!stat.ok()) {
    LOG_FIRST_FEW_WARNING_WITH_PREFIX << "Failed to execute engine: " << stat
                                      << " Retrying with native segment for "
                                      << name();
    if (!may_execute_native_segment()) {
      return;
    }
    // Release any outputs that are allocated, ExecuteNativeSegment will
    // re-allocate them and fail if they are currently allocated.
    // The Tensor pointer in the returned TensorValue must be explicitly
    // deleted.
    for (int i = 0; i < ctx->num_outputs(); i++) {
      delete ctx->release_output(i).tensor;
    }
    ExecuteNativeSegment(ctx, helper);
    return;
  }
}

Status TRTEngineOp::ExecuteTrtEngine(OpKernelContext* ctx,
                                     EngineContext* engine_context,
                                     int trt_context_idx) {
  VLOG(1) << "Executing TRT engine: " << name();
  auto& cuda_engine = engine_context->cuda_engine;

  if (VLOG_IS_ON(2)) {
#if IS_TRT_VERSION_GE(6, 0, 0, 0)
    VLOG(2) << "  Network name: " << cuda_engine->getName();
#endif  // #if IS_TRT_VERSION_GE(6, 0, 0, 0)
    VLOG(2) << "  Activation size: " << cuda_engine->getDeviceMemorySize()
            << " bytes";
    VLOG(2) << "  Workspace size: " << cuda_engine->getWorkspaceSize()
            << " bytes";
    VLOG(2) << "  Datatype of " << cuda_engine->getNbBindings()
            << " inputs/outputs";
    string binding_types = "";
    for (int i = 0; i < cuda_engine->getNbBindings(); i++) {
      binding_types += "    " + string(cuda_engine->getBindingName(i)) + ": " +
                       DebugString(cuda_engine->getBindingDataType(i)) + "\n";
    }
    VLOG(2) << binding_types;
  }

  const int num_binding = cuda_engine->getNbBindings();
  std::vector<void*> buffers(num_binding);

  // nvinfer1::IExecutionContext::enqueue is not thread safe and we need a mutex
  // for it.
  mutex_lock lock(engine_context->mu);
  nvinfer1::IExecutionContext* execution_context;
  TF_RETURN_IF_ERROR(
      engine_context->GetExecutionContext(trt_context_idx, &execution_context));

  const int num_batch =
      use_implicit_batch_ ? ctx->input(0).shape().dim_size(0) : 0;

  TF_RETURN_IF_ERROR(SetTrtEngineInputs(cuda_engine.get(), execution_context,
                                        trt_context_idx, buffers,
                                        use_implicit_batch_, num_batch, ctx));

  TF_RETURN_IF_ERROR(SetTrtEngineOutputs(cuda_engine.get(), execution_context,
                                         trt_context_idx, buffers,
                                         use_implicit_batch_, num_batch, ctx));

  // Copied from cuda_kernel_helper since it seems only valid in *.cu.cc files
  const cudaStream_t* stream = CHECK_NOTNULL(
      reinterpret_cast<const cudaStream_t*>(ctx->op_device_context()
                                                ->stream()
                                                ->implementation()
                                                ->GpuStreamMemberHack()));

  TF_RETURN_IF_ERROR(TrtEnqueue(execution_context, buffers, *stream,
                                use_implicit_batch_, num_batch));
  return Status::OK();
}

Status TRTEngineOp::GetEngineCacheResource(OpKernelContext* ctx,
                                           TRTEngineCacheResource** cache_res) {
  // Canonicalize the op name by removing the scopes if any. This is mainly
  // because in TFv2, the function graph can be instantiated in various ways and
  // it'll insert scope names to the name of the TRTEngineOps, which will result
  // in many different engine caches if we use the instantiated op name
  // directly, but we still want all of them share the same cache (if they were
  // representing the same subgraph).
  absl::string_view resource_name = name();
  size_t last_slash = resource_name.find_last_of('/');
  if (last_slash != absl::string_view::npos) {
    resource_name.remove_prefix(last_slash + 1);
  }

  // Get engine cache.
  return ctx->resource_manager()->LookupOrCreate(
      std::string(kTfTrtContainerName), std::string(resource_name), cache_res,
      {[this, ctx](TRTEngineCacheResource** cr) -> Status {
        *cr = new TRTEngineCacheResource(ctx, this->max_cached_engines_);
        return Status::OK();
      }});
}

StatusOr<TrtUniquePtrType<nvinfer1::ICudaEngine>> TRTEngineOp::BuildEngine(
    const std::vector<TensorShape>& input_concrete_shapes, int batch_size,
    bool use_calibration, TRTInt8Calibrator* calibrator,
    TRTEngineCacheResource* cache_resource) {
  VLOG(1) << "Building a new TensorRT engine for " << name()
          << " with input shapes: "
          << TensorShapeUtils::ShapeListString(input_concrete_shapes);

  // Use concrete shapes for implicit batch mode and partial shapes for
  // explicit batch mode.
  const std::vector<PartialTensorShape>& conversion_input_shapes =
      use_implicit_batch_
          ? std::vector<PartialTensorShape>(input_concrete_shapes.begin(),
                                            input_concrete_shapes.end())
          : input_partial_shapes_;
  TrtUniquePtrType<nvinfer1::ICudaEngine> engine;
  auto status = convert::ConvertGraphDefToEngine(
      segment_graph_def_, precision_mode_, batch_size, workspace_size_,
      conversion_input_shapes, &logger, cache_resource->allocator_.get(),
      calibrator, &engine, use_calibration, use_implicit_batch_, nullptr,
      &cache_resource->profiles_);
  if (!status.ok()) {
    LOG_FIRST_FEW_WARNING_WITH_PREFIX
        << "Engine creation for " << name() << " failed. "
        << "The native segment will be used instead. "
        << "Reason: " << status;
    // Store an empty engine in the cache for these input shapes so we don't try
    // to build the same failing engine again.
    cache_resource->cache_.emplace(input_concrete_shapes,
                                   absl::make_unique<EngineContext>());
    return status;
  }
  return engine;
}

StatusOr<std::pair<EngineContext*, int>> TRTEngineOp::GetEngine(
    const std::vector<TensorShape>& input_concrete_shapes, OpKernelContext* ctx,
    TRTEngineCacheResource* cache_res) {
  static EngineContext empty_context;

  mutex_lock lock(engine_mutex_);
  // Using first input to get batch size is reliable - VerifyInputShapes()
  // guarantees that the first input is not a scalar. As such we can always use
  // the first input to get the batch size for implicit batch mode. For explicit
  // batch mode, this value is not used.
  const int batch_size = input_concrete_shapes[0].dim_size(0);
  // TODO(Tamas): remove the need for batch_size in explicit_batch mode
  auto& cache = cache_res->cache_;
  auto allocator = cache_res->allocator_.get();
  if (allocator == nullptr) {
    return std::pair<EngineContext*, int>(&empty_context, 0);
  }

  // Handle the static engine case. For static engines, the cache will have a
  // single element containing the only engine.
  if (static_engine_) {
    if (cache.size()) {
      // TODO(laigd): need a better shape compatibility check for the case where
      // implicit batch is disabled.
      if (!use_implicit_batch_ ||
          AreShapesCompatible(input_concrete_shapes, cache.begin()->first)) {
        return std::pair<EngineContext*, int>(cache.begin()->second.get(), 0);
      }
      return std::pair<EngineContext*, int>(&empty_context, 0);
    }

    TrtUniquePtrType<IRuntime> infer(nvinfer1::createInferRuntime(logger));
    infer->setGpuAllocator(allocator);
    // Need to initialize plugins in order to deserialize engines that contain
    // plugins.
    MaybeInitializeTrtPlugins(&logger);
    TrtUniquePtrType<nvinfer1::ICudaEngine> static_engine(
        infer->deserializeCudaEngine(serialized_segment_.c_str(),
                                     serialized_segment_.size(), nullptr));
    if (!static_engine) {
      if (!allow_build_at_runtime_) {
        // Store an empty engine in the cache so we don't try to load the same
        // failing engine again.
        cache.emplace(input_concrete_shapes,
                      absl::make_unique<EngineContext>());
        return std::pair<EngineContext*, int>(&empty_context, 0);
      }
      if (segment_graph_def_.node().empty()) {
        Status status = ImportSegmentGraphDef(ctx->function_library(),
                                              ctx->device()->name());
        if (!status.ok()) {
          LOG_FIRST_FEW_WARNING_WITH_PREFIX << "Getting segment graph for "
                                            << name() << " failed. "
                                            << "Reason: " << status;
        }
      }
      auto result = BuildEngine(input_concrete_shapes, batch_size,
                                /*use_calibration=*/false,
                                /*calibrator=*/nullptr, cache_res);
      if (!result.ok()) {
        return std::pair<EngineContext*, int>(&empty_context, 0);
      }
      static_engine = std::move(result.ValueOrDie());
    }
    auto raw_static_engine = static_engine.get();
    const auto max_batch_size = raw_static_engine->getMaxBatchSize();
    // Static engine will have max_batch_size for batch size so that all inputs
    // will map to this single engine.
    std::vector<TensorShape> engine_input_shapes(input_concrete_shapes);
    for (int i = 0; i < engine_input_shapes.size(); i++) {
      engine_input_shapes[i].set_dim(0, max_batch_size);
    }
    // TODO(laigd): here we assume engine_input_shapes matches the actual input
    // shapes of the engine, we should verify that.
    cache.emplace(engine_input_shapes,
                  absl::make_unique<EngineContext>(
                      std::move(static_engine),
                      TrtUniquePtrType<nvinfer1::IExecutionContext>(
                          raw_static_engine->createExecutionContext())));
    // Runtime is safe to delete after engine creation
    VLOG(1) << "Size of serialized TRT engine: "
            << serialized_segment_.capacity();
    string tmp;
    // Swap with temporary empty string to deallocate the CPU memory.
    serialized_segment_.swap(tmp);
    if (use_implicit_batch_ && (max_batch_size < batch_size)) {
      return std::pair<EngineContext*, int>(&empty_context, 0);
    }
    return std::pair<EngineContext*, int>(cache.at(engine_input_shapes).get(),
                                          0);
  }  // static_engine_

  int profile_id = -1;
  if (!use_implicit_batch_) {
    profile_id = cache_res->profiles_.GetProfileNumber(input_concrete_shapes);
    // Since all profiles are already created at this point, finding no
    // compatible profiles results in falling back to native TF.
    if (profile_id == -1) {
      return std::pair<EngineContext*, int>(&empty_context, 0);
    }
  }

  EngineContext* engine_contexts;
  if (use_implicit_batch_) {
    engine_contexts = cache_res->GetEngineContext(input_concrete_shapes);
  } else {
    engine_contexts = cache_res->GetEngineContext(profile_id);
  }

  // If cache does not have a compatible engine then create a new engine.
  if (engine_contexts == nullptr) {
    if (!allow_build_at_runtime_) {
      LOG_FIRST_FEW_WARNING_WITH_PREFIX
          << "Found no engine in cache matching input shapes. "
          << "Not building a new engine because "
          << "allow_build_at_runtime=False. "
          << "The native segment will be used instead.";
      // Store an empty engine in the cache for these input shapes so we don't
      // try to build the same failing engine again.
      cache.emplace(input_concrete_shapes, absl::make_unique<EngineContext>());
      return std::pair<EngineContext*, int>(&empty_context, 0);
    }

    // Up to this point, calibrator_ can never be empty, since otherwise it
    // means calibration_mode_ is true and this path won't get executed.
    auto result = BuildEngine(input_concrete_shapes, batch_size,
                              use_calibration_, calibrator_.get(), cache_res);
    if (!result.ok()) {
      return std::pair<EngineContext*, int>(&empty_context, 0);
    }
    TrtUniquePtrType<nvinfer1::ICudaEngine> engine =
        std::move(result.ValueOrDie());
    std::vector<TrtUniquePtrType<nvinfer1::IExecutionContext>> exec_context;
    TF_RETURN_IF_ERROR(cache_res->profiles_.CreateExecutionContexts(
        engine.get(), exec_context));
    cache.emplace(input_concrete_shapes,
                  absl::make_unique<EngineContext>(std::move(engine),
                                                   std::move(exec_context)));
    VLOG(1) << "Added new engine to cache of " << name()
            << ". Cache size: " << cache.size();
    engine_contexts = cache.at(input_concrete_shapes).get();
  }
  return std::pair<EngineContext*, int>(engine_contexts,
                                        use_implicit_batch_ ? 0 : profile_id);
}

// TODO(hinsu): Move this allocation to CalibrationContext constructor, if
// possible.
Status TRTEngineOp::AllocateCalibrationResources(
    OpKernelContext* ctx, TRTEngineCacheResource* cache_res) {
  cache_res->calib_ctx_ = absl::make_unique<CalibrationContext>();
  auto* cres = cache_res->calib_ctx_.get();

  // Get the input shapes.
  const int batch_size = ctx->input(0).dim_size(0);
  const int num_inputs = ctx->num_inputs();
  std::vector<TensorShape> shapes;
  cres->device_tensors_.resize(num_inputs);
  VLOG(1) << "Constructing calibrator";
  for (int i = 0; i < num_inputs; i++) {
    // allocate workspace on device for inputs
    const Tensor& t = ctx->input(i);
    shapes.emplace_back(t.shape());
    Tensor* device_tensor;
    TF_RETURN_IF_ERROR(ctx->allocate_persistent(
        t.dtype(), t.shape(), &cres->device_tensors_.at(i), &device_tensor));
    CHECK_EQ(t.TotalBytes(), device_tensor->TotalBytes());
    void* device_address = GetTensorAddress(device_tensor);
    if (device_address == nullptr) {
      return errors::InvalidArgument(
          "Unsupported data type encountered in input ", i);
    }
    cres->device_buffers_.emplace(
        StrCat(IONamePrefixes::kInputPHName, i),
        std::pair<void*, size_t>(device_address, device_tensor->TotalBytes()));
  }
  cres->calibrator_.reset(
      new TRTInt8Calibrator(cres->device_buffers_, batch_size, name()));
  const int platform_gpu_id =
      ctx->device()->tensorflow_gpu_device_info()->gpu_id;
  if (platform_gpu_id < 0) {
    LOG(ERROR) << "Can't get gpu_device_info from context->device()";
    return errors::InvalidArgument(
        "Context->device doesn't contain device info!");
  }

  cache_res->Ref();
  cres->thr_.reset(new std::thread([this, cres, shapes, platform_gpu_id,
                                    cache_res]() {
    core::ScopedUnref sc(cache_res);

    VLOG(1) << "Starting calibration thread on device " << platform_gpu_id
            << ", Calibration Resource @ " << cres;
    auto err = cudaSetDevice(platform_gpu_id);
    if (err != cudaSuccess) {
      // TODO(aaroey): should return error here.
      LOG(ERROR) << "Couldn't set cuda device to " << platform_gpu_id
                 << " in calibration thread";
    }
    std::vector<PartialTensorShape> partial_shapes(shapes.begin(),
                                                   shapes.end());
    // ConvertGraphDefToEngine() will try to build the engine. This thread
    // will loop inside buildCudaEngine() consuming the calibration data
    // that is set by the TF op, and drive the builder until calibrator
    // returns false. Engine is discarded after calibration table is
    // generated
    //
    // TODO(aaroey): maybe setting the max batch size using the python
    // calibration wrapper class.
    auto s = convert::ConvertGraphDefToEngine(
        this->segment_graph_def_, TrtPrecisionMode::INT8,
        cres->calibrator_->getBatchSize(), this->workspace_size_,
        partial_shapes, &cache_res->GetLogger(), cache_res->allocator_.get(),
        cres->calibrator_.get(), &cres->engine_, /*use_calibration=*/true,
        this->use_implicit_batch_, /*convert_successfully=*/nullptr,
        /*profiles=*/nullptr);
    if (!s.ok()) {
      LOG(ERROR) << "Calibration failed: " << s;
      cres->calibrator_->setDone();  // Ignore further pushes
    } else {
      // Transfer the ownership of the engine to the engine cache, so we can
      // dump it out during conversion for TF 2.0.
      mutex_lock lock(this->engine_mutex_);
      this->calibrator_ = std::move(cres->calibrator_);
      TrtUniquePtrType<nvinfer1::IExecutionContext> exec_context(
          cres->engine_->createExecutionContext());
      cache_res->cache_.emplace(
          shapes, absl::make_unique<EngineContext>(std::move(cres->engine_),
                                                   std::move(exec_context)));
    }

    VLOG(1) << "Calibration loop terminated " << this->name();
  }));
  VLOG(1) << "initialized calibrator resource";
  return Status::OK();
}

REGISTER_KERNEL_BUILDER(Name("TRTEngineOp").Device(DEVICE_GPU), TRTEngineOp);

}  // namespace tensorrt
}  // namespace tensorflow

#endif  // GOOGLE_CUDA && GOOGLE_TENSORRT
