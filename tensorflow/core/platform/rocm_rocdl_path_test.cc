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

#include "tensorflow/core/platform/rocm_rocdl_path.h"

#include "rocm/rocm_config.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {

#if TENSORFLOW_USE_ROCM
TEST(RocmRocdlPathTest, ROCDLPath) {
  VLOG(2) << "ROCm-Device-Libs root = " << RocdlRoot();
  std::vector<string> rocdl_files;
  TF_EXPECT_OK(Env::Default()->GetMatchingPaths(
#if TF_ROCM_VERSION >= 30900
      io::JoinPath(RocdlRoot(), "*.bc"), &rocdl_files));
#else
      io::JoinPath(RocdlRoot(), "*.amdgcn.bc"), &rocdl_files));
#endif
  EXPECT_LT(0, rocdl_files.size());
}
#endif

}  // namespace tensorflow
