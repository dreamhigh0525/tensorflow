/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/tpu/tpu_initializer_helper.h"

#include <stdlib.h>

#include "absl/strings/str_split.h"

namespace tensorflow {
namespace tpu {

std::pair<std::vector<std::string>, std::vector<const char*>>
GetLibTpuInitArguments() {
  // We make copies of the arguments returned by getenv because the memory
  // returned may be altered or invalidated by further calls to getenv.
  std::vector<std::string> argv;
  std::vector<const char*> argv_ptr;

  // Retrieve arguments from environment if applicable.
  char* env = getenv("LIBTPU_INIT_ARGS");
  if (env != nullptr) {
    // TODO(frankchn): Handles quotes properly if necessary.
    argv = absl::StrSplit(env, ' ');
  }

  argv_ptr.reserve(argv.size());
  for (int i = 0; i < argv.size(); ++i) {
    argv_ptr.push_back(argv[i].data());
  }
  argv_ptr.push_back(nullptr);

  return {argv, argv_ptr};
}

}  // namespace tpu
}  // namespace tensorflow
