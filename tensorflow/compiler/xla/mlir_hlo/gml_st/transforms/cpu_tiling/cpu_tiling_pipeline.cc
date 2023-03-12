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

#include <algorithm>
#include <functional>
#include <iostream>

#include "gml_st/transforms/passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Transforms/Passes.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

namespace mlir {
namespace gml_st {

GmlStCPUTilingOptions getDefaultCPUPipelineOptions(StringRef cpuName) {
  GmlStCPUTilingOptions opts;
  opts.vectorSize = 8;
  opts.reduction1DTileSize = 32;
  opts.reduction2DTileSizes = {4, 4};
  opts.matmulTileSizes = {};
  opts.lowerToMmt4d = false;
  opts.cpuName = cpuName;
  return opts;
}

namespace {

int64_t roundDownToPowerOfTwo(int64_t n) {
  if ((n & (n - 1)) == 0) return n;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  n |= n >> 32;
  return (n + 1) >> 1;
}

// Tiling heuristic that was tuned for static power-of-two sized shapes on
// Skylake.
MatmulSizes skylakeTilingHeuristic(MatmulSizes sizes) {
  if (sizes.m == 1) {
    return {1, sizes.n, 1};
  }

  if (sizes.n == 1) {
    if (sizes.k <= 8) {
      return {1, 1, 1};
    }
    return {std::min<int64_t>(8, sizes.m), 1, 4};
  }

  MatmulSizes result;
  result.k = sizes.k <= 8 ? 1 : 4;
  result.n = std::min<int64_t>(8, sizes.n) << (sizes.m <= 16 ? 1 : 0);
  result.m = std::min<int64_t>(32, sizes.m) << (sizes.n <= 4 ? 1 : 0);
  return result;
}

// Tiling heuristic that was tuned for static power-of-two sized shapes on Zen
// v2 ("Rome").
MatmulSizes znver2TilingHeuristic(MatmulSizes sizes) {
  MatmulSizes result;
  result.k = sizes.n == 1 ? 8 : 1;
  if (sizes.n == 1) {
    result.m = sizes.k >= 32 ? 16 : 8;
  } else {
    result.m = sizes.n <= 8 ? 8 : 4;
  }
  if (sizes.m == 1) {
    result.n = std::min<int64_t>(64, sizes.n) * (sizes.k <= 64 ? 1 : 2);
  } else {
    result.n = std::min<int64_t>(16, sizes.n);
  }
  return result;
}

std::function<MatmulSizes(MatmulSizes)> wrapHeuristic(
    const std::function<MatmulSizes(MatmulSizes)>& heuristic,
    MatmulSizes dynamicDefault) {
  return [=](MatmulSizes sizes) {
    if (sizes.n < 0 || sizes.m < 0 || sizes.k < 0) {
      return dynamicDefault;
    }

    sizes.m = roundDownToPowerOfTwo(sizes.m);
    sizes.n = roundDownToPowerOfTwo(sizes.n);
    sizes.k = roundDownToPowerOfTwo(sizes.k);

    return heuristic(sizes);
  };
}

}  // namespace

void addCPUTilingPipeline(OpPassManager& pm,
                          const GmlStCPUTilingOptions& options) {
  using func::FuncOp;

  if (options.enableFusionClusters) {
    pm.addNestedPass<FuncOp>(createFusionPlanningForCpuPass());
  }

  // Outline and deduplicate fusion clusters.
  if (options.enableFusionClusterOutlining) {
    pm.addPass(createFusionOutliningPass());
    pm.addPass(func::createDuplicateFunctionEliminationPass());
  }

  pm.addNestedPass<FuncOp>(createTransformConvForCpuPass());
  pm.addNestedPass<FuncOp>(createTransformScatterForCpuPass());
  pm.addNestedPass<FuncOp>(createTransformReduceForCpuPass(
      options.vectorSize, options.reduction1DTileSize,
      options.reduction2DTileSizes));
  std::function<MatmulSizes(MatmulSizes)> tilingHeuristic;
  if (!options.matmulTileSizes.empty()) {
    MatmulSizes fixedSizes{options.matmulTileSizes[0],
                           options.matmulTileSizes[1],
                           options.matmulTileSizes[2]};
    tilingHeuristic = [=](MatmulSizes) { return fixedSizes; };
  } else {
    tilingHeuristic = options.cpuName.starts_with("znver")
                          ? wrapHeuristic(znver2TilingHeuristic, {16, 8, 8})
                          : wrapHeuristic(skylakeTilingHeuristic, {16, 16, 4});
  }
  pm.addNestedPass<FuncOp>(createTransformDotForCpuPass(tilingHeuristic));
  pm.addNestedPass<FuncOp>(
      createTransformMatmulForCpuPass(tilingHeuristic, options.lowerToMmt4d));
  // TODO(b/270534416): Re-enable.
  // pm.addNestedPass<FuncOp>(createTransformGenericForCpuPass());
  pm.addNestedPass<FuncOp>(createTransformTransposeForCpuPass());
  pm.addNestedPass<FuncOp>(createTransformMapForCpuPass(options.vectorSize));
  pm.addNestedPass<FuncOp>(createTransformSortForCpuPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::gml_st::createTransformReverseForCpuPass());

  pm.addNestedPass<FuncOp>(createInlineFusionClustersPass());

  pm.addPass(createCSEPass());
  pm.addPass(createCanonicalizerPass());

  pm.addNestedPass<FuncOp>(createRewriteForallOpPass());
  pm.addNestedPass<FuncOp>(createComposeExtractInsertSlicePass());
  pm.addNestedPass<FuncOp>(createVectorizeForCPUPass());

  // Tile remaining ops by size one and scalarize what we can.
  pm.addNestedPass<FuncOp>(createTileByOnePass());
  pm.addNestedPass<FuncOp>(createScalarizationPass());
}

void addDefaultCPUTilingPipeline(OpPassManager& pm, StringRef cpuName) {
  addCPUTilingPipeline(pm, getDefaultCPUPipelineOptions(cpuName));
}

}  // namespace gml_st
}  // namespace mlir
