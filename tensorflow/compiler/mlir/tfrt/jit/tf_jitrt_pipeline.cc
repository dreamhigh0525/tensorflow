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

#include "tensorflow/compiler/mlir/tfrt/jit/tf_jitrt_pipeline.h"

#include "mlir/Conversion/BufferizationToMemRef/BufferizationToMemRef.h"
#include "mlir/Conversion/ComplexToStandard/ComplexToStandard.h"
#include "mlir/Conversion/ShapeToStandard/ShapeToStandard.h"
#include "mlir/Conversion/VectorToSCF/VectorToSCF.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/Shape/Transforms/Passes.h"
#include "mlir/Dialect/Tensor/Transforms/Passes.h"
#include "mlir/Transforms/Passes.h"
#include "tensorflow/compiler/jit/flags.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/compiler/mlir/tfrt/jit/transforms/tf_jitrt_passes.h"
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir/backends/cpu/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir/runtime/ir/rt_dialect.h"
#include "tensorflow/compiler/xla/mlir/runtime/transforms/compiler.h"
#include "tensorflow/compiler/xla/mlir_hlo/gml_st/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir_hlo/gml_st/transforms/transforms.h"
#include "tensorflow/compiler/xla/mlir_hlo/mhlo/transforms/passes.h"
#include "tensorflow/compiler/xla/mlir_hlo/transforms/passes.h"

// -------------------------------------------------------------------------- //
// Custom passes that are missing upstream.
// -------------------------------------------------------------------------- //

namespace tensorflow {
namespace {

using mlir::OpPassManager;
using mlir::func::FuncOp;

// Adds a Tensorflow producer version to the module to enable shape inference.
struct AddTensorflowProducerVersion
    : public mlir::PassWrapper<AddTensorflowProducerVersion,
                               mlir::OperationPass<mlir::ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AddTensorflowProducerVersion)

  void runOnOperation() override {
    mlir::ModuleOp module = getOperation();

    // Tensorflow producer version does not really impact anything during the
    // shape inference. Set it to `0` (any random number will do the work) to
    // bypass attribute checks.
    mlir::Builder builder(module);
    auto version =
        builder.getNamedAttr("producer", builder.getI32IntegerAttr(0));
    module->setAttr("tf.versions", builder.getDictionaryAttr({version}));
  }
};

// Adds Linalg passes to perform fusion, tiling, peeling and vectorization.
void AddLinalgTransformations(OpPassManager& pm,
                              const TfJitRtPipelineOptions& options) {
  pm.addNestedPass<FuncOp>(CreateFusionPass());

  if (!options.vectorize) return;

  pm.addNestedPass<FuncOp>(CreateDetensorizeLinalgPass());

  pm.addNestedPass<FuncOp>(CreateTileReductionPass(
      options.vector_size, options.reduction_1d_tile_size,
      options.reduction_2d_tile_sizes));

  if (options.vectorize && options.codegen_transpose)
    pm.addNestedPass<FuncOp>(CreateTileTransposePass());
  pm.addNestedPass<FuncOp>(CreateTileCWisePass(options.vector_size));
  if (options.peel) {
    pm.addNestedPass<FuncOp>(CreatePeelTiledLoopsPass());
  }
  pm.addNestedPass<FuncOp>(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());
  if (options.fuse_fill) {
    pm.addNestedPass<FuncOp>(CreateFuseFillIntoTiledReductionPass());
  }
  pm.addNestedPass<FuncOp>(CreateTileFillPass(options.vector_size));
  pm.addNestedPass<FuncOp>(mlir::gml_st::createCollapseMaterializeOpsPass());
  pm.addNestedPass<FuncOp>(mlir::gml_st::createVectorizeGmlStLoopsPass());
  pm.addNestedPass<FuncOp>(mlir::gml_st::createLowerVectorContractPass());
}

