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

#include "tensorflow/compiler/tf2tensorrt/utils/trt_logger.h"

#if GOOGLE_CUDA && GOOGLE_TENSORRT

#include <vector>

#include "tensorflow/compiler/tf2tensorrt/common/utils.h"
#include "tensorflow/compiler/tf2tensorrt/convert/logger_registry.h"
#include "tensorflow/compiler/tf2tensorrt/utils/trt_experimental_features.h"
#include "tensorflow/core/platform/logging.h"

namespace tensorflow {
namespace tensorrt {

bool filter_string(string msg) {
  // This function checks for known substrings that shall be ignored.

  static const std::vector<string> substr_patterns{
      // Automatic messages generated by TensorRT when combined with
      // Automatic Mixed Precision - TensorRT 8.2
      "Missing scale and zero-point for",
      "Subnormal FP16 values detected",
      "If this is not the desired behavior, please modify the weights",
      "had the following issues when converted to FP16",
      "Values less than smallest positive FP16 Subnormal value detected.",
      // Deprecation Warnings
      "The implicit batch dimension mode has been deprecated.",
      "The getMaxBatchSize() function should not be used with an engine built",
      // Input-Warnings
      "[RemoveDeadLayers] Input Tensor input is unused or used only at",
      "Unused Input:",
      // Data Type Warnings
      "Tensor DataType is determined at build time for tensors not marked as",
      // Myelin Performance Warning in dynamic shape mode
      "Myelin graph with multiple dynamic values may have poor performance",
      "(# 0 (SHAPE",
      "CUDA lazy loading is not enabled. Enabling it can significantly reduce",
  };

  for (int i = 0; i < substr_patterns.size(); i++) {
    std::size_t is_found = msg.find(substr_patterns[i]);
    if (is_found != string::npos) {
      return true;
    }
  }
  return false;
}

// Use TF logging for TensorRT informations
void Logger::log(Severity severity, const char* msg) noexcept {
  static const bool filter_messages = []() {
    return !isExperimentalFeatureActivated("disable_logger_filtering");
  }();

  if (filter_messages && filter_string(msg)) return;

  if (!isValidSeverity(severity, msg) || suppressedMsg_ & (1 << (int)severity))
    return;

  // Suppress info-level messages
  switch (severity) {
    case Severity::kVERBOSE:
    case Severity::kINFO: {  // Mark TRT info messages as debug!
      VLOG(2) << name_ << " " << msg;
      break;
    }
    case Severity::kWARNING: {
      LOG_WARNING_WITH_PREFIX << name_ << " " << msg;
      break;
    }
    case Severity::kERROR: {
      LOG(ERROR) << name_ << " " << msg;
      break;
    }
    case Severity::kINTERNAL_ERROR: {
      LOG(FATAL) << name_ << " " << msg;
      break;
    }
  }
}

void Logger::suppressLoggerMsgs(Severity severity) {
  if (isValidSeverity(severity)) {
    suppressedMsg_ |= 1 << (int)severity;
  }
}

void Logger::unsuppressLoggerMsgs(Severity severity) {
  if (isValidSeverity(severity)) {
    suppressedMsg_ &= (-1) ^ (1 << (int)severity);
  }
}

bool Logger::isValidSeverity(Severity severity, const char* msg) noexcept {
  switch (severity) {
    case Severity::kVERBOSE:
    case Severity::kINFO:
    case Severity::kWARNING:
    case Severity::kERROR:
    case Severity::kINTERNAL_ERROR:
      return true;
  }
  return false;
}

// static
Logger* Logger::GetLogger() {
  static Logger* logger = new Logger("DefaultLogger");
  return logger;
}

REGISTER_TENSORRT_LOGGER("DefaultLogger", Logger::GetLogger());

}  // namespace tensorrt
}  // namespace tensorflow

#endif  // GOOGLE_CUDA && GOOGLE_TENSORRT
