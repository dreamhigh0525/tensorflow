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

//===- kernel_creator.cc ----------------------------------------*- C++ -*-===//
//
// This file implements the function to compile a TF kernel function to gpu
// binary (hsaco for AMD, cubin for NVIDIA) or to a gpu binary with host side.
//
//===----------------------------------------------------------------------===//
#include "tensorflow/compiler/mlir/tools/kernel_gen/kernel_creator.h"

#include "llvm/Support/raw_ostream.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"  // from @llvm-project
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"  // from @llvm-project
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"  // from @llvm-project
#include "mlir/Conversion/SCFToGPU/SCFToGPUPass.h"  // from @llvm-project
#include "mlir/Conversion/SCFToStandard/SCFToStandard.h"  // from @llvm-project
#include "mlir/Conversion/ShapeToStandard/ShapeToStandard.h"  // from @llvm-project
#include "mlir/Dialect/GPU/GPUDialect.h"  // from @llvm-project
#include "mlir/Dialect/GPU/ParallelLoopMapper.h"  // from @llvm-project
#include "mlir/Dialect/GPU/Passes.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/Linalg/Passes.h"  // from @llvm-project
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"  // from @llvm-project
#include "mlir/Dialect/SCF/Passes.h"  // from @llvm-project
#include "mlir/Dialect/SCF/SCF.h"  // from @llvm-project
#include "mlir/Dialect/SCF/Transforms.h"  // from @llvm-project
#include "mlir/Dialect/Shape/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Parser.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Transforms/Bufferize.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/chlo_ops.h"
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/dialect_registration.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/dump_mlir_util.h"
#include "tensorflow/compiler/mlir/tools/kernel_gen/transforms/passes.h"
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/path.h"

namespace tensorflow {
namespace kernel_gen {
namespace {

using tensorflow::Status;
using xla::InternalError;
using xla::StatusOr;

constexpr llvm::StringRef kGpuBinaryAttrName = "gpu.binary";

// TODO(herhut): Remove this once leftover tensor_to_memref are handled in core.
namespace {
struct RemoveUnusedTensorToMemrefOperations
    : public mlir::PassWrapper<RemoveUnusedTensorToMemrefOperations,
                               mlir::FunctionPass> {
  void runOnFunction() override {
    getFunction().walk([](mlir::TensorToMemrefOp op) {
      // Drop all tensor_to_memref that have no more users. Currently this will
      // not happen, as tensor_to_memref has a side-effect. See
      // https://reviews.llvm.org/D91967 for a dicsussion.
      if (op.memref().getUsers().empty()) {
        op.erase();
      }
    });
  }
};
}  // end anonymous namespace

Status LowerTFtoGPU(mlir::ModuleOp module, llvm::ArrayRef<uint32_t> tile_sizes,
                    llvm::ArrayRef<uint32_t> unroll_factors,
                    bool embed_memref_prints) {
  mlir::PassManager pm(module.getContext());
  applyTensorflowAndCLOptions(pm);

  pm.addNestedPass<mlir::FuncOp>(mlir::mhlo::createLegalizeTFPass(
      /*allow_partial_conversion=*/false, /*legalize_chlo=*/false));
  pm.addNestedPass<mlir::FuncOp>(mlir::createTransformUnrankedHloPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::mhlo::createChloLegalizeToHloPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createCanonicalizerPass());

  // Transform HLO operations to LinAlg.
  pm.addNestedPass<mlir::FuncOp>(::mlir::mhlo::createLegalizeHloToLinalgPass());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createCSEPass());
  // We have to anticipate later unrolling in tiling to make sure that we get
  // the requested tiling after unrolling. Compute the new tiling here if
  // needed.
  llvm::SmallVector<int64_t, 4> tiling_for_unrolling, inner_tile;
  tiling_for_unrolling.reserve(tile_sizes.size());
  for (auto pair : llvm::zip(tile_sizes, unroll_factors)) {
    tiling_for_unrolling.push_back(std::get<0>(pair) * std::get<1>(pair));
    inner_tile.push_back(std::get<1>(pair));
  }
  tiling_for_unrolling.append(
      tile_sizes.drop_front(unroll_factors.size()).begin(), tile_sizes.end());
  // Fuse linalg operations.
  pm.addNestedPass<mlir::FuncOp>(mlir::createLinalgFusionOfTensorOpsPass());

