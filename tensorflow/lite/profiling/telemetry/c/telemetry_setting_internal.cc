/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/lite/profiling/telemetry/c/telemetry_setting_internal.h"

extern "C" {

const TfLiteTelemetryConversionMetadata*
TfLiteTelemetryInterpreterSettingsGetConversionMetadata(
    const TfLiteTelemetryInterpreterSettings* settings) {
  if (settings == nullptr) return nullptr;
  return settings->conversion_metadata.get();
}

const int32_t* TfLiteTelemetryConversionMetadataGetModelOptimizationModes(
    const TfLiteTelemetryConversionMetadata* metadata) {
  if (metadata == nullptr) return nullptr;
  return metadata->model_optimization_modes.data();
}

size_t TfLiteTelemetryConversionMetadataGetNumModelOptimizationModes(
    const TfLiteTelemetryConversionMetadata* metadata) {
  if (metadata == nullptr) return 0;
  return metadata->model_optimization_modes.size();
}

}  // extern "C"