void AddBufferizationPasses(OpPassManager& pm) {
  // Rewrite tensor.empty ops to bufferization.alloc_tensor ops.
  pm.addNestedPass<FuncOp>(
      mlir::bufferization::createEmptyTensorToAllocTensorPass());
  pm.addPass(mlir::hlo::createOneShotBufferizePass());
}

}  // namespace

// -------------------------------------------------------------------------- //
// Assemble a TF JitRt pipeline to lower from Tensorflow dialects to Linalg on
// buffers via progressive lowering to MHLO and Linalg.
// -------------------------------------------------------------------------- //
void CreateTfJitRtPipeline(OpPassManager& pm,
                           const TfJitRtPipelineOptions& options) {
  // Break Tensorflow fused operations into primitive operations before
  // lowering to HLO.
  pm.addNestedPass<FuncOp>(CreateFissionPass());

  // Run shape inference to propagate potentially specialized input shapes.
  pm.addPass(std::make_unique<AddTensorflowProducerVersion>());
  pm.addPass(mlir::TF::CreateTFShapeInferencePass());
  pm.addPass(mlir::createCanonicalizerPass());

  // This will add regions to IfOp/WhileOp (turning them into IfRegionOp
  // and WhileRegionOp), but be aware that those regions will still contain
  // calls.
  pm.addPass(mlir::TF::CreateTFFunctionalControlFlowToRegions());

  // Transform TF operation to HLO.
  pm.addPass(mlir::mhlo::createLegalizeTFControlFlowPass());
  pm.addNestedPass<FuncOp>(mlir::mhlo::createLegalizeTFPass());

  if (options.legalize_i1_tensors) {
    // Convert 'i1' tensors into 'i8' tensors.
    pm.addPass(CreateJitRtLegalizeI1TypesPass());
  }

  // Remove redundant shape operations left after legalizing to HLO.
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createCSEPass());

  // Analyze shapes and try to simplify the IR early.
  pm.addNestedPass<FuncOp>(mlir::mhlo::createSymbolicShapeOptimizationPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());

  // Move up broadcasting operations to allow for more fusion opportunities.
  pm.addNestedPass<FuncOp>(mlir::mhlo::createMergeAssumingOpsPass());
  pm.addNestedPass<FuncOp>(mlir::mhlo::createBroadcastPropagationPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());

  // Group reduction and parallel dimensions of reduction operations and realize
  // them through equivalent 1D or 2D reductions, if possible.
  pm.addNestedPass<FuncOp>(mlir::mhlo::createGroupReductionDimensionsPass());

  // Also, try to simplify reshape operations.
  pm.addNestedPass<FuncOp>(mlir::mhlo::createSymbolicShapeOptimizationPass());

  // Transform HLO operations to Linalg and Standard.
  pm.addNestedPass<FuncOp>(mlir::mhlo::createLegalizeControlFlowPass());
  pm.addNestedPass<mlir::func::FuncOp>(mlir::mhlo::createLegalizeSortPass());
  pm.addNestedPass<FuncOp>(xla::cpu::createLegalizeCollectiveOpsPass());
  pm.addNestedPass<FuncOp>(mlir::mhlo::createLegalizeHloToLinalgPass(
      /*enablePrimitiveOps=*/options.enable_xla_cpu_transformations));
  pm.addPass(mlir::mhlo::createLegalizeToArithmeticPass());
  pm.addNestedPass<FuncOp>(
      mlir::mhlo::createLegalizeHloShapeOpsToStandardPass());

  // Now that all compute operations are converted to standard (as a side effect
  // of bufferizing to memref dialect) we can remove the remaining references
  // to unsigned types.
  pm.addPass(mlir::mhlo::createConvertToSignlessPass());

  // Lower shape dialect to standard to enable linalg canonicalizations (e.g.
  // use linalg inputs instead of outputs for memref.dim operations).
  pm.addNestedPass<FuncOp>(mlir::mhlo::createShapeSimplification());
  pm.addNestedPass<FuncOp>(mlir::createShapeToShapeLowering());
  pm.addPass(mlir::createConvertShapeToStandardPass());
  pm.addNestedPass<FuncOp>(mlir::createConvertShapeConstraintsPass());

  // Fuse Linalg on tensors operations.
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::memref::createResolveShapedTypeResultDimsPass());
  // Lower index cast on tensors to tensor.generate.
  pm.addNestedPass<FuncOp>(mlir::createLowerIndexCastPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());

  // Convert complex types.
  pm.addPass(mlir::createConvertComplexToStandardPass());

  // Add passes to perform fusion, tiling, peeling and vectorization.
  if (options.enable_xla_cpu_transformations) {
    mlir::gml_st::GmlStCPUPipelineOptions gml_st_opts;
    gml_st_opts.vectorize = options.vectorize;
    gml_st_opts.vectorSize = options.vector_size;
    gml_st_opts.reduction1DTileSize = options.reduction_1d_tile_size;
    gml_st_opts.reduction2DTileSizes = options.reduction_2d_tile_sizes;
    gml_st_opts.matmulTileSizes = options.matmul_tile_sizes;
    gml_st_opts.lowerToMmt4d = options.lower_to_mmt4d;

    mlir::gml_st::addTileableOpsTransformationsForCPU(pm, gml_st_opts);
  } else {
    AddLinalgTransformations(pm, options);
  }

  // Inline everything, bufferization doesn't model ownership across calls.
  pm.addPass(mlir::createInlinerPass());

  // Always run canonicalizer (which does dead code removal) before bufferizing
  // anything.
  pm.addPass(mlir::createCanonicalizerPass());

  AddBufferizationPasses(pm);

  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());

  if (options.vectorize)
    pm.addNestedPass<FuncOp>(mlir::gml_st::createVectorizeCopyPass());

  if (options.enable_xla_cpu_transformations)
    pm.addNestedPass<FuncOp>(mlir::gml_st::createSimplifyDeadCopyPass());

  // Deallocate all temporary buffers.
  pm.addNestedPass<FuncOp>(mlir::bufferization::createBufferDeallocationPass());

  // Do trivial buffer forwarding across linalg.generic operations.
  pm.addNestedPass<FuncOp>(CreateLinalgTrivialBufferForwardingPass());

  // Remove trivial copy operations.
  pm.addNestedPass<FuncOp>(CreateLinalgTrivialCopyRemovalPass());

  if (options.vectorize)
    pm.addNestedPass<FuncOp>(mlir::gml_st::createGmlStToScfPass());

  pm.addPass(mlir::createBufferizationToMemRefPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createCanonicalizerPass());

  if (options.vectorize && options.codegen_transpose)
    pm.addNestedPass<FuncOp>(CreateLowerVectorTransposePass());

  mlir::VectorTransferToSCFOptions vec_to_scf_options;
  vec_to_scf_options.unroll = true;
  pm.addNestedPass<FuncOp>(
      mlir::createConvertVectorToSCFPass(vec_to_scf_options));
  pm.addNestedPass<FuncOp>(createRewriteVectorMultiReductionPass());

  pm.addNestedPass<FuncOp>(CreateMathApproximationPass({"all"}));
}

void CreateDefaultTfJitRtPipeline(OpPassManager& pm) {
  TfJitRtPipelineOptions options;
  options.vectorize = tensorflow::GetJitRtFlags().vectorize;
  CreateTfJitRtPipeline(pm, options);
}

void CreateJitRtSpecializationPipeline(xla::runtime::PassManager& passes) {
  passes->addPass(std::make_unique<AddTensorflowProducerVersion>());
  passes->addPass(mlir::TF::CreateTFShapeInferencePass());
  passes->addPass(mlir::createCanonicalizerPass());
}

static mlir::PassPipelineRegistration<TfJitRtPipelineOptions> tf_jitrt_pipeline(
    "tf-jitrt-pipeline",
    "Convert Tensorflow dialect to TFRT's JitRt compatible dialects",
    CreateTfJitRtPipeline);

}  // namespace tensorflow