  // Partial bufferization: Transforms inparticular HLO and Linalg operations to
  // their corresponding LHLO operations and converts the function signature.
  // Leaves shape operations untouched.
  //
  // TODO(pifon): Rename the pass to CreateHloLinalgBufferizePass or bufferize
  // in 2 steps: first Linalg, then Hlo. That would need refactoring of
  // BufferizeTypeConverter.
  pm.addPass(mlir::kernel_gen::transforms::CreateHloBufferizePass());
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCanonicalizerPass());
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCSEPass());
  // Find candidates for buffer reuse. This is only successful if buffer size
  // equality can be determined based on `linalg.generic` operations.
  pm.addNestedPass<mlir::FuncOp>(
      mlir::kernel_gen::transforms::CreateBufferReusePass());
  pm.addNestedPass<mlir::FuncOp>(
      mlir::createLinalgTilingToParallelLoopsPass((tiling_for_unrolling)));
  // Transform the Linalg ops inside of the loop nest into parallel loops.
  pm.addNestedPass<mlir::FuncOp>(
      ::mlir::createConvertLinalgToParallelLoopsPass());
  // Canonicalize the code to simplify index computations. This is needed so
  // that loop bounds have the same value.
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCanonicalizerPass());
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCSEPass());
  // Fuse the inner-most loops.
  pm.addNestedPass<mlir::FuncOp>(
      mlir::kernel_gen::transforms::CreateFuseInnerParallelLoopsPass());
  // Run CSE to ensure that loads and stores to the same subview get
  // recognized as such.
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCSEPass());
  if (!unroll_factors.empty()) {
    pm.addNestedPass<mlir::FuncOp>(
        ::mlir::createParallelLoopTilingPass(inner_tile));
  }
  // Some basic cleanup.
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCanonicalizerPass());
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCSEPass());
  // Greedily map the remaining loop to GPU hardware dimensions.
  pm.addNestedPass<::mlir::FuncOp>(
      mlir::kernel_gen::transforms::CreateMapParallelLoopsPass());

  // Now lower the shape computations, bufferize all remaining ops and insert
  // deallocs.
  pm.addNestedPass<mlir::FuncOp>(::mlir::createBufferHoistingPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createCopyRemovalPass());
  // Expand memref_reshape to its ranked form so that we can propagate
  // scalars and avoid allocation.
  pm.addNestedPass<mlir::FuncOp>(mlir::createStdExpandOpsPass());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::kernel_gen::transforms::CreateShapeToDescriptorsPass());
  // Before bufferizing further, remove unused tensor_to_memref, so that we do
  // not create allocations for tensor computations that are not actually
  // needed.
  pm.addPass(mlir::createCanonicalizerPass());
  // TODO(herhut) Remove once handled in mlir core.
  pm.addNestedPass<mlir::FuncOp>(
      std::make_unique<RemoveUnusedTensorToMemrefOperations>());
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createCSEPass());
  // Before inserting more allocs, map the ones we already have to the
  // tf runtime. That ensures that all allocations for the actual computation
  // end up on the device, whereas allocations for shape computation and host
  // side things remain on the host.
  // Longer term, this should be handled by proper device placement.
  pm.addPass(mlir::kernel_gen::tf_framework::
                 CreateEmbedTFFrameworkFunctionAndAllocPass());
  pm.addPass(mlir::kernel_gen::transforms::CreateFinalBufferizePass());
  pm.addNestedPass<mlir::FuncOp>(mlir::createPromoteBuffersToStackPass(64));
  // TODO(herhut): Depends on https://bugs.llvm.org/show_bug.cgi?id=48385.
  // We also cannot properly free temporaries until
  // https://llvm.discourse.group/t/remove-tight-coupling-of-the-bufferdeallocation-pass-to-std-and-linalg-operations/2162
  // is resolved.
  // pm.addNestedPass<mlir::FuncOp>(::mlir::createBufferDeallocationPass());
  // Apply the mapping and go to GPU. We cannot do this earlier due to missing
  // interfaces on the GPU dialect.
  // TODO(b/174830459): Move up once implemented.
  pm.addNestedPass<::mlir::FuncOp>(mlir::createParallelLoopToGpuPass());

  // Some basic cleanup.
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCanonicalizerPass());
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCSEPass());
  // Make loops with min bounds into a conditional plus static bounds.
  // Only do this if we unrolled in the first place.
  if (!unroll_factors.empty()) {
    pm.addNestedPass<::mlir::FuncOp>(mlir::createForLoopSpecializationPass());
  }
  // Approximate Tanh using standard operations.
  pm.addNestedPass<::mlir::FuncOp>(
      ::mlir::mhlo::createLegalizeTrigonometricToApproximationPass());
  // Take launches to launches with kernels.
  pm.addPass(::mlir::createGpuKernelOutliningPass());

  pm.addPass(::mlir::createLowerAffinePass());
  // Constraints are removed as late as possible and before lowering to CFG.
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createConvertShapeConstraintsPass());
  pm.addNestedPass<::mlir::FuncOp>(::mlir::createCanonicalizerPass());
  pm.addPass(::mlir::createLowerToCFGPass());
  // Map asserts to the tensorflow framework.
  pm.addPass(
      mlir::kernel_gen::tf_framework::CreateEmbedTFFrameworkAssertPass());
  if (embed_memref_prints) {
    pm.addNestedPass<::mlir::FuncOp>(
        mlir::kernel_gen::transforms::CreateEmbedMemRefPrintsPass());
  }
  if (failed(pm.run(module))) {
    return InternalError("Lowering to GPU kernels failed.");
  }
  return Status::OK();
}

