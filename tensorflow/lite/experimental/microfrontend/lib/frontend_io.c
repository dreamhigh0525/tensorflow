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
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_io.h"

#include <stdio.h>

#include "tensorflow/lite/experimental/microfrontend/lib/fft_io.h"
#include "tensorflow/lite/experimental/microfrontend/lib/filterbank_io.h"
#include "tensorflow/lite/experimental/microfrontend/lib/log_scale_io.h"
#include "tensorflow/lite/experimental/microfrontend/lib/noise_reduction_io.h"
#include "tensorflow/lite/experimental/microfrontend/lib/window_io.h"

int WriteFrontendStateMemmap(const char* header, const char* source,
                             const struct FrontendState* state) {
  // Write a header that just has our init function.
  FILE* fp = fopen(header, "w");
  if (!fp) {
    MICROFRONTEND_FPRINTF(stderr, "Failed to open header '%s' for write\n", header);
    return 0;
  }
  MICROFRONTEND_FPRINTF(fp, "#ifndef FRONTEND_STATE_MEMMAP_H_\n");
  MICROFRONTEND_FPRINTF(fp, "#define FRONTEND_STATE_MEMMAP_H_\n");
  MICROFRONTEND_FPRINTF(fp, "\n");
  MICROFRONTEND_FPRINTF(fp, "#include \"frontend.h\"\n");
  MICROFRONTEND_FPRINTF(fp, "\n");
  MICROFRONTEND_FPRINTF(fp, "struct FrontendState* GetFrontendStateMemmap();\n");
  MICROFRONTEND_FPRINTF(fp, "\n");
  MICROFRONTEND_FPRINTF(fp, "#endif  // FRONTEND_STATE_MEMMAP_H_\n");
  fclose(fp);

  // Write out the source file that actually has everything in it.
  fp = fopen(source, "w");
  if (!fp) {
    MICROFRONTEND_FPRINTF(stderr, "Failed to open source '%s' for write\n", source);
    return 0;
  }
  MICROFRONTEND_FPRINTF(fp, "#include \"%s\"\n", header);
  MICROFRONTEND_FPRINTF(fp, "\n");
  WindowWriteMemmapPreamble(fp, &state->window);
  FftWriteMemmapPreamble(fp, &state->fft);
  FilterbankWriteMemmapPreamble(fp, &state->filterbank);
  NoiseReductionWriteMemmapPreamble(fp, &state->noise_reduction);
  MICROFRONTEND_FPRINTF(fp, "static struct FrontendState state;\n");
  MICROFRONTEND_FPRINTF(fp, "struct FrontendState* GetFrontendStateMemmap() {\n");
  WindowWriteMemmap(fp, &state->window, "  (&state.window)");
  FftWriteMemmap(fp, &state->fft, "  (&state.fft)");
  FilterbankWriteMemmap(fp, &state->filterbank, "  (&state.filterbank)");
  NoiseReductionWriteMemmap(fp, &state->noise_reduction,
                            "  (&state.noise_reduction)");
  LogScaleWriteMemmap(fp, &state->log_scale, "  (&state.log_scale)");
  MICROFRONTEND_FPRINTF(fp, "  FftInit(&state.fft);\n");
  MICROFRONTEND_FPRINTF(fp, "  FrontendReset(&state);\n");
  MICROFRONTEND_FPRINTF(fp, "  return &state;\n");
  MICROFRONTEND_FPRINTF(fp, "}\n");
  fclose(fp);
  return 1;
}
