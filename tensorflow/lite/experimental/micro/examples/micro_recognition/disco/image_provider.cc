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

#include "tensorflow/lite/experimental/micro/examples/micro_recognition/image_provider.h"

#include "BSP_DISCO_F746NG/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_camera.h"

TfLiteStatus InitCamera(tflite::ErrorReporter* error_reporter) {
  if (BSP_CAMERA_Init(RESOLUTION_R160x120) != CAMERA_OK) {
    error_reporter->Report("Failed to init camera.\n");
    return kTfLiteError;
  }

  return kTfLiteOk;
}

TfLiteStatus GetImage(tflite::ErrorReporter* error_reporter, int frame_width,
                      int frame_height, int channels, uint8_t* frame) {
  BSP_CAMERA_SnapshotStart(frame);
  return kTfLiteOk;
}
