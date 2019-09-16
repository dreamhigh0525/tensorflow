/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

%include "tensorflow/python/platform/base.i"

%{

#include "mlir/Pass/PassRegistry.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/import_model.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/error_util.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/import_utils.h"

namespace tensorflow {
namespace swig {

// Simple wrapper to support tf.mlir.experimental.convert_graph_def.
// Load a .pbptx, convert to MLIR, and (optionally) optimize the module before
// returning it as a string.
// This is an early experimental API, ideally we should return a wrapper object
// around a Python binding to the MLIR module.
string ImportGraphDef(const string &proto, const string &pass_pipeline, TF_Status* status) {
  GraphDef graphdef;
  auto s = tensorflow::LoadProtoFromBuffer(proto, &graphdef);
  if (!s.ok()) {
    Set_TF_Status_from_Status(status, s);
    return "// error";
  }
  GraphDebugInfo debug_info;
  NodeSpecs specs;
  mlir::MLIRContext context;
  auto module = ConvertGraphdefToMlir(graphdef, debug_info, specs, &context);
  if (!module.ok()) {
    Set_TF_Status_from_Status(status, module.status());
    return "// error";
  }

  // Run the pass_pipeline on the module if not empty.
  if (!pass_pipeline.empty()) {
    mlir::PassManager pm(&context);
    std::string error;
    llvm::raw_string_ostream error_stream(error);
    if (failed(mlir::parsePassPipeline(pass_pipeline, pm, error_stream))) {
      TF_SetStatus(status, TF_INVALID_ARGUMENT,
                   ("Invalid pass_pipeline: " + error_stream.str()).c_str());
      return "// error";
    }

    mlir::StatusScopedDiagnosticHandler statusHandler(&context);
    if (failed(pm.run(*module.ValueOrDie()))) {
      Set_TF_Status_from_Status(status, statusHandler.ConsumeStatus());
      return "// error";
    }
  }
  return MlirModuleToString(*module.ConsumeValueOrDie());
}

}  // namespace swig
}  // namespace tensorflow

%}

%ignoreall

%unignore tensorflow;
%unignore tensorflow::swig;
%unignore tensorflow::swig::ImportGraphDef;

// Wrap this function
namespace tensorflow {
namespace swig {
static string ImportGraphDef(const string &graphdef,
                             const string &pass_pipeline,
                             TF_Status* status);
}  // namespace swig
}  // namespace tensorflow

%insert("python") %{
def import_graphdef(graphdef, pass_pipeline):
  return ImportGraphDef(str(graphdef).encode('utf-8'), pass_pipeline.encode('utf-8')).decode('utf-8');
%}

%unignoreall
