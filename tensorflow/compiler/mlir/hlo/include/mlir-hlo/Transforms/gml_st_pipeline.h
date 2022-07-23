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

#ifndef MLIR_HLO_TRANSFORMS_GML_ST_PIPELINE_H
#define MLIR_HLO_TRANSFORMS_GML_ST_PIPELINE_H

#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassOptions.h"

namespace mlir {
struct GmlStPipelineOptions
    : public mlir::PassPipelineOptions<GmlStPipelineOptions> {
  ListOption<int64_t> tileSizes{
      *this, "tile-sizes", llvm::cl::desc("Tiling sizes for the tiling pass")};
  Option<bool> fuse{*this, "fuse",
                    llvm::cl::desc("Fuse into GmlSt loop nests."),
                    llvm::cl::init(false)};
  Option<bool> lowerToLoops{
      *this, "lower-to-loops",
      llvm::cl::desc("Enable bufferization and lowering to SCF dialect for "
                     "GmlSt and Linalg ops."),
      llvm::cl::init(false)};
};

void createGmlStPipeline(mlir::OpPassManager& pm,
                         const GmlStPipelineOptions& options);

}  // namespace mlir

#endif  // MLIR_HLO_TRANSFORMS_GML_ST_PIPELINE_H
