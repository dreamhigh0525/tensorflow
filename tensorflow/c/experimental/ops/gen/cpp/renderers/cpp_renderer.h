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
#ifndef TENSORFLOW_C_EXPERIMENTAL_OPS_GEN_CPP_RENDERERS_CPP_RENDERER_H_
#define TENSORFLOW_C_EXPERIMENTAL_OPS_GEN_CPP_RENDERERS_CPP_RENDERER_H_

#include <vector>

#include "tensorflow/c/experimental/ops/gen/cpp/renderers/guard_renderer.h"
#include "tensorflow/c/experimental/ops/gen/cpp/renderers/include_renderer.h"
#include "tensorflow/c/experimental/ops/gen/cpp/renderers/namespace_renderer.h"
#include "tensorflow/c/experimental/ops/gen/cpp/renderers/renderer.h"
#include "tensorflow/c/experimental/ops/gen/cpp/renderers/renderer_context.h"
#include "tensorflow/c/experimental/ops/gen/cpp/views/op_view.h"

namespace tensorflow {
namespace generator {
namespace cpp {

class CppRenderer : public Renderer {
 public:
  explicit CppRenderer(RendererContext context, const std::vector<OpView> &ops);
  void Render();

 private:
  GuardRenderer guard_;
  NamespaceRenderer name_space_;
  IncludeRenderer includes_;
  std::vector<OpView> ops_;
};

}  // namespace cpp
}  // namespace generator
}  // namespace tensorflow

#endif  // TENSORFLOW_C_EXPERIMENTAL_OPS_GEN_CPP_RENDERERS_CPP_RENDERER_H_
