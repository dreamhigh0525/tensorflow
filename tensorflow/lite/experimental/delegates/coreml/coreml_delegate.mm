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
#include "tensorflow/lite/experimental/delegates/coreml/coreml_delegate.h"

#include <vector>

#include "tensorflow/lite/builtin_ops.h"
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/context_util.h"
#include "tensorflow/lite/experimental/delegates/coreml/builders/op_validator.h"
#include "tensorflow/lite/experimental/delegates/coreml/builders/util.h"
#include "tensorflow/lite/experimental/delegates/coreml/coreml_delegate_kernel.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/minimal_logging.h"

namespace tflite {
namespace {
using delegates::coreml::CoreMlDelegateKernel;

bool IsNodeSupportedByDelegate(const TfLiteRegistration* registration, const TfLiteNode* node,
                               TfLiteContext* context) {
  if (@available(iOS 11.0, *)) {
  } else {
    return false;
  }

  // For most ops, only version 1 is supported.
  if (registration->version > 1) {
    switch (registration->builtin_code) {
      case kTfLiteBuiltinDepthwiseConv2d:
        if (registration->version > 2) return false;
        break;
      default:
        return false;
    }
  }

  // The model should not be full-integer quantized. For ops supported by Core ML delegate,
  // Testing if the first input is float is sufficient to filter full-integer quantized ops.
  if (GetInput(context, node, 0)->type != kTfLiteFloat32) {
    return false;
  }

  // TODO(b/149179044): Add extra validation if this is not sufficient.

  // TODO(karimnossier): Refactor this function.
  // TODO(karimnosseir): Add
  // 1) Checks for versioning.
  // 2) Checks for input constraints.
  // Follow the ordering of TfLiteBuiltinOperator enum.
  switch (registration->builtin_code) {
    case kTfLiteBuiltinAdd: {
      return node->builtin_data != nullptr &&
             delegates::coreml::IsBinaryOpSupported(registration, node, context);
    }
    case kTfLiteBuiltinAveragePool2d: {
      const auto* params = reinterpret_cast<const TfLitePoolParams*>(node->builtin_data);
      return params != nullptr && params->activation == kTfLiteActNone;
    }
    case kTfLiteBuiltinConcatenation: {
      return delegates::coreml::IsConcatenationOpSupported(registration, node, context);
    }
    case kTfLiteBuiltinConv2d: {
      return delegates::coreml::IsConvolutionOpSupported(registration, node, context);
    }
    case kTfLiteBuiltinDepthwiseConv2d: {
      return delegates::coreml::IsDepthwiseConvolutionOpSupported(registration, node, context);
    }
    case kTfLiteBuiltinLogistic: {
      return true;
    }
    case kTfLiteBuiltinMaxPool2d: {
      const auto* params = reinterpret_cast<const TfLitePoolParams*>(node->builtin_data);
      return params != nullptr && params->activation == kTfLiteActNone;
    }
    case kTfLiteBuiltinMul: {
      return node->builtin_data != nullptr &&
             delegates::coreml::IsBinaryOpSupported(registration, node, context);
    }
    case kTfLiteBuiltinRelu: {
      return true;
    }
    case kTfLiteBuiltinReluN1To1: {
      return true;
    }
    case kTfLiteBuiltinRelu6: {
      return true;
    }
    case kTfLiteBuiltinReshape: {
      return delegates::coreml::IsReshapeOpSupported(registration, node, context);
    }
    case kTfLiteBuiltinResizeBilinear: {
      return delegates::coreml::IsResizeBilinearOpSupported(registration, node, context);
    }
    case kTfLiteBuiltinSoftmax: {
      // Only supports when beta is 1.0 for now.
      const auto* softmax_params = reinterpret_cast<const TfLiteSoftmaxParams*>(node->builtin_data);
      return softmax_params != nullptr && softmax_params->beta == 1.0;
    }
    case kTfLiteBuiltinTanh: {
      return true;
    }
    case kTfLiteBuiltinHardSwish: {
      return true;
    }
    default:
      return false;
  }
  return false;
}

TfLiteRegistration GetCoreMlKernelRegistration() {
  // This is the registration for the Delegate Node that gets added to
  // the TFLite graph instead of the subGraph it replaces it.
  // It is treated as an OP node. But in our case
  // Init will initialize the delegate
  // Invoke will run the delegate graph.
  // Prepare for prearing the delegate.
  // Free for any cleaning needed by the delegate.
  TfLiteRegistration kernel_registration;
  kernel_registration.builtin_code = kTfLiteBuiltinDelegate;
  kernel_registration.custom_name = "TfLiteCoreMlDelegate";
  kernel_registration.free = [](TfLiteContext* context, void* buffer) -> void {
    delete reinterpret_cast<CoreMlDelegateKernel*>(buffer);
  };
  kernel_registration.init = [](TfLiteContext* context, const char* buffer,
                                size_t length) -> void* {
    const TfLiteDelegateParams* params = reinterpret_cast<const TfLiteDelegateParams*>(buffer);
    CoreMlDelegateKernel* coreml_kernel = new CoreMlDelegateKernel();
    if (coreml_kernel->Init(context, params) != kTfLiteOk) {
      delete coreml_kernel;
      return nullptr;
    }
    return coreml_kernel;
  };
  kernel_registration.invoke = [](TfLiteContext* context, TfLiteNode* node) -> TfLiteStatus {
    CoreMlDelegateKernel* kernel = reinterpret_cast<CoreMlDelegateKernel*>(node->user_data);
    if (!kernel) {
      TF_LITE_KERNEL_LOG(context, "CoreMl Kernel was not initialized");
      return kTfLiteError;
    }
    return kernel->Invoke(context, node);
  };
  kernel_registration.prepare = [](TfLiteContext* context, TfLiteNode* node) -> TfLiteStatus {
    CoreMlDelegateKernel* kernel = reinterpret_cast<CoreMlDelegateKernel*>(node->user_data);
    if (kernel == nullptr) {
      TF_LITE_KERNEL_LOG(context, "CoreMl Kernel was not initialized");
      return kTfLiteError;
    }
    return kernel->Prepare(context, node);
  };

  return kernel_registration;
}

TfLiteStatus DelegatePrepare(TfLiteContext* context, TfLiteDelegate* delegate) {
  // Reserve 1 element, since we need first element to be size, will be updated
  // later.
  std::vector<int> supported_nodes(1);
  TfLiteIntArray* plan;
  TF_LITE_ENSURE_STATUS(context->GetExecutionPlan(context, &plan));
  TfLiteNode* node;
  TfLiteRegistration* registration;

  for (int node_index : TfLiteIntArrayView(plan)) {
    TF_LITE_ENSURE_STATUS(
        context->GetNodeAndRegistration(context, node_index, &node, &registration));
    if (IsNodeSupportedByDelegate(registration, node, context)) {
      supported_nodes.push_back(node_index);
    }
  }
  // Set first element to the number of nodes to replace.
  supported_nodes[0] = supported_nodes.size() - 1;
  TfLiteRegistration coreml_kernel_registration = GetCoreMlKernelRegistration();
  TFLITE_LOG_PROD(tflite::TFLITE_LOG_INFO, "CoreML delegate: %d nodes delegated out of %d nodes.\n",
                  supported_nodes[0], plan->size);

  return context->ReplaceNodeSubsetsWithDelegateKernels(
      context, coreml_kernel_registration,
      reinterpret_cast<TfLiteIntArray*>(supported_nodes.data()), delegate);
}

class CoreMlDelegate : public TfLiteDelegate {
 public:
  explicit CoreMlDelegate(const TfLiteCoreMlDelegateOptions* params)
      : params_(params != nullptr ? *params : TfLiteCoreMlDelegateOptions()) {}

