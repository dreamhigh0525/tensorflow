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
#include <string>

#include "tensorflow/lite/tools/benchmark/delegate_provider.h"
#include "tensorflow/lite/tools/evaluation/utils.h"
#if defined(__APPLE__)
#if TARGET_OS_IPHONE && !TARGET_IPHONE_SIMULATOR
// Only enable metal delegate when using a real iPhone device.
#define REAL_IPHONE_DEVICE
#include "tensorflow/lite/experimental/delegates/coreml/coreml_delegate.h"
#endif
#endif

namespace tflite {
namespace benchmark {

class CoreMlDelegateProvider : public DelegateProvider {
 public:
  CoreMlDelegateProvider() {
#if defined(REAL_IPHONE_DEVICE)
    default_params_.AddParam("use_coreml", BenchmarkParam::Create<bool>(true));
#endif
  }
  std::vector<Flag> CreateFlags(BenchmarkParams* params) const final;

  void LogParams(const BenchmarkParams& params) const final;

  TfLiteDelegatePtr CreateTfLiteDelegate(
      const BenchmarkParams& params) const final;

  std::string GetName() const final { return "COREML"; }
};
REGISTER_DELEGATE_PROVIDER(CoreMlDelegateProvider);

std::vector<Flag> CoreMlDelegateProvider::CreateFlags(
    BenchmarkParams* params) const {
  std::vector<Flag> flags = {
      CreateFlag<bool>("use_coreml", params, "use Core ML"),
  };
  return flags;
}

void CoreMlDelegateProvider::LogParams(const BenchmarkParams& params) const {
#if defined(REAL_IPHONE_DEVICE)
  TFLITE_LOG(INFO) << "Use Core ML : [" << params.Get<bool>("use_coreml")
                   << "]";
#endif
}

TfLiteDelegatePtr CoreMlDelegateProvider::CreateTfLiteDelegate(
    const BenchmarkParams& params) const {
  TfLiteDelegatePtr delegate(nullptr, [](TfLiteDelegate*) {});

#if defined(REAL_IPHONE_DEVICE)
  if (params.Get<bool>("use_coreml")) {
    TfLiteCoreMlDelegateOptions coreml_opts = {
        .enabled_devices = TfLiteCoreMlDelegateAllDevices};
    delegate = TfLiteDelegatePtr(TfLiteCoreMlDelegateCreate(&coreml_opts),
                                 &TfLiteCoreMlDelegateDelete);
    if (!delegate) {
      TFLITE_LOG(WARN)
          << "CoreML acceleration is unsupported on this platform.";
    }
  }
#endif

  return delegate;
}

}  // namespace benchmark
}  // namespace tflite
