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

#include "tensorflow/compiler/xla/tests/filecheck.h"

#include <cstdlib>

#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/subprocess.h"

namespace xla {

StatusOr<bool> RunFileCheck(const string& input, const string& pattern) {
  using tensorflow::io::JoinPath;

  // Generate an input file for the FileCheck pattern.
  string pattern_path;
  auto env = tensorflow::Env::Default();
  if (!env->LocalTempFilename(&pattern_path)) {
    return tensorflow::errors::Internal("couldn't get a pattern file name");
  }
  TF_RETURN_IF_ERROR(tensorflow::WriteStringToFile(env, pattern_path, pattern));

  // Invoke FileCheck to check whether input matches `pattern`.
  const char* file_check_path_suffix = "external/llvm/FileCheck";
  string file_check_path;
  if (const char* test_srcdir = getenv("TEST_SRCDIR")) {
    file_check_path = JoinPath(test_srcdir, file_check_path_suffix);
  } else {
    file_check_path = file_check_path_suffix;
  }

  tensorflow::SubProcess file_check_process;
  file_check_process.SetProgram(file_check_path,
                                {file_check_path, pattern_path});
  file_check_process.SetChannelAction(tensorflow::CHAN_STDIN,
                                      tensorflow::ACTION_PIPE);
  file_check_process.SetChannelAction(tensorflow::CHAN_STDERR,
                                      tensorflow::ACTION_PIPE);
  if (!file_check_process.Start()) {
    return tensorflow::errors::Internal("couldn't start FileCheck");
  }

  string standard_error;
  int exit_status = file_check_process.Communicate(
      /*stdin_input=*/&input, /*stdout_output=*/nullptr,
      /*stderr_output=*/&standard_error);

  // FileCheck returns 0 when the inputs match. If matching failed, log
  // the error message generated by FileCheck and the inputs.
  bool succeeded = (exit_status == 0);
  if (!succeeded) {
    LOG(WARNING) << "FileCheck error: " << standard_error;
    LOG(WARNING) << "FileCheck input was:";
    XLA_LOG_LINES(tensorflow::WARNING, input);
    LOG(WARNING) << "FileCheck pattern was:";
    XLA_LOG_LINES(tensorflow::WARNING, pattern);
  }
  return succeeded;
}

}  // namespace xla