  TfLiteCoreMlDelegateOptions* params() { return &params_; }

  bool VerifyDelegate() { return true; }

 private:
  TfLiteCoreMlDelegateOptions params_;
};

TfLiteDelegate* CreateCoreMlDelegate(const TfLiteCoreMlDelegateOptions* options) {
  TfLiteDelegate* delegate = new CoreMlDelegate(options);
  if (!static_cast<CoreMlDelegate*>(delegate)->VerifyDelegate()) {
    delete delegate;
    return nullptr;
  }

  delegate->data_ = static_cast<tflite::CoreMlDelegate*>(delegate)->params();
  delegate->flags = kTfLiteDelegateFlagsNone;
  delegate->Prepare = &DelegatePrepare;
  delegate->CopyFromBufferHandle = nullptr;
  delegate->CopyToBufferHandle = nullptr;
  delegate->FreeBufferHandle = nullptr;

  return delegate;
}
}  // namespace
}  // namespace tflite

TfLiteDelegate* TfLiteCoreMlDelegateCreate(const TfLiteCoreMlDelegateOptions* options) {
  if (@available(iOS 11.0, *)) {
    return tflite::CreateCoreMlDelegate(options);
  } else {
    NSLog(@"Core ML delegate is not supported in this iOS version. "
           "Minimum required iOS version is 11.0.");
    return nullptr;
  }
}

void TfLiteCoreMlDelegateDelete(TfLiteDelegate* delegate) { delete delegate; }
