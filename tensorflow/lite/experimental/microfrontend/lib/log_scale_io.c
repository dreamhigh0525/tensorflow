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
#include "tensorflow/lite/experimental/microfrontend/lib/log_scale_io.h"

void LogScaleWriteMemmap(FILE* fp, const struct LogScaleState* state,
                         const char* variable) {
  MICROFRONTEND_FPRINTF(fp, "%s->enable_log = %d;\n", variable, state->enable_log);
  MICROFRONTEND_FPRINTF(fp, "%s->scale_shift = %d;\n", variable, state->scale_shift);
}
