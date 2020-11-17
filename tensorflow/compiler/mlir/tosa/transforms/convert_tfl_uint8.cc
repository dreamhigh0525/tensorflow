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

// This pass converts a TFLite uint8 graph to the int8 domain, with adaptors at
// input and output tensors. This is needed because TOSA precision is
// implemented in the int8 domain. This pass does:
// 1. match TFL::QConst with uint8, generate TFL::QConst with int8 with value
// remapped.
// 2. insert tosa.RESCALE uint8 -> int8 if block argument (placeholder of graph)
// is uint8 typed.
// 3. insert tosa.RESCALE int8 -> uint8 if original returned tensor is uint8
// typed.

#include <climits>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>

#include "mlir/Dialect/Quant/FakeQuantSupport.h"
#include "mlir/Dialect/Quant/UniformSupport.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/Dialect/Tosa/Utils/QuantUtils.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/IR/Types.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"
#include "tensorflow/compiler/mlir/tosa/transforms/legalize_common.h"
#include "tensorflow/compiler/mlir/tosa/transforms/legalize_utils.h"
#include "tensorflow/compiler/mlir/tosa/transforms/passes.h"

#define PASS_NAME "tosa-convert-tfl-uint8"
#define DEBUG_TYPE PASS_NAME

namespace mlir {

namespace tosa {

namespace {
// Performs lowering to TOSA dialect.
class ConvertUint8ToInt8
    : public PassWrapper<ConvertUint8ToInt8, FunctionPass> {
 public:
  explicit ConvertUint8ToInt8() {}
  void runOnFunction() override;
};

struct ConvertUint8QConstOp : public RewritePattern {
  explicit ConvertUint8QConstOp(MLIRContext *context)
      : RewritePattern(TFL::QConstOp::getOperationName(), 1, context) {}

