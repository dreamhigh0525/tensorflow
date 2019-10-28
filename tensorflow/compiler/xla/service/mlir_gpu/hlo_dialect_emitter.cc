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

#include "tensorflow/compiler/xla/service/mlir_gpu/hlo_dialect_emitter.h"

#include "llvm/ADT/STLExtras.h"
#include "mlir/Dialect/StandardOps/Ops.h"  // TF:local_config_mlir
#include "mlir/IR/Attributes.h"  // TF:local_config_mlir
#include "mlir/IR/StandardTypes.h"  // TF:local_config_mlir
#include "mlir/IR/Types.h"  // TF:local_config_mlir
#include "tensorflow/compiler/mlir/xla/hlo_utils.h"
#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.h"
#include "tensorflow/compiler/xla/comparison_util.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"

namespace xla {
namespace mlir_gpu {
namespace {

using ::mlir::ArrayRef;
using ::mlir::Attribute;
using ::mlir::Identifier;
using ::mlir::Location;
using ::mlir::NamedAttribute;
using ::mlir::OpBuilder;
using ::mlir::RankedTensorType;
using ::mlir::Type;
using ::mlir::Value;

namespace hlo = ::mlir::xla_hlo;

// TODO(b/137624192) Use tablegen for this.
StatusOr<Value*> InsertMlirOp(
    HloOpcode opcode, OpBuilder func_builder, Location loc, ArrayRef<Type> rets,
    ArrayRef<Value*> args, ArrayRef<std::pair<Identifier, Attribute>> attrs) {
  switch (opcode) {
    case HloOpcode::kAdd:
      return {func_builder.create<hlo::AddOp>(loc, rets, args, attrs)};
    case HloOpcode::kAnd:
      return {func_builder.create<hlo::AndOp>(loc, rets, args, attrs)};
    case HloOpcode::kDivide:
      return {func_builder.create<hlo::DivOp>(loc, rets, args, attrs)};
    case HloOpcode::kExp:
      return {func_builder.create<hlo::ExpOp>(loc, rets, args, attrs)};
    case HloOpcode::kMaximum:
      return {func_builder.create<hlo::MaxOp>(loc, rets, args, attrs)};
    case HloOpcode::kMinimum:
      return {func_builder.create<hlo::MinOp>(loc, rets, args, attrs)};
    case HloOpcode::kMultiply:
      return {func_builder.create<hlo::MulOp>(loc, rets, args, attrs)};
    case HloOpcode::kSelect:
      return {func_builder.create<hlo::SelectOp>(loc, rets, args, attrs)};
    case HloOpcode::kSubtract:
      return {func_builder.create<hlo::SubOp>(loc, rets, args, attrs)};
    default:
      return tensorflow::errors::Internal(absl::StrCat(
          "HLO Opcode ", HloOpcodeString(opcode), " is not supported."));
  }
}

}  // namespace

mlir::Location HloDialectEmitter::getLocation(
    const HloInstruction* instr) const {
  return emission_context_->getLocation(instr);
}

StatusOr<Value*> HloDialectEmitter::EmitComputation(
    const HloComputation& computation) {
  const auto root = computation.root_instruction();
  TF_RETURN_IF_ERROR(root->Accept(this));
  return instruction_to_values_[root];
}

Status HloDialectEmitter::DefaultAction(HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto res_type, ConvertTensorShapeToType<RankedTensorType>(
                                         instr->shape(), builder_));

