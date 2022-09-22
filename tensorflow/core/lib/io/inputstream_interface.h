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

#ifndef TENSORFLOW_CORE_LIB_IO_INPUTSTREAM_INTERFACE_H_
#define TENSORFLOW_CORE_LIB_IO_INPUTSTREAM_INTERFACE_H_

#include "tensorflow/core/platform/cord.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/tsl/lib/io/inputstream_interface.h"

namespace tensorflow {
namespace io {
using tsl::io::InputStreamInterface;  // NOLINT(misc-unused-using-decls)
}
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_LIB_IO_INPUTSTREAM_INTERFACE_H_
