/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#ifndef THIRD_PARTY_TENSORFLOW_CORE_PLATFORM_FILE_STATISTICS_H_
#define THIRD_PARTY_TENSORFLOW_CORE_PLATFORM_FILE_STATISTICS_H_

#include "tensorflow/core/platform/types.h"

namespace tensorflow {

struct FileStatistics {
  // The length of the file or -1 if finding file length is not supported.
  int64 length = -1;
  // The last modified time in nanoseconds.
  int64 mtime_nsec = 0;
  // This is the mode_t from stat.h containing file type and permission
  // information.
  mode_t mode = 0;

  FileStatistics() {}
  ~FileStatistics() {}
};

}  // namespace tensorflow

#endif  // THIRD_PARTY_TENSORFLOW_CORE_PLATFORM_FILE_STATISTICS_H_
