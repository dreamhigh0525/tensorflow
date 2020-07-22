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

// This transformation pass transforms functional control flow operations in the
// TensorFlow dialect to their region based counterparts, i.e.,
// tf.If -> tf.IfRegion and tf.While -> tf.WhileRegion

#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/Verifier.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Pass/PassRegistry.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_types.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"

#define DEBUG_TYPE "tf-functional-cf-to-region"

namespace mlir {
namespace TF {

namespace {

struct FunctionalControlFlowToRegions
    : public PassWrapper<FunctionalControlFlowToRegions,
                         OperationPass<ModuleOp>> {
  void runOnOperation() override;
};

// Creates a call to function `callee` in region `caller_region`. Use `args` as
// the call arguments, and terminate the region with a yield. The arguments are
// cast to the required type before the call. `use_region_args` control whether
// the input arguments are used as is (for IfOp) or block arguments of the same
// type as the input arguments are created and then used as call arguments (for
// While).
void CreateCall(Operation* op, StringRef callee, Region& caller_region,
                ValueRange args, bool use_region_args) {
  assert(caller_region.empty() &&
         "Expected empty region for newly created ops");
  OpBuilder builder(caller_region);
  Block* entry = builder.createBlock(&caller_region);
  auto func = op->getParentOfType<ModuleOp>().lookupSymbol<FuncOp>(callee);

  if (use_region_args) {
    entry->addArguments(args.getType());
    args = entry->getArguments();
  }
  llvm::SmallVector<Value, 4> casted_args;
  casted_args.reserve(func.getNumArguments());
  for (const auto& ArgAndType : zip(args, func.getType().getInputs())) {
    Value arg = std::get<0>(ArgAndType);
    Type expected_type = std::get<1>(ArgAndType);
    if (arg.getType() != expected_type) {
      arg = builder.create<CastOp>(op->getLoc(), expected_type, arg,
                                   /*Truncate=*/builder.getBoolAttr(false));
    }
    casted_args.push_back(arg);
  }
  auto call = builder.create<CallOp>(op->getLoc(), func, casted_args);
  builder.create<YieldOp>(op->getLoc(), call.getResults());
}

// Transform a functional IfOp to a region based IfRegionOp.
LogicalResult ConvertIfOp(IfOp if_op) {
  auto if_region = OpBuilder(if_op).create<TF::IfRegionOp>(
      if_op.getLoc(), if_op.getResultTypes(), if_op.cond(),
      if_op.is_stateless());

  CreateCall(if_op, /*callee=*/if_op.then_branch(),
             /*caller_region=*/if_region.then_branch(), if_op.input(),
             /*use_region_args=*/false);
  CreateCall(if_op, /*callee=*/if_op.else_branch(),
             /*caller_region=*/if_region.else_branch(), if_op.input(),
             /*use_region_args=*/false);
  if_op.replaceAllUsesWith(if_region.getResults());
  if_op.erase();
  return success();
}

LogicalResult ConvertWhileOp(WhileOp while_op) {
  auto while_region = OpBuilder(while_op).create<TF::WhileRegionOp>(
      while_op.getLoc(), while_op.getResultTypes(), while_op.input(),
      while_op.is_stateless(), while_op.parallel_iterations());

  CreateCall(while_op, while_op.cond(), while_region.cond(), while_op.input(),
             /*use_region_args=*/true);
  CreateCall(while_op, while_op.body(), while_region.body(), while_op.input(),
             /*use_region_args=*/true);
  while_op.replaceAllUsesWith(while_region.getResults());
  while_op.erase();
  return success();
}

void FunctionalControlFlowToRegions::runOnOperation() {
  ModuleOp module = getOperation();
  auto result = module.walk([](Operation* op) {
    if (IfOp if_op = llvm::dyn_cast<IfOp>(op)) {
      if (failed(ConvertIfOp(if_op))) {
        op->emitOpError() << "failed to convert to region form";
        return WalkResult::interrupt();
      }
    } else if (auto while_op = llvm::dyn_cast<WhileOp>(op)) {
      if (failed(ConvertWhileOp(while_op))) {
        op->emitOpError() << "failed to convert to region form";
        return WalkResult::interrupt();
      }
    }
    return WalkResult::advance();
  });
  if (result.wasInterrupted()) return signalPassFailure();
}
}  // namespace

std::unique_ptr<OperationPass<ModuleOp>>
CreateTFFunctionalControlFlowToRegions() {
  return std::make_unique<FunctionalControlFlowToRegions>();
}

static PassRegistration<FunctionalControlFlowToRegions> pass(
    "tf-functional-control-flow-to-regions",
    "Transform functional control flow Ops to Region based counterparts");

}  // namespace TF
}  // namespace mlir