Status LowerKernelBodiesToLowLevelIr(mlir::ModuleOp module) {
#if !defined(TENSORFLOW_USE_ROCM) && !defined(GOOGLE_CUDA)
  return InternalError(
      "Neither TENSORFLOW_USE_ROCM nor GOOGLE_CUDA are defined."
      " Did you specify either --config=rocm or --config=cuda ?");
#endif
  mlir::PassManager pm(module.getContext());
  // We cannot verify as the signature of the kernel is rewritten.
  // pm.enableVerifier(false);
  tensorflow::applyTensorflowAndCLOptions(pm);
  auto& kernelPm = pm.nest<::mlir::gpu::GPUModuleOp>();
  kernelPm.addPass(::mlir::createLowerToCFGPass());
#if TENSORFLOW_USE_ROCM
  kernelPm.addPass(mlir::kernel_gen::transforms::CreateGpuKernelToRocdlPass());
#elif GOOGLE_CUDA
  kernelPm.addPass(mlir::kernel_gen::transforms::CreateGpuKernelToNvvmPass());
#endif
  // Remove all location information to prevent a debug build.
  pm.addPass(::mlir::createStripDebugInfoPass());

  if (failed(pm.run(module))) {
    return InternalError("Lowering to low-level device IR failed.");
  }

  return Status::OK();
}

Status AmendKernelLLVMIRWithStaticKnowledge(mlir::ModuleOp module) {
  mlir::PassManager pm(module.getContext());
  applyTensorflowAndCLOptions(pm);

  pm.addNestedPass<mlir::FuncOp>(
      mlir::kernel_gen::transforms::CreatePropagateShapeKnowledgeToKernels());
  pm.addNestedPass<mlir::FuncOp>(
      mlir::kernel_gen::transforms::CreatePropagateTfAbiKnowledgeToKernels());

  return failed(pm.run(module))
             ? InternalError("Amending LLVMIR with static knowledge failed.")
             : Status::OK();
}

