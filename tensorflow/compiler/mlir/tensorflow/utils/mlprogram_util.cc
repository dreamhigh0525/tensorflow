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

#include "tensorflow/compiler/mlir/tensorflow/utils/mlprogram_util.h"

#include <string>
#include <utility>

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/Twine.h"
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "tensorflow/cc/saved_model/loader.h"
#include "tensorflow/cc/saved_model/tag_constants.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/bridge.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/tf_saved_model_passes.h"
#include "tensorflow/compiler/mlir/tensorflow/translate/import_model.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/compile_mlir_util.h"
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"

namespace tensorflow {

void PopulateLowerToMlProgramAndHloPipeline(mlir::OpPassManager& pm) {
  mlir::TF::CreateTFXLABridgePipeline(pm);

  // Has to be a non-empty string, so tf2xla fallbacks kick in.
  llvm::StringRef tf2xla_fallback_device_type = "cpu/gpu/tpu";

  pm.addNestedPass<mlir::func::FuncOp>(mlir::mhlo::createLegalizeTFPass(
      /*allow_partial_conversion=*/true, /*legalize_chlo=*/true,
      tf2xla_fallback_device_type, /*prefer_tf2xla=*/false));

  // Remove unused global tensors, or make then immutable if possible.
  pm.addPass(mlir::tf_saved_model::CreateOptimizeGlobalTensorsPass());

  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::tf_saved_model::CreateLowerVariableOpsToMlProgramPass());
  pm.addPass(mlir::tf_saved_model::CreateLowerGlobalsToMlProgramPass());

  pm.addPass(mlir::createInlinerPass());
  pm.addPass(mlir::createSymbolDCEPass());
  pm.addPass(mlir::createCanonicalizerPass());
}

mlir::LogicalResult LowerToMlProgramAndHlo(mlir::ModuleOp module) {
  mlir::PassManager pm(module.getContext());
  PopulateLowerToMlProgramAndHloPipeline(pm);
  return pm.run(module);
}

void RegisterMlProgramPasses() {
  mlir::registerPassPipeline(
      "tf-lower-to-mlprogram-and-hlo", "Lower TF to ml_program + mhlo",
      [](mlir::OpPassManager& pm, llvm::StringRef options,
         llvm::function_ref<mlir::LogicalResult(const llvm::Twine&)>
             errorHandler) {
        PopulateLowerToMlProgramAndHloPipeline(pm);
        return mlir::success();
      },
      [](llvm::function_ref<void(const mlir::detail::PassOptions&)>) {});
}

}  // namespace tensorflow
