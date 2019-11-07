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

#include "tensorflow/compiler/mlir/tensorflow/transforms/bridge.h"

#include "mlir/Pass/PassManager.h"  // TF:local_config_mlir
#include "mlir/Transforms/Passes.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/error_util.h"

namespace mlir {
namespace TFTPU {

void createTPUBridge(OpPassManager &pm) {
  OpPassManager &func_pm = pm.nest<FuncOp>();
  func_pm.addPass(tf_executor::CreateTFExecutorIslandCoarseningPass());
  func_pm.addPass(createCanonicalizerPass());
  func_pm.addPass(CreateTPUClusterFormationPass());
  func_pm.addPass(tf_executor::CreateTFExecutorConstantSinkingPass());
  func_pm.addPass(TFDevice::CreateResourceOpLiftingPass());

  pm.addPass(TFDevice::CreateClusterOutliningPass());
  pm.addPass(CreateTPURewritePass());
  pm.addNestedPass<FuncOp>(createCanonicalizerPass());
}

tensorflow::Status TPUBridge(ModuleOp module) {
  // Populate a passmanager with the list of passes that implement the bridge.
  PassManager bridge(module.getContext());
  createTPUBridge(bridge);

  // Run the bridge on the module, in case of failure, the `diag_handler`
  // converts MLIR errors emitted to the MLIRContext into a tensorflow::Status.
  mlir::StatusScopedDiagnosticHandler diag_handler(module.getContext());
  if (failed(bridge.run(module))) return diag_handler.ConsumeStatus();
  return diag_handler.ConsumeStatus();
}

}  // namespace TFTPU
}  // namespace mlir