  LogicalResult matchAndRewrite(Operation *op, PatternRewriter &builder) const {
    auto tfl_qconst_op = cast<TFL::QConstOp>(op);

    // Skip if it's not ranked tensor type.
    auto output_type =
        tfl_qconst_op.getResult().getType().dyn_cast<mlir::RankedTensorType>();
    if (!output_type) return failure();

    // Skip if output is not per-tensor quantized type.
    auto output_element_type =
        output_type.getElementType()
            .dyn_cast<mlir::quant::UniformQuantizedType>();
    if (!output_element_type) return failure();

    // Skip if output is not uint8.
    if (output_element_type.isSigned() ||
        output_element_type.getStorageTypeIntegralWidth() != 8) {
      return failure();
    }

    auto src_dense_attr =
        op->getAttr("value").dyn_cast<mlir::DenseElementsAttr>();

    double type_range_min = double(output_element_type.getStorageTypeMin() -
                                   output_element_type.getZeroPoint()) *
                            output_element_type.getScale();
    double type_range_max = double(output_element_type.getStorageTypeMax() -
                                   output_element_type.getZeroPoint()) *
                            output_element_type.getScale();
    bool narrow_range =
        output_element_type.getStorageTypeMin() == 1 ? true : false;

    auto dst_qconst_type = TypeAttr::get(RankedTensorType::get(
        output_type.getShape(),
        buildQTypeFromMinMax(
            builder, output_element_type.getExpressedType(),
            builder.getF64FloatAttr(type_range_min),
            builder.getF64FloatAttr(type_range_max),
            builder.getI32IntegerAttr(
                output_element_type.getStorageTypeIntegralWidth()),
            0, true /* signed */, builder.getBoolAttr(narrow_range))));

    Type dst_dense_element_type = builder.getIntegerType(8);
    llvm::function_ref<APInt(const APInt &)> mapping =
        [](const APInt &in) -> APInt {
      int64_t in_i64 = in.getLimitedValue();
      int64_t out_i64 = in_i64 - 128;
      return APInt(8, out_i64, true);
    };

    auto dst_dense_attr =
        src_dense_attr.mapValues(dst_dense_element_type, mapping);

    auto dst_qconst_op = builder.create<TFL::QConstOp>(
        op->getLoc(), dst_qconst_type, dst_dense_attr);

    builder.replaceOp(op, {dst_qconst_op.getResult()});
    return success();
  }
};

int convert_graph_uint8_tensor(mlir::MLIRContext &context,
                               mlir::FuncOp &function) {
  size_t num_blocks_in_main = 0;
  mlir::Region *region = function.getCallableRegion();
  OpBuilder builder(&context);

  auto tmp_const_type = RankedTensorType::get({1}, builder.getIntegerType(8));
  auto tmp_const_attr = DenseElementsAttr::get(tmp_const_type, {0});

  for (mlir::Block &bb : region->getBlocks()) {
    // Always have one block for each region right now.
    num_blocks_in_main++;
    if (num_blocks_in_main > 1) {
      llvm::errs() << "Invalid MLIR: multiple blocks in a region\n";
      return 1;
    }

    if (!bb.isEntryBlock()) {
      llvm::errs() << "Invalid MLIR: block must be entry block\n";
      return 1;
    }

    // Insert rescale uint8->int8 after placeholders.
    for (Value arg : bb.getArguments()) {
      auto uint8_type = arg.getType().dyn_cast<mlir::RankedTensorType>();
      if (!uint8_type) continue;

      auto uint8_element_type =
          uint8_type.getElementType()
              .dyn_cast<mlir::quant::UniformQuantizedType>();
      if (!uint8_element_type) continue;

      if (uint8_element_type.isSigned() ||
          uint8_element_type.getStorageTypeIntegralWidth() != 8)
        continue;

      double type_range_min = double(uint8_element_type.getStorageTypeMin() -
                                     uint8_element_type.getZeroPoint()) *
                              uint8_element_type.getScale();
      double type_range_max = double(uint8_element_type.getStorageTypeMax() -
                                     uint8_element_type.getZeroPoint()) *
                              uint8_element_type.getScale();
      bool narrow_range =
          uint8_element_type.getStorageTypeMin() == 1 ? true : false;

      Type int8_type = RankedTensorType::get(
          uint8_type.getShape(),
          buildQTypeFromMinMax(
              builder, uint8_element_type.getExpressedType(),
              builder.getF64FloatAttr(type_range_min),
              builder.getF64FloatAttr(type_range_max),
              builder.getI32IntegerAttr(
                  uint8_element_type.getStorageTypeIntegralWidth()),
              0, true /* signed */, builder.getBoolAttr(narrow_range)));

      int32_t uint8_zp = uint8_element_type.getZeroPoint();
      int32_t int8_zp = uint8_zp - 128;

      // Keep original input_val use with tmp_val.
      Value tmp_val = builder.create<TFL::ConstOp>(
          builder.getUnknownLoc(), tmp_const_type, tmp_const_attr);
      arg.replaceAllUsesWith(tmp_val);
      auto rescale_op = builder.create<tosa::RescaleOp>(
          builder.getUnknownLoc(), int8_type, arg,
          builder.getI32IntegerAttr(uint8_zp),
          builder.getI32IntegerAttr(int8_zp),
          builder.getI32ArrayAttr({1 << 30}), builder.getI32ArrayAttr({30}),
          builder.getBoolAttr(true), builder.getBoolAttr(false),
          builder.getBoolAttr(false));

      Operation *op_rescale_op = (Operation *)rescale_op;
      bb.push_front(op_rescale_op);
      tmp_val.replaceAllUsesWith(rescale_op.getResult());
    }

    // Record types of original graph output before we convert intermediate
    // tensor.
    auto terminator = bb.getTerminator();
    std::vector<Type> output_types;
    for (Value val : terminator->getOperands()) {
      output_types.push_back(val.getType());
    }

    // Convert intermediate tensor.
    for (auto &op : bb) {
      for (Value output_val : op.getResults()) {
        // Skip if output value is not RankedTensorType.
        auto output_type =
            output_val.getType().dyn_cast<mlir::RankedTensorType>();
        if (!output_type) continue;

        // Skip if output value is not per-tensor quantized element type.
        auto output_element_type =
            output_type.getElementType()
                .dyn_cast<mlir::quant::UniformQuantizedType>();
        if (!output_element_type) continue;

        // Skip if output is not uint8.
        if (output_element_type.isSigned() ||
            output_element_type.getStorageTypeIntegralWidth() != 8)
          continue;

        double type_range_min = double(output_element_type.getStorageTypeMin() -
                                       output_element_type.getZeroPoint()) *
                                output_element_type.getScale();
        double type_range_max = double(output_element_type.getStorageTypeMax() -
                                       output_element_type.getZeroPoint()) *
                                output_element_type.getScale();
        bool narrow_range =
            output_element_type.getStorageTypeMin() == 1 ? true : false;

        Type new_type = RankedTensorType::get(
            output_type.getShape(),
            buildQTypeFromMinMax(
                builder, output_element_type.getExpressedType(),
                builder.getF64FloatAttr(type_range_min),
                builder.getF64FloatAttr(type_range_max),
                builder.getI32IntegerAttr(
                    output_element_type.getStorageTypeIntegralWidth()),
                0, true /* signed */, builder.getBoolAttr(narrow_range)));

        output_val.setType(new_type);
      }
    }

    if (terminator->getNumOperands() != output_types.size()) {
      llvm::errs() << "Terminator size should be " << output_types.size()
                   << " instead of " << terminator->getNumOperands() << "\n";
      return 1;
    }

    // Insert int8->uint8 rescale before all terminator's operand.
    for (int32_t i = 0; i < terminator->getNumOperands(); i++) {
      Value input_val = terminator->getOperand(i).getDefiningOp()->getResult(0);

      // Check if graph output is uint8 type.
      auto uint8_output_type =
          output_types[i].dyn_cast<mlir::RankedTensorType>();
      if (!uint8_output_type) continue;

      auto uint8_output_element_type =
          uint8_output_type.getElementType()
              .dyn_cast<mlir::quant::UniformQuantizedType>();
      if (!uint8_output_element_type) continue;

      if (uint8_output_element_type.isSigned() ||
          uint8_output_element_type.getStorageTypeIntegralWidth() != 8)
        continue;

      // Check if output coming into terminator is int8 type.
      auto int8_output_type = terminator->getOperand(i)
                                  .getType()
                                  .dyn_cast<mlir::RankedTensorType>();
      if (!int8_output_type) continue;

      auto int8_output_element_type =
          int8_output_type.getElementType()
              .dyn_cast<mlir::quant::UniformQuantizedType>();
      if (!int8_output_element_type) continue;

      if (!int8_output_element_type.isSigned() ||
          int8_output_element_type.getStorageTypeIntegralWidth() != 8)
        continue;

      int32_t int8_zp = int8_output_element_type.getZeroPoint();
      int32_t uint8_zp = uint8_output_element_type.getZeroPoint();

      // Sanity check if uint8/int8's scale and zeropoint match.
      if (((uint8_zp - int8_zp) != 128) ||
          (int8_output_element_type.getScale() !=
           uint8_output_element_type.getScale())) {
        llvm::errs() << "convert_uint8_to_int8: scale mismatch\n";
        return 1;
      }

      // Keep original input_val use with tmp_val.
      Value tmp_val = builder.create<TFL::ConstOp>(
          builder.getUnknownLoc(), tmp_const_type, tmp_const_attr);
      input_val.replaceAllUsesWith(tmp_val);
      auto rescale_op = builder.create<tosa::RescaleOp>(
          builder.getUnknownLoc(), uint8_output_type, input_val,
          builder.getI32IntegerAttr(int8_zp),
          builder.getI32IntegerAttr(uint8_zp),
          builder.getI32ArrayAttr({1 << 30}), builder.getI32ArrayAttr({30}),
          builder.getBoolAttr(true), builder.getBoolAttr(false),
          builder.getBoolAttr(false));

      Operation *op_rescale_op = (Operation *)rescale_op;
      bb.push_back(op_rescale_op);
      op_rescale_op->moveBefore(terminator);
      tmp_val.replaceAllUsesWith(rescale_op.getResult());
    }
  }

  return 0;
}

void ConvertUint8ToInt8::runOnFunction() {
  OwningRewritePatternList patterns;
  auto &ctx = getContext();
  auto func = getFunction();

  // Convert uint8 const tensor. const needs to be handled specifically.
  patterns.insert<ConvertUint8QConstOp>(&ctx);
  applyPatternsAndFoldGreedily(func, std::move(patterns));

  // Replace uint8 tensor in the graph and insert rescale as needed.
  convert_graph_uint8_tensor(ctx, func);
}

}  // anonymous namespace

std::unique_ptr<OperationPass<FuncOp>> createConvertTFLUint8Pass() {
  return std::make_unique<ConvertUint8ToInt8>();
}

static PassRegistration<ConvertUint8ToInt8> pass(
    PASS_NAME, "Convert uint8 graph to int8.");

}  // namespace tosa

}  // namespace mlir
