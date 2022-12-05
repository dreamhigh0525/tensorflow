/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/jit/tf_graph_to_hlo_compiler.h"

#include <vector>

namespace tensorflow {

Status TfGraphToHloCompiler::Compile(const XlaCompiler::CompileOptions& options,
                                     const NameAttrList& function,
                                     absl::Span<const XlaArgument> args,
                                     XlaCompilationResult* result) {
  return xla_compiler_.CompileFunction(options, function, args, result);
}

Status TfGraphToHloCompiler::CompileSingleOp(
    const XlaCompiler::CompileOptions& options, const OpKernelContext* ctx,
    absl::Span<const XlaArgument> args, XlaCompilationResult* result) {
  return xla_compiler_.CompileSingleOp(
      options, XlaCompiler::SingleOpCompileArgument(*ctx), args, result);
}

}  // namespace tensorflow