Status GenerateDeviceCode(mlir::ModuleOp module,
                          llvm::StringRef gpu_binary_attr_name,
                          llvm::ArrayRef<std::string> architectures,
                          bool generate_fatbin, bool print_ptx,
                          bool enable_ftz) {
  mlir::PassManager pm(module.getContext());
  applyTensorflowAndCLOptions(pm);

  auto& kernel_pm = pm.nest<mlir::gpu::GPUModuleOp>();
  // Remove debug information to ensure we do not create debug PTX.
  kernel_pm.addPass(mlir::createStripDebugInfoPass());
  kernel_pm.addPass(mlir::kernel_gen::transforms::CreateGpuKernelToBlobPass(
      gpu_binary_attr_name, architectures, generate_fatbin, print_ptx,
      enable_ftz));

  return failed(pm.run(module))
             ? InternalError("Generating device code failed.")
             : Status::OK();
}

Status LowerHostSideToFinalForm(mlir::ModuleOp module) {
  mlir::PassManager pm(module.getContext());
  applyTensorflowAndCLOptions(pm);

  pm.addPass(mlir::kernel_gen::transforms::CreateTFKernelToLLVMPass(
      kGpuBinaryAttrName));
  pm.addPass(mlir::createCanonicalizerPass());
  pm.addPass(mlir::createCSEPass());

  return failed(pm.run(module))
             ? InternalError("Final lowering of host side failed.")
             : Status::OK();
}

}  // namespace

StatusOr<mlir::OwningModuleRef> GenerateKernelForTfCode(
    mlir::MLIRContext& context, llvm::StringRef tf_code,
    llvm::ArrayRef<std::string> architectures,
    llvm::ArrayRef<uint32_t> tile_sizes,
    llvm::ArrayRef<uint32_t> unroll_factors, bool embed_memref_prints,
    bool generate_fatbin, bool print_ptx, bool enable_ftz) {
  auto& registry = context.getDialectRegistry();
  mlir::RegisterAllTensorFlowDialects(registry);
  registry.insert<mlir::chlo::HloClientDialect, mlir::mhlo::MhloDialect>();
  mlir::OwningModuleRef module = mlir::parseSourceString(tf_code, &context);
  TF_RETURN_IF_ERROR(LowerTFtoGPU(module.get(), tile_sizes, unroll_factors,
                                  embed_memref_prints));
  TF_RETURN_IF_ERROR(LowerKernelBodiesToLowLevelIr(module.get()));
  TF_RETURN_IF_ERROR(AmendKernelLLVMIRWithStaticKnowledge(module.get()));
  TF_RETURN_IF_ERROR(GenerateDeviceCode(module.get(), kGpuBinaryAttrName,
                                        architectures, generate_fatbin,
                                        print_ptx, enable_ftz));
  TF_RETURN_IF_ERROR(LowerHostSideToFinalForm(module.get()));
  return module;
}

StatusOr<std::string> ExtractGpuBinary(mlir::ModuleOp module) {
  auto gpu_modules = module.getOps<mlir::gpu::GPUModuleOp>();
  if (std::distance(gpu_modules.begin(), gpu_modules.end()) != 1) {
    return InternalError("There should be exactly one GPU Module");
  }
  mlir::gpu::GPUModuleOp gpu_mod = *gpu_modules.begin();
  auto blob = gpu_mod->getAttrOfType<mlir::StringAttr>(kGpuBinaryAttrName);
  if (blob == nullptr) {
    return InternalError("No binary blob found in the module");
  }
  return blob.getValue().str();
}

}  // namespace kernel_gen
}  // namespace tensorflow