  auto name_attr =
      builder_.getNamedAttr("name", builder_.getStringAttr(instr->name()));
  llvm::SmallVector<Value*, 4> arguments;
  for (auto operand : instr->operands()) {
    arguments.push_back(instruction_to_values_[operand]);
  }
  TF_ASSIGN_OR_RETURN(
      auto inserted, InsertMlirOp(instr->opcode(), builder_, getLocation(instr),
                                  res_type, arguments, name_attr));
  instruction_to_values_[instr] = inserted;
  return Status::OK();
}

Status HloDialectEmitter::HandleBroadcast(HloInstruction* broadcast) {
  mlir::DenseIntElementsAttr broadcast_dim =
      CreateDenseIntElementsAttrFromVector(broadcast->dimensions(), builder_);
  TF_ASSIGN_OR_RETURN(Type res_type, ConvertTensorShapeToType<RankedTensorType>(
                                         broadcast->shape(), builder_));

  auto broadcast_op = builder_.create<hlo::BroadcastInDimOp>(
      getLocation(broadcast), llvm::makeArrayRef(res_type),
      instruction_to_values_[broadcast->operand(0)], broadcast_dim);
  broadcast_op.setAttr("name", builder_.getStringAttr(broadcast->name()));

  instruction_to_values_[broadcast] = broadcast_op;
  return Status::OK();
}

Status HloDialectEmitter::HandleParameter(HloInstruction* param) {
  auto argValue = arguments_[param->parameter_number()];
  instruction_to_values_[param] = argValue;
  return Status::OK();
}

Status HloDialectEmitter::HandleConstant(HloInstruction* constant) {
  TF_ASSIGN_OR_RETURN(auto type, ConvertTensorShapeToType<RankedTensorType>(
                                     constant->shape(), builder_));

  TF_ASSIGN_OR_RETURN(auto value, CreateDenseElementsAttrFromLiteral(
                                      constant->literal(), builder_));

  auto const_value =
      builder_.create<hlo::ConstOp>(getLocation(constant), type, value);
  instruction_to_values_[constant] = const_value;
  return Status::OK();
}

Status HloDialectEmitter::HandleReduce(HloInstruction* reduce) {
  llvm::SmallVector<Value*, 4> operands;
  for (auto operand : reduce->operands()) {
    operands.push_back(instruction_to_values_.at(operand));
  }
  const unsigned num_inputs = operands.size() / 2;
  TF_ASSIGN_OR_RETURN(
      const auto return_type,
      ConvertTensorShapeToType<RankedTensorType>(reduce->shape(), builder_));
  const auto dimensions_attr =
      CreateDenseIntElementsAttrFromVector(reduce->dimensions(), builder_);
  auto reduceOp = builder_.create<hlo::ReduceOp>(
      getLocation(reduce), return_type,
      llvm::makeArrayRef(operands).take_front(num_inputs),
      llvm::makeArrayRef(operands).take_back(num_inputs), dimensions_attr);
  {
    auto computation = reduce->to_apply();
    auto block = new mlir::Block();
    llvm::SmallVector<Value*, 4> arguments;
    arguments.reserve(computation->num_parameters());
    for (auto parameter : computation->parameter_instructions()) {
      TF_ASSIGN_OR_RETURN(auto param_type,
                          ConvertTensorShapeToType<RankedTensorType>(
                              parameter->shape(), builder_));
      arguments.push_back(block->addArgument(param_type));
    }
    reduceOp.body().push_back(block);
    HloDialectEmitter emitter(emission_context_, &reduceOp.body(), arguments);
    TF_ASSIGN_OR_RETURN(auto result, emitter.EmitComputation(*computation));
    OpBuilder body_builder(block);
    body_builder.setInsertionPointToEnd(block);
    body_builder.create<hlo::ReturnOp>(getLocation(reduce),
                                       ArrayRef<Value*>{result});
  }
  // TODO(b/137624192) Add support for multiple results.
  instruction_to_values_[reduce] = reduceOp.getResult(0);
  return Status::OK();
}

Status HloDialectEmitter::HandleCompare(HloInstruction* compare) {
  TF_ASSIGN_OR_RETURN(Type res_type, ConvertTensorShapeToType<RankedTensorType>(
                                         compare->shape(), builder_));
  llvm::SmallVector<NamedAttribute, 2> attributes{
      builder_.getNamedAttr("name", builder_.getStringAttr(compare->name())),
      builder_.getNamedAttr("comparison_direction",
                            builder_.getStringAttr(ComparisonDirectionToString(
                                compare->comparison_direction())))};
  llvm::SmallVector<Value*, 4> arguments;
  for (auto operand : compare->operands()) {
    arguments.push_back(instruction_to_values_[operand]);
  }
  instruction_to_values_[compare] = builder_.create<hlo::CompareOp>(
      getLocation(compare), llvm::makeArrayRef(res_type), arguments,
      attributes);
  return Status::OK();
}

Status HloDialectEmitter::HandleIota(HloInstruction* iota) {
  mlir::IntegerAttr iota_dim = builder_.getI64IntegerAttr(
      static_cast<HloIotaInstruction*>(iota)->iota_dimension());
  TF_ASSIGN_OR_RETURN(Type res_type, ConvertTensorShapeToType<RankedTensorType>(
                                         iota->shape(), builder_));

  auto iota_op =
      builder_.create<hlo::IotaOp>(getLocation(iota), res_type, iota_dim);
  iota_op.setAttr("name", builder_.getStringAttr(iota->name()));

  instruction_to_values_[iota] = iota_op;
  return Status::OK();
}

}  // namespace mlir_gpu
}  // namespace xla
