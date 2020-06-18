/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_C_EXPERIMENTAL_SAVED_MODEL_CORE_OPS_OWNED_EAGER_CONTEXT_H_
#define TENSORFLOW_C_EXPERIMENTAL_SAVED_MODEL_CORE_OPS_OWNED_EAGER_CONTEXT_H_

#include <memory>

#include "tensorflow/c/eager/immediate_execution_context.h"
#include "tensorflow/core/common_runtime/eager/context.h"

namespace tensorflow {
namespace internal {

struct ImmediateExecutionContextDeleter {
  void operator()(ImmediateExecutionContext* p) const {
    if (p != nullptr) {
      p->Release();
    }
  }
};

struct EagerContextDeleter {
  void operator()(EagerContext* p) const {
    if (p != nullptr) {
      p->Release();
    }
  }
};

}  // namespace internal

using AbstractContextPtr =
    std::unique_ptr<ImmediateExecutionContext,
                    internal::ImmediateExecutionContextDeleter>;

using EagerContextPtr =
    std::unique_ptr<EagerContext, internal::EagerContextDeleter>;

}  // namespace tensorflow

#endif  // THIRD_PARTY_TENSORFLOW_C_EXPERIMENTAL_SAVED_MODEL_CORE_OPS_OWNED_EAGER_CONTEXT_H_
