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

// This file implements logic for lowering LHLO GPU dialect to TFRT CUDA
// dialect.

#include "tensorflow/compiler/mlir/tfrt/transforms/lhlo_gpu_to_tfrt_gpu/gpu_passes.h"

#include <memory>
#include <utility>

#include "mlir-hlo/Dialect/mhlo/IR/lhlo_gpu_ops.h"
#include "mlir-hlo/Dialect/mhlo/IR/lhlo_ops.h"
#include "mlir/Dialect/GPU/GPUDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/Transforms/FuncConversions.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/Types.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/lhlo_ops.h"
#include "tensorflow/compiler/xla/service/gpu/xlir_ops.h"
#include "tfrt/gpu/kernels/gpu_ops.h"  // from @tf_runtime
#include "tfrt/gpu/passes/passes.h"  // from @tf_runtime
#include "tfrt/basic_kernels/opdefs/basic_kernels.h"  // from @tf_runtime

namespace tensorflow {

void populateCclConversionPattern(RewritePatternSet&, TypeConverter&);
void populateCustomCallConversionPattern(RewritePatternSet&, TypeConverter&);
void populateGemmConversionPattern(RewritePatternSet&, TypeConverter&);
void populateMemcpyConversionPattern(RewritePatternSet&, TypeConverter&);
void populateMemsetConversionPattern(RewritePatternSet&, TypeConverter&);

#define GEN_PASS_CLASSES
#include "tensorflow/compiler/mlir/tfrt/transforms/lhlo_gpu_to_tfrt_gpu/gpu_passes.h.inc"

namespace {

struct ConvertLmhloToGpuPass
    : public ConvertLmhloToGpuPassBase<ConvertLmhloToGpuPass> {
 private:
  void runOnFunction() override;
};

}  // namespace

static Value MaterializeCast(OpBuilder& builder, Type type, ValueRange values,
                             Location loc) {
  auto cast_op = builder.create<UnrealizedConversionCastOp>(loc, type, values);
  return cast_op.getResult(0);
}

void ConvertLmhloToGpuPass::runOnFunction() {
  auto* context = &getContext();

  TypeConverter converter;
  converter.addConversion([](Type type) { return type; });
  auto buffer_type = tfrt::gpu::BufferType::get(context);
  converter.addConversion([&](BaseMemRefType) { return buffer_type; });
  converter.addArgumentMaterialization(MaterializeCast);
  converter.addSourceMaterialization(MaterializeCast);
  converter.addTargetMaterialization(MaterializeCast);

  RewritePatternSet patterns(context);
  populateCclConversionPattern(patterns, converter);
  populateCustomCallConversionPattern(patterns, converter);
  populateGemmConversionPattern(patterns, converter);
  populateMemcpyConversionPattern(patterns, converter);
  populateMemsetConversionPattern(patterns, converter);
  populateFuncOpTypeConversionPattern(patterns, converter);
  populateReturnOpTypeConversionPattern(patterns, converter);

  // Set of ops that need to be wrapped in tfrt_gpu_conversion.async.execute
  // before lowering directly to tfrt_gpu ops (and therefore require some chain
  // and stream, which the wrapper op provides as block arguments). On the other
  // hand, ops which lower to the gpu dialect do not need to be wrapped.
  ConversionTarget wrap_target(*context);
  wrap_target
      .addLegalDialect<lmhlo_gpu::LmhloGpuDialect, mlir::gpu::GPUDialect>();
  wrap_target.addLegalOp<lmhlo::AllGatherOp, lmhlo::AllReduceOp,
                         lmhlo::ReduceScatterOp, lmhlo::AllToAllOp,
                         lmhlo::CollectivePermuteOp, lmhlo::CustomCallOp>();
  tfrt::gpu::populateGpuAsyncConversionPatterns(patterns, converter,
                                                wrap_target);

  ConversionTarget target(*context);
  target.addIllegalOp<memref::ReinterpretCastOp, memref::ViewOp>();
  target.addDynamicallyLegalOp<FuncOp>([&](FuncOp op) {
    return converter.isSignatureLegal(op.getType()) &&
           converter.isLegal(&op.body());
  });
  target.addDynamicallyLegalOp<tfrt::gpu::conversion::AsyncExecuteOp>(
      [&](tfrt::gpu::conversion::AsyncExecuteOp op) {
        return converter.isLegal(&op.body());
      });
  target.markUnknownOpDynamicallyLegal([&](Operation* op) {
    if (op->hasTrait<OpTrait::ReturnLike>()) return converter.isLegal(op);
    return !wrap_target.isLegal(op);  // Wrapped ops are immediately lowered.
  });

  if (failed(
          applyPartialConversion(getOperation(), target, std::move(patterns))))
    return signalPassFailure();
}

std::unique_ptr<FunctionPass> createConvertLmhloToGpuPass() {
  return std::make_unique<ConvertLmhloToGpuPass>();
}

}  // namespace tensorflow
