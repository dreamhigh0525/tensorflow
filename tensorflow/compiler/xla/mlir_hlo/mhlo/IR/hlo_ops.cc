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

// This file defines the operations used in the MHLO dialect.

#include "mhlo/IR/hlo_ops.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MathExtras.h"
#include "mhlo/IR/hlo_ops.h.inc"
#include "mhlo/IR/hlo_ops_common.h"
#include "mhlo/IR/mhlo_bytecode.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Complex/IR/Complex.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Shape/IR/Shape.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/FunctionInterfaces.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/InliningUtils.h"
#include "stablehlo/dialect/AssemblyFormat.h"
#include "stablehlo/dialect/TypeInference.h"
#include "utils/convert_op_folder.h"
#include "utils/hlo_utils.h"

namespace mlir {
#include "hlo_patterns.cc.inc"
}  // namespace mlir

using mlir::hlo::parseDimSizes;
using mlir::hlo::printDimSizes;

#include "mhlo/IR/hlo_ops_enums.cc.inc"
#define GET_ATTRDEF_CLASSES
#include "mhlo/IR/hlo_ops_attrs.cc.inc"
#define GET_TYPEDEF_CLASSES
#include "mhlo/IR/hlo_ops_typedefs.cc.inc"

namespace mlir {
namespace mhlo {

namespace detail {
/// A type representing a collection of other types.
struct AsyncBundleTypeStorage final
    : public TypeStorage,
      public llvm::TrailingObjects<AsyncBundleTypeStorage, Type> {
  using KeyTy = TypeRange;

  explicit AsyncBundleTypeStorage(unsigned numTypes) : numElements(numTypes) {}

  // Construction.
  static AsyncBundleTypeStorage* construct(TypeStorageAllocator& allocator,
                                           TypeRange key) {
    // Allocate a new storage instance.
    auto byteSize = AsyncBundleTypeStorage::totalSizeToAlloc<Type>(key.size());
    auto* rawMem =
        allocator.allocate(byteSize, alignof(AsyncBundleTypeStorage));
    auto* result = ::new (rawMem) AsyncBundleTypeStorage(key.size());

    // Copy in the element types into the trailing storage.
    std::uninitialized_copy(key.begin(), key.end(),
                            result->getTrailingObjects<Type>());
    return result;
  }

  bool operator==(const KeyTy& key) const { return key == getTypes(); }

  // Return the number of held types.
  unsigned size() const { return numElements; }

  // Return the held types.
  ArrayRef<Type> getTypes() const {
    return {getTrailingObjects<Type>(), size()};
  }

  void getFlattenedTypes(SmallVectorImpl<Type>& types) {
    for (Type type : getTypes()) {
      if (auto nestedTuple = type.dyn_cast<TupleType>())
        nestedTuple.getFlattenedTypes(types);
      else
        types.push_back(type);
    }
  }

  // The number of tuple elements.
  unsigned numElements;
};

}  // namespace detail
/// Return the elements types for this tuple.
ArrayRef<Type> AsyncBundleType::getTypes() const {
  return getImpl()->getTypes();
}
void AsyncBundleType::getFlattenedTypes(SmallVectorImpl<Type>& types) {
  getImpl()->getFlattenedTypes(types);
}

namespace {
void createArgs(ArrayRef<OpAsmParser::UnresolvedOperand> operands,
                ArrayRef<Type> types,
                SmallVector<OpAsmParser::Argument>& args) {
  for (auto argAndType : llvm::zip(operands, types)) {
    auto& arg = args.emplace_back();
    arg.ssaName = std::get<0>(argAndType);
    arg.type = std::get<1>(argAndType);
  }
}

//===----------------------------------------------------------------------===//
// Utilities for the canonicalize patterns
//===----------------------------------------------------------------------===//

// This is an upper limit on how many elements can be folded by an op folder.
// This limit doesn't apply to some special cases like adding a zero,
// multiplying by one, doing many operations with splats.
constexpr int64_t kFoldOpEltLimit = 65536;

// Clamps value to the range [lower, upper].  Requires lower <= upper.
template <typename T>
static T clamp(const T& value, const T& lower, const T& upper) {
  assert(lower <= upper);
  return std::max(lower, std::min(value, upper));
}

// Verifies that dimension attribute for the op correctly indexes in operand or
// result shape.
template <typename OpT>
static LogicalResult verifyDimAttr(OpT op) {
  int64_t rank = -1;
  if (auto ty =
          op.getOperand().getType().template dyn_cast<RankedTensorType>()) {
    rank = ty.getRank();
  } else if (auto ty = op.getType().template dyn_cast<RankedTensorType>()) {
    rank = ty.getRank();
  } else {
    return success();
  }

  int64_t dim = op.getDimension();
  if (dim < 0 || dim >= rank)
    return op.emitOpError() << "requires dimension attribute in range [0, "
                            << rank << "); found (" << dim << ")";
  return success();
}

// Given the start indices and slice sizes for a dynamic-slice that can be
// converted to a static slice, returns the limits for the static slice.
DenseIntElementsAttr buildSliceLimits(DenseIntElementsAttr startIndices,
                                      DenseIntElementsAttr sliceSizes,
                                      Builder* builder) {
  SmallVector<int64_t, 4> sliceLimits;
  for (int64_t i = 0; i < sliceSizes.getNumElements(); ++i) {
    int64_t startIndex = startIndices.getValues<IntegerAttr>()[i].getInt();
    int64_t sliceSize = sliceSizes.getValues<IntegerAttr>()[i].getInt();
    sliceLimits.push_back(startIndex + sliceSize);
  }
  return builder->getI64TensorAttr(sliceLimits);
}

/// Replaces the given op with the contents of the given single-block region,
/// using the operands of the block terminator to replace operation results.
static void replaceOpWithRegion(PatternRewriter& rewriter, Operation* op,
                                Region& region, ValueRange blockArgs = {}) {
  assert(llvm::hasSingleElement(region) && "expected single-block region");
  Block* block = &region.front();
  Operation* terminator = block->getTerminator();
  ValueRange results = terminator->getOperands();
  rewriter.mergeBlockBefore(block, op, blockArgs);
  rewriter.replaceOp(op, results);
  rewriter.eraseOp(terminator);
}

#include "mhlo/IR/mhlo_canonicalize.inc"

// Returns a new scalar integer value having type `type`. Here `type` must be
// an integer or index type.
Value maybeCastTo(OpBuilder& b, Location loc, Value value, Type type) {
  if (type == value.getType()) return value;
  assert(type.isIndex() || value.getType().isIndex());
  return b.create<arith::IndexCastOp>(loc, type, value);
}

DenseElementsAttr reshape(DenseElementsAttr attr, ShapedType newType) {
  // TODO(b/232866626): DenseElementsAttr::reshape is broken for bool splats.
  // Once that ticket is fixed, we can remove this conditional.
  if (attr.isSplat() && newType.getElementType().isInteger(/*width=*/1)) {
    auto splatValue = attr.getValues<bool>()[0];
    return DenseElementsAttr::get(newType, {splatValue});
  }
  return attr.reshape(newType);
}

//===----------------------------------------------------------------------===//
// Utilities for verifiers
//===----------------------------------------------------------------------===//

// Convert a 1D dense int64 attribute to a list of values.
SmallVector<int64_t> convertDenseIntAttr(
    llvm::Optional<mlir::DenseIntElementsAttr> optionalAttr) {
  if (!optionalAttr.has_value()) return SmallVector<int64_t>{};

  mlir::DenseIntElementsAttr attr = *optionalAttr;
  auto values = attr.getValues<int64_t>();
  return {values.begin(), values.end()};
}

// Convert a 1D or Nx2 dense int64 attribute to a list of tuples.
FailureOr<SmallVector<std::pair<int64_t, int64_t>>> convertNx2Attribute(
    llvm::Optional<mlir::DenseIntElementsAttr> optionalAttr, Location loc) {
  if (!optionalAttr.has_value())
    return SmallVector<std::pair<int64_t, int64_t>>{};
  mlir::DenseIntElementsAttr attr = *optionalAttr;

  auto attrType = attr.getType().cast<RankedTensorType>();  // ensured by ODS.
  if (attrType.getRank() > 1) {
    if (attrType.getRank() != 2 || attrType.getShape()[1] != 2)
      return (mlir::emitError(loc) << "expects the shape of padding-attribute "
                                      "to be {N, 2}, but got {"
                                   << attrType.getShape() << "}.",
              failure());
  } else {
    // Padding values can be provided as a 1D vector as well.
    if (attr.getValues<int64_t>().size() % 2 != 0)
      return (mlir::emitError(loc)
                  << "expects the padding-entries to have even number of "
                     "elements, but got "
                  << attr.getValues<int64_t>().size() << " elements.",
              failure());
  }

  auto it = attr.getValues<int64_t>().begin();
  SmallVector<std::pair<int64_t, int64_t>> out(attr.getNumElements() / 2);
  for (auto& item : out) {
    int64_t first = *it;
    ++it;
    int64_t second = *it;
    ++it;
    item = {first, second};
  }
  return out;
}
}  // namespace

//===----------------------------------------------------------------------===//
// Utilities for attributes
//===----------------------------------------------------------------------===//

LogicalResult TypeExtensionsAttr::verifyEncoding(
    llvm::ArrayRef<int64_t> shape, mlir::Type elementType,
    llvm::function_ref<mlir::InFlightDiagnostic()> emitError) const {
  return hlo::verifyBounds(
      getBounds(), RankedTensorType::get(shape, elementType), emitError);
}

//===----------------------------------------------------------------------===//
// CollectivePermuteOp
//===----------------------------------------------------------------------===//

void CollectivePermuteOp::build(OpBuilder& odsBuilder, OperationState& odsState,
                                Type resultType, Value operand,
                                DenseIntElementsAttr sourceTargetPairs) {
  CollectivePermuteOp::build(odsBuilder, odsState, resultType, operand,
                             sourceTargetPairs, /*channel_handle=*/nullptr);
}

//===----------------------------------------------------------------------===//
// ReduceScatterOp
//===----------------------------------------------------------------------===//

LogicalResult ReduceScatterOp::verify() {
  return hlo::verifyReduceScatterOp(
      getLoc(), getOperand(), getScatterDimension(), getReplicaGroups(),
      getUseGlobalDeviceIds(), getComputation(), getResult());
}

void ReduceScatterOp::build(OpBuilder& odsBuilder, OperationState& odsState,
                            Type resultType, Value operand,
                            IntegerAttr scatterDimension,
                            DenseIntElementsAttr replicaGroups,
                            ChannelHandleAttr channelHandle) {
  ReduceScatterOp::build(odsBuilder, odsState, resultType, operand,
                         scatterDimension, replicaGroups, channelHandle,
                         /*use_global_device_ids=*/nullptr);
}

//===----------------------------------------------------------------------===//
// CompatibleOperandsAndResultType
//===----------------------------------------------------------------------===//

// TODO(b/231358795): Review the use of InferTypeOpInterface for ops that
// support quantization or sparsity.
#define INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(Op)                        \
  LogicalResult Op::inferReturnTypeComponents(                                \
      MLIRContext* context, Optional<Location> location,                      \
      ValueShapeRange operands, DictionaryAttr attributes,                    \
      RegionRange regions,                                                    \
      SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {          \
    return inferReturnTypeComponentsFromOperands(context, location, operands, \
                                                 attributes, regions,         \
                                                 inferredReturnShapes);       \
  }

INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(AddOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(AllReduceOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(AndOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(Atan2Op)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(CbrtOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(CeilOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(ClzOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(CollectivePermuteOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(CopyOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(CosineOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(CrossReplicaSumOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(DivOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(DomainOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(ExpOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(Expm1Op)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(FloorOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(LogOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(Log1pOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(LogisticOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(MaxOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(MinOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(MulOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(NegOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(NotOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(OrOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(PopulationCountOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(PowOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(ReducePrecisionOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(RemOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(ReverseOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(RoundNearestEvenOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(RoundOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(RsqrtOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(ShiftLeftOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(ShiftRightArithmeticOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(ShiftRightLogicalOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(SignOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(SineOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(SqrtOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(SubtractOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(TanOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(TanhOp)
INFER_RETURN_TYPE_COMPONENTS_FROM_OPERANDS(XorOp)

//===----------------------------------------------------------------------===//
// Async ops
//===----------------------------------------------------------------------===//

Type maybeTupleFromTypes(MLIRContext* ctx, ArrayRef<Type> types) {
  if (types.size() == 1 && !types[0].isa<TupleType>()) return types[0];
  return TupleType::get(ctx, TypeRange(types));
}

LogicalResult AsyncStartOp::verify() {
  ModuleOp module = getOperation()->getParentOfType<ModuleOp>();
  func::FuncOp callee =
      module.lookupSymbol<func::FuncOp>(getCalledComputation());
  if (!callee) {
    return emitOpError() << "can't find function: " << getCalledComputation();
  }
  FunctionType calleeType = callee.getFunctionType();
  auto calleeInputTypes = calleeType.getInputs();
  auto calleeResultTypes = calleeType.getResults();

  auto calleeThreadName = callee->getAttrOfType<StringAttr>("execution_thread");
  if (!calleeThreadName)
    return emitOpError() << "callee must have execution_thread attribute.";
  if (calleeThreadName != getExecutionThread()) {
    return emitOpError()
           << "execution_thread does not match the execution_thread of "
           << getCalledComputation() << ".  Got: \"" << getExecutionThread()
           << "\", but expected " << calleeThreadName << ".";
  }

  if (calleeType.getNumInputs() != getOperands().size()) {
    return emitOpError() << "number of operands doesn't match operands for "
                         << getCalledComputation()
                         << ". Got: " << getOperands().size()
                         << ", but expected: " << calleeType.getNumInputs()
                         << ".";
  }
  for (int i = 0; i < static_cast<int64_t>(getOperands().size()); ++i) {
    if (calleeType.getInput(i) != getOperandTypes()[i]) {
      return emitOpError() << "type mismatch on argument #" << i << " of "
                           << getCalledComputation()
                           << ". Got: " << getOperandTypes()[i]
                           << ", but expected: " << calleeType.getInput(i)
                           << ".";
    }
  }

  auto resultTypes = getResult().getType().cast<AsyncBundleType>().getTypes();
  if (resultTypes.size() < 2)
    return emitOpError() << "result is expected to be a bundle of at least 2 "
                            "components, but got "
                         << resultTypes.size();
  if (resultTypes[0] != maybeTupleFromTypes(getContext(), calleeInputTypes)) {
    return emitOpError()
           << "component #0 of return type doesn't match callee input types";
  }
  if (resultTypes[1] != maybeTupleFromTypes(getContext(), calleeResultTypes)) {
    return emitOpError()
           << "component #1 of return type doesn't match callee result types";
  }

  return success();
}

LogicalResult AsyncUpdateOp::verify() {
  ModuleOp module = getOperation()->getParentOfType<ModuleOp>();
  func::FuncOp callee =
      module.lookupSymbol<func::FuncOp>(getCalledComputation());
  if (!callee) {
    return emitOpError() << "can't find function: " << getCalledComputation();
  }
  FunctionType calleeType = callee.getFunctionType();
  auto calleeInputTypes = calleeType.getInputs();
  auto calleeResultTypes = calleeType.getResults();
  auto bundleTypes = getBundle().getType().cast<AsyncBundleType>().getTypes();

  auto calleeThreadName = callee->getAttrOfType<StringAttr>("execution_thread");
  if (!calleeThreadName)
    return emitOpError() << "callee must have execution_thread attribute.";
  if (calleeThreadName != getExecutionThread()) {
    return emitOpError() << "execution_thread does not match name of "
                         << getCalledComputation() << ".  Got: \""
                         << getExecutionThread() << "\", but expected "
                         << calleeThreadName << ".";
  }

  if (bundleTypes.size() < 2)
    return emitOpError() << "operand is expected to be a bundle of at least 2 "
                            "components, but got "
                         << bundleTypes.size();
  if (bundleTypes[0] != maybeTupleFromTypes(getContext(), calleeInputTypes)) {
    return emitOpError() << "component #0 of operand bundle type doesn't match "
                            "callee input types";
  }
  if (bundleTypes[1] != maybeTupleFromTypes(getContext(), calleeResultTypes)) {
    return emitOpError() << "component #1 of operand bundle type doesn't match "
                            "callee result types";
  }

  return success();
}

LogicalResult AsyncUpdateOp::inferReturnTypes(
    MLIRContext*, Optional<Location>, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  AsyncUpdateOp::Adaptor adaptor(operands, attributes, regions);
  auto stateType = adaptor.getBundle().getType().cast<AsyncBundleType>();
  inferredReturnTypes.push_back(stateType);
  return success();
}

LogicalResult AsyncDoneOp::verify() {
  ModuleOp module = getOperation()->getParentOfType<ModuleOp>();
  func::FuncOp callee =
      module.lookupSymbol<func::FuncOp>(getCalledComputation());
  if (!callee) {
    return emitOpError() << "can't find function: " << getCalledComputation();
  }
  FunctionType calleeType = callee.getFunctionType();
  auto calleeInputTypes = calleeType.getInputs();
  auto calleeResultTypes = calleeType.getResults();
  auto bundleTypes = getBundle().getType().cast<AsyncBundleType>().getTypes();

  auto calleeThreadName = callee->getAttrOfType<StringAttr>("execution_thread");
  if (!calleeThreadName)
    return emitOpError() << "callee must have execution_thread attribute.";
  if (calleeThreadName != getExecutionThread()) {
    return emitOpError() << "execution_thread does not match name of "
                         << getCalledComputation() << ".  Got: \""
                         << getExecutionThread() << "\", but expected "
                         << calleeThreadName << ".";
  }

  if (bundleTypes.size() < 2)
    return emitOpError() << "operand is expected to be a bundle of at least 2 "
                            "components, but got "
                         << bundleTypes.size();
  if (bundleTypes[0] != maybeTupleFromTypes(getContext(), calleeInputTypes)) {
    return emitOpError()
           << "operand type component #0 doesn't match callee input types";
  }
  if (bundleTypes[1] != maybeTupleFromTypes(getContext(), calleeResultTypes)) {
    return emitOpError()
           << "operand type component #1 doesn't match callee result types";
  }

  return success();
}

LogicalResult AsyncDoneOp::inferReturnTypes(
    MLIRContext*, Optional<Location>, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  AsyncDoneOp::Adaptor adaptor(operands, attributes, regions);
  ModuleOp module =
      adaptor.getBundle().getDefiningOp()->getParentOfType<ModuleOp>();
  auto calledComputation = adaptor.getCalledComputationAttr();
  func::FuncOp callee = module.lookupSymbol<func::FuncOp>(calledComputation);
  if (!callee) {
    return adaptor.getBundle().getDefiningOp()->emitOpError()
           << "can't find function: " << calledComputation;
  }
  llvm::append_range(inferredReturnTypes,
                     callee.getFunctionType().getResults());
  return success();
}

//===----------------------------------------------------------------------===//
// AfterAllOp
//===----------------------------------------------------------------------===//

LogicalResult AfterAllOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange,
    DictionaryAttr, RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  auto dialect = context->getLoadedDialect<MhloDialect>();
  return hlo::inferAfterAllOp(dialect, location, inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

OpFoldResult ConstantOp::fold(FoldAdaptor adaptor) {
  assert(adaptor.getOperands().empty() && "constant has no operands");

  // Return the held attribute value.
  return getValue();
}

// Builds a constant op with the specified attribute `value`.
void ConstantOp::build(OpBuilder& /*builder*/, OperationState& result,
                       Attribute value) {
  Type type;
  if (auto elemAttr = value.dyn_cast<ElementsAttr>()) {
    type = elemAttr.getType();
  } else if (value.isa<BoolAttr, FloatAttr, IntegerAttr>()) {
    // All XLA types must be tensor types. In the build() method, we want to
    // provide more flexibility by allowing attributes of scalar types. But we
    // need to wrap it up with ElementsAttr to construct valid XLA constants.
    type =
        RankedTensorType::get(/*shape=*/{}, value.cast<TypedAttr>().getType());
    value = DenseElementsAttr::get(type.cast<TensorType>(), value);
  } else if (auto complexAttr = value.dyn_cast<complex::NumberAttr>()) {
    type = RankedTensorType::get(/*shape=*/{},
                                 complexAttr.cast<TypedAttr>().getType());
    value =
        DenseElementsAttr::get(type.cast<TensorType>(), complexAttr.getValue());
  }

  // TODO: support other XLA specific types.
  assert(type && "unsupported attribute type for building mhlo.constant");
  result.types.push_back(type);
  result.addAttribute("value", value);
}

LogicalResult ConstantOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  ConstantOpAdaptor adaptor(operands, attributes);
  return hlo::inferConstantOp(location, adaptor.getValue(),
                              inferredReturnTypes);
}

bool ConstantOp::isCompatibleReturnTypes(TypeRange l, TypeRange r) {
  if (l.size() != r.size() || l.size() != 1) return false;
  auto lhsTy = l.front().cast<TensorType>();
  auto rhsTy = r.front().cast<TensorType>();
  // For comparisons of the uniform quantized element based tensor type, use the
  // storage type since the constant value will be stored through the underlying
  // storage type.
  if (auto rhsElemTy =
          rhsTy.getElementType().dyn_cast<quant::QuantizedType>()) {
    rhsTy = hlo::getSameShapeTensorType(rhsTy, rhsElemTy.getStorageType());
  }
  return lhsTy == rhsTy;
}

ParseResult ConstantOp::parse(OpAsmParser& parser, OperationState& result) {
  // Parse the generic form.
  if (succeeded(parser.parseOptionalLParen())) {
    if (parser.parseRParen()) return failure();
    if (parser.parseOptionalAttrDict(result.attributes)) return failure();
    if (parser.parseColon() || parser.parseLParen() || parser.parseRParen() ||
        parser.parseArrow())
      return failure();
    Type resultTy;
    if (parser.parseType(resultTy)) {
      return failure();
    }
    result.addTypes(resultTy);
    return success();
  }

  ElementsAttr valueAttr;
  if (parser.parseOptionalAttrDict(result.attributes)) return failure();

  if (parser.parseCustomAttributeWithFallback(valueAttr, Type{}, "value",
                                              result.attributes)) {
    return failure();
  }
  result.addTypes(valueAttr.getType());
  return success();
}

/// Print a `constant` op.
///
/// op ::= attr-dict $value
///
/// When the `value` and `output` have different type, it just uses the default
/// operator assembly format as a fallback.
void ConstantOp::print(::mlir::OpAsmPrinter& p) {
  // If not all types are the same, use generic form.
  if (getValue().getType() != getType()) {
    p.printGenericOp(getOperation(), /*printOpName=*/false);
    return;
  }

  p.printOptionalAttrDict((*this)->getAttrs(), /*elidedAttrs=*/{"value"});
  p << ' ';
  p.printStrippedAttrOrType(getValueAttr());
}

//===----------------------------------------------------------------------===//
// Helper function to verify output operand aliasing (FusionOp and CustomCallOp)
//===----------------------------------------------------------------------===//

template <typename CallableOpType>
LogicalResult verifyOutputOperandAliasing(CallableOpType* op) {
  auto aliasArrayAttr = op->getOutputOperandAliases();
  for (auto attr : aliasArrayAttr) {
    auto alias = attr.template cast<OutputOperandAliasAttr>();
    auto outputTupleIndices = alias.getOutputTupleIndices();
    auto operandIndex = alias.getOperandIndex();
    auto operandTupleIndices = alias.getOperandTupleIndices();
    if (operandIndex < 0 ||
        operandIndex >= static_cast<int64_t>(op->getInputs().size()))
      return op->emitOpError()
             << "expects operandIndex in the output_operand_alias attribute "
                "to be in range [0, "
             << op->getInputs().size() << "); got: " << operandIndex << ".";
    Type operandPart = op->getOperand(operandIndex).getType();
    for (auto i : operandTupleIndices) {
      if (!operandPart.isa<TupleType>() ||
          i >= static_cast<int64_t>(operandPart.cast<TupleType>().size()) ||
          i < 0)
        return op->emitOpError()
               << "operand_tuple_indices in the output_operand_alias "
                  "attribute out of bounds";
      operandPart = operandPart.cast<TupleType>().getType(i);
    }
    Type outputPart =
        op->getNumResults() > 1
            ? TupleType::get(op->getContext(), op->getResultTypes())
            : op->getResult(0).getType();
    for (auto i : outputTupleIndices) {
      if (!outputPart.isa<TupleType>() ||
          i >= static_cast<int64_t>(outputPart.cast<TupleType>().size()) ||
          i < 0)
        return op->emitOpError()
               << "output_tuple_indices in the output_operand_alias "
                  "attribute out of bounds";
      outputPart = outputPart.cast<TupleType>().getType(i);
    }
    if (operandPart != outputPart)
      return op->emitOpError()
             << "shapes mismatch in the output_operand_alias attribute: "
             << "operand part has type " << operandPart
             << " and output part has type " << outputPart;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// FusionOp
//===----------------------------------------------------------------------===//

LogicalResult FusionOp::verify() { return verifyOutputOperandAliasing(this); }

//===----------------------------------------------------------------------===//
// CreateTokenOp
//===----------------------------------------------------------------------===//

LogicalResult CreateTokenOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange,
    DictionaryAttr, RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  auto dialect = context->getLoadedDialect<MhloDialect>();
  return hlo::inferCreateTokenOp(dialect, location, inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// CustomCallOp
//===----------------------------------------------------------------------===//

void CustomCallOp::build(
    ::mlir::OpBuilder& odsBuilder, ::mlir::OperationState& odsState,
    ::mlir::TypeRange resultType, ::mlir::ValueRange operands,
    ::mlir::StringAttr callTargetName, ::mlir::BoolAttr hasSideEffect,
    ::mlir::StringAttr backendConfig,
    ::mlir::mhlo::CustomCallApiVersionAttr apiVersion,
    ::mlir::ArrayAttr calledComputations, ::mlir::ArrayAttr operandLayouts,
    ::mlir::ArrayAttr resultLayouts) {
  return CustomCallOp::build(
      odsBuilder, odsState, resultType, operands, callTargetName, hasSideEffect,
      backendConfig, apiVersion, calledComputations,
      CustomCallScheduleAttr::get(odsBuilder.getContext(),
                                  CustomCallSchedule::NONE),
      operandLayouts, resultLayouts, nullptr);
}

LogicalResult CustomCallOp::verify() {
  // If both operand and result layout attributes are not specified then nothing
  // to verify.
  if (getOperandLayouts().has_value() || getResultLayouts().has_value()) {
    // Layout constraints for either both operands & results or none should be
    // specified.
    if (getOperandLayouts().has_value() != getResultLayouts().has_value())
      return emitOpError() << "Layout attributes should be specified for "
                              "either both operands and results or none.";

    // Helper function to verify types and the corresponding layouts.
    auto verifyTypesAndLayouts =
        [this](TypeRange types, mlir::ArrayAttr layouts,
               const std::string& valueName) -> LogicalResult {
      if (types.size() != layouts.size())
        return emitOpError()
               << "Number of " << valueName << "s must match the number of "
               << valueName << " layouts, " << types.size()
               << " != " << layouts.size();

      for (const auto& indexedTypeAndLayout :
           llvm::enumerate(llvm::zip(types, layouts))) {
        // Get index for more descriptive error message.
        auto index = indexedTypeAndLayout.index();

        auto type = std::get<0>(indexedTypeAndLayout.value());
        auto layout = std::get<1>(indexedTypeAndLayout.value())
                          .cast<DenseIntElementsAttr>();

        if (type.isa<TupleType>())
          return emitOpError() << "Tuple types are not fully supported with "
                                  "layout constraints yet";
        auto tensorType = type.dyn_cast<TensorType>();

        // For non-tensor types such as !mhlo.token, the layout should be empty.
        if (!tensorType) {
          if (layout.empty()) continue;
          return emitOpError()
                 << "Only tensor types can have non-empty layout: " << valueName
                 << " #" << index << " of type " << type << " has layout "
                 << layout;
        }

        // For unranked tensors, we cannot verify the compatibility with layout
        // any further.
        if (!tensorType.hasRank()) continue;

        // Layout must be a permutation of [0, N) where N is the rank of the
        // tensor type.
        std::vector<int64_t> range(tensorType.getRank());
        std::iota(range.begin(), range.end(), 0);
        if (tensorType.getRank() != layout.size() ||
            !std::is_permutation(range.begin(), range.end(), layout.begin()))
          return emitOpError()
                 << "incorrect layout " << layout << " for type " << type
                 << ", layout must be a permutation of [0, "
                 << tensorType.getRank() << ")";
      }
      return success();
    };

    // At this point both `operand_layouts` and `result_layouts` are defined.
    ArrayAttr operandLayouts = this->getOperandLayouts().value();
    ArrayAttr resultLayouts = this->getResultLayouts().value();

    // Full support for layouts for arbitrary nesting of tuples is not
    // supported yet.
    //
    // If result does not have any tuples, then i-th element of `result_layouts`
    // specifies the layout constraints on i-th result.
    //
    // For the common case of a single tuple result packing non-tuple values,
    // the i-th element of `result_layouts` specifies layout for i-th element of
    // the result tuple.
    TypeRange resultTypes;
    if (getNumResults() == 1 && getResult(0).getType().isa<TupleType>())
      resultTypes = getResult(0).getType().cast<TupleType>().getTypes();
    else
      resultTypes = getResultTypes();

    // Verify that operands and operand layouts match.
    if (failed(verifyTypesAndLayouts(getOperandTypes(), operandLayouts,
                                     "operand")))
      return failure();

    // Verify that results and result layouts match.
    if (failed(verifyTypesAndLayouts(resultTypes, resultLayouts, "result")))
      return failure();
  }

  // Check output_operand_aliases
  if (failed(verifyOutputOperandAliasing(this))) return failure();

  // Check backend_config attribute.
  if (auto backendConfig = getBackendConfig()) {
    if (getApiVersion() == CustomCallApiVersion::API_VERSION_TYPED_FFI) {
      // Typed FFI custom calls require `backend_config` to be a DictionaryAttr.
      if (backendConfig->isa<mlir::StringAttr>())
        return emitOpError()
               << "unsupported user-encoded backend config,"
                  " backend config must be a dictionary attribute.";
    } else {
      // Older API versions require user-encoded `backend_config` string.
      if (backendConfig->isa<mlir::DictionaryAttr>())
        return emitOpError()
               << "unsupported dictionary attribute backend config, backend"
                  " config must be a user-encoded string attribute.";
    }
  }

  return success();
}

void CustomCallOp::getEffects(
    SmallVectorImpl<SideEffects::EffectInstance<MemoryEffects::Effect>>&
        effects) {
  // CustomCall has "all possible effects" unless the has_side_effect is present
  // and set to false.
  auto hasSideEffect = (*this)->getAttrOfType<BoolAttr>("has_side_effect");
  if (hasSideEffect && !hasSideEffect.getValue()) return;
  effects.emplace_back(MemoryEffects::Allocate::get());
  effects.emplace_back(MemoryEffects::Free::get());
  effects.emplace_back(MemoryEffects::Write::get());
  effects.emplace_back(MemoryEffects::Read::get());
}

//===----------------------------------------------------------------------===//
// CholeskyOp
//===----------------------------------------------------------------------===//

LogicalResult CholeskyOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  CholeskyOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferCholeskyOp(location, adaptor.getA(), inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// DotOp
//===----------------------------------------------------------------------===//

LogicalResult DotOp::verify() {
  return hlo::verifyDotOp(getLoc(), getLhs(), getRhs(), getPrecisionConfig(),
                          getResult());
}

//===----------------------------------------------------------------------===//
// DotGeneralOp
//===----------------------------------------------------------------------===//

LogicalResult DotGeneralOp::verify() {
  return hlo::verifyDotGeneralOp(
      getLoc(), getLhs(), getRhs(),
      getDotDimensionNumbersAttr().getLhsBatchingDimensions(),
      getDotDimensionNumbersAttr().getRhsBatchingDimensions(),
      getDotDimensionNumbersAttr().getLhsContractingDimensions(),
      getDotDimensionNumbersAttr().getRhsContractingDimensions(),
      getPrecisionConfig(), getResult());
}

namespace {

constexpr char kFrontendAttributesAttr[] = "mhlo.frontend_attributes";

// Handle the generic case of DotGeneral and convert to a regulat DotOp.
struct DotGeneralToDot : public OpRewritePattern<DotGeneralOp> {
  using OpRewritePattern<DotGeneralOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DotGeneralOp dot,
                                PatternRewriter& rewriter) const override {
    auto lhs = dot.getLhs();
    auto rhs = dot.getRhs();
    auto lhsTy = lhs.getType().cast<ShapedType>();
    auto rhsTy = rhs.getType().cast<ShapedType>();

    int64_t lhsRank = lhsTy.getRank();
    int64_t rhsRank = rhsTy.getRank();
    if ((lhsRank != 1 && lhsRank != 2) || (rhsRank != 1 && rhsRank != 2)) {
      return rewriter.notifyMatchFailure(
          dot, "input tensors must have rank of 1 or 2");
    }

    auto nums = dot.getDotDimensionNumbers();
    if ((!nums.getLhsBatchingDimensions().empty()) ||
        (!nums.getRhsBatchingDimensions().empty())) {
      return rewriter.notifyMatchFailure(dot, "cannot have batch dimensions");
    }

    auto lhsContract = nums.getLhsContractingDimensions();
    auto rhsContract = nums.getRhsContractingDimensions();

    if (lhsContract.size() != 1 || rhsContract.size() != 1) {
      return rewriter.notifyMatchFailure(
          dot, "input tensors must only have 1 contracting dimension");
    }
    if (rhsContract.front() != 0) {
      return rewriter.notifyMatchFailure(
          dot, "rhs must contract the first dimension");
    }
    if (lhsContract.front() != lhsRank - 1) {
      return rewriter.notifyMatchFailure(
          dot, "lhs must contract the last dimension");
    }

    DictionaryAttr frontendAttributes =
        dot->getAttrOfType<DictionaryAttr>(kFrontendAttributesAttr);
    auto newDotOp = rewriter.replaceOpWithNewOp<mhlo::DotOp>(
        dot, dot.getType(), lhs, rhs,
        dot.getPrecisionConfig().value_or(nullptr));
    if (frontendAttributes) {
      newDotOp->setAttr(kFrontendAttributesAttr, frontendAttributes);
    }

    return success();
  }
};
}  // namespace

void DotGeneralOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                               MLIRContext* context) {
  results.add<DotGeneralToDot>(context);
}

LogicalResult DotGeneralOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  auto lhsType = getLhs().getType().dyn_cast<ShapedType>();
  auto rhsType = getRhs().getType().dyn_cast<ShapedType>();
  if (!lhsType || !rhsType) {
    return failure();
  }

  Adaptor adaptor(operands);
  auto dimNumbers = getDotDimensionNumbers();
  SmallVector<Value> dimensions;
  for (const int64_t lhsDim : dimNumbers.getLhsBatchingDimensions()) {
    dimensions.push_back(
        builder.create<tensor::DimOp>(getLoc(), adaptor.getLhs(), lhsDim));
  }

  for (int64_t i = 0; i < lhsType.getRank(); i++) {
    if (!llvm::is_contained(dimNumbers.getLhsContractingDimensions(), i) &&
        !llvm::is_contained(dimNumbers.getLhsBatchingDimensions(), i)) {
      dimensions.push_back(
          builder.create<tensor::DimOp>(getLoc(), adaptor.getLhs(), i));
    }
  }
  for (int64_t i = 0; i < rhsType.getRank(); i++) {
    if (!llvm::is_contained(dimNumbers.getRhsContractingDimensions(), i) &&
        !llvm::is_contained(dimNumbers.getRhsBatchingDimensions(), i)) {
      dimensions.push_back(
          builder.create<tensor::DimOp>(getLoc(), adaptor.getRhs(), i));
    }
  }

  reifiedReturnShapes.push_back(
      builder.create<tensor::FromElementsOp>(getLoc(), dimensions));
  return success();
}

//===----------------------------------------------------------------------===//
// FftOp
//===----------------------------------------------------------------------===//

LogicalResult FftOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  FftOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferFftOp(location, adaptor.getOperand(),
                         adaptor.getFftType() == FftType::RFFT,
                         adaptor.getFftType() == FftType::IRFFT,
                         adaptor.getFftLength(), inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// GatherOp
//===----------------------------------------------------------------------===//

// Converts gather ops to slice ops in case we have a single set of constant
// indices.
struct GatherSlice : public OpRewritePattern<GatherOp> {
  using OpRewritePattern<GatherOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(GatherOp gather,
                                PatternRewriter& rewriter) const override {
    DenseIntElementsAttr index;
    if (!matchPattern(gather.getStartIndices(), m_Constant(&index)))
      return failure();

    const auto& dnums = gather.getDimensionNumbers();
    if (dnums.getIndexVectorDim() != 0 || index.getType().getRank() > 1)
      return failure();

    // TODO(tberghammer): Remove when the verifier catches this case what is
    // invalid if all previous condition holds.
    if (index.getNumElements() !=
        static_cast<int64_t>(dnums.getStartIndexMap().size()))
      return failure();

    RankedTensorType operandType =
        gather->getOperand(0).getType().dyn_cast<RankedTensorType>();
    if (!operandType || !operandType.hasStaticShape()) return failure();

    auto sliceEnd =
        llvm::to_vector<8>(gather.getSliceSizes().getValues<int64_t>());
    llvm::SmallVector<int64_t, 8> sliceStart(sliceEnd.size(), 0);
    for (auto it :
         llvm::zip(dnums.getStartIndexMap(), index.getValues<APInt>())) {
      int64_t mapIndex = std::get<0>(it);
      // Clamp the indices within bounds to faithfully mirror gather semantics.
      int64_t offset =
          clamp(std::get<1>(it).getSExtValue(), static_cast<int64_t>(0),
                operandType.getDimSize(mapIndex) - sliceEnd[mapIndex]);
      sliceStart[mapIndex] += offset;
      sliceEnd[mapIndex] += offset;
    }

    llvm::SmallVector<int64_t, 8> sliceStride(sliceEnd.size(), 1);
    llvm::SmallVector<int64_t, 8> sliceShape(sliceEnd.size());
    for (size_t i = 0; i < sliceEnd.size(); ++i) {
      sliceShape[i] = sliceEnd[i] - sliceStart[i];
    }
    Type elementType = gather.getType().cast<TensorType>().getElementType();
    auto sliceType = RankedTensorType::get(sliceShape, elementType);
    Value result = rewriter.create<SliceOp>(
        gather.getLoc(), sliceType, gather.getOperand(),
        rewriter.getI64TensorAttr(sliceStart),
        rewriter.getI64TensorAttr(sliceEnd),
        rewriter.getI64TensorAttr(sliceStride));

    auto collapsedSliceDims = dnums.getCollapsedSliceDims();
    if (!collapsedSliceDims.empty()) {
      llvm::SmallVector<int64_t, 8> reshapeShape;
      for (size_t i = 0; i < sliceShape.size(); ++i) {
        if (llvm::count(collapsedSliceDims, i) == 0) {
          reshapeShape.push_back(sliceShape[i]);
        }
      }
      auto reshapeType = RankedTensorType::get(reshapeShape, elementType);
      result = rewriter.create<ReshapeOp>(gather.getLoc(), reshapeType, result);
    }

    result.setType(gather.getType());
    rewriter.replaceOp(gather, result);
    return success();
  }
};

void GatherOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                           MLIRContext* context) {
  results.add<GatherSlice>(context);
}

namespace {

// following https://www.tensorflow.org/xla/operation_semantics#gather
// The bounds for the output array along dimension i is computed as follows:
// (1) If i is present in batch_dims (i.e. is equal to batch_dims[k] for some k)
// then we pick
// the corresponding dimension bounds out of start_indices.shape, skipping
// index_vector_dim
// (i.e. pick start_indices.shape.dims[k] if k < index_vector_dim and
// start_indices.shape.dims[k+1] otherwise).
// (2) If i is present in offset_dims (i.e. equal to offset_dims[k] for some k)
// then we pick
// the corresponding bound out of slice_sizes after accounting for
// collapsed_slice_dims
// (i.e. we pick adjusted_slice_sizes[k] where adjusted_slice_sizes is
// slice_sizes with the bounds at indices collapsed_slice_dims removed).

void getSliceSizeValues(GatherOp* gather, OpBuilder& builder, Location loc,
                        ValueRange operands,
                        SmallVectorImpl<Value>& sliceSizes) {
  for (int64_t val : gather->getSliceSizes().getValues<int64_t>()) {
    sliceSizes.push_back(builder.create<arith::ConstantIndexOp>(loc, val));
  }
}

void getSliceSizeValues(DynamicGatherOp* /*dGather*/, OpBuilder& builder,
                        Location loc, ValueRange operands,
                        SmallVectorImpl<Value>& sliceSizeValues) {
  DynamicGatherOp::Adaptor adaptor(operands);
  Value sliceSizes = adaptor.getSliceSizes();
  auto sliceSizesTy = sliceSizes.getType().cast<ShapedType>();
  for (int64_t i = 0; i < sliceSizesTy.getDimSize(0); ++i) {
    Value idx = builder.create<arith::ConstantIndexOp>(loc, i);
    sliceSizeValues.push_back(
        builder.create<tensor::ExtractOp>(loc, sliceSizes, idx));
  }
}

template <typename Op>
LogicalResult reifyGatherShape(Op* op, OpBuilder& builder, ValueRange operands,
                               SmallVectorImpl<Value>& reifiedReturnShapes) {
  // No support for unranked gather output shape a.t.m.
  auto resultTy =
      op->getResult().getType().template dyn_cast<RankedTensorType>();
  if (!resultTy) return failure();

  typename Op::Adaptor adaptor(operands);
  Value startIndices = adaptor.getStartIndices();

  Location loc = op->getLoc();
  int resultRank = resultTy.getRank();
  Type shapeElTy = builder.getIndexType();
  auto toShapeElType = [&](Value v) {
    return maybeCastTo(builder, loc, v, shapeElTy);
  };

  SmallVector<Value, 4> sliceSizes;
  getSliceSizeValues(op, builder, loc, operands, sliceSizes);
  llvm::transform(sliceSizes, sliceSizes.begin(),
                  [&](Value v) { return toShapeElType(v); });

  auto getStartIndicesDim = [&](int64_t index) {
    return toShapeElType(
        builder.create<tensor::DimOp>(loc, startIndices, index));
  };
  SmallVector<Value, 4> shapeValues;
  auto getSliceDim = [&sliceSizes](int64_t index) -> Value {
    return sliceSizes[index];
  };
  hlo::reifyGatherDimSizes(resultRank, getStartIndicesDim, getSliceDim,
                           op->getDimensionNumbers().getOffsetDims(),
                           op->getDimensionNumbers().getCollapsedSliceDims(),
                           op->getDimensionNumbers().getStartIndexMap(),
                           op->getDimensionNumbers().getIndexVectorDim(),
                           shapeValues);

  Value outputShape = builder.create<tensor::FromElementsOp>(
      loc, RankedTensorType::get({resultRank}, shapeElTy), shapeValues);
  reifiedReturnShapes.push_back(outputShape);

  return success();
}

}  // namespace

LogicalResult GatherOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  return reifyGatherShape(this, builder, operands, reifiedReturnShapes);
}

LogicalResult GatherOp::inferReturnTypeComponents(
    MLIRContext* context, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  GatherOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferGatherOp(
      location, adaptor.getOperand(), adaptor.getStartIndices(),
      adaptor.getDimensionNumbers().getOffsetDims(),
      adaptor.getDimensionNumbers().getCollapsedSliceDims(),
      adaptor.getDimensionNumbers().getStartIndexMap(),
      adaptor.getDimensionNumbers().getIndexVectorDim(),
      adaptor.getSliceSizes(), inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// DynamicGatherOp
//===----------------------------------------------------------------------===//

// Canonicalize mhlo.dynamic_gather to mhlo.gather when slice_sizes is constant.
LogicalResult simplifyDynamicGatherToGather(DynamicGatherOp op,
                                            PatternRewriter& rewriter) {
  DenseIntElementsAttr dynamicGatherSliceSizes;
  if (!matchPattern(op.getSliceSizes(), m_Constant(&dynamicGatherSliceSizes))) {
    return failure();
  }

  // DynamicGatherOp's slice_sizes is 1DTensorOf<[HLO_DimensionValue]>
  // where HLO_DimensionValue is AnyTypeOf<[Index, HLO_Int]>.
  // However, GatherOp's slice_sizes is I64ElementsAttr.
  // Therefore, we need to convert the elements in case there is a mismatch
  // of element types.
  DenseIntElementsAttr gatherSliceSizes = dynamicGatherSliceSizes;
  if (!dynamicGatherSliceSizes.getType().getElementType().isInteger(64)) {
    SmallVector<int64_t> sliceSizes;
    for (APInt sliceSize : dynamicGatherSliceSizes.getValues<APInt>()) {
      sliceSizes.push_back(sliceSize.getSExtValue());
    }
    gatherSliceSizes = rewriter.getI64TensorAttr(sliceSizes);
  }

  rewriter.replaceOpWithNewOp<mhlo::GatherOp>(
      op, op.getOperand(), op.getStartIndices(), op.getDimensionNumbersAttr(),
      gatherSliceSizes, op.getIndicesAreSortedAttr());
  return success();
}

void DynamicGatherOp::getCanonicalizationPatterns(RewritePatternSet& result,
                                                  MLIRContext* context) {
  result.add(simplifyDynamicGatherToGather);
}

LogicalResult DynamicGatherOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  return reifyGatherShape(this, builder, operands, reifiedReturnShapes);
}

LogicalResult DynamicGatherOp::inferReturnTypeComponents(
    MLIRContext* context, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  DynamicGatherOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferDynamicGatherOp(
      location, adaptor.getOperand(), adaptor.getStartIndices(),
      adaptor.getSliceSizes(), adaptor.getDimensionNumbers().getOffsetDims(),
      adaptor.getDimensionNumbers().getCollapsedSliceDims(),
      adaptor.getDimensionNumbers().getStartIndexMap(),
      adaptor.getDimensionNumbers().getIndexVectorDim(), inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// GetDimensionSizeOp
//===----------------------------------------------------------------------===//

LogicalResult GetDimensionSizeOp::verify() { return verifyDimAttr(*this); }

LogicalResult GetDimensionSizeOp::inferReturnTypeComponents(
    MLIRContext*, std::optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  GetDimensionSizeOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferGetDimensionSizeOp(location, adaptor.getOperand().getType(),
                                      adaptor.getDimension(),
                                      inferredReturnShapes);
}

/// Fold get_dimension_size when the said shape dimension is a constant.
OpFoldResult GetDimensionSizeOp::fold(FoldAdaptor) {
  RankedTensorType type = getOperand().getType().dyn_cast<RankedTensorType>();
  if (!type) return {};

  int32_t dim = getDimension();
  if (type.isDynamicDim(dim)) return {};
  // The result type is always is a 0-d i32 tensor.
  return DenseIntElementsAttr::get<int32_t>(
      getResult().getType().cast<RankedTensorType>(), type.getDimSize(dim));
}

//===----------------------------------------------------------------------===//
// IotaOp
//===----------------------------------------------------------------------===//

LogicalResult IotaOp::verify() {
  return hlo::verifyIotaOp(getLoc(), getIotaDimension(), getResult());
}

// Iota operations across multiple dimensions can be reduced to an iota and a
// ranked broadcast.
struct IotaBroadcast : public OpRewritePattern<IotaOp> {
  using OpRewritePattern<IotaOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IotaOp iota,
                                PatternRewriter& rewriter) const override {
    auto resultTy = iota.getType().cast<ShapedType>();
    if (!resultTy.hasRank() || resultTy.getRank() < 2) {
      return failure();
    }

    auto iotaDimension = iota.getIotaDimension();

    auto iotaType = RankedTensorType::get({resultTy.getDimSize(iotaDimension)},
                                          resultTy.getElementType());

    auto newIota = rewriter.create<IotaOp>(iota.getLoc(), iotaType,
                                           rewriter.getI64IntegerAttr(0));

    auto broadcastAttr = DenseIntElementsAttr::get(
        RankedTensorType::get({1}, rewriter.getIntegerType(64)),
        {iotaDimension});
    rewriter.replaceOpWithNewOp<BroadcastInDimOp>(iota, resultTy, newIota,
                                                  broadcastAttr);
    return success();
  }
};

void IotaOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                         MLIRContext* context) {
  results.add<IotaBroadcast>(context);
}

OpFoldResult IotaOp::fold(FoldAdaptor /*adaptor*/) {
  auto dimension = getIotaDimension();
  auto resultTy = getResult().getType().cast<ShapedType>();
  if (resultTy.hasRank() && resultTy.getDimSize(dimension) == 1) {
    Builder builder(getContext());
    return builder.getZeroAttr(resultTy);
  }

  return {};
}

//===----------------------------------------------------------------------===//
// DynamicIotaOp
//===----------------------------------------------------------------------===//

namespace {

struct DynamicIotaIsStatic : public OpRewritePattern<DynamicIotaOp> {
  using OpRewritePattern<DynamicIotaOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DynamicIotaOp iota,
                                PatternRewriter& rewriter) const override {
    // Result type has static shape, replace with iota.
    auto resultTy = iota.getType().cast<ShapedType>();
    if (resultTy.hasStaticShape()) {
      rewriter.replaceOpWithNewOp<IotaOp>(iota, resultTy,
                                          iota.getIotaDimension());
      return success();
    }

    return rewriter.notifyMatchFailure(iota, "requires output static shape");
  }
};

// Dynamic Iota operations across multiple dimensions can be reduced to an iota
// and a ranked broadcast.
struct DynamicIotaBroadcast : public OpRewritePattern<DynamicIotaOp> {
  using OpRewritePattern<DynamicIotaOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DynamicIotaOp iota,
                                PatternRewriter& rewriter) const override {
    auto resultTy = iota.getType().cast<ShapedType>();
    if (!resultTy.hasRank() || resultTy.getRank() < 2) {
      return failure();
    }

    auto iotaDimension = iota.getIotaDimension();
    auto iotaDimensionInt = iotaDimension;

    auto convertedShape = rewriter.create<arith::IndexCastOp>(
        iota.getLoc(),
        RankedTensorType::get(
            iota.getOutputShape().getType().cast<ShapedType>().getShape(),
            rewriter.getI64Type()),
        iota.getOutputShape());

    auto slicedShape = rewriter.create<SliceOp>(
        iota.getLoc(), convertedShape,
        rewriter.getI64TensorAttr(iotaDimensionInt),
        rewriter.getI64TensorAttr(iotaDimensionInt + 1),
        rewriter.getI64TensorAttr(1));

    auto convertedSlicedShape = rewriter.create<arith::IndexCastOp>(
        iota.getLoc(),
        RankedTensorType::get({1}, iota.getOutputShape()
                                       .getType()
                                       .cast<ShapedType>()
                                       .getElementType()),
        slicedShape);

    auto iotaType = RankedTensorType::get(
        {resultTy.getDimSize(iotaDimensionInt)}, resultTy.getElementType());

    auto newIota = rewriter.create<DynamicIotaOp>(
        iota.getLoc(), iotaType, convertedSlicedShape,
        rewriter.getI64IntegerAttr(0));

    auto broadcastAttr = DenseIntElementsAttr::get(
        RankedTensorType::get({1}, rewriter.getIntegerType(64)),
        {iotaDimension});
    rewriter.replaceOpWithNewOp<DynamicBroadcastInDimOp>(
        iota, resultTy, newIota, iota.getOutputShape(), broadcastAttr);
    return success();
  }
};

}  // namespace

void DynamicIotaOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                MLIRContext* context) {
  results.add<DynamicIotaIsStatic>(context);
  results.add<DynamicIotaBroadcast>(context);
}

static Value castToIndexTensor(OpBuilder& builder, Location loc,
                               Value shapeOp) {
  ShapedType resultTy = shape::getExtentTensorType(
      builder.getContext(), shapeOp.getType().cast<ShapedType>().getDimSize(0));
  if (shapeOp.getType() == resultTy) return shapeOp;  // Nothing to do.
  return builder.create<arith::IndexCastOp>(loc, resultTy, shapeOp);
}

LogicalResult DynamicIotaOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  DynamicIotaOp::Adaptor adaptor(operands);
  reifiedReturnShapes.push_back(
      castToIndexTensor(builder, getLoc(), adaptor.getOutputShape()));
  return success();
}

//===----------------------------------------------------------------------===//
// DynamicUpdateSliceOp
//===----------------------------------------------------------------------===//

LogicalResult DynamicUpdateSliceOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  DynamicUpdateSliceOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferDynamicUpdateSliceOp(
      location, adaptor.getOperand(), adaptor.getUpdate(),
      adaptor.getStartIndices(), inferredReturnShapes);
}

OpFoldResult DynamicUpdateSliceOp::fold(FoldAdaptor /*adaptor*/) {
  auto operandShape = this->getOperand().getType().cast<RankedTensorType>();
  auto updateShape = this->getUpdate().getType().cast<RankedTensorType>();

  // If any of the dimensions are length-0, the update does nothing.
  for (auto dim : updateShape.getShape()) {
    if (dim == 0) {
      return this->getOperand();
    }
  }

  if (operandShape != updateShape || !operandShape.hasStaticShape()) {
    return {};
  }

  // Ensure that indices are 0 constants. The 0 check mostly ensures
  // correctness. For non-constants, the pattern does not fold to avoid hiding
  // the behavior of incorrect user input.
  for (Value index : this->getStartIndices()) {
    DenseIntElementsAttr deAttr;
    if (!matchPattern(index, m_Constant(&deAttr))) return {};
    if (!deAttr.getSplatValue<IntegerAttr>().getValue().isZero()) return {};
  }
  return this->getUpdate();
}

//===----------------------------------------------------------------------===//
// AbsOp
//===----------------------------------------------------------------------===//

LogicalResult AbsOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  AbsOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferAbsOp(location, adaptor.getOperand(), inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// CollectivePermuteOp
//===----------------------------------------------------------------------===//

LogicalResult CollectivePermuteOp::verify() {
  return hlo::verifyCollectivePermuteOp(getLoc(), getSourceTargetPairs());
}

//===----------------------------------------------------------------------===//
// ConvolutionOp
//===----------------------------------------------------------------------===//

namespace {
// Infer the return-shape of ConvolutionOp.
// Precondition:
//  1. Input args to ConvolutionOp 'op' are RankedTypes.
//  2. rank-of(input-type) == rank-of(output-type)
SmallVector<int64_t> inferConvolutionOpReturnShape(
    ConvolutionOp op, const ArrayRef<hlo::WindowDimension> window) {
  // We keep the 'unknown' dimensions (cl/415132294) as it is in the
  // output-shape. To do that we initilize the output dimensions with the shape
  // of the return-type and updates only the spatial + non-spatial dimensions.
  // Precondition 2 ensures that size of output-shape == size of input-shape.
  // NOTE: This is a divergence from StableHLO which doesn't allow us to fully
  // share ConvolutionOp's verification / shape inference logic with StableHLO.
  SmallVector<int64_t> outputDimensions =
      to_vector(op.getResult().getType().cast<ShapedType>().getShape());

  // Infer the output spatial dimensions.
  auto lhsType = op.getLhs().getType().cast<RankedTensorType>();
  auto inputSpatialDims = op.getDimensionNumbers().getInputSpatialDimensions();
  auto numSpatialDims = inputSpatialDims.size();
  SmallVector<int64_t> inputSpatialDimVals(numSpatialDims);
  for (int64_t i = 0; i < static_cast<int64_t>(numSpatialDims); ++i)
    inputSpatialDimVals[i] = lhsType.getShape()[inputSpatialDims[i]];

  auto windowOutputShape = inferWindowOutputShape(inputSpatialDimVals, window);

  for (int64_t i = 0; i < static_cast<int64_t>(window.size()); ++i)
    outputDimensions[op.getDimensionNumbers().getOutputSpatialDimensions()[i]] =
        windowOutputShape[i];

  // Infer the output-batch-dimension and output-feature-dimension.
  auto rhsType = op.getRhs().getType().cast<RankedTensorType>();
  const int64_t inputBatch =
      lhsType.getShape()[op.getDimensionNumbers().getInputBatchDimension()];
  const int64_t kernelOutputFeatures =
      rhsType.getShape()[op.getDimensionNumbers()
                             .getKernelOutputFeatureDimension()];

  outputDimensions[op.getDimensionNumbers().getOutputBatchDimension()] =
      hlo::isDynamicDimSize(inputBatch) ? ShapedType::kDynamic
                                        : inputBatch / op.getBatchGroupCount();
  outputDimensions[op.getDimensionNumbers().getOutputFeatureDimension()] =
      kernelOutputFeatures;

  return outputDimensions;
}

// Some mhlo.convolutions are dot products, specifically when there is no
// padding and no spatial dimensions. DotGeneralOp is general enough that it
// can sufficiently describe it.
struct ConvolutionIsDot : public OpRewritePattern<mhlo::ConvolutionOp> {
  using OpRewritePattern<mhlo::ConvolutionOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(mhlo::ConvolutionOp op,
                                PatternRewriter& rewriter) const override {
    auto lhs = op.getLhs();
    auto rhs = op.getRhs();
    auto lhsTy = lhs.getType().cast<RankedTensorType>();
    auto rhsTy = rhs.getType().cast<RankedTensorType>();
    auto resultTy = op.getType().cast<RankedTensorType>();

    if (lhsTy.getRank() != 2) return failure();
    if (rhsTy.getRank() != 2) return failure();

    if (op.getBatchGroupCount() != 1) return failure();

    // There should not be any padding if this is a matmul.
    auto dNums = op.getDimensionNumbers();
    assert(!op.getPadding() || op.getPadding()->empty());
    assert(dNums.getKernelSpatialDimensions().empty());

    auto lhsBatchDim = dNums.getInputBatchDimension();
    auto rhsBatchDim = dNums.getKernelOutputFeatureDimension();
    auto lhsContractDim = dNums.getInputFeatureDimension();
    auto rhsContractDim = dNums.getKernelInputFeatureDimension();
    auto outBatchDim = dNums.getOutputBatchDimension();
    auto outFeatureDim = dNums.getOutputFeatureDimension();

    // If the input features are not grouped then we can directly convert to an
    // mhlo.dot_general.
    if (op.getFeatureGroupCount() == 1) {
      // We can swap the lhs and rhs sides to avoid a transpose.
      if (outBatchDim == 1 && outFeatureDim == 0) {
        std::swap(lhs, rhs);
        std::swap(outBatchDim, outFeatureDim);
        std::swap(lhsContractDim, rhsContractDim);
      }

      auto dotNums = DotDimensionNumbersAttr::get(
          op.getContext(), {}, {}, {lhsContractDim}, {rhsContractDim});
      auto dotOp = rewriter.create<mhlo::DotGeneralOp>(
          op.getLoc(), op.getType(), lhs, rhs, dotNums,
          op.getPrecisionConfig().value_or(nullptr));

      rewriter.replaceOp(op, dotOp.getResult());
      return success();
    }

    int64_t featureGroupCount = op.getFeatureGroupCount();
    int64_t lhsBatchSize = lhsTy.getDimSize(lhsBatchDim);
    int64_t lhsContractSize = lhsTy.getDimSize(lhsContractDim);
    int64_t rhsBatchSize = rhsTy.getDimSize(rhsBatchDim);
    int64_t rhsContractSize = rhsTy.getDimSize(rhsContractDim);

    llvm::SmallVector<int64_t> lhsShape;
    llvm::SmallVector<int64_t> rhsShape;
    lhsShape.resize(3, lhsBatchSize);
    rhsShape.resize(3, rhsContractSize);
    lhsShape[lhsContractDim] = featureGroupCount;
    lhsShape[lhsContractDim + 1] = lhsContractSize / featureGroupCount;
    rhsShape[rhsContractDim] = featureGroupCount;
    rhsShape[rhsContractDim + 1] = rhsBatchSize / featureGroupCount;

    lhsTy = RankedTensorType::get(lhsShape, lhsTy.getElementType());
    rhsTy = RankedTensorType::get(rhsShape, rhsTy.getElementType());

    lhs = rewriter.create<mhlo::ReshapeOp>(op.getLoc(), lhsTy, lhs);
    rhs = rewriter.create<mhlo::ReshapeOp>(op.getLoc(), rhsTy, rhs);

    auto dotTy = RankedTensorType::get(
        {featureGroupCount, lhsBatchSize, rhsBatchSize / featureGroupCount},
        resultTy.getElementType());

    auto dotNums = DotDimensionNumbersAttr::get(
        op.getContext(), {lhsContractDim}, {rhsContractDim},
        {lhsContractDim + 1}, {rhsContractDim == 0 ? 2 : 0});
    auto dotOp = rewriter.create<mhlo::DotGeneralOp>(
        op.getLoc(), dotTy, lhs, rhs, dotNums,
        op.getPrecisionConfig().value_or(nullptr));

    llvm::SmallVector<int64_t> perms;
    perms.resize(3, dNums.getOutputBatchDimension() == 0 ? 0 : 2);
    perms[0] = dNums.getOutputFeatureDimension();
    perms[2] = dNums.getOutputFeatureDimension() + 1;

    auto transposeTy = RankedTensorType::get(
        {dotTy.getDimSize(perms[0]), dotTy.getDimSize(perms[1]),
         dotTy.getDimSize(perms[2])},
        dotTy.getElementType());
    auto transposeOp = rewriter.create<mhlo::TransposeOp>(
        op.getLoc(), transposeTy, dotOp, rewriter.getI64TensorAttr(perms));

    rewriter.replaceOpWithNewOp<mhlo::ReshapeOp>(op, resultTy, transposeOp);
    return success();
  }
};

}  // namespace

void ConvolutionOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                MLIRContext* context) {
  results.add<ConvolutionIsDot>(context);
}

/*
 * We intend to verify the following properties
 *  P1. Verify the input, kernel types.
 *  P2. Verify the convolution atributes.
 *  P3. Verify and collect the window atributes.
 *  P4. Verify the return shape.
 */
LogicalResult ConvolutionOp::verify() {
  auto lhsType = getLhs().getType().dyn_cast<RankedTensorType>();
  auto rhsType = getRhs().getType().dyn_cast<RankedTensorType>();

  if (!lhsType || !rhsType) return success();

  // P1.
  int numDims = lhsType.getRank();
  if (numDims != rhsType.getRank())
    return emitOpError()
           << "expects convolution arguments to have same number of "
              "dimensions. Got: "
           << lhsType << " and " << rhsType << ".";

  if (numDims < 2)
    return emitOpError()
           << "expects convolution arguments to have >= 2 dimensions. "
              "Got: "
           << lhsType << " and " << rhsType << ".";

  // P2.
  if (failed(hlo::verifyConvolutionAttributes(
          getLoc(), getLhs(), getRhs(),
          getDimensionNumbers().getInputBatchDimension(),
          getDimensionNumbers().getInputFeatureDimension(),
          getDimensionNumbers().getInputSpatialDimensions(),
          getDimensionNumbers().getKernelInputFeatureDimension(),
          getDimensionNumbers().getKernelOutputFeatureDimension(),
          getDimensionNumbers().getKernelSpatialDimensions(),
          getDimensionNumbers().getOutputBatchDimension(),
          getDimensionNumbers().getOutputFeatureDimension(),
          getDimensionNumbers().getOutputSpatialDimensions(),
          getFeatureGroupCount(), getBatchGroupCount(), getPrecisionConfig())))
    return failure();

  // P3.
  auto kernelSpatialDimensions =
      getDimensionNumbers().getKernelSpatialDimensions();
  SmallVector<int64_t> windowDimensions(kernelSpatialDimensions.size());
  for (size_t i = 0; i < windowDimensions.size(); i++)
    windowDimensions[i] = rhsType.getShape()[kernelSpatialDimensions[i]];

  auto paddingOrErr = convertNx2Attribute(this->getPadding(), getLoc());
  if (failed(paddingOrErr)) return failure();
  SmallVector<std::pair<int64_t, int64_t>> padding = *paddingOrErr;

  auto windowOrErr = hlo::verifyWindowAttributesAndInferWindowDimensions(
      windowDimensions, convertDenseIntAttr(getWindowStrides()), padding,
      convertDenseIntAttr(getLhsDilation()),
      convertDenseIntAttr(getRhsDilation()),
      *hlo::convertWindowReversalAttribute(getWindowReversal(), getLoc(),
                                           "window_reversal"),
      getLoc());
  if (failed(windowOrErr)) return failure();

  // P4.
  auto actualReturnType = getResult().getType().cast<TensorType>();
  if (!actualReturnType.hasRank()) return success();

  auto actualReturnRankedType = actualReturnType.cast<RankedTensorType>();
  if (numDims != actualReturnRankedType.getRank())
    return emitOpError() << "expects rank of convolution return-type to be "
                            "equal to input-ranks ("
                         << numDims << "), but got "
                         << actualReturnRankedType.getRank() << ".";

  auto expectedReturnShape = inferConvolutionOpReturnShape(*this, *windowOrErr);
  if (failed(verifyCompatibleShape(expectedReturnShape,
                                   actualReturnRankedType.getShape())))
    return emitOpError() << "inferred shape '"
                         << hlo::dimSizesToString(expectedReturnShape) << "' "
                         << "is incompatible with return type of operation "
                         << actualReturnRankedType;

  return success();
}

//===----------------------------------------------------------------------===//
// DynamicConvOp
//===----------------------------------------------------------------------===//

namespace {

struct DynamicConvIsConv : public OpRewritePattern<mhlo::DynamicConvOp> {
  using OpRewritePattern<mhlo::DynamicConvOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(mhlo::DynamicConvOp op,
                                PatternRewriter& rewriter) const override {
    DenseIntElementsAttr padAttr;
    if (!matchPattern(op.getDPadding(), m_Constant(&padAttr))) {
      return rewriter.notifyMatchFailure(op, "non-constant d_padding found");
    }

    SmallVector<int64_t> padArray;
    for (APInt pad : padAttr.getValues<APInt>()) {
      padArray.push_back(pad.getZExtValue());
    }

    int64_t paddedDimCount = padArray.size() / 2;
    auto newPadAttr = DenseIntElementsAttr::get(
        RankedTensorType::get({paddedDimCount, 2}, rewriter.getI64Type()),
        padArray);

    rewriter.replaceOpWithNewOp<mhlo::ConvolutionOp>(
        op, op.getType(), op.getLhs(), op.getRhs(), op.getWindowStridesAttr(),
        newPadAttr, op.getLhsDilationAttr(), op.getRhsDilationAttr(),
        op.getWindowReversalAttr(), op.getDimensionNumbers(),
        op.getFeatureGroupCount(), op.getBatchGroupCount(),
        op.getPrecisionConfigAttr());
    return success();
  }
};

}  // namespace

void DynamicConvOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                MLIRContext* context) {
  results.add<DynamicConvIsConv>(context);
}

//===----------------------------------------------------------------------===//
// ConvertOp
//===----------------------------------------------------------------------===//

void ConvertOp::build(OpBuilder& builder, OperationState& result, Value operand,
                      Type resultElementTy) {
  Type resultTy;
  Type operandTy = operand.getType();
  if (auto rankedTy = operandTy.dyn_cast<RankedTensorType>()) {
    resultTy = RankedTensorType::get(rankedTy.getShape(), resultElementTy);
  } else {
    resultTy = UnrankedTensorType::get(resultElementTy);
  }
  build(builder, result, resultTy, operand);
}

OpFoldResult ConvertOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  auto operandTy = getOperand().getType().cast<TensorType>();
  auto resultTy = getResult().getType().cast<TensorType>();
  if (operandTy == resultTy) return getOperand();

  // If the result has non-static shape, a convert op is necessary to go from
  // static shape to non-static shape.
  if (!resultTy.hasStaticShape()) return {};

  // If the operand is constant, we can do the conversion now.
  auto elementsAttr = operands.front().dyn_cast_or_null<ElementsAttr>();
  if (!elementsAttr) return {};

  // Prevent folding if the result is too large.
  if (elementsAttr.getNumElements() > kFoldOpEltLimit) return {};
  return hlo::convertElementsAttr(elementsAttr,
                                  getElementTypeOrSelf(getResult()));
}

namespace {

struct EliminateRedundantConvert : public OpRewritePattern<ConvertOp> {
  using OpRewritePattern<ConvertOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(ConvertOp op,
                                PatternRewriter& rewriter) const override {
    auto convertOp = op.getOperand().getDefiningOp<ConvertOp>();
    if (!convertOp) {
      return failure();
    }
    auto firstType =
        convertOp.getOperand().getType().cast<TensorType>().getElementType();
    auto secondType =
        op.getOperand().getType().cast<TensorType>().getElementType();
    auto thirdType =
        op.getResult().getType().cast<TensorType>().getElementType();
    auto loc = rewriter.getFusedLoc({convertOp->getLoc(), op->getLoc()});
    if (firstType.isa<FloatType>() && secondType.isa<FloatType>() &&
        thirdType.isa<FloatType>()) {
      // fold when the second float type's width is longer than first,
      // like fp16 -> fp32 -> fp64, bf16 -> fp32 -> fp16
      if (secondType.cast<FloatType>().getWidth() >
          firstType.cast<FloatType>().getWidth()) {
        Value result = rewriter.create<ConvertOp>(loc, op.getResult().getType(),
                                                  convertOp.getOperand());
        rewriter.replaceOp(op, result);
        return success();
      }
    } else if (firstType.isa<IntegerType>() && secondType.isa<IntegerType>() &&
               thirdType.isa<IntegerType>()) {
      // fold when the second integer type's width is longer than first,
      // like i16 -> i32 -> i64, u16 -> i32 -> u32
      if (secondType.cast<IntegerType>().getWidth() >
          firstType.cast<IntegerType>().getWidth()) {
        Value result = rewriter.create<ConvertOp>(loc, op.getResult().getType(),
                                                  convertOp.getOperand());
        rewriter.replaceOp(op, result);
        return success();
      }
    }
    return failure();
  }
};

}  // namespace

void ConvertOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                            MLIRContext* context) {
  results.add<EliminateIdentityConvert>(context);
  results.add<EliminateRedundantConvert>(context);
}

//===----------------------------------------------------------------------===//
// StochasticConvertOp
//===----------------------------------------------------------------------===//

LogicalResult StochasticConvertOp::verify() {
  DataLayout dataLayout = DataLayout::closest(*this);
  unsigned operandElementSize =
      dataLayout.getTypeSizeInBits(getOperand().getType().getElementType());
  unsigned randomElementSize =
      dataLayout.getTypeSizeInBits(getRandom().getType().getElementType());
  if (operandElementSize != randomElementSize) {
    return emitOpError() << "requires the random's bitwidth to match the "
                            "operand's, but got: "
                         << randomElementSize << " and " << operandElementSize;
  }
  return success();
}

namespace {

// Pattern for unpacking and repacking the same tuple.
struct UnpackRepackSameTuple : public OpRewritePattern<TupleOp> {
  using OpRewritePattern<TupleOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TupleOp op,
                                PatternRewriter& rewriter) const override {
    if (op.getVal().empty()) return failure();

    Value firstElement = op.getVal().front();
    auto firstElementOp = firstElement.getDefiningOp<GetTupleElementOp>();
    if (!firstElementOp || firstElementOp.getIndexAttr().getInt() != 0)
      return failure();

    Value tuplePredecessor = firstElementOp.getOperand();
    if (tuplePredecessor.getType() != op.getType()) return failure();

    for (const auto& elementAndIdx :
         llvm::enumerate(op.getVal().drop_front(1))) {
      auto elementOp = elementAndIdx.value().getDefiningOp<GetTupleElementOp>();
      if (!elementOp ||
          elementOp.getIndexAttr().getInt() !=
              static_cast<int64_t>(elementAndIdx.index() + 1) ||
          elementOp.getOperand() != tuplePredecessor)
        return failure();
    }

    rewriter.replaceOp(op, tuplePredecessor);
    return success();
  }
};

}  // namespace

void TupleOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                          MLIRContext* context) {
  results.add<UnpackRepackSameTuple>(context);
}

//===----------------------------------------------------------------------===//
// AllToAllOp
//===----------------------------------------------------------------------===//

LogicalResult AllToAllOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  AllToAllOp::Adaptor adaptor(operands, attributes, regions);

  bool isArrayAllToAll = adaptor.getSplitDimension() &&
                         adaptor.getConcatDimension() &&
                         adaptor.getSplitCount();
  if (!isArrayAllToAll) {
    if (adaptor.getSplitDimension() || adaptor.getConcatDimension() ||
        adaptor.getSplitCount()) {
      return emitOptionalError(location,
                               "TupleAllToAll should not have split_dimension, "
                               "concat_dimension or split_count attributes");
    }

    // TupleAllToAll has identical result and operand shapes.
    for (size_t i = 0; i < operands.size(); ++i) {
      auto rankedOperand = operands[i].getType().dyn_cast<RankedTensorType>();
      if (rankedOperand)
        inferredReturnShapes.emplace_back(rankedOperand.getShape(),
                                          rankedOperand.getElementType(),
                                          rankedOperand.getEncoding());
      else
        inferredReturnShapes.emplace_back(
            operands[i].getType().cast<ShapedType>());
    }

    return success();
  }

  if (adaptor.getOperand().size() != 1) {
    return emitOptionalError(location,
                             "ArrayAllToAll should have exactly one operand");
  }

  return hlo::inferAllToAllOp(
      location, adaptor.getOperand()[0], *adaptor.getSplitDimension(),
      *adaptor.getConcatDimension(), *adaptor.getSplitCount(),
      adaptor.getReplicaGroups(), inferredReturnShapes);
}

void AllToAllOp::build(OpBuilder& odsBuilder, OperationState& odsState,
                       Type resultType, Value operand,
                       IntegerAttr splitDimension, IntegerAttr concatDimension,
                       IntegerAttr splitCount,
                       DenseIntElementsAttr replicaGroups) {
  AllToAllOp::build(odsBuilder, odsState, resultType, operand, splitDimension,
                    concatDimension, splitCount, replicaGroups,
                    /*channel_handle=*/nullptr);
}

void AllToAllOp::build(OpBuilder& odsBuilder, OperationState& odsState,
                       ::mlir::TypeRange resultType, ::mlir::ValueRange operand,
                       IntegerAttr splitDimension, IntegerAttr concatDimension,
                       IntegerAttr splitCount,
                       DenseIntElementsAttr replicaGroups) {
  AllToAllOp::build(odsBuilder, odsState, resultType, operand, splitDimension,
                    concatDimension, splitCount, replicaGroups,
                    /*channel_handle=*/nullptr);
}

//===----------------------------------------------------------------------===//
// AllGatherOp
//===----------------------------------------------------------------------===//

LogicalResult AllGatherOp::verify() {
  return hlo::verifyAllGatherOp(getLoc(), getOperand(), getAllGatherDim(),
                                getReplicaGroups(), getUseGlobalDeviceIds(),
                                getResult());
}

void AllGatherOp::build(OpBuilder& odsBuilder, OperationState& odsState,
                        Type resultType, Value operand,
                        IntegerAttr allGatherDim,
                        DenseIntElementsAttr replicaGroups,
                        ChannelHandleAttr channelHandle) {
  AllGatherOp::build(odsBuilder, odsState, resultType, operand, allGatherDim,
                     replicaGroups, channelHandle,
                     /*use_global_device_ids=*/nullptr);
}

//===----------------------------------------------------------------------===//
// AllReduceOp
//===----------------------------------------------------------------------===//

LogicalResult AllReduceOp::verify() {
  return hlo::verifyAllReduceOp(getLoc(), getOperand(), getReplicaGroups(),
                                getUseGlobalDeviceIds(), getComputation());
}

//===----------------------------------------------------------------------===//
// BatchNormGradOp
//===----------------------------------------------------------------------===//

LogicalResult BatchNormGradOp::inferReturnTypeComponents(
    MLIRContext* context, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  BatchNormGradOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferBatchNormGradOp(
      location, adaptor.getOperand(), adaptor.getScale(), adaptor.getMean(),
      adaptor.getVariance(), adaptor.getGradOutput(), adaptor.getFeatureIndex(),
      inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// BatchNormTrainingOp
//===----------------------------------------------------------------------===//

LogicalResult BatchNormTrainingOp::inferReturnTypeComponents(
    MLIRContext* context, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  BatchNormTrainingOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferBatchNormTrainingOp(
      location, adaptor.getOperand(), adaptor.getScale(), adaptor.getOffset(),
      adaptor.getFeatureIndex(), inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// BatchNormInferenceOp
//===----------------------------------------------------------------------===//

LogicalResult BatchNormInferenceOp::inferReturnTypeComponents(
    MLIRContext* context, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  BatchNormInferenceOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferBatchNormInferenceOp(
      location, adaptor.getOperand(), adaptor.getScale(), adaptor.getOffset(),
      adaptor.getMean(), adaptor.getVariance(), adaptor.getFeatureIndex(),
      inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// BitcastOp
//===----------------------------------------------------------------------===//

OpFoldResult BitcastOp::fold(FoldAdaptor) {
  if (getResult().getType() != getOperand().getType()) {
    return {};
  }

  auto sourceLayout =
      getOperation()->getAttrOfType<DenseIntElementsAttr>("source_layout");
  auto resultLayout =
      getOperation()->getAttrOfType<DenseIntElementsAttr>("result_layout");

  if (sourceLayout == resultLayout) {
    return getOperand();
  }

  return {};
}

//===----------------------------------------------------------------------===//
// BitcastConvertOp
//===----------------------------------------------------------------------===//

LogicalResult BitcastConvertOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  auto operandType = operands[0].getType().dyn_cast<RankedTensorType>();
  auto resultType = getType().dyn_cast<RankedTensorType>();

  // Only ranked tensors are supported.
  if (!operandType || !resultType) return failure();

  // Shape-changing bitcast convert is not implemented.
  // TODO(kramerb): This could be done by adjusting the last dimension.
  DataLayout dataLayout = DataLayout::closest(*this);
  unsigned operandElementSize =
      dataLayout.getTypeSizeInBits(operandType.getElementType());
  unsigned resultElementSize =
      dataLayout.getTypeSizeInBits(resultType.getElementType());
  if (operandElementSize != resultElementSize) return failure();

  return hlo::deriveShapeFromOperand(&builder, getOperation(), operands.front(),
                                     &reifiedReturnShapes);
}

LogicalResult BitcastConvertOp::verify() {
  return hlo::verifyBitcastConvertOp(getLoc(), getOperand(), getResult());
}

//===----------------------------------------------------------------------===//
// BroadcastOp
//===----------------------------------------------------------------------===//

OpFoldResult BroadcastOp::fold(FoldAdaptor adaptor) {
  auto attrs = adaptor.getOperands();
  auto type = getType().cast<RankedTensorType>();
  auto sizesType = getBroadcastSizes().getType();
  if (sizesType.getNumElements() == 0) {
    return getOperand();
  }

  // Constant fold when an operand is a splat tensor attribute.
  if (!attrs[0] || !type.hasStaticShape()) return {};
  auto splatOperandAttr = attrs[0].dyn_cast<SplatElementsAttr>();
  if (!splatOperandAttr) return {};

  // Handle complex type
  if (type.getElementType().isa<ComplexType>()) {
    ComplexType complex = type.getElementType().cast<ComplexType>();
    if (complex.getElementType().isa<FloatType>()) {
      return DenseElementsAttr::get(
          type, {splatOperandAttr.getSplatValue<std::complex<APFloat>>()});
    }
    if (complex.getElementType().isa<IntegerType>()) {
      return DenseElementsAttr::get(
          type, {splatOperandAttr.getSplatValue<std::complex<APInt>>()});
    }
    return {};
  }

  return SplatElementsAttr::get(
      type, splatOperandAttr.getSplatValue<mlir::Attribute>());
}

LogicalResult BroadcastOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  BroadcastOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferBroadcastOp(location, adaptor.getOperand(),
                               adaptor.getBroadcastSizes(),
                               inferredReturnShapes);
}

LogicalResult BroadcastOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  BroadcastOp::Adaptor adaptor(operands);
  Value operand = adaptor.getOperand();

  auto operandType = operand.getType().dyn_cast<RankedTensorType>();
  // Unranked tensors are not supported.
  if (!operandType) return failure();

  Location loc = getLoc();
  SmallVector<Value, 4> shapeValues;

  // Collect the broadcast sizes.
  for (const auto& size : getBroadcastSizes()) {
    shapeValues.push_back(
        builder.create<arith::ConstantIndexOp>(loc, size.getZExtValue()));
  }

  // Collect the operand sizes.
  for (auto index : llvm::seq<int64_t>(0, operandType.getRank())) {
    shapeValues.push_back(
        builder.createOrFold<tensor::DimOp>(loc, operand, index));
  }

  reifiedReturnShapes.push_back(builder.create<tensor::FromElementsOp>(
      loc,
      RankedTensorType::get({static_cast<int64_t>(shapeValues.size())},
                            builder.getIndexType()),
      shapeValues));

  return success();
}

//===----------------------------------------------------------------------===//
// BroadcastInDimOp
//===----------------------------------------------------------------------===//

LogicalResult BroadcastInDimOp::verify() {
  return hlo::verifyBroadcastInDimOp(getLoc(), getOperand(),
                                     getBroadcastDimensions(), getResult());
}

OpFoldResult BroadcastInDimOp::fold(FoldAdaptor adaptor) {
  auto attrs = adaptor.getOperands();
  auto type = getType().cast<RankedTensorType>();
  if (type == getOperand().getType()) {
    auto broadcastValues = getBroadcastDimensions().getValues<int64_t>();
    if (!std::equal(broadcastValues.begin(), broadcastValues.end(),
                    llvm::seq<int64_t>(0, type.getRank()).begin())) {
      return {};
    }
    return getOperand();
  }

  // Constant fold when an operand is a splat tensor attribute.
  if (!attrs[0] || !type.hasStaticShape()) return {};
  auto splatOperandAttr = attrs[0].dyn_cast<SplatElementsAttr>();
  if (!splatOperandAttr) return {};

  // Handle complex type
  if (type.getElementType().isa<ComplexType>()) {
    ComplexType complex = type.getElementType().cast<ComplexType>();
    if (complex.getElementType().isa<FloatType>()) {
      return DenseElementsAttr::get(
          type, {splatOperandAttr.getSplatValue<std::complex<APFloat>>()});
    }
    if (complex.getElementType().isa<IntegerType>()) {
      return DenseElementsAttr::get(
          type, {splatOperandAttr.getSplatValue<std::complex<APInt>>()});
    }
    return {};
  }

  return SplatElementsAttr::get(
      type, splatOperandAttr.getSplatValue<mlir::Attribute>());
}

// Simplify BroadcastInDim has the following behaviors: replace BroadcastInDim
// with Reshape or Transpose if they are equivalent or replace
// BroadcastInDim(BroadcastInDim(X)) with BroadcastInDim(X)
class BroadcastInDimSimplifier : public OpRewritePattern<BroadcastInDimOp> {
 public:
  using OpRewritePattern<BroadcastInDimOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(BroadcastInDimOp op,
                                PatternRewriter& rewriter) const override {
    auto operandType = op.getOperand().getType().dyn_cast<RankedTensorType>();
    auto resultType = op.getResult().getType().dyn_cast<RankedTensorType>();
    if (!operandType || !resultType) {
      return failure();
    }
    auto bsDimIndices = op.getBroadcastDimensions().getValues<int64_t>();
    if (operandType.hasStaticShape() && resultType.hasStaticShape()) {
      bool sameTotalElements =
          operandType.getNumElements() == resultType.getNumElements();
      // BroadcastInDim equivalent to reshape
      if (llvm::is_sorted(bsDimIndices) && sameTotalElements) {
        rewriter.replaceOpWithNewOp<ReshapeOp>(op, op.getType(),
                                               op.getOperand());
        return success();
      }
      // BroadcastInDim equivalent to transpose
      if (operandType.getRank() == resultType.getRank() && sameTotalElements) {
        rewriter.replaceOpWithNewOp<TransposeOp>(
            op, op.getType(), op.getOperand(), op.getBroadcastDimensions());
        return success();
      }
    }
    // eliminate redundant BroadcastInDim
    if (auto broadcastInDimOp = llvm::dyn_cast_or_null<BroadcastInDimOp>(
            op.getOperand().getDefiningOp())) {
      auto newIndices =
          broadcastInDimOp.getBroadcastDimensions()
              .mapValues(op.getBroadcastDimensions().getElementType(),
                         [&bsDimIndices](const APInt& dim) -> APInt {
                           return APInt(dim.getBitWidth(),
                                        bsDimIndices[dim.getSExtValue()], true);
                         })
              .cast<DenseIntElementsAttr>();
      rewriter.replaceOpWithNewOp<BroadcastInDimOp>(
          op, op.getType(), broadcastInDimOp.getOperand(), newIndices);
      return success();
    }
    return failure();
  }
};

void BroadcastInDimOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                   MLIRContext* context) {
  results.add<BroadcastInDimSimplifier>(context);
}

//===----------------------------------------------------------------------===//
// DynamicBroadcastInDimOp
//===----------------------------------------------------------------------===//

LogicalResult DynamicBroadcastInDimOp::verify() {
  return hlo::verifyDynamicBroadcastInDimOp(
      getLoc(), getOperand(), getOutputDimensions(), getBroadcastDimensions(),
      getKnownExpandingDimensions(), getKnownNonexpandingDimensions(),
      getResult());
}

namespace {
// Does the same as PatternRewriter::replaceOpWithNewOp, but with a twist.
//
// Sometimes, we want to replace an op with a new op and simultaneously refine
// the result type from a dynamically-shaped type to a statically-shaped type.
// (Search for usages of this function for examples).
//
// Oftentimes, this works just fine because MHLO is designed to accommodate
// this kind of type refinements. But sometimes, this doesn't work - when
// the op is used outside of the MHLO dialect (e.g. in func.return). In these
// cases, we insert a tensor.cast to smooth things out.
template <typename OpTy, typename... Args>
OpTy refineOpWithNewOp(PatternRewriter& rewriter, Operation* op,
                       Args&&... args) {
  auto newOp = rewriter.create<OpTy>(op->getLoc(), std::forward<Args>(args)...);

  llvm::SmallVector<Value> replacementResults;
  assert(op->getNumResults() == newOp->getNumResults() &&
         "replacement op doesn't match results of original op");
  for (auto [opResult, newOpResult] :
       llvm::zip(op->getResults(), newOp->getResults())) {
    Value replacementResult = newOpResult;
    if (llvm::any_of(opResult.getUsers(), [&](Operation* user) {
          return user->getDialect() != op->getDialect();
        })) {
      replacementResult = rewriter.create<tensor::CastOp>(
          op->getLoc(), opResult.getType(), newOpResult);
    }
    replacementResults.push_back(replacementResult);
  }

  rewriter.replaceOp(op, replacementResults);
  return newOp;
}

// If a DynamicBroadCastInDimOp is not actually dynamic, use an ordinary
// BroadcastInDimOp.
class DynamicBroadcastInDimOpNotActuallyDynamic
    : public OpRewritePattern<DynamicBroadcastInDimOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(DynamicBroadcastInDimOp op,
                                PatternRewriter& rewriter) const override {
    auto type = op.getType().dyn_cast<RankedTensorType>();
    auto operandType = op.getOperand().getType().dyn_cast<RankedTensorType>();
    auto* outputDimOp = op.getOutputDimensions().getDefiningOp();
    if (!type || !operandType || !operandType.hasStaticShape()) {
      return rewriter.notifyMatchFailure(op, "requires operand static shape");
    }
    // output has static shape, replace with broadcast_in_dim
    if (type.hasStaticShape()) {
      rewriter.replaceOpWithNewOp<BroadcastInDimOp>(
          op, type, op.getOperand(), op.getBroadcastDimensions());
      return success();
    }
    // output_dimensions are constant, set output shape with output_dimensions,
    // then replace with broadcast_in_dim
    if (outputDimOp && outputDimOp->hasTrait<mlir::OpTrait::ConstantLike>()) {
      DenseIntElementsAttr shapeAttr;
      if (matchPattern(outputDimOp, m_Constant(&shapeAttr))) {
        SmallVector<int64_t> outputShape;
        for (APInt shape : shapeAttr.getValues<APInt>()) {
          outputShape.push_back(shape.getZExtValue());
        }
        refineOpWithNewOp<BroadcastInDimOp>(
            rewriter, op,
            RankedTensorType::get(outputShape, type.getElementType()),
            op.getOperand(), op.getBroadcastDimensions());
        return success();
      }
    }
    return rewriter.notifyMatchFailure(
        op, "requires output static shape or constant broadcast dimensions");
  }
};

class ChainedDynamicBroadcastInDimCanonicalization
    : public OpRewritePattern<DynamicBroadcastInDimOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(DynamicBroadcastInDimOp bcast,
                                PatternRewriter& rewriter) const override {
    auto precedingBcast =
        bcast.getOperand().getDefiningOp<DynamicBroadcastInDimOp>();
    if (!precedingBcast) return failure();

    // Compose broadcast dimensions.
    DenseIntElementsAttr precedingBcastDims =
        precedingBcast.getBroadcastDimensions();
    DenseIntElementsAttr bcastDims = bcast.getBroadcastDimensions();
    SmallVector<APInt, 4> composition;
    for (APInt precedingDim : precedingBcastDims) {
      composition.push_back(
          bcastDims.getValues<APInt>()[precedingDim.getZExtValue()]);
    }
    auto composedBcastDims =
        DenseIntElementsAttr::get(precedingBcastDims.getType(), composition);

    rewriter.replaceOpWithNewOp<DynamicBroadcastInDimOp>(
        bcast, bcast.getType(), precedingBcast.getOperand(),
        bcast.getOutputDimensions(), composedBcastDims);
    return success();
  }
};

// If all dimensions are known to be nonexpanding from the attribute, replace
// the dynamic broadcast with a cast.
class DynamicBroadcastInDimAllDimsNonExpanding
    : public OpRewritePattern<DynamicBroadcastInDimOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(DynamicBroadcastInDimOp op,
                                PatternRewriter& rewriter) const override {
    auto resultType = op.getResult().getType().dyn_cast<RankedTensorType>();
    if (!resultType)
      return rewriter.notifyMatchFailure(op, "requires ranked result type");

    if (!op.getKnownNonexpandingDimensions().has_value() ||
        op.getKnownNonexpandingDimensions()->size() != resultType.getRank()) {
      return rewriter.notifyMatchFailure(
          op, "known_nonexpanding_dimensions don't cover all output dims");
    }

    auto cast = rewriter.createOrFold<tensor::CastOp>(op.getLoc(), resultType,
                                                      op.getOperand());
    rewriter.replaceOp(op, cast);
    return success();
  }
};
}  // namespace

void DynamicBroadcastInDimOp::getCanonicalizationPatterns(
    RewritePatternSet& results, MLIRContext* context) {
  results.add<ChainedDynamicBroadcastInDimCanonicalization,
              DynamicBroadcastInDimOpNotActuallyDynamic,
              DynamicBroadcastInDimAllDimsNonExpanding,
              DynamicBroadcastToOwnShape_1, DynamicBroadcastToOwnShape_2,
              DynamicBroadcastToOwnShape_3, DynamicBroadcastToOwnShape_4>(
      context);
}

LogicalResult DynamicBroadcastInDimOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  DynamicBroadcastInDimOp::Adaptor adaptor(operands);
  reifiedReturnShapes.push_back(
      castToIndexTensor(builder, getLoc(), adaptor.getOutputDimensions()));
  return success();
}

//===----------------------------------------------------------------------===//
// ComplexOp
//===----------------------------------------------------------------------===//

LogicalResult ComplexOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  ComplexOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferComplexOp(location, adaptor.getLhs(), inferredReturnTypes);
}

OpFoldResult ComplexOp::fold(FoldAdaptor) {
  auto realOp = getOperand(0).getDefiningOp<mhlo::RealOp>();
  auto imagOp = getOperand(1).getDefiningOp<mhlo::ImagOp>();
  if (realOp && imagOp && realOp.getOperand() == imagOp.getOperand()) {
    return realOp.getOperand();
  }

  return {};
}

//===----------------------------------------------------------------------===//
// ImagOp
//===----------------------------------------------------------------------===//

LogicalResult ImagOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  ImagOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferImagOp(location, adaptor.getOperand(), inferredReturnTypes);
}

OpFoldResult ImagOp::fold(FoldAdaptor) {
  if (auto complexOp = getOperand().getDefiningOp<mhlo::ComplexOp>()) {
    return complexOp.getOperand(1);
  }

  return {};
}

//===----------------------------------------------------------------------===//
// IsFiniteOp
//===----------------------------------------------------------------------===//

LogicalResult IsFiniteOp::inferReturnTypes(
    MLIRContext* ctx, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  IsFiniteOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferIsFiniteOp(ctx, location, adaptor.getX(),
                              inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// RealOp
//===----------------------------------------------------------------------===//

LogicalResult RealOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  RealOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferRealOp(location, adaptor.getOperand(), inferredReturnTypes);
}

OpFoldResult RealOp::fold(FoldAdaptor) {
  if (auto complexOp = getOperand().getDefiningOp<mhlo::ComplexOp>()) {
    return complexOp.getOperand(0);
  }

  return {};
}

//===----------------------------------------------------------------------===//
// ConcatenateOp
//===----------------------------------------------------------------------===//

namespace {
class SingleOperandConcatenateToCast : public OpRewritePattern<ConcatenateOp> {
 public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ConcatenateOp op,
                                PatternRewriter& rewriter) const override {
    if (op.getVal().size() != 1) return failure();

    rewriter.replaceOpWithNewOp<tensor::CastOp>(op, op.getType(),
                                                op.getVal().front());
    return success();
  }
};

class ConcatenateOperandRemoval : public OpRewritePattern<ConcatenateOp> {
 public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ConcatenateOp op,
                                PatternRewriter& rewriter) const override {
    auto axis = op.getDimension();
    llvm::SmallVector<Value, 6> newOperands;
    for (auto operand : op.getOperands()) {
      auto ty = operand.getType().cast<ShapedType>();
      if (!ty.hasRank() || ty.getDimSize(axis) != 0) {
        newOperands.push_back(operand);
      }
    }

    if (!newOperands.empty() && newOperands.size() < op.getNumOperands()) {
      rewriter.replaceOpWithNewOp<ConcatenateOp>(
          op, op.getResult().getType(), newOperands, op.getDimension());
      return success();
    }

    return failure();
  }
};

class ConcatenateForwarding : public OpRewritePattern<ConcatenateOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(ConcatenateOp op,
                                PatternRewriter& rewriter) const override {
    auto getFlattenedOperands = [&](const Value& val) -> ValueRange {
      auto definingOp = dyn_cast_or_null<ConcatenateOp>(val.getDefiningOp());
      // To avoid inflate the memory footprint, only flatten the ConcatenateOp
      // when it has only one use.
      if (definingOp && definingOp->hasOneUse() &&
          definingOp.getDimension() == op.getDimension())
        return definingOp.getVal();
      return val;
    };

    bool needToFlatten = false;
    int operandCount = 0;
    llvm::for_each(op.getVal(), [&](Value val) {
      auto result = getFlattenedOperands(val);
      if (result.size() != 1 || result[0] != val) needToFlatten = true;
      operandCount += result.size();
    });

    if (!needToFlatten) return failure();

    llvm::SmallVector<Value, 6> newOperands;
    newOperands.reserve(operandCount);

    for (auto operand : op.getVal()) {
      auto flattenedOperands = getFlattenedOperands(operand);
      newOperands.append(flattenedOperands.begin(), flattenedOperands.end());
    }

    rewriter.replaceOpWithNewOp<ConcatenateOp>(op, op.getResult().getType(),
                                               newOperands, op.getDimension());
    return success();
  }
};

}  // namespace

LogicalResult ConcatenateOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  ConcatenateOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferConcatenateOp(location, adaptor.getVal().getTypes(),
                                 adaptor.getDimension(), inferredReturnTypes);
}

void ConcatenateOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                MLIRContext* context) {
  results.add<ConcatenateOperandRemoval, ConcatenateForwarding,
              SingleOperandConcatenateToCast>(context);
}

template <typename T>
static Attribute foldConcatenateHelper(ConcatenateOp* op,
                                       ArrayRef<Attribute> operands) {
  auto axis = op->getDimension();
  auto type = op->getType().cast<ShapedType>();
  auto shape = type.getShape();

  size_t topSize = 1;
  for (int i = 0, e = axis; i < e; i++) {
    topSize = topSize * shape[i];
  }

  // Prevent folding if the result is too large.
  if (type.getNumElements() > kFoldOpEltLimit) return {};

  SmallVector<T, 6> values;
  for (size_t i = 0; i < topSize; i++) {
    for (auto operand : operands) {
      DenseElementsAttr attr = operand.cast<DenseElementsAttr>();
      size_t bottomSize = attr.getNumElements() / topSize;
      auto iter = attr.getValues<T>().begin() + i * bottomSize;
      values.append(iter, iter + bottomSize);
    }
  }

  return DenseElementsAttr::get(type, values);
}

static Attribute foldConcatenate(ConcatenateOp* op,
                                 ArrayRef<Attribute> operands) {
  for (auto operand : operands) {
    if (!operand) return {};
  }

  auto type = op->getResult().getType().cast<ShapedType>();
  auto etype = type.getElementType();
  if (etype.isa<IntegerType>()) {
    return foldConcatenateHelper<APInt>(op, operands);
  }

  if (etype.isa<FloatType>()) {
    return foldConcatenateHelper<APFloat>(op, operands);
  }

  return {};
}

OpFoldResult ConcatenateOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  if (getNumOperands() == 1) return getOperand(0);

  ShapedType type = getResult().getType().cast<ShapedType>();
  if (!type.hasStaticShape()) return {};

  auto axis = getDimension();
  if (auto attr = foldConcatenate(this, operands)) {
    return attr;
  }

  for (auto operand : getOperands()) {
    auto ty = operand.getType().cast<ShapedType>();
    if (ty.getDimSize(axis) != 0) {
      return {};
    }
  }

  return DenseElementsAttr::get(type, ArrayRef<Attribute>());
}

LogicalResult ConcatenateOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  ConcatenateOp::Adaptor adaptor(operands);
  auto inputs = adaptor.getVal();

  auto operandType = inputs[0].getType().dyn_cast<RankedTensorType>();
  // Not support unranked type a.t.m.
  if (!operandType) return failure();

  Location loc = this->getLoc();
  Type shapeScalarType = builder.getIndexType();
  auto toShapeScalarType = [&](Value v) {
    return maybeCastTo(builder, loc, v, shapeScalarType);
  };

  SmallVector<SmallVector<Value, 4>, 4> allShapeValues;
  for (size_t inputId = 0; inputId < inputs.size(); ++inputId) {
    Value operand = inputs[inputId];
    auto operandType = operand.getType().dyn_cast<RankedTensorType>();
    if (!operandType) return failure();

    SmallVector<Value, 4> shapeVals;
    for (const auto& element : llvm::enumerate(operandType.getShape())) {
      Value valueDim = toShapeScalarType(
          builder.create<tensor::DimOp>(loc, operand, element.index()));
      shapeVals.push_back(valueDim);
    }
    allShapeValues.emplace_back(std::move(shapeVals));
  }

  int axis = this->getDimension();
  auto& shapeValues = allShapeValues[0];
  for (size_t vecId = 1; vecId < allShapeValues.size(); ++vecId) {
    auto& otherShapeValues = allShapeValues[vecId];
    if (otherShapeValues.size() != shapeValues.size()) {
      this->emitOpError()
          << "Concatenate expects all operands must be of the same rank";
      return failure();
    }
    shapeValues[axis] = builder.create<arith::AddIOp>(loc, shapeValues[axis],
                                                      otherShapeValues[axis]);
  }

  Value outputShape = builder.create<tensor::FromElementsOp>(
      loc,
      RankedTensorType::get({static_cast<int64_t>(shapeValues.size())},
                            shapeScalarType),
      shapeValues);
  reifiedReturnShapes.push_back(outputShape);

  return success();
}

//===----------------------------------------------------------------------===//
// DynamicReshapeOp
//===----------------------------------------------------------------------===//

LogicalResult DynamicReshapeOp::verify() {
  return hlo::verifyDynamicReshapeOp(getLoc(), getOutputShape(), getResult());
}

LogicalResult DynamicReshapeOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  DynamicReshapeOp::Adaptor adaptor(operands);
  reifiedReturnShapes.push_back(
      castToIndexTensor(builder, getLoc(), adaptor.getOutputShape()));
  return success();
}

namespace {
class DynamicReshapeOpNotActuallyDynamic
    : public OpRewritePattern<DynamicReshapeOp> {
 public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(DynamicReshapeOp op,
                                PatternRewriter& rewriter) const override {
    auto type = op.getResult().getType().dyn_cast<RankedTensorType>();
    if (!type || !type.hasStaticShape()) {
      return rewriter.notifyMatchFailure(op, "requires static shape tensor");
    }
    rewriter.replaceOpWithNewOp<ReshapeOp>(op, op.getType(), op.getOperand());
    return success();
  }
};

// Canonicalizes
// %0 = some_op(%tensor)
// %1 = "mhlo.dynamic_reshape"(%0, %shape)
//      (tensor<?xT>, tensor<1xindex>) -> tensor<?xT>
// ... uses of %1.
//
// into
//
// ... uses of %0.
// This canonicalization is only correct if the input is correct!
// TODO(b/178779691): Use a more sophisticated canonicalization that preserves
// errors in input, and still allows us to get rid of redundant reshapes.
class RemoveRedundantRank1DynamicReshape
    : public OpRewritePattern<DynamicReshapeOp> {
 public:
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(DynamicReshapeOp op,
                                PatternRewriter& rewriter) const override {
    auto type = op.getResult().getType().dyn_cast<RankedTensorType>();
    if (!type || type.getRank() != 1 || type.hasStaticShape()) {
      return rewriter.notifyMatchFailure(
          op, "requires rank 1 shape tensor with dynamic dimension");
    }
    auto operandType = op.getOperand().getType().dyn_cast<RankedTensorType>();
    if (!operandType || operandType.getRank() != 1 ||
        operandType.hasStaticShape()) {
      return rewriter.notifyMatchFailure(
          op, "requires rank 1 shape tensor with dynamic dimension");
    }
    rewriter.replaceOp(op, {op.getOperand()});
    return success();
  }
};

// Canonicalizes
// %0 = "mhlo.dynamic_reshape"(%tensor, %shape)
// %1 = same_operands_and_result_shape_op(%tensor)
// %2 = "mhlo.dynamic_reshape"(%1, %shape)
// ... uses of %2.
//
// into
//
// %0 = "mhlo.dynamic_reshape"(%tensor, %shape)
// %1 = same_operands_and_result_shape_op(%tensor)
// ... uses of %1.
class DynamicReshapeOpSameShapeOpResult
    : public OpRewritePattern<DynamicReshapeOp> {
 public:
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(DynamicReshapeOp op,
                                PatternRewriter& rewriter) const override {
    Operation* defOp = op.getOperand().getDefiningOp();
    if (!defOp ||
        !defOp->hasTrait<mlir::OpTrait::SameOperandsAndResultShape>()) {
      return failure();
    }
    Operation* inputDefOp = defOp->getOperand(0).getDefiningOp();
    if (!inputDefOp) {
      return failure();
    }
    auto reshape = dyn_cast<DynamicReshapeOp>(*inputDefOp);
    if (reshape && reshape.getOutputShape() == op.getOutputShape()) {
      rewriter.replaceOp(op, {defOp->getResult(0)});
      return success();
    }
    return failure();
  }
};
}  // namespace

void DynamicReshapeOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                   MLIRContext* context) {
  // clang-format off
  results.add<
      DynamicReshapeOpNotActuallyDynamic,
      DynamicReshapeOpSameShapeOpResult,
      RemoveRedundantDynamicBroadcast,
      RemoveRedundantDynamicReshape,
      RemoveRedundantRank1DynamicReshape,
      ShapeOfDynamicReshape
    >(context);
  // clang-format on
}

//===----------------------------------------------------------------------===//
// DynamicSliceOp
//===----------------------------------------------------------------------===//

namespace {
// Canonicalizes DynamicSlice ops that can be replaced instead with Slice ops.
// This canonicalization is applied the case when the `begin` input values are
// compile time constants and thus can be made into a tensor.
struct DynamicSliceToSlice : public OpRewritePattern<DynamicSliceOp> {
  using OpRewritePattern<DynamicSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DynamicSliceOp dynamicSlice,
                                PatternRewriter& rewriter) const override {
    Value input = dynamicSlice.getOperand();
    auto inputTensor = input.getType().dyn_cast<RankedTensorType>();
    if (!inputTensor || !inputTensor.hasStaticShape()) return failure();

    auto sliceSizes = dynamicSlice.getSliceSizes().getValues<int64_t>();
    SmallVector<int64_t, 4> tempStartIndices;
    for (const auto& indexAndSliceStart :
         llvm::enumerate(dynamicSlice.getStartIndices())) {
      APInt val;
      Value start = indexAndSliceStart.value();
      int64_t index = indexAndSliceStart.index();
      if (!matchPattern(start, m_ConstantInt(&val))) {
        return failure();
      }
      // Clamp the indices within bounds to faithfully mirror dynamic slice
      // semantics.
      int64_t clampedStart =
          clamp(val.getSExtValue(), static_cast<int64_t>(0),
                inputTensor.getDimSize(index) - sliceSizes[index]);
      tempStartIndices.push_back(clampedStart);
    }

    // At this point we've determined that the start indices are all constants;
    // pack them into a single tensor.
    auto loc = dynamicSlice.getLoc();
    int64_t inputRank = inputTensor.getRank();
    auto sliceStartIndices = rewriter.getI64TensorAttr(tempStartIndices);
    DenseIntElementsAttr sliceLimits = buildSliceLimits(
        sliceStartIndices, dynamicSlice.getSliceSizes(), &rewriter);
    DenseIntElementsAttr sliceStrides =
        rewriter.getI64TensorAttr(SmallVector<int64_t, 4>(inputRank, 1));
    auto result = rewriter.create<SliceOp>(loc, input, sliceStartIndices,
                                           sliceLimits, sliceStrides);
    rewriter.replaceOp(dynamicSlice, {result});
    return success();
  }
};

}  // namespace

void DynamicSliceOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                 MLIRContext* context) {
  results.add<DynamicSliceToSlice>(context);
}

LogicalResult DynamicSliceOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  DynamicSliceOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferDynamicSliceOp(location, adaptor.getOperand().getType(),
                                  adaptor.getStartIndices().getTypes(),
                                  adaptor.getSliceSizes(),
                                  inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// RealDynamicSliceOp
//===----------------------------------------------------------------------===//
// Verifies that operand rank matches start_indices/limit_indices/strides size
LogicalResult RealDynamicSliceOp::verify() {
  return hlo::verifyRealDynamicSliceOp(getLoc(), getOperand(),
                                       getStartIndices(), getLimitIndices(),
                                       getStrides());
}

namespace {
struct RealDSliceToDSlice : public OpRewritePattern<RealDynamicSliceOp> {
  using OpRewritePattern<RealDynamicSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(RealDynamicSliceOp op,
                                PatternRewriter& rewriter) const override {
    // This rewrite only works for unit strides because DynamicSliceOp
    // doesn't support strides (i.e. it implicitly has unit strides).
    DenseIntElementsAttr stridesAttr;
    if (!matchPattern(op.getStrides(), m_Constant(&stridesAttr)))
      return rewriter.notifyMatchFailure(op, "requires constant strides");
    if (!llvm::all_of(stridesAttr.getValues<APInt>(),
                      [&](APInt stride) { return stride == 1; }))
      return rewriter.notifyMatchFailure(op, "requires unit strides");

    // Check that slice sizes are fully static (DynamicSliceOp style).
    // To detect that, we check whether `limit_indices` is defined as
    // `start_indices + constant` or `constant + start_indices`.
    DenseIntElementsAttr sliceSizesAttr;
    auto m_startIndices = matchers::m_Val(op.getStartIndices());
    if (!matchPattern(
            op.getLimitIndices(),
            m_Op<AddOp>(m_startIndices, m_Constant(&sliceSizesAttr))) &&
        !matchPattern(op.getLimitIndices(),
                      m_Op<AddOp>(m_Constant(&sliceSizesAttr), m_startIndices)))
      return rewriter.notifyMatchFailure(
          op, "requires limit indices equal to start indices plus constant");

    // RealDynamicSliceOp can take tensors of integer or index element types.
    // DynamicSliceOp::slice_sizes only supports i64 element type.
    // Adapt accordingly in order to be compatible with DynamicSliceOp.
    SmallVector<int64_t> sliceSizes;
    for (auto element : sliceSizesAttr.getValues<APInt>()) {
      sliceSizes.push_back(element.getSExtValue());
    }

    // RealDynamicSliceOp::start_indices is a 1-dimensional tensor.
    // DynamicSliceOp::start_indices is a vararg of 0-dimensional tensors.
    // Adapt accordingly in order to be compatible with DynamicSliceOp.
    SmallVector<Value> startIndices;
    for (auto i = 0; i < static_cast<int64_t>(sliceSizes.size()); ++i) {
      auto startIndex1D = rewriter.create<SliceOp>(
          op.getLoc(), op.getStartIndices(), rewriter.getI64TensorAttr(i),
          rewriter.getI64TensorAttr(i + 1), rewriter.getI64TensorAttr(1));
      auto startIndex0DType = RankedTensorType::get(
          {},
          op.getStartIndices().getType().cast<ShapedType>().getElementType());
      auto startIndex0D = rewriter.create<ReshapeOp>(
          op.getLoc(), startIndex0DType, startIndex1D);
      startIndices.push_back(startIndex0D);
    }

    rewriter.replaceOpWithNewOp<mhlo::DynamicSliceOp>(
        op, op.getOperand(), startIndices,
        rewriter.getI64TensorAttr(sliceSizes));
    return success();
  }
};
}  // namespace

void RealDynamicSliceOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                     MLIRContext* context) {
  results.add<RealDSliceToSlice, RealDSliceToDSlice>(context);
}

LogicalResult RealDynamicSliceOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  RealDynamicSliceOp::Adaptor adaptor(operands);
  Value operand = adaptor.getOperand();
  Value startIndices = adaptor.getStartIndices();
  Value limitIndices = adaptor.getLimitIndices();
  Value strides = adaptor.getStrides();

  auto operandType = operand.getType().dyn_cast<RankedTensorType>();
  // Not support unranked type a.t.m.
  if (!operandType) return failure();

  Location loc = this->getLoc();
  SmallVector<Value, 4> shapeValues;
  shapeValues.reserve(operandType.getRank());
  Type shapeScalarType =
      startIndices.getType().cast<ShapedType>().getElementType();
  Value one = builder.create<arith::ConstantIndexOp>(loc, 1);
  one = maybeCastTo(builder, loc, one, shapeScalarType);
  for (const auto& element : llvm::enumerate(operandType.getShape())) {
    Value offset = builder.create<arith::ConstantIndexOp>(loc, element.index());
    Value valueStart =
        builder.create<tensor::ExtractOp>(loc, startIndices, offset);
    Value valueLimit =
        builder.create<tensor::ExtractOp>(loc, limitIndices, offset);
    Value valueStride = builder.create<tensor::ExtractOp>(loc, strides, offset);
    // size = (limit - start + stride - 1) / stride
    shapeValues.push_back(builder.create<arith::DivSIOp>(
        loc,
        builder.create<arith::SubIOp>(
            loc,
            builder.create<arith::AddIOp>(
                loc, valueStride,
                builder.create<arith::SubIOp>(loc, valueLimit, valueStart)),
            one),
        valueStride));
  }

  reifiedReturnShapes.push_back(builder.create<tensor::FromElementsOp>(
      loc,
      RankedTensorType::get({static_cast<int64_t>(shapeValues.size())},
                            shapeScalarType),
      shapeValues));
  return success();
}

//===----------------------------------------------------------------------===//
// InfeedOp
//===----------------------------------------------------------------------===//

LogicalResult InfeedOp::verify() {
  auto dialect = getContext()->getLoadedDialect<MhloDialect>();
  return hlo::verifyInfeedOp(dialect, getLoc(), getLayout(), getResults());
}

//===----------------------------------------------------------------------===//
// MapOp
//===----------------------------------------------------------------------===//

LogicalResult MapOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  MapOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferMapOp(location, adaptor.getInputs(), adaptor.getDimensions(),
                         adaptor.getComputation(), inferredReturnShapes);
}

OpFoldResult MapOp::fold(FoldAdaptor) {
  mlir::Block& bb = getComputation().front();
  mlir::Operation& frontOp = bb.front();

  auto retOp = mlir::dyn_cast<ReturnOp>(frontOp);
  if (!retOp) return nullptr;
  if (retOp.getResults().size() != 1) return nullptr;

  for (mlir::BlockArgument barg : bb.getArguments()) {
    if (barg == retOp.getResults()[0])
      return getOperands()[barg.getArgNumber()];
  }
  return nullptr;
}

LogicalResult MapOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  return hlo::deriveShapeFromOperand(&builder, getOperation(), operands.front(),
                                     &reifiedReturnShapes);
}

//===----------------------------------------------------------------------===//
// OutfeedOp
//===----------------------------------------------------------------------===//

LogicalResult OutfeedOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange,
    DictionaryAttr, RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  auto dialect = context->getLoadedDialect<MhloDialect>();
  return hlo::inferOutfeedOp(dialect, location, inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// SendOp
//===----------------------------------------------------------------------===//

LogicalResult SendOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange,
    DictionaryAttr, RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  auto dialect = context->getLoadedDialect<MhloDialect>();
  return hlo::inferSendOp(dialect, location, inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// RecvOp
//===----------------------------------------------------------------------===//

LogicalResult RecvOp::verify() {
  auto dialect = getContext()->getLoadedDialect<MhloDialect>();
  return hlo::verifyRecvOp(dialect, getLoc(), getResults());
}

//===----------------------------------------------------------------------===//
// CopyOp
//===----------------------------------------------------------------------===//

OpFoldResult CopyOp::fold(FoldAdaptor) { return getOperand(); }

//===----------------------------------------------------------------------===//
// ReduceWindowOp
//===----------------------------------------------------------------------===//

LogicalResult ReduceWindowOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  ReduceWindowOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferReduceWindowOp(
      location, adaptor.getInputs(), adaptor.getInitValues(),
      adaptor.getWindowDimensions(), adaptor.getWindowStrides(),
      adaptor.getBaseDilations(), adaptor.getWindowDilations(),
      adaptor.getPadding(), inferredReturnShapes);
}

LogicalResult ReduceWindowOp::verify() {
  return hlo::verifyReduceWindowOp(getLoc(), getInputs(), getInitValues(),
                                   getWindowDimensions(), getWindowStrides(),
                                   getBaseDilations(), getWindowDilations(),
                                   getPadding(), getBody());
}

// Get the operation used for reduction applied to `result_index`th result. Its
// expected to be a binary operation that consumes `result_index`th and
// `result_index + getInputs().size`th arguments of the body.
Operation* ReduceWindowOp::getReductionOp(int resultIndex) {
  auto returnOp = cast<ReturnOp>(getBody().front().getTerminator());
  Operation* computeOp = returnOp.getResults()[resultIndex].getDefiningOp();
  if (computeOp->getNumOperands() != 2) return nullptr;
  auto arg0 = computeOp->getOperand(0).dyn_cast<BlockArgument>();
  auto arg1 = computeOp->getOperand(1).dyn_cast<BlockArgument>();
  if (!arg0 || !arg1) return nullptr;
  int64_t arg0Num = arg0.getArgNumber();
  int64_t arg1Num = arg1.getArgNumber();
  int64_t otherArgIndex = resultIndex + getInputs().size();
  if (arg0Num == resultIndex && arg1Num == otherArgIndex) return computeOp;
  if (arg0Num == otherArgIndex && arg1Num == resultIndex &&
      computeOp->hasTrait<mlir::OpTrait::IsCommutative>())
    return computeOp;
  return nullptr;
}

bool isSplatZero(SplatElementsAttr attr) {
  if (!attr) return false;
  if (attr.getElementType().isa<FloatType>()) {
    return attr.getSplatValue<APFloat>().isZero();
  }
  if (attr.getElementType().isa<IntegerType>()) {
    return attr.getSplatValue<APInt>().isZero();
  }
  return false;
}

LogicalResult ReduceWindowOp::fold(FoldAdaptor adaptor,
                                   SmallVectorImpl<OpFoldResult>& results) {
  auto operands = adaptor.getOperands();
  const auto emptyOrAllEq = [](const Optional<DenseIntElementsAttr> opt,
                               const int64_t n) {
    return !opt.has_value() ||
           (opt->isSplat() && opt->getSplatValue<IntegerAttr>().getInt() == n);
  };
  const auto isSumReductionBody = [](mlir::Region& body) {
    if (body.getNumArguments() != 2) return false;
    auto returnOp = dyn_cast_or_null<ReturnOp>(body.back().getTerminator());
    if (!returnOp || returnOp.getNumOperands() != 1) return false;
    auto addOp = returnOp.getOperand(0).getDefiningOp<AddOp>();
    if (!addOp) return false;
    return (addOp.getLhs() == body.getArgument(0) &&
            addOp.getRhs() == body.getArgument(1)) ||
           (addOp.getLhs() == body.getArgument(1) &&
            addOp.getRhs() == body.getArgument(0));
  };

  // Fold no-op single input sum reduction.
  if (getInputs().size() == 1 &&
      isSplatZero(operands[1].dyn_cast_or_null<SplatElementsAttr>()) &&
      emptyOrAllEq(getWindowDimensionsAttr(), 1) &&
      emptyOrAllEq(getWindowStrides(), 1) &&
      emptyOrAllEq(getBaseDilations(), 1) &&
      emptyOrAllEq(getWindowDilations(), 1) && emptyOrAllEq(getPadding(), 0) &&
      isSumReductionBody(getBody())) {
    results.push_back(getInputs()[0]);
    return success();
  }

  return failure();
}

// Builder that takes a constructor for its region and infers result types
void ReduceWindowOp::build(
    OpBuilder& odsBuilder, OperationState& odsState, ValueRange inputs,
    ValueRange init_values, DenseIntElementsAttr window_dimensions,
    /*optional*/ DenseIntElementsAttr window_strides,
    /*optional*/ DenseIntElementsAttr base_dilations,
    /*optional*/ DenseIntElementsAttr window_dilations,
    /*optional*/ DenseIntElementsAttr padding,
    function_ref<void(OpBuilder&, Location, ValueRange)> bodyBuilder) {
  odsState.addOperands(inputs);
  odsState.addOperands(init_values);
  odsState.addAttribute(getWindowDimensionsAttrName(odsState.name),
                        window_dimensions);
  if (window_strides) {
    odsState.addAttribute(getWindowStridesAttrName(odsState.name),
                          window_strides);
  }
  if (base_dilations) {
    odsState.addAttribute(getBaseDilationsAttrName(odsState.name),
                          base_dilations);
  }
  if (window_dilations) {
    odsState.addAttribute(getWindowDilationsAttrName(odsState.name),
                          window_dilations);
  }
  if (padding) {
    odsState.addAttribute(getPaddingAttrName(odsState.name), padding);
  }
  Region* region = odsState.addRegion();

  llvm::SmallVector<Type> blockArgTypes;
  llvm::SmallVector<Location> locs;
  auto numValues = inputs.size() + init_values.size();
  blockArgTypes.reserve(numValues);
  locs.reserve(numValues);
  for (auto i : inputs) {
    auto iType = i.getType().cast<ShapedType>();
    blockArgTypes.push_back(iType.cloneWith(
        llvm::ArrayRef<int64_t>(std::nullopt), iType.getElementType()));
    locs.push_back(i.getLoc());
  }
  for (auto i : init_values) {
    auto iType = i.getType().cast<ShapedType>();
    blockArgTypes.push_back(iType.cloneWith(
        llvm::ArrayRef<int64_t>(std::nullopt), iType.getElementType()));
    locs.push_back(i.getLoc());
  }

  {
    OpBuilder::InsertionGuard g(odsBuilder);
    Block* body =
        odsBuilder.createBlock(region, /*insertPt=*/{}, blockArgTypes, locs);
    bodyBuilder(odsBuilder, odsState.location, body->getArguments());
  }

  llvm::SmallVector<mlir::Type, 2> inferredReturnTypes;
  if (mlir::succeeded(ReduceWindowOp::inferReturnTypes(
          odsBuilder.getContext(), odsState.location, odsState.operands,
          odsState.attributes.getDictionary(odsState.getContext()),
          odsState.regions, inferredReturnTypes)))
    odsState.addTypes(inferredReturnTypes);
  else
    llvm::report_fatal_error("Failed to infer result type(s).");
}

//===----------------------------------------------------------------------===//
// ReducePrecisionOp
//===----------------------------------------------------------------------===//

// The following property is already enforced by the ODS:
//  P0. operand element type is float
//  P1. mantissa_bits >= 0
// We intend to verify the following properties
//  P2. exponent_bits >= 1
LogicalResult ReducePrecisionOp::verify() {
  return hlo::verifyReducePrecisionOp(getLoc(), getExponentBits(),
                                      getMantissaBits());
}

//===----------------------------------------------------------------------===//
// ReverseOp
//===----------------------------------------------------------------------===//

template <typename T>
static Attribute foldReverseHelper(DenseElementsAttr& attr, ShapedType& type,
                                   DenseIntElementsAttr& dims) {
  int64_t numElements = attr.getNumElements();
  // No-op if the tensor has 0 elements.
  // No-op if the result of folding is too large.
  if (numElements == 0 || numElements > kFoldOpEltLimit) return {};

  SmallVector<T> result(attr.getValues<T>().begin(), attr.getValues<T>().end());

  size_t rank = type.getRank();
  SmallVector<int64_t> stride(rank + 1, numElements);
  for (size_t i = 0; i < rank; i++) {
    if (type.getDimSize(i) == 0) return {};
    stride[i + 1] = stride[i] / type.getDimSize(i);
  }

  for (auto dim : dims.getValues<int64_t>()) {
    // For example, given:
    //   * tensor: tensor<2x3x2xi32>
    //     [[[1, 2], [3, 4], [5, 6]], [[7, 8], [9,10], [11, 12]]]
    //   * dim: [1]
    //
    // We're going to reverse the tensor with respect to dim as follows:
    //   1) Split the tensor into blocks, i.e. smaller tensors whose type is
    //   derived from the tensor by dropping the first `dim` dimensions, i.e.
    //   tensor<3x2xi32> for the running example.
    //   2) Split each block into windows, i.e. even smaller tensors whose type
    //   is derived from the block by dropping the first dimension of the
    //   block, i.e. tensor<2xi32> for the running example.
    //   3) Within each block, swap windows but don't change the order of
    //   elements within the windows: 0th window goes to N-1st spot, 1st window
    //   goes to N-2nd spot etc.
    //
    // For the running example, the result will be:
    //   [[[5, 6], [3, 4], [1, 2]], [[11, 12], [9, 10], [7, 8]]].
    //
    // Note how elements within windows haven't changed their order with respect
    // to each other and how blocks haven't changed their order with respect to
    // each other.
    int64_t numWindows = type.getDimSize(dim);
    int64_t windowSize = stride[dim] / numWindows;

    for (int64_t index = 0; index < numElements; index++) {
      int64_t blockNumber = index / stride[dim];
      int64_t windowNumber = (index % stride[dim]) / windowSize;
      int64_t reversedWindowNumber = numWindows - windowNumber - 1;
      if (windowNumber >= reversedWindowNumber) continue;
      int64_t reversedIndex = blockNumber * stride[dim] +
                              reversedWindowNumber * windowSize +
                              index % windowSize;
      std::swap(result[index], result[reversedIndex]);
    }
  }
  return DenseElementsAttr::get(type, result);
}

OpFoldResult ReverseOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  Value input = getOperand();

  // No dimensions to reverse.
  DenseIntElementsAttr dims = getDimensions();
  if (dims.getNumElements() == 0) return input;

  // If size of all dimensions to reverse equals 1, then the reverse is a no-op.
  // Eg. Reverse dimensions {0,1} of a 1x1x2 tensor
  auto shapedType = input.getType().cast<ShapedType>();
  if (llvm::all_of(dims.getValues<int64_t>(), [&](int64_t dim) {
        return shapedType.getDimSize(dim) == 1;
      }))
    return input;

  // If the operand is a static shaped tensor of constants, return reversed
  // tensor
  DenseElementsAttr inputAttr =
      operands.begin()->dyn_cast_or_null<DenseElementsAttr>();
  if (inputAttr && shapedType.hasStaticShape()) {
    auto etype = shapedType.getElementType();
    if (etype.isa<IntegerType>())
      return foldReverseHelper<APInt>(inputAttr, shapedType, dims);
    if (etype.isa<FloatType>())
      return foldReverseHelper<APFloat>(inputAttr, shapedType, dims);
  }

  return {};
}

//===----------------------------------------------------------------------===//
// ReduceOp
//===----------------------------------------------------------------------===//

LogicalResult ReduceOp::fold(FoldAdaptor /*adaptor*/,
                             SmallVectorImpl<OpFoldResult>& results) {
  // No dimensions to reduce.
  if (getDimensions().getNumElements() == 0) {
    for (Value operand : this->getInputs()) {
      results.push_back(operand);
    }
    return success();
  }

  // If all returned values in the ReduceOp region exists outside
  // the region replace the ReduceOp with those values.
  mlir::Block& bb = this->getBody().front();
  SmallVector<Value> replacedResults;
  if (auto retOp = mlir::dyn_cast<ReturnOp>(bb.back())) {
    for (Value result : retOp.getResults()) {
      if (result.getParentRegion() == retOp->getParentRegion())
        return failure();
      replacedResults.push_back(result);
    }

    results.insert(results.end(), replacedResults.begin(),
                   replacedResults.end());
    return success();
  }

  return failure();
}

bool hasSameOperandAndResultTypes(Operation& op) {
  Type expected;
  if (op.getNumResults() != 0) expected = op.getResult(0).getType();
  if (op.getNumOperands() != 0) expected = op.getOperand(0).getType();
  if (!expected) return false;

  auto typeMatch = [&](Type actual) { return actual == expected; };
  return llvm::all_of(op.getOperandTypes(), typeMatch) &&
         llvm::all_of(op.getResultTypes(), typeMatch);
}

// Checks the following eligibility criteria for compact printing of
// mhlo.reduce:
// E1. The reduce-op wraps a single inner-op in the associated region.
// E2. The single operation is a commutative binary-op from mhlo dialect, zero
//     region, producing single result such that the operands and result all
//     have the same type.
// E3. The reduce-op consist of at least one input-operand; The operand-types of
//     inner-op should be derived trivially from the element-type of reduce-op's
//     first input-operand.
// E4. The  arguments of the region's only basic block are forwarded perfectly
//     to inner-op's operands.
// E5. The reduce-op, inner-op, blocks arguments, and the return-op all have the
//     same location.
// E6. The single operation result is perfectly forwarded to the reduce op
//     return.
static bool isEligibleForCompactPrint(ReduceOp op) {
  // Check E1.
  auto& block = op.getBody().front();
  if (!hasSingleElement(block.without_terminator())) return false;

  Operation& innerOp = *block.begin();

  // Check E2.
  if (innerOp.getDialect() != op->getDialect()) return false;

  if (innerOp.getNumOperands() != 2 ||
      !innerOp.hasTrait<mlir::OpTrait::OneResult>() ||
      !hasSameOperandAndResultTypes(innerOp) ||
      !innerOp.hasTrait<mlir::OpTrait::IsCommutative>() ||
      !innerOp.hasTrait<mlir::OpTrait::ZeroRegions>())
    return false;

  // Check E3.
  if (op.getInputs().empty()) return false;

  auto elemType =
      op.getInputs()[0].getType().cast<TensorType>().getElementType();
  auto expectedInnerOpType = RankedTensorType::get(/*shape=*/{}, elemType);
  if (innerOp.getOperands()[0].getType() != expectedInnerOpType) return false;

  // Check E4.
  if (!llvm::equal(block.getArguments(), innerOp.getOperands())) return false;

  // Check E5.
  auto retOp = dyn_cast<ReturnOp>(block.getTerminator());
  if (!retOp) return false;

  auto blockArgLoc = block.getArgument(0).getLoc();
  if (blockArgLoc != block.getArgument(1).getLoc()) return false;

  if (innerOp.getLoc() != op.getLoc() || retOp.getLoc() != op.getLoc() ||
      blockArgLoc != op.getLoc())
    return false;

  // Check E6.
  return llvm::equal(innerOp.getResults(), retOp.getOperands());
}

void ReduceOp::print(OpAsmPrinter& p) {
  {
    // Print the pairs of operands under the form:
    //   (%arg0 init: %arg3), (%arg1 init: %arg4), (%arg2 init: %arg5)
    StringRef comma = "";
    int numOperandPairs = getNumOperands() / 2;
    for (int opId : llvm::seq<int>(0, numOperandPairs)) {
      p << comma << "(" << getOperand(opId)
        << " init: " << getOperand(opId + numOperandPairs) << ")";
      comma = ", ";
    }
  }

  // If the reduce-op is eligible for compact printing, we emit the one-liner:
  //  mhlo.reduce applies <inner-op> across dimensions = [...] : <func-type>
  // Note: We are not printing the function type of reduction operation. We
  // have some simplifying assumptions (refer to IsEligibleForCompactPrint::E3)
  // to derive the type from that of reduce-op.
  if (isEligibleForCompactPrint(*this)) {
    Operation& innerOp = getBody().front().front();
    p << " applies ";
    printEscapedString(innerOp.getName().getStringRef(), p.getStream());

    p << " across dimensions = [";
    llvm::interleaveComma(getDimensions().getValues<int64_t>(), p);
    p << "]";
    p << " : ";
    p.printFunctionalType(*this);
  } else {
    p << " across dimensions = [";
    llvm::interleaveComma(getDimensions().getValues<int64_t>(), p);
    p << "]";
    p.printOptionalAttrDict(getOperation()->getAttrs(), {"dimensions"});
    p << " : ";
    p.printFunctionalType(*this);
    p.printNewline();
    p << " reducer";
    {
      // Print the pairs of block operands under the form:
      //   (%arg0_elt, %arg0_acc) (%arg1_elt, %arg1_acc):
      Block& reducer = getBody().front();
      int numOperandPairs = getNumOperands() / 2;
      for (int opId : llvm::seq<int>(0, numOperandPairs)) {
        p << "(";
        p.printRegionArgument(reducer.getArgument(opId));
        p << ", ";
        p.printRegionArgument(reducer.getArgument(opId + numOperandPairs));
        p << ") ";
      }
    }
    p << ' ';
    p.printRegion(getBody(), /*printEntryBlockArgs=*/false);
  }
}

ParseResult ReduceOp::parse(OpAsmParser& parser, OperationState& result) {
  llvm::SMLoc loc = parser.getCurrentLocation();
  Location currLocation = parser.getEncodedSourceLoc(loc);

  // Parse the operands of reduce-op, this is a list of pair under the form:
  //   (%arg0 init: %arg3), (%arg1 init: %arg4), (%arg2 init: %arg5)
  // Each input to reduce is paired with its init value, even though in memory
  // they are stored with the input first and the init values after.
  SmallVector<OpAsmParser::UnresolvedOperand, 2> operands;
  SmallVector<OpAsmParser::UnresolvedOperand, 2> initOperands;
  do {
    (void)parser.parseOptionalComma();
    if (parser.parseOptionalLParen()) break;
    OpAsmParser::UnresolvedOperand operand, initOperand;
    if (parser.parseOperand(operand) || parser.parseKeyword("init") ||
        parser.parseColon() || parser.parseOperand(initOperand) ||
        parser.parseRParen())
      return failure();
    operands.push_back(operand);
    initOperands.push_back(initOperand);
  } while (true);
  operands.append(initOperands);

  // Check if we are parsing the compact version of reduce-op:
  //  mhlo.reduce applies <inner-op> across dimensions = [...] : <func-type>
  // else parse the "region-based" variant.
  if (failed(parser.parseOptionalKeyword("applies"))) {
    // Parse the inner-op dimensions, reduce-op's function-type and
    // optional location.
    SmallVector<int64_t> dimensions;
    auto parseDim = [&]() -> ParseResult {
      if (parser.parseInteger(dimensions.emplace_back())) return failure();
      return success();
    };

    FunctionType reduceOpFntype;
    if (parser.parseKeyword("across") || parser.parseKeyword("dimensions") ||
        parser.parseEqual() ||
        parser.parseCommaSeparatedList(AsmParser::Delimiter::Square,
                                       parseDim) ||
        parser.parseOptionalAttrDict(result.attributes) ||
        parser.parseColon() || parser.parseType(reduceOpFntype) ||
        parser.parseKeyword("reducer"))
      return failure();
    OpBuilder builder(parser.getBuilder().getContext());
    result.addAttribute("dimensions", builder.getI64TensorAttr(dimensions));

    // Parse the "reducer" region now.
    SmallVector<OpAsmParser::UnresolvedOperand, 2> reducerOperands;
    SmallVector<OpAsmParser::UnresolvedOperand, 2> reducerInitOperands;
    SmallVector<Type, 2> reducerTypes;
    SmallVector<Type, 2> reducerInitTypes;
    SmallVector<Optional<Location>, 2> reducerLocs;
    SmallVector<Optional<Location>, 2> reducerInitLocs;
    auto parseBlockOperand =
        [&](SmallVectorImpl<OpAsmParser::UnresolvedOperand>& operands,
            SmallVectorImpl<Type>& types,
            SmallVectorImpl<Optional<Location>>& locs) -> ParseResult {
      OpAsmParser::UnresolvedOperand operand;
      Type type;
      Optional<Location> loc;
      if (parser.parseOperand(operand, /*allowResultNumber=*/false) ||
          parser.parseColon() || parser.parseType(type) ||
          parser.parseOptionalLocationSpecifier(loc))
        return failure();
      operands.push_back(operand);
      types.push_back(type);
      locs.push_back(loc);
      return success();
    };
    do {
      if (failed(parser.parseOptionalLParen())) break;
      if (parseBlockOperand(reducerOperands, reducerTypes, reducerLocs) ||
          parser.parseComma() ||
          parseBlockOperand(reducerInitOperands, reducerInitTypes,
                            reducerInitLocs) ||
          parser.parseRParen())
        return failure();
    } while (true);
    reducerOperands.append(reducerInitOperands);
    reducerTypes.append(reducerInitTypes);
    reducerLocs.append(reducerInitLocs);
    result.addTypes(reduceOpFntype.getResults());
    SmallVector<OpAsmParser::Argument> reducerArgs;
    createArgs(reducerOperands, reducerTypes, reducerArgs);

    // Derive the SSA-values for reduce-op's operands and parse the region, and
    // the optional trailing location.
    Optional<Location> trailingLoc;
    if (parser.resolveOperands(operands, reduceOpFntype.getInputs(), loc,
                               result.operands) ||
        parser.parseRegion(*result.addRegion(), reducerArgs))
      return failure();
    // Set the individual block arguments.
    for (auto argAndLoc :
         llvm::zip(result.regions.front()->front().getArguments(), reducerLocs))
      if (std::get<1>(argAndLoc))
        std::get<0>(argAndLoc).setLoc(std::get<1>(argAndLoc).value());
    result.location = trailingLoc.value_or(currLocation);
    return success();
  }

  // Parse the inner-op name and check if the contract on inner-op
  // mentioned in "isEligibleForCompactPrint::E2" for pretty-priting is met.
  FailureOr<OperationName> innerOpNameInfo = parser.parseCustomOperationName();
  if (failed(innerOpNameInfo)) return failure();

  StringRef innerOpName = innerOpNameInfo->getStringRef();
  Dialect* innerOpDialect = innerOpNameInfo->getDialect();
  if (!innerOpDialect || !innerOpDialect->getNamespace().equals("mhlo") ||
      !innerOpNameInfo->hasTrait<mlir::OpTrait::NOperands<2>::Impl>() ||
      !innerOpNameInfo->hasTrait<mlir::OpTrait::OneResult>() ||
      !innerOpNameInfo->hasTrait<mlir::OpTrait::IsCommutative>() ||
      !innerOpNameInfo->hasTrait<mlir::OpTrait::ZeroRegions>()) {
    parser.emitError(loc,
                     "expected the inner-op to be a commutative binary-op from "
                     "mhlo dialect, zero region, producing single result");
    return failure();
  }

  // Parse the inner-op dimensions, reduce-op's function-type and
  // optional location.
  SmallVector<int64_t> dimensions;
  auto parseDim = [&]() -> ParseResult {
    if (parser.parseInteger(dimensions.emplace_back())) return failure();
    return success();
  };

  Optional<Location> explicitLoc;
  FunctionType reduceOpFntype;
  if (parser.parseKeyword("across") || parser.parseKeyword("dimensions") ||
      parser.parseEqual() ||
      parser.parseCommaSeparatedList(AsmParser::Delimiter::Square, parseDim) ||
      parser.parseColon() || parser.parseType(reduceOpFntype) ||
      parser.parseOptionalLocationSpecifier(explicitLoc))
    return failure();

  if (!reduceOpFntype || reduceOpFntype.getInputs().empty()) {
    if (!reduceOpFntype) return parser.emitError(loc, "expected function type");
    return parser.emitError(loc,
                            "input types missing in reduce-op function type");
  }

  // If location of reduce-op is explicitly provided, then use it; Else use
  // the parser's current location.
  Location reduceOpLoc = explicitLoc.value_or(currLocation);

  // Derive the SSA-values for reduce-op's operands.
  if (parser.resolveOperands(operands, reduceOpFntype.getInputs(), loc,
                             result.operands))
    return failure();

  // Derive the type of inner-op from that of reduce-op's input operand.
  auto innerOpType = RankedTensorType::get(
      /*shape=*/{}, getElementTypeOrSelf(reduceOpFntype.getInput(0)));

  // Add a region for reduce-op.
  Region& region = *result.addRegion();

  // Create a basic-block inside reduce-op's region.
  Block& block = region.emplaceBlock();
  auto lhs = block.addArgument(innerOpType, reduceOpLoc);
  auto rhs = block.addArgument(innerOpType, reduceOpLoc);

  // Create and insert an "inner-op" operation in the block.
  OpBuilder builder(parser.getBuilder().getContext());
  builder.setInsertionPointToStart(&block);

  OperationState innerOpState(reduceOpLoc, innerOpName);
  innerOpState.operands.push_back(lhs);
  innerOpState.operands.push_back(rhs);
  innerOpState.addTypes(innerOpType);

  Operation* innerOp = builder.create(innerOpState);

  // Insert a return statement in the block returning the inner-op's result.
  builder.create<ReturnOp>(innerOp->getLoc(), innerOp->getResults());

  // Populate the reduce-op operation-state with result-type, location, and
  // dimension attribute.
  result.addTypes(reduceOpFntype.getResults());
  result.location = innerOp->getLoc();
  result.addAttribute("dimensions", builder.getI64TensorAttr(dimensions));

  return success();
}

LogicalResult ReduceOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  ReduceOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferReduceOp(location, adaptor.getInputs().getTypes(),
                            adaptor.getInitValues().getTypes(),
                            adaptor.getDimensions(), inferredReturnShapes);
}

LogicalResult ReduceOp::verify() {
  return hlo::verifyReduceOp(getLoc(), getInputs(), getInitValues(),
                             getDimensions(), getBody());
}

// Enable constant folding to occur within the region of the ReduceOp
// by replacing block argument uses with constants if:
//  1. All the ReduceOp operands are splat constants.
//  2. The ReduceOp region consists of a single logical AND or logical OR.
// The pattern leverages the idempotent property of the AND and OR operators
// to determine the value of a reduction on splat constants. Other boolean
// operators do not have this property, and need separate patterns to resolve
// reductions of their splat constants.
struct LowerBoolSplatConstantsIntoRegion : public OpRewritePattern<ReduceOp> {
  using OpRewritePattern<ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ReduceOp op,
                                PatternRewriter& rewriter) const override {
    mlir::Block& bb = op.getBody().front();

    // Ensure only a compute op and return op exist and the
    // compute op is an AND or OR op.
    if (bb.getOperations().size() != 2) return failure();
    if (!mlir::isa<mhlo::AndOp, mhlo::OrOp>(bb.front())) return failure();

    // Ensure all operands are splat constants.
    SmallVector<DenseElementsAttr, 4> bargCstAttrs;
    for (auto inpAndBarg : llvm::zip(op.getOperands(), bb.getArguments())) {
      Value inp = std::get<0>(inpAndBarg);
      BlockArgument barg = std::get<1>(inpAndBarg);
      ConstantOp cst = inp.getDefiningOp<ConstantOp>();
      if (!cst) return failure();

      auto cstAttr = cst.getValue().dyn_cast_or_null<DenseElementsAttr>();
      if (!cstAttr.isSplat()) {
        return rewriter.notifyMatchFailure(op, "Must be splat constant.");
      }

      auto bargShapedType = barg.getType().dyn_cast<ShapedType>();
      if (!bargShapedType) return failure();

      auto bargCstAttr = DenseElementsAttr::get(
          bargShapedType, cstAttr.getSplatValue<mlir::Attribute>());
      bargCstAttrs.push_back(bargCstAttr);
    }

    // Create new splat constants to replace block arguments.
    for (BlockArgument barg : bb.getArguments()) {
      int argIdx = barg.getArgNumber();
      mhlo::ConstantOp newCst = rewriter.create<mhlo::ConstantOp>(
          bb.front().getLoc(), barg.getType(), bargCstAttrs[argIdx]);
      barg.replaceAllUsesWith(newCst);
    }
    return success();
  }
};

static LogicalResult convertEmptyReduces(ReduceOp op,
                                         PatternRewriter& rewriter) {
  // We require all reduce shapes to be the same, up to the element types, so we
  // can just the first operand and the first result as a representative.
  RankedTensorType t =
      dyn_cast<RankedTensorType>(op.getInputs().getType().front());
  if (!t)
    return rewriter.notifyMatchFailure(op.getLoc(),
                                       "unranked input unsupported");
  bool zeroExtent = any_of(t.getShape(), [](int64_t d) { return d == 0; });
  if (zeroExtent) {
    auto empty = rewriter.getI64TensorAttr({});
    if (t.hasStaticShape()) {
      for (auto [init, out] : llvm::zip(op.getInitValues(), op.getResults())) {
        out.replaceAllUsesWith(rewriter.create<BroadcastInDimOp>(
            op.getLoc(), out.getType(), init, empty));
      }
      return success();
    }

    SmallVector<Value, 4> shapes;
    if (failed(op.reifyReturnTypeShapes(rewriter, op.getOperands(), shapes)))
      return failure();
    for (auto [init, shape, out] :
         llvm::zip(op.getInitValues(), shapes, op.getResults())) {
      out.replaceAllUsesWith(rewriter.create<DynamicBroadcastInDimOp>(
          op.getLoc(), out.getType(), init, shape, empty));
    }
    return success();
  }
  return rewriter.notifyMatchFailure(op.getLoc(), "non-empty input");
}

void ReduceOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                           MLIRContext* context) {
  results.add<LowerBoolSplatConstantsIntoRegion>(context);
  results.add(convertEmptyReduces);
}

LogicalResult ReduceOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  ReduceOp::Adaptor adaptor(operands);
  auto inputs = adaptor.getInputs();

  auto operandType = inputs[0].getType().dyn_cast<RankedTensorType>();
  // Not support unranked type a.t.m.
  if (!operandType) return failure();

  Location loc = this->getLoc();
  SmallVector<Value, 4> shapeValues;
  SmallVector<int64_t, 4> dimensions(
      this->getDimensions().getValues<int64_t>());
  shapeValues.reserve(operandType.getRank());
  Type shapeScalarType = builder.getIndexType();
  auto toShapeScalarType = [&](Value v) {
    return maybeCastTo(builder, loc, v, shapeScalarType);
  };

  for (const auto& element : llvm::enumerate(operandType.getShape())) {
    int64_t idx = element.index();
    auto* it = std::find(dimensions.begin(), dimensions.end(), idx);
    if (it != dimensions.end()) {
      continue;
    }
    Value valueDim = toShapeScalarType(
        builder.create<tensor::DimOp>(loc, inputs[0], element.index()));
    shapeValues.push_back(valueDim);
  }

  Value outputShape = builder.create<tensor::FromElementsOp>(
      loc,
      RankedTensorType::get({static_cast<int64_t>(shapeValues.size())},
                            shapeScalarType),
      shapeValues);
  for (size_t i = 0; i < inputs.size(); ++i) {
    reifiedReturnShapes.push_back(outputShape);
  }

  return success();
}

//===----------------------------------------------------------------------===//
// OptimizationBarrierOp
//===----------------------------------------------------------------------===//
LogicalResult OptimizationBarrierOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  OptimizationBarrierOp::Adaptor adaptor(operands, attributes);
  return hlo::inferOptimizationBarrierOp(location, adaptor.getOperand(),
                                         inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// ReverseOp
//===----------------------------------------------------------------------===//
LogicalResult ReverseOp::verify() {
  return hlo::verifyReverseOp(getLoc(), getOperand(), getDimensions());
}

//===----------------------------------------------------------------------===//
// RngBitGeneratorOp
//===----------------------------------------------------------------------===//

// Verify that input state has the same shape as output shape
LogicalResult RngBitGeneratorOp::verify() {
  return hlo::verifyRngBitGeneratorOp(getLoc(), getInitialState(),
                                      getOutputState());
}

//===----------------------------------------------------------------------===//
// RngOp
//===----------------------------------------------------------------------===//

LogicalResult RngOp::inferReturnTypeComponents(
    MLIRContext* context, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  RngOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferRngOp(
      location, adaptor.getA(), adaptor.getB(), adaptor.getShape(),
      adaptor.getRngDistribution() == RngDistribution::UNIFORM,
      inferredReturnShapes);
}

LogicalResult RngOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  RngOp::Adaptor adaptor(operands);
  reifiedReturnShapes.push_back(
      castToIndexTensor(builder, getLoc(), adaptor.getShape()));
  return success();
}

//===----------------------------------------------------------------------===//
// XlaRngGetAndUpdateStateOp
//===----------------------------------------------------------------------===//

LogicalResult XlaRngGetAndUpdateStateOp::verify() {
  auto resultTy = getType().cast<RankedTensorType>();
  if (!resultTy) return emitOpError() << "Output is not ranked.";
  if (!resultTy.hasStaticShape())
    return emitOpError() << "Output is not statically shaped.";
  auto rank = resultTy.getRank();
  if (rank != 1)
    return emitOpError() << "Output is of rank " << rank << " instead of 1";
  auto extent = resultTy.getDimSize(0);
  if (extent != 2)
    return emitOpError() << "Output size is " << extent << " instead of 2";

  return success();
}

LogicalResult XlaRngGetAndUpdateStateOp::inferReturnTypes(
    MLIRContext* ctx, Optional<Location>, ValueRange, DictionaryAttr,
    RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  inferredReturnTypes.push_back(mlir::RankedTensorType::get(
      {2}, mlir::IntegerType::get(ctx, 64, IntegerType::Unsigned)));
  return success();
}

//===----------------------------------------------------------------------===//
// SelectOp
//===----------------------------------------------------------------------===//

OpFoldResult SelectOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  if (getOnTrue() == getOnFalse()) {
    return getOnTrue();
  }

  auto predicate = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!predicate) {
    return {};
  }

  auto predicateTy = predicate.getType().cast<ShapedType>();
  if (!predicateTy.getElementType().isInteger(1)) {
    return {};
  }

  if (predicate.isSplat()) {
    return predicate.getSplatValue<APInt>().getBoolValue() ? getOnTrue()
                                                           : getOnFalse();
  }

  return {};
}

void SelectOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                           MLIRContext* context) {
  results.add<FusePredNegIntoSelect, FuseBroadcastedPredNegIntoSelect>(context);
}

// Makes it such that a SelectOp that is a non-root operation in a DRR infers
// the return type based on operand type.
LogicalResult SelectOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  SelectOp::Adaptor op(operands, attributes);
  return hlo::inferSelectOp(location, op.getPred(), op.getOnTrue(),
                            op.getOnFalse(), inferredReturnShapes);
}

LogicalResult SelectOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  // For `hlo.select`, the first operand may be a scalar.
  return hlo::deriveShapeFromOperand(&builder, getOperation(), operands[1],
                                     &reifiedReturnShapes);
}

//===----------------------------------------------------------------------===//
// SetDimensionSizeOp
//===----------------------------------------------------------------------===//

LogicalResult SetDimensionSizeOp::verify() {
  if (auto size = this->getSize().getType().dyn_cast<RankedTensorType>()) {
    if (size.getRank() != 0)
      return emitOpError() << "size operand should be of rank-0";
  }

  return verifyDimAttr(*this);
}

OpFoldResult SetDimensionSizeOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  DenseElementsAttr input = operands[0].dyn_cast_or_null<DenseElementsAttr>();
  if (input) return input;

  DenseElementsAttr size = operands[1].dyn_cast_or_null<DenseElementsAttr>();
  if (!size || !size.isSplat()) return {};

  auto ty = getType().dyn_cast<RankedTensorType>();
  if (!ty) return {};

  int64_t dimSize = ty.getDimSize(getDimension());
  if (dimSize == size.getSplatValue<IntegerAttr>().getInt())
    return getOperand();
  return {};
}

// TODO(b/238903565): Switch to inferReturnTypeComponents after adding support
// for the encoding upstream.
LogicalResult SetDimensionSizeOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  Location loc = location.value_or(UnknownLoc::get(context));

  SetDimensionSizeOp::Adaptor adaptor(operands, attributes, regions);
  if (failed(adaptor.verify(loc))) return failure();

  auto inputType = adaptor.getOperand().getType().dyn_cast<RankedTensorType>();
  if (!inputType) {
    inferredReturnTypes.push_back(adaptor.getOperand().getType());
    return success();
  }

  int64_t dim = adaptor.getDimension();
  int64_t rank = inputType.getRank();
  if (dim < 0 || dim >= rank) {
    return mlir::emitError(loc) << "expects dimension to be in range [0, "
                                << rank << "); got: [" << dim << "].";
  }

  auto shape = llvm::to_vector<4>(inputType.getShape());
  llvm::SmallVector<int64_t, 4> bounds(rank, ShapedType::kDynamic);
  if (auto encoding =
          inputType.getEncoding().dyn_cast_or_null<TypeExtensionsAttr>())
    bounds = llvm::to_vector<4>(encoding.getBounds());

  if (shape[dim] != ShapedType::kDynamic) bounds[dim] = shape[dim];
  shape[dim] = ShapedType::kDynamic;

  DenseIntElementsAttr sizeAttr;
  if (matchPattern(adaptor.getSize(), m_Constant(&sizeAttr))) {
    int64_t splat =
        sizeAttr.getSplatValue<IntegerAttr>().getValue().getSExtValue();
    if (splat == bounds[dim]) {
      shape[dim] = splat;
      bounds[dim] = ShapedType::kDynamic;
    }
  }

  auto extensions = TypeExtensionsAttr::get(context, bounds);
  auto resultType =
      llvm::all_of(bounds, [](int64_t v) { return v == ShapedType::kDynamic; })
          ? RankedTensorType::get(shape, inputType.getElementType())
          : RankedTensorType::get(shape, inputType.getElementType(),
                                  extensions);
  inferredReturnTypes.push_back(resultType);
  return success();
}

//===----------------------------------------------------------------------===//
// PadOp
//===----------------------------------------------------------------------===//

LogicalResult PadOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  PadOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferPadOp(location, adaptor.getOperand(),
                         adaptor.getPaddingValue(), adaptor.getEdgePaddingLow(),
                         adaptor.getEdgePaddingHigh(),
                         adaptor.getInteriorPadding(), inferredReturnTypes);
}

template <typename T>
OpFoldResult padOpFoldHelper(DenseElementsAttr input, DenseElementsAttr padding,
                             RankedTensorType returnType,
                             DenseIntElementsAttr edgePaddingLow,
                             DenseIntElementsAttr /*edgePaddingHigh*/,
                             DenseIntElementsAttr interiorPadding) {
  // Prevent folding if the result is too large.
  if (returnType.getNumElements() > kFoldOpEltLimit) return {};

  // Fill the full result tensor with the padding value.
  llvm::SmallVector<T, 4> result(returnType.getNumElements(),
                                 padding.getValues<T>()[0]);

  auto nextIndex = [](llvm::SmallVector<uint64_t, 8>& index,
                      llvm::ArrayRef<int64_t> shape) {
    for (int64_t i = index.size() - 1; i >= 0; --i) {
      ++index[i];
      if (static_cast<int64_t>(index[i]) < shape[i]) return;
      index[i] = 0;
    }
  };

  // Iterate over all elements of the input tensor and copy it to the correct
  // location in the output tensor.
  llvm::SmallVector<uint64_t, 8> index(input.getType().getRank(), 0);
  uint64_t numElements = input.getNumElements();
  for (uint64_t operandIdx = 0; operandIdx < numElements; operandIdx++) {
    uint64_t resultIdx = 0;
    uint64_t idxMultiplyer = 1;
    for (int64_t i = index.size() - 1; i >= 0; --i) {
      resultIdx += (edgePaddingLow.getValues<int64_t>()[i] +
                    index[i] * (interiorPadding.getValues<int64_t>()[i] + 1)) *
                   idxMultiplyer;
      idxMultiplyer *= returnType.getDimSize(i);
    }
    result[resultIdx] = input.getValues<T>()[index];
    nextIndex(index, input.getType().getShape());
  }
  return DenseElementsAttr::get(returnType, result);
}

OpFoldResult PadOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  // If all padding is zero then it is an identity pad.
  auto isZero = [](const APInt& i) { return i == 0; };
  if (llvm::all_of(getEdgePaddingLow().getValues<APInt>(), isZero) &&
      llvm::all_of(getEdgePaddingHigh().getValues<APInt>(), isZero) &&
      llvm::all_of(getInteriorPadding().getValues<APInt>(), isZero))
    return getOperand();

  // If any padding is negative then it isn't supported by the folder (yet).
  auto isNegative = [](const APInt& i) { return i.slt(0); };
  if (llvm::any_of(getEdgePaddingLow().getValues<APInt>(), isNegative) ||
      llvm::any_of(getEdgePaddingHigh().getValues<APInt>(), isNegative) ||
      llvm::any_of(getInteriorPadding().getValues<APInt>(), isNegative))
    return {};

  DenseElementsAttr input = operands[0].dyn_cast_or_null<DenseElementsAttr>();
  DenseElementsAttr padding = operands[1].dyn_cast_or_null<DenseElementsAttr>();
  RankedTensorType returnType = getType().dyn_cast_or_null<RankedTensorType>();
  if (!input || !input.getType().hasRank() || !padding || !returnType ||
      !returnType.hasStaticShape())
    return {};

  if (returnType.getElementType().isa<IntegerType>())
    return padOpFoldHelper<APInt>(input, padding, returnType,
                                  getEdgePaddingLow(), getEdgePaddingHigh(),
                                  getInteriorPadding());
  if (returnType.getElementType().isa<FloatType>())
    return padOpFoldHelper<APFloat>(input, padding, returnType,
                                    getEdgePaddingLow(), getEdgePaddingHigh(),
                                    getInteriorPadding());
  if (ComplexType complex =
          returnType.getElementType().dyn_cast_or_null<ComplexType>()) {
    // TODO(atondwal): Allow int types in HLO_complex
    if (complex.getElementType().isa<FloatType>())
      return padOpFoldHelper<std::complex<APFloat>>(
          input, padding, returnType, getEdgePaddingLow(), getEdgePaddingHigh(),
          getInteriorPadding());
  }
  return {};
}

LogicalResult PadOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  PadOp::Adaptor adaptor(operands, this->getOperation()->getAttrDictionary());
  auto loc = this->getLoc();
  Value operand = adaptor.getOperand();
  auto operandTy = operand.getType().cast<RankedTensorType>();

  llvm::SmallVector<int32_t> padHigh;
  llvm::SmallVector<int32_t> padLow;
  llvm::SmallVector<int32_t> padInterior;

  auto padHighAttr = adaptor.getEdgePaddingHigh();
  auto padLowAttr = adaptor.getEdgePaddingLow();
  auto padInteriorAttr = adaptor.getInteriorPadding();

  padHigh.reserve(padHighAttr.getNumElements());
  padLow.reserve(padLowAttr.getNumElements());
  padInterior.reserve(padInteriorAttr.getNumElements());

  for (const APInt& val : padHighAttr.getValues<APInt>())
    padHigh.push_back(val.getSExtValue());

  for (const APInt& val : padLowAttr.getValues<APInt>())
    padLow.push_back(val.getSExtValue());

  for (const APInt& val : padInteriorAttr.getValues<APInt>())
    padInterior.push_back(val.getSExtValue());

  Value one = builder.create<arith::ConstantIndexOp>(loc, 1).getResult();
  Value zero = builder.create<arith::ConstantIndexOp>(loc, 0).getResult();

  llvm::SmallVector<Value> dimensions;
  dimensions.reserve(operandTy.getRank());
  for (int i = 0, s = operandTy.getRank(); i < s; ++i) {
    Value padEdge =
        builder.create<arith::ConstantIndexOp>(loc, padHigh[i] + padLow[i]);

    // First we grab the initial interior size.
    Value dim = builder.create<tensor::DimOp>(loc, operand, i).getResult();

    // Compute the interior of the tensor and determine padding size.
    if (padInterior[i] > 0) {
      Value padInter =
          builder.create<arith::ConstantIndexOp>(loc, padInterior[i])
              .getResult();
      Value interior = builder.create<arith::SubIOp>(loc, dim, one).getResult();
      interior = builder.create<arith::MaxSIOp>(loc, interior, zero);
      interior = builder.create<arith::MulIOp>(loc, interior, padInter);
      dim = builder.create<arith::AddIOp>(loc, dim, interior).getResult();
    }

    // Then we add the padding on the edge of the tensor.
    dim = builder.create<arith::AddIOp>(loc, dim, padEdge).getResult();
    dimensions.push_back(dim);
  }

  Value dimensionTensor =
      builder.create<tensor::FromElementsOp>(loc, dimensions).getResult();
  reifiedReturnShapes.push_back(dimensionTensor);
  return success();
}

// If the input tensor has a dimension of length-0, the input tensor is
// irrelevant. Instead we can broadcast the pad value to the output size rather
// than pad the input tensor.
struct PadEmptyTensor : public OpRewritePattern<PadOp> {
  using OpRewritePattern<PadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(PadOp op,
                                PatternRewriter& rewriter) const override {
    auto operand = op.getOperand();
    auto padVal = op.getPaddingValue();

    auto operandTy = operand.getType().cast<RankedTensorType>();
    auto resultTy = op.getType().cast<RankedTensorType>();

    if (llvm::all_of(operandTy.getShape(), [](int64_t d) { return d != 0; })) {
      return failure();
    }

    if (resultTy.hasStaticShape()) {
      auto dimsType = RankedTensorType::get({0}, rewriter.getIntegerType(64));
      auto dims =
          DenseIntElementsAttr::get(dimsType, SmallVector<int64_t, 1>{});
      rewriter.replaceOpWithNewOp<mhlo::BroadcastInDimOp>(op, resultTy, padVal,
                                                          dims);
      return success();
    }

    llvm::SmallVector<Value> reifiedShapes;
    if (failed(op.reifyReturnTypeShapes(rewriter, op.getOperands(),
                                        reifiedShapes)))
      return failure();

    auto dimsType = RankedTensorType::get({0}, rewriter.getIntegerType(64));
    auto broadcastDims =
        DenseIntElementsAttr::get(dimsType, SmallVector<int64_t, 1>{});
    rewriter.replaceOpWithNewOp<mhlo::DynamicBroadcastInDimOp>(
        op, op.getType(), padVal, reifiedShapes.front(), broadcastDims);

    return failure();
  }
};

void PadOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                        MLIRContext* context) {
  results.add<PadEmptyTensor>(context);
}

//===----------------------------------------------------------------------===//
// DynamicPadOp
//===----------------------------------------------------------------------===//

// If the input tensor has a dimension of length-0, the input tensor is
// irrelevant. Instead we can broadcast the pad value to the output size rather
// than pad the input tensor.
struct DynamicPadEmptyTensor : public OpRewritePattern<DynamicPadOp> {
  using OpRewritePattern<DynamicPadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(DynamicPadOp op,
                                PatternRewriter& rewriter) const override {
    // auto loc = op.getLoc();
    auto operand = op.getOperand();
    auto padVal = op.getPaddingValue();

    auto operandTy = operand.getType().cast<RankedTensorType>();

    if (llvm::all_of(operandTy.getShape(), [](int64_t d) { return d != 0; })) {
      return failure();
    }

    llvm::SmallVector<Value> reifiedShapes;
    if (failed(op.reifyReturnTypeShapes(rewriter, op->getOperands(),
                                        reifiedShapes)))
      return failure();

    auto dimsType = RankedTensorType::get({0}, rewriter.getIntegerType(64));
    auto broadcastDims =
        DenseIntElementsAttr::get(dimsType, SmallVector<int64_t, 1>{});
    rewriter.replaceOpWithNewOp<mhlo::DynamicBroadcastInDimOp>(
        op, op.getType(), padVal, reifiedShapes.front(), broadcastDims);

    return failure();
  }
};

void DynamicPadOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                               MLIRContext* context) {
  results.add<DPadToPad, DynamicPadEmptyTensor>(context);
}

LogicalResult DynamicPadOp::verify() {
  return hlo::verifyDynamicPadOp(getLoc(), getOperand(), getPaddingValue(),
                                 getEdgePaddingLow(), getEdgePaddingHigh(),
                                 getInteriorPadding(), getResult());
}

LogicalResult DynamicPadOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  DynamicPadOp::Adaptor adaptor(operands);
  Value operand = adaptor.getOperand();
  Value edgePaddingLow = adaptor.getEdgePaddingLow();
  Value edgePaddingHigh = adaptor.getEdgePaddingHigh();
  Value interiorPadding = adaptor.getInteriorPadding();

  auto operandType = operand.getType().dyn_cast<RankedTensorType>();
  // Not support unranked pad a.t.m.
  if (!operandType) return failure();

  auto loc = this->getLoc();
  SmallVector<Value, 4> shapeValues;
  shapeValues.reserve(operandType.getRank());
  Type shapeScalarType =
      edgePaddingLow.getType().cast<ShapedType>().getElementType();

  auto toShapeScalarType = [&](Value v) {
    return maybeCastTo(builder, loc, v, shapeScalarType);
  };

  Value zero =
      toShapeScalarType(builder.create<arith::ConstantIndexOp>(loc, 0));
  Value one = toShapeScalarType(builder.create<arith::ConstantIndexOp>(loc, 1));

  for (int idx : llvm::seq<int>(0, operandType.getShape().size())) {
    Value valueDim =
        toShapeScalarType(builder.create<tensor::DimOp>(loc, operand, idx));
    Value offset = builder.create<arith::ConstantIndexOp>(loc, idx);
    Value valueLow =
        builder.create<tensor::ExtractOp>(loc, edgePaddingLow, offset);
    Value valueHigh =
        builder.create<tensor::ExtractOp>(loc, edgePaddingHigh, offset);
    Value valueInterior =
        builder.create<tensor::ExtractOp>(loc, interiorPadding, offset);
    // output_size = input_size + padding_low + padding_high + interior *
    // max(input_size - 1, 0)
    Value valueDimLessThanOne = builder.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::slt, valueDim, one);
    Value interiorSize = builder.create<arith::MulIOp>(
        loc, valueInterior,
        builder.create<mlir::arith::SelectOp>(
            loc, valueDimLessThanOne, zero,
            builder.create<arith::SubIOp>(loc, valueDim, one)));
    shapeValues.push_back(builder.create<arith::AddIOp>(
        loc,
        builder.create<arith::AddIOp>(
            loc, builder.create<arith::AddIOp>(loc, interiorSize, valueDim),
            valueLow),
        valueHigh));
  }

  reifiedReturnShapes.push_back(builder.create<tensor::FromElementsOp>(
      loc,
      RankedTensorType::get({static_cast<int64_t>(shapeValues.size())},
                            shapeScalarType),
      shapeValues));

  return success();
}

//===----------------------------------------------------------------------===//
// ReshapeOp
//===----------------------------------------------------------------------===//

LogicalResult ReshapeOp::verify() {
  return hlo::verifyReshapeOp(getLoc(), getOperand(), getResult());
}

OpFoldResult ReshapeOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  if (getOperand().getType() == getType()) {
    return getOperand();
  }

  if (auto prevOp = getOperand().getDefiningOp<ReshapeOp>()) {
    setOperand(prevOp.getOperand());
    return getResult();
  }

  if (auto elements = operands.front().dyn_cast_or_null<DenseElementsAttr>()) {
    return reshape(elements, getResult().getType().cast<ShapedType>());
  }

  return {};
}

void ReshapeOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                            MLIRContext* context) {
  results.add<IdentityBroadcastReshape, IdentityBroadcastInDimReshape,
              EliminateRedundantReshape, EliminateIdentityReshape>(context);
}

//===----------------------------------------------------------------------===//
// ReplicaId Op
//===----------------------------------------------------------------------===//

LogicalResult ReplicaIdOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange /*operands*/,
    DictionaryAttr, RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  return hlo::inferReplicaIdOp(context, location, inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// PartitionId Op
//===----------------------------------------------------------------------===//

LogicalResult PartitionIdOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange /*operands*/,
    DictionaryAttr, RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  return hlo::inferPartitionIdOp(context, location, inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// AddDependency Op
//===----------------------------------------------------------------------===//

LogicalResult AddDependencyOp::inferReturnTypes(
    MLIRContext* context, Optional<Location>, ValueRange operands,
    DictionaryAttr, RegionRange, SmallVectorImpl<Type>& inferredReturnTypes) {
  inferredReturnTypes.push_back(operands.getTypes()[0]);
  return success();
}

//===----------------------------------------------------------------------===//
// If Op
//===----------------------------------------------------------------------===//

LogicalResult IfOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  IfOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferIfOp(location, adaptor.getPred(), adaptor.getRegions(),
                        inferredReturnTypes);
}

static LogicalResult inlineIfConstantCondition(IfOp ifOp,
                                               PatternRewriter& rewriter) {
  DenseIntElementsAttr predAttr;
  if (!matchPattern(ifOp.getPred(), m_Constant(&predAttr))) return failure();

  if (predAttr.getSplatValue<BoolAttr>().getValue()) {
    replaceOpWithRegion(rewriter, ifOp, ifOp.getTrueBranch());
  } else {
    replaceOpWithRegion(rewriter, ifOp, ifOp.getFalseBranch());
  }
  return success();
}

void IfOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                       MLIRContext* context) {
  results.add(&inlineIfConstantCondition);
}

//===----------------------------------------------------------------------===//
// Case Op
//===----------------------------------------------------------------------===//

LogicalResult CaseOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  CaseOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferCaseOp(location, adaptor.getIndex(), adaptor.getRegions(),
                          inferredReturnTypes);
}

static LogicalResult inlineCaseConstantCondition(CaseOp caseOp,
                                                 PatternRewriter& rewriter) {
  DenseIntElementsAttr indexAttr;
  if (!matchPattern(caseOp.getIndex(), m_Constant(&indexAttr))) {
    return failure();
  }
  int64_t index =
      indexAttr.getSplatValue<IntegerAttr>().getValue().getSExtValue();
  // For an OOB index, the last branch is executed as the default branch:
  // https://www.tensorflow.org/xla/operation_semantics#conditional
  if (index < 0 || index >= caseOp.getNumRegions())
    index = caseOp.getNumRegions() - 1;

  Region& region = caseOp.getRegion(index);
  if (!llvm::hasSingleElement(region)) return failure();
  replaceOpWithRegion(rewriter, caseOp, region);
  return success();
}

void CaseOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                         MLIRContext* context) {
  results.add(&inlineCaseConstantCondition);
}

//===----------------------------------------------------------------------===//
// UnaryOps
//===----------------------------------------------------------------------===//

template <typename ValType>
struct AnyValue {
  bool operator()(const ValType&) { return true; }
};

template <typename ValType>
struct NonNegativeValue {
  bool operator()(const ValType& v) { return !v.isNegative(); }
};

template <typename ValType>
struct PositiveValue {
  bool operator()(const ValType& v) { return !v.isNegative() && !v.isZero(); }
};

static const APFloat& addSign(const APFloat& v, Type) { return v; }
static APSInt addSign(const APInt& v, Type t) {
  // Add signedness information to the value, treating signless as signed,
  // unless it's i1.
  return APSInt(v, t.isUnsignedInteger() || t.isSignlessInteger(1));
}

template <typename Op, typename ElementType, typename ValType, typename Convert,
          typename Validate = AnyValue<ValType>>
static Attribute UnaryFolder(Op* op, ArrayRef<Attribute> attrs) {
  if (!attrs[0]) return {};

  DenseElementsAttr val = attrs[0].dyn_cast<DenseElementsAttr>();
  if (!val) return {};

  ShapedType type = op->getType().template cast<ShapedType>();
  if (!type.hasStaticShape()) {
    return {};
  }

  Type etype = type.getElementType();

  // Evaluate for integer values.
  if (!etype.isa<ElementType>()) {
    return {};
  }

  // Prevent folding if the result is too large.
  if (val.getNumElements() > kFoldOpEltLimit) return {};

  SmallVector<ValType, 6> values;
  values.reserve(val.getNumElements());
  for (const auto v : val.getValues<ValType>()) {
    if (!Validate()(v)) return {};
    Optional<ValType> r = Convert()(addSign(v, type));
    if (!r) return {};
    values.push_back(r.value());
  }

  return DenseElementsAttr::get(type, values);
}

struct Round {
  Optional<APFloat> operator()(const APFloat& f) {
    APFloat r = f;
    r.roundToIntegral(llvm::RoundingMode::NearestTiesToAway);
    return r;
  }
};

struct RoundNearestEven {
  Optional<APFloat> operator()(const APFloat& f) {
    APFloat r = f;
    r.roundToIntegral(llvm::RoundingMode::NearestTiesToEven);
    return r;
  }
};

template <typename FloatOrInt>
struct Sign {
  APFloat compute(const APFloat& f) {
    if (f.isZero() || f.isNaN()) return f;
    double value = f.isNegative() ? -1.0 : 1.0;
    APFloat val(value);
    bool unused;
    val.convert(f.getSemantics(), APFloat::rmNearestTiesToEven, &unused);
    return val;
  }

  APInt compute(const APInt& i) {
    APInt r = i;
    if (r == 0) return r;
    if (r.isNegative()) {
      return APInt(r.getBitWidth(), -1, /*isSigned=*/true);
    }
    return APInt(r.getBitWidth(), 1, /*isSigned=*/true);
  }

  Optional<FloatOrInt> operator()(const FloatOrInt& fi) { return compute(fi); }
};

template <typename FloatOrInt>
struct Abs {
  APFloat compute(const APFloat& f) { return abs(f); }

  APInt compute(const APInt& i) { return i.abs(); }

  Optional<FloatOrInt> operator()(const FloatOrInt& fi) { return compute(fi); }
};

double rsqrt(double d) { return 1.0 / std::sqrt(d); }

double logistic(double d) { return 1.0 / (1.0 + std::exp(-d)); }

// NOLINTBEGIN(bugprone-macro-parentheses)
#define UNARY_FOLDER(Op, Func)                                                \
  OpFoldResult Op::fold(FoldAdaptor adaptor) {                                \
    auto attrs = adaptor.getOperands();                                       \
    /* AbsOp could take complex but return float */                           \
    if (getElementTypeOrSelf(getOperation()->getOperand(0).getType()) !=      \
        getElementTypeOrSelf(getType())) {                                    \
      return {};                                                              \
    }                                                                         \
    if (getElementTypeOrSelf(getType()).isa<FloatType>())                     \
      return UnaryFolder<Op, FloatType, APFloat, Func<APFloat>>(this, attrs); \
    if (getElementTypeOrSelf(getType()).isa<IntegerType>())                   \
      return UnaryFolder<Op, IntegerType, APInt, Func<APInt>>(this, attrs);   \
    return {};                                                                \
  }

#define UNARY_FOLDER_INT(Op, Func)                                          \
  OpFoldResult Op::fold(FoldAdaptor adaptor) {                              \
    auto attrs = adaptor.getOperands();                                     \
    if (getElementTypeOrSelf(getType()).isa<IntegerType>())                 \
      return UnaryFolder<Op, IntegerType, APInt, Func<APInt>>(this, attrs); \
    return {};                                                              \
  }

#define UNARY_FOLDER_FLOAT(Op, Func)                                 \
  OpFoldResult Op::fold(FoldAdaptor adaptor) {                       \
    auto attrs = adaptor.getOperands();                              \
    if (getElementTypeOrSelf(getType()).isa<FloatType>())            \
      return UnaryFolder<Op, FloatType, APFloat, Func>(this, attrs); \
    return {};                                                       \
  }

#define UNARY_FOLDER_UPCAST_TO_F64(Op, Func, Validate)               \
  struct Op##Folder {                                                \
    Optional<APFloat> operator()(const APFloat& input) {             \
      APFloat f = input;                                             \
      const llvm::fltSemantics& oldSemantics = f.getSemantics();     \
                                                                     \
      bool unusedLoseInfo;                                           \
      f.convert(APFloat::IEEEdouble(), APFloat::rmNearestTiesToEven, \
                &unusedLoseInfo);                                    \
                                                                     \
      APFloat result(Func(f.convertToDouble()));                     \
      result.convert(oldSemantics, APFloat::rmNearestTiesToEven,     \
                     &unusedLoseInfo);                               \
      return result;                                                 \
    }                                                                \
  };                                                                 \
  OpFoldResult Op::fold(FoldAdaptor adaptor) {                       \
    auto attrs = adaptor.getOperands();                              \
    if (getElementTypeOrSelf(getType()).isa<FloatType>())            \
      return UnaryFolder<Op, FloatType, APFloat, Op##Folder,         \
                         Validate<APFloat>>(this, attrs);            \
    return {};                                                       \
  }
// NOLINTEND(bugprone-macro-parentheses)

UNARY_FOLDER(NegOp, std::negate)
UNARY_FOLDER(SignOp, Sign)
UNARY_FOLDER(AbsOp, Abs)
UNARY_FOLDER_INT(NotOp, std::bit_not)
UNARY_FOLDER_FLOAT(RoundNearestEvenOp, RoundNearestEven)
UNARY_FOLDER_FLOAT(RoundOp, Round)

UNARY_FOLDER_UPCAST_TO_F64(CosineOp, std::cos, AnyValue)
UNARY_FOLDER_UPCAST_TO_F64(ExpOp, std::exp, AnyValue)
UNARY_FOLDER_UPCAST_TO_F64(LogisticOp, logistic, AnyValue)
UNARY_FOLDER_UPCAST_TO_F64(LogOp, std::log, PositiveValue)
UNARY_FOLDER_UPCAST_TO_F64(RsqrtOp, rsqrt, PositiveValue)
UNARY_FOLDER_UPCAST_TO_F64(SineOp, std::sin, AnyValue)
UNARY_FOLDER_UPCAST_TO_F64(SqrtOp, std::sqrt, NonNegativeValue)
UNARY_FOLDER_UPCAST_TO_F64(TanOp, std::tan, AnyValue)
UNARY_FOLDER_UPCAST_TO_F64(TanhOp, std::tanh, AnyValue)

#undef UNARY_FOLDER
#undef UNARY_FOLDER_INT
#undef UNARY_FOLDER_FLOAT
#undef UNARY_FOLDER_UPCAST_TO_F64

//===----------------------------------------------------------------------===//
// BinaryOps
//===----------------------------------------------------------------------===//

template <typename Op, typename ElementType = Type, typename ValType,
          typename Convert>
static Attribute BinaryFolder(Op* op, ArrayRef<Attribute> attrs) {
  if (!attrs[0] || !attrs[1]) return {};

  DenseElementsAttr lhs = attrs[0].dyn_cast<DenseElementsAttr>();
  DenseElementsAttr rhs = attrs[1].dyn_cast<DenseElementsAttr>();
  if (!lhs || !rhs) return {};

  ShapedType type = op->getType().template cast<ShapedType>();
  if (!type.hasStaticShape()) {
    return {};
  }

  Type etype = type.getElementType();

  // Evaluate for integer values.
  if (!etype.isa<ElementType>()) {
    return {};
  }

  // Special case for folding splats no matter how large.
  // Only covers the case of both attrs being splats; operation-specific cases
  // like adding a zero or multiplying by one are handled elsewhere.
  SplatElementsAttr splatLhs = lhs.dyn_cast<SplatElementsAttr>();
  SplatElementsAttr splatRhs = rhs.dyn_cast<SplatElementsAttr>();
  if (splatLhs && splatRhs) {
    auto signedLhs = addSign(splatLhs.getSplatValue<ValType>(), etype);
    auto signedRhs = addSign(splatRhs.getSplatValue<ValType>(), etype);
    FailureOr<decltype(signedLhs)> result(Convert()(signedLhs, signedRhs));
    return succeeded(result) ? SplatElementsAttr::get(type, *result)
                             : Attribute();
  }

  // Prevent folding if the result is too large.
  if (lhs.getNumElements() > kFoldOpEltLimit) return {};

  SmallVector<ValType, 6> values;
  values.reserve(lhs.getNumElements());
  for (const auto zip :
       llvm::zip(lhs.getValues<ValType>(), rhs.getValues<ValType>())) {
    auto signedLhs = addSign(std::get<0>(zip), etype);
    auto signedRhs = addSign(std::get<1>(zip), etype);
    FailureOr<decltype(signedLhs)> result(Convert()(signedLhs, signedRhs));
    if (failed(result)) {
      return {};
    }
    values.push_back(std::move(*result));
  }

  return DenseElementsAttr::get(type, values);
}

template <typename T>
struct Divide : std::divides<T> {};

template <>
struct Divide<APSInt> {
  FailureOr<APSInt> operator()(const APSInt& a, const APSInt& b) const {
    if (b.isZero()) return failure();
    return a / b;
  }
};

template <typename T>
struct Remainder : std::modulus<T> {};

template <>
struct Remainder<APSInt> {
  FailureOr<APSInt> operator()(const APSInt& a, const APSInt& b) const {
    if (b.isZero()) return failure();
    return a % b;
  }
};

template <>
struct Remainder<APFloat> {
  APFloat operator()(const APFloat& a, const APFloat& b) const {
    APFloat result(a);
    // Using .mod instead of .remainder is important for behavior around signed
    // zeros
    result.mod(b);
    return result;
  }
};

template <typename T>
struct Max {
  T operator()(const T& a, const T& b) const { return std::max<T>(a, b); }
};

template <>
struct Max<APFloat> {
  // maximum on APFloat is required for NaN propagation logic
  APFloat operator()(const APFloat& a, const APFloat& b) const {
    return llvm::maximum(a, b);
  }
};

template <typename T>
struct Min {
  T operator()(const T& a, const T& b) const { return std::min<T>(a, b); }
};

template <>
struct Min<APFloat> {
  // minimum on APFloat is required for NaN propagation logic
  APFloat operator()(const APFloat& a, const APFloat& b) const {
    return llvm::minimum(a, b);
  }
};

#define BINARY_FOLDER_INTERNAL(Op, Func)                                     \
  if (getElementTypeOrSelf(getType()).isa<FloatType>())                      \
    return BinaryFolder<Op, FloatType, APFloat, Func<APFloat>>(this, attrs); \
  if (getElementTypeOrSelf(getType()).isa<IntegerType>())                    \
    return BinaryFolder<Op, IntegerType, APInt, Func<APSInt>>(this, attrs);  \
  return {};

#define BINARY_FOLDER(Op, Func)                \
  OpFoldResult Op::fold(FoldAdaptor adaptor) { \
    auto attrs = adaptor.getOperands();        \
    BINARY_FOLDER_INTERNAL(Op, Func)           \
  }

// Addition, subtraction and multiplication use the std:: versions of the ops.
// Due to the other ops behaving differently in signed vs unsigned integers,
// APInts need a special implementation. Currently, it replicates signed int
// op behavior.
BINARY_FOLDER(SubtractOp, std::minus)
BINARY_FOLDER(DivOp, Divide)
BINARY_FOLDER(RemOp, Remainder)
BINARY_FOLDER(MaxOp, Max)
BINARY_FOLDER(MinOp, Min)

OpFoldResult AddOp::fold(FoldAdaptor adaptor) {
  auto attrs = adaptor.getOperands();
  // Handle special case where one operand is 0:  x + 0 => x
  if (attrs[0] || attrs[1]) {
    SplatElementsAttr splatLhs = attrs[0].dyn_cast_or_null<SplatElementsAttr>();
    SplatElementsAttr splatRhs = attrs[1].dyn_cast_or_null<SplatElementsAttr>();
    if (isSplatZero(splatLhs))
      return splatRhs ? (OpFoldResult)splatRhs : getRhs();
    if (isSplatZero(splatRhs))
      return splatLhs ? (OpFoldResult)splatLhs : getLhs();
  }
  if (attrs[0] && attrs[1]) {
    BINARY_FOLDER_INTERNAL(AddOp, std::plus)
  }
  return {};
}

bool isSplatOne(SplatElementsAttr attr) {
  if (!attr) return false;
  if (attr.getElementType().isa<FloatType>()) {
    return attr.getSplatValue<APFloat>().convertToDouble() == 1.0;
  }
  if (attr.getElementType().isa<IntegerType>()) {
    return attr.getSplatValue<APInt>().getSExtValue() == 1;
  }
  return false;
}

OpFoldResult MulOp::fold(FoldAdaptor adaptor) {
  auto attrs = adaptor.getOperands();
  // Handle special case where one operand is 1: x * 1 => x
  if (attrs[0] || attrs[1]) {
    SplatElementsAttr splatLhs = attrs[0].dyn_cast_or_null<SplatElementsAttr>();
    SplatElementsAttr splatRhs = attrs[1].dyn_cast_or_null<SplatElementsAttr>();
    if (isSplatOne(splatLhs))
      return splatRhs ? (OpFoldResult)splatRhs : getRhs();
    if (isSplatOne(splatRhs))
      return splatLhs ? (OpFoldResult)splatLhs : getLhs();
  }
  if (attrs[0] && attrs[1]) {
    BINARY_FOLDER_INTERNAL(MulOp, std::multiplies);
  }
  return {};
}

//===----------------------------------------------------------------------===//
// Logical Ops
//===----------------------------------------------------------------------===//

OpFoldResult AndOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  if (getLhs() == getRhs()) return getLhs();

  auto lhsVal = operands[0].dyn_cast_or_null<DenseElementsAttr>();
  auto rhsVal = operands[1].dyn_cast_or_null<DenseElementsAttr>();

  if (lhsVal && lhsVal.isSplat()) {
    if (lhsVal.getSplatValue<IntegerAttr>().getValue().isAllOnes()) {
      return getRhs();
    }

    if (lhsVal.getSplatValue<IntegerAttr>().getValue().isZero()) {
      return lhsVal;
    }
  }

  if (rhsVal && rhsVal.isSplat()) {
    if (rhsVal.getSplatValue<IntegerAttr>().getValue().isAllOnes()) {
      return getLhs();
    }

    if (rhsVal.getSplatValue<IntegerAttr>().getValue().isZero()) {
      return rhsVal;
    }
  }

  if (!rhsVal || !lhsVal) return {};
  return BinaryFolder<AndOp, IntegerType, APInt, std::bit_and<APSInt>>(
      this, operands);
}

OpFoldResult OrOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  if (getLhs() == getRhs()) return getLhs();

  auto lhsVal = operands[0].dyn_cast_or_null<DenseElementsAttr>();
  auto rhsVal = operands[1].dyn_cast_or_null<DenseElementsAttr>();

  if (lhsVal && lhsVal.isSplat()) {
    if (lhsVal.getSplatValue<IntegerAttr>().getValue().isAllOnes()) {
      return lhsVal;
    }

    if (lhsVal.getSplatValue<IntegerAttr>().getValue().isZero()) {
      return getRhs();
    }
  }

  if (rhsVal && rhsVal.isSplat()) {
    if (rhsVal.getSplatValue<IntegerAttr>().getValue().isAllOnes()) {
      return rhsVal;
    }

    if (rhsVal.getSplatValue<IntegerAttr>().getValue().isZero()) {
      return getLhs();
    }
  }

  if (!rhsVal || !lhsVal) return {};
  return BinaryFolder<OrOp, IntegerType, APInt, std::bit_or<APSInt>>(this,
                                                                     operands);
}

OpFoldResult XorOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  // Fold x^x to 0. Attributes only support static shapes.
  auto rType = getType().cast<ShapedType>();
  if (getLhs() == getRhs() && rType.hasStaticShape()) {
    Builder builder(getContext());
    return builder.getZeroAttr(rType);
  }

  auto lhsVal = operands[0].dyn_cast_or_null<DenseElementsAttr>();
  auto rhsVal = operands[1].dyn_cast_or_null<DenseElementsAttr>();

  if (lhsVal && lhsVal.isSplat()) {
    if (lhsVal.getSplatValue<IntegerAttr>().getValue().isZero()) {
      return getRhs();
    }
  }

  if (rhsVal && rhsVal.isSplat()) {
    if (rhsVal.getSplatValue<IntegerAttr>().getValue().isZero()) {
      return getLhs();
    }
  }

  if (!rhsVal || !lhsVal) return {};
  return BinaryFolder<XorOp, IntegerType, APInt, std::bit_xor<APSInt>>(
      this, operands);
}

#undef BINARY_FOLDER_INTERNAL
#undef BINARY_FOLDER

//===----------------------------------------------------------------------===//
// ClampOp
//===----------------------------------------------------------------------===//

OpFoldResult ClampOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  auto operand = operands[1].dyn_cast_or_null<ElementsAttr>();
  auto min = operands[0].dyn_cast_or_null<ElementsAttr>();
  auto max = operands[2].dyn_cast_or_null<ElementsAttr>();
  if (!operand || !min || !max) {
    return {};
  }
  if (min.getType().getRank() == 0) {
    min = DenseElementsAttr::get(operand.getType(),
                                 min.getValues<Attribute>()[0]);
  }
  if (max.getType().getRank() == 0) {
    max = DenseElementsAttr::get(operand.getType(),
                                 max.getValues<Attribute>()[0]);
  }
  Attribute result = {};
  if (operand.getType().getElementType().isa<FloatType>()) {
    result = BinaryFolder<ClampOp, FloatType, APFloat, Max<APFloat>>(
        this, ArrayRef<Attribute>{min, operand});
    result = BinaryFolder<ClampOp, FloatType, APFloat, Min<APFloat>>(
        this, ArrayRef<Attribute>{max, result});
  } else if (operand.getType().getElementType().isa<IntegerType>()) {
    result = BinaryFolder<ClampOp, IntegerType, APInt, Max<APSInt>>(
        this, ArrayRef<Attribute>{min, operand});
    result = BinaryFolder<ClampOp, IntegerType, APInt, Min<APSInt>>(
        this, ArrayRef<Attribute>{max, result});
  }
  return result;
}

LogicalResult ClampOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  ClampOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferClampOp(location, adaptor.getMin(), adaptor.getOperand(),
                           adaptor.getMax(), inferredReturnShapes);
}

LogicalResult ClampOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  // For `mhlo.clamp`, the first operand may be a scalar.
  return hlo::deriveShapeFromOperand(&builder, getOperation(), operands[1],
                                     &reifiedReturnShapes);
}

//===----------------------------------------------------------------------===//
// SliceOp
//===----------------------------------------------------------------------===//

LogicalResult SliceOp::inferReturnTypes(
    MLIRContext* /*context*/, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange /*regions*/,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  SliceOpAdaptor adaptor(operands, attributes);
  return hlo::inferSliceOp(location, adaptor.getOperand().getType(),
                           adaptor.getStartIndices(), adaptor.getLimitIndices(),
                           adaptor.getStrides(), inferredReturnTypes);
}

template <typename I, typename E>
static void sliceElements(I values, ArrayRef<int64_t> sizes,
                          ArrayRef<int64_t> starts, ArrayRef<int64_t> limits,
                          ArrayRef<int64_t> strides,
                          llvm::SmallVectorImpl<E>* outValues) {
  assert(starts.size() == limits.size());
  assert(starts.size() == strides.size());
  if (starts.empty()) return;

  int64_t start = starts.front();
  int64_t limit = limits.front();
  int64_t stride = strides.front();
  if (starts.size() == 1) {
    for (int i = start; i < limit; i += stride) {
      outValues->push_back(*(values + i));
    }
    return;
  }

  for (; start < limit; start += stride) {
    auto begin = values + start * sizes.front();
    sliceElements<I, E>(begin, sizes.drop_front(), starts.drop_front(),
                        limits.drop_front(), strides.drop_front(), outValues);
  }
}

template <typename I, typename E>
static Attribute foldSlice(SliceOp* op, I values) {
  auto start = llvm::to_vector<6>(op->getStartIndices().getValues<int64_t>());
  auto limit = llvm::to_vector<6>(op->getLimitIndices().getValues<int64_t>());
  auto stride = llvm::to_vector<6>(op->getStrides().getValues<int64_t>());

  // TODO(b/235903849): This should be op->getType().case<ShapedType>().
  auto resultType = op->getOperand().getType().cast<ShapedType>();
  if (!resultType.hasStaticShape()) return {};

  auto shape = resultType.getShape();
  int64_t count = resultType.getNumElements();
  if (count == 0) {
    return DenseElementsAttr::get<E>(
        op->getResult().getType().cast<ShapedType>(),
        /*list=*/{});
  }

  // Compute the striding for each dimension.
  llvm::SmallVector<int64_t, 6> sizes;
  sizes.reserve(shape.size());
  for (auto v : shape) {
    count = count / v;
    sizes.push_back(count);
  }

  // Prevent folding if the result is too large.
  if (resultType.getNumElements() > kFoldOpEltLimit) return {};

  llvm::SmallVector<E, 6> outValues;
  outValues.reserve(resultType.getNumElements());
  sliceElements<I, E>(values, sizes, start, limit, stride, &outValues);

  return DenseElementsAttr::get(op->getResult().getType().cast<ShapedType>(),
                                outValues);
}

OpFoldResult SliceOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  // Check if the SliceOp is a NoOp operation.
  auto operandType = getOperand().getType().cast<ShapedType>();
  auto resultType = getResult().getType().cast<ShapedType>();

  if (operandType.hasStaticShape() && resultType.hasStaticShape() &&
      (operandType.getShape() == resultType.getShape())) {
    return getOperand();
  }

  if (operands.empty() || !operands.front()) return {};

  // Evaluate for statically valued inputs.
  DenseElementsAttr elements = operands.front().dyn_cast<DenseElementsAttr>();
  if (!elements) return {};

  auto etype = elements.getType().getElementType();
  if (etype.isa<IntegerType>()) {
    return foldSlice<DenseElementsAttr::IntElementIterator, APInt>(
        this, elements.value_begin<APInt>());
  }
  if (etype.isa<FloatType>()) {
    return foldSlice<DenseElementsAttr::FloatElementIterator, APFloat>(
        this, elements.value_begin<APFloat>());
  }

  return {};
}

namespace {
// In cases where a concat is fed into a slice, it is possible the concat
// can be simplified or bypassed. This checks which inputs to the concat are
// used by the slice, either reducing the number of concatenated values or
// entirely removes the concat.
struct SimplifyConcatSlice : public OpRewritePattern<SliceOp> {
  using OpRewritePattern<SliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(SliceOp slice,
                                PatternRewriter& rewriter) const override {
    auto resultTy = slice.getType().cast<ShapedType>();
    if (!resultTy.hasStaticShape()) {
      return failure();
    }

    auto sliceInput = slice.getOperand();
    auto sliceInputTy = sliceInput.getType().cast<ShapedType>();
    auto concat = sliceInput.getDefiningOp<ConcatenateOp>();
    if (!concat) {
      return failure();
    }

    auto dimension = concat.getDimension();

    auto start = slice.getStartIndices().getValues<APInt>();
    auto limit = slice.getLimitIndices().getValues<APInt>();

    auto sliceStart = (*(start.begin() + dimension)).getSExtValue();
    auto sliceLimit = (*(limit.begin() + dimension)).getSExtValue();

    // We need to determine what inputs from the concat affect the slice, and
    // how the bounds of the slice need to be updated for the minimally required
    // inputs.
    int64_t runningSize = 0;
    int64_t frontOffset = sliceInputTy.getShape()[dimension];

    auto subsetStart = concat.operand_end();
    auto subsetEnd = concat.operand_end();
    for (auto it = concat.operand_begin(); it < concat.operand_end(); ++it) {
      auto input = *it;
      ShapedType inputTy = input.getType().cast<ShapedType>();
      if (inputTy.isDynamicDim(dimension)) {
        return failure();
      }
      auto dimSize = inputTy.getShape()[dimension];

      // If this position is in the slice its the start of the subset and we
      // need to update the start and limit values.
      if (runningSize + dimSize > sliceStart &&
          subsetStart == concat.operand_end()) {
        subsetStart = it;
        frontOffset = runningSize;
      }

      // Determine the last required offset.
      if (runningSize < sliceLimit) {
        subsetEnd = it + 1;
      }

      runningSize += dimSize;
    }

    auto subsetSize = subsetEnd - subsetStart;
    // We need all inputs so no optimization.
    if (subsetSize == concat.getNumOperands()) {
      return failure();
    }

    // If there's nothing to slice that means the output is an empty tensor and
    // there is dead code. We do nothing here and rely on other passes to clean
    // this up.
    if (subsetSize == 0) {
      return failure();
    }

    if (subsetSize > 1 && !concat.getResult().hasOneUse()) {
      return failure();
    }

    auto concatRange = OperandRange(subsetStart, subsetEnd);
    auto newConcat = rewriter.create<ConcatenateOp>(
        concat.getLoc(), concatRange, concat.getDimension());

    llvm::SmallVector<APInt, 6> newStart(start);
    llvm::SmallVector<APInt, 6> newLimit(limit);
    newStart[dimension] -= frontOffset;
    newLimit[dimension] -= frontOffset;

    auto attrType = slice.getStartIndices().getType().cast<ShapedType>();
    auto create = rewriter.create<SliceOp>(
        slice.getLoc(), newConcat,
        DenseIntElementsAttr::get(attrType, newStart),
        DenseIntElementsAttr::get(attrType, newLimit), slice.getStrides());
    rewriter.replaceOp(slice, create.getResult());
    return success();
  }
};
}  // namespace

void SliceOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                          MLIRContext* context) {
  results.add<SimplifyConcatSlice>(context);
}

//===----------------------------------------------------------------------===//
// SortOp
//===----------------------------------------------------------------------===//

void SortOp::build(OpBuilder& builder, OperationState& state,
                   ValueRange operands, int64_t dimension, bool isStable) {
  state.addOperands(operands);
  state.addAttribute("dimension", builder.getI64IntegerAttr(dimension));
  state.addAttribute("is_stable", builder.getBoolAttr(isStable));

  for (Value operand : operands) state.addTypes(operand.getType());

  state.addRegion();
}

LogicalResult SortOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  SortOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferSortOp(location, adaptor.getInputs(), inferredReturnShapes);
}

LogicalResult SortOp::verify() {
  return hlo::verifySortOp(getLoc(), getInputs(), getDimension(),
                           getComparator());
}

/// Drops the operands if the results are not used and they are not used in
/// op.comparator().
static LogicalResult sortDropEmptyUseArgs(SortOp op,
                                          PatternRewriter& rewriter) {
  DenseSet<unsigned> erasedArgs;
  unsigned numOperands = op.getNumOperands();
  for (unsigned i = 0; i < numOperands; ++i) {
    if (!op.getResult(i).use_empty()) continue;
    Block& block = op.getComparator().front();
    if (!block.getArgument(i * 2).use_empty()) continue;
    if (!block.getArgument(i * 2 + 1).use_empty()) continue;
    erasedArgs.insert(i);
  }
  if (erasedArgs.empty()) return failure();

  SmallVector<Value> newOperands;
  BitVector erasedBlockArgs(op.getNumOperands() * 2);
  for (const auto& en : llvm::enumerate(op.getInputs())) {
    if (erasedArgs.contains(en.index())) {
      erasedBlockArgs.set(en.index() * 2);
      erasedBlockArgs.set(en.index() * 2 + 1);
    } else {
      newOperands.push_back(en.value());
    }
  }

  auto newOp = rewriter.create<SortOp>(op.getLoc(), newOperands,
                                       op.getDimension(), op.getIsStable());
  Region& region = newOp.getComparator();
  rewriter.inlineRegionBefore(op.getComparator(), region, region.end());
  region.front().eraseArguments(erasedBlockArgs);

  SmallVector<Value> results;
  for (unsigned i = 0, j = 0; i < numOperands; ++i) {
    if (erasedArgs.contains(i)) {
      results.push_back({});
    } else {
      results.push_back(newOp.getResult(j++));
    }
  }
  rewriter.replaceOp(op, results);

  return success();
}

/// Set the sorting dimension to the last dimension if it's not set and the rank
/// is known.
static LogicalResult sortOpInferDefaultDimension(SortOp op,
                                                 PatternRewriter& rewriter) {
  auto ty = op.getResultTypes()[0].dyn_cast<ShapedType>();
  if (!ty) {
    return failure();
  }
  if (static_cast<int64_t>(op.getDimension()) != -1) {
    return failure();
  }

  IntegerAttr dim = rewriter.getI64IntegerAttr(ty.getRank() - 1);
  auto newOp =
      rewriter.create<SortOp>(op.getLoc(), op.getResultTypes(), op.getInputs(),
                              dim, op.getIsStableAttr());
  Region& region = newOp.getComparator();
  rewriter.inlineRegionBefore(op.getComparator(), region, region.end());
  rewriter.replaceOp(op, newOp.getResults());

  return success();
}

void SortOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                         MLIRContext* /*context*/) {
  results.add(sortDropEmptyUseArgs);
  results.add(sortOpInferDefaultDimension);
}

//===----------------------------------------------------------------------===//
// TransposeOp
//===----------------------------------------------------------------------===//

OpFoldResult TransposeOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  if (auto elements = operands.front().dyn_cast_or_null<SplatElementsAttr>()) {
    return reshape(elements, getResult().getType().cast<ShapedType>());
  }
  for (const auto& it : llvm::enumerate(getPermutation().getValues<APInt>())) {
    if (it.index() != it.value()) {
      return {};
    }
  }
  return getOperand();
}

// transpose(transpose(X)) => transpose(X)
static LogicalResult eliminateRedundantTranspse(TransposeOp op,
                                                PatternRewriter& rewriter) {
  auto tranposeOperand = op.getOperand().getDefiningOp<TransposeOp>();
  if (!tranposeOperand) {
    return failure();
  }
  auto operandPermutation = tranposeOperand.getPermutation().getValues<APInt>();
  auto newPermutation =
      op.getPermutation()
          .mapValues(op.getPermutation().getElementType(),
                     [&operandPermutation](const APInt& index) -> APInt {
                       return operandPermutation[index.getSExtValue()];
                     })
          .cast<DenseIntElementsAttr>();
  rewriter.replaceOpWithNewOp<TransposeOp>(op, op.getResult().getType(),
                                           tranposeOperand.getOperand(),
                                           newPermutation);
  return success();
}

// transpose(broadcast_in_dim(X)) => broadcast_in_dim(X)
static LogicalResult eliminateBroadcastInDimTranspose(
    TransposeOp op, PatternRewriter& rewriter) {
  auto broadcastInDimOp = op.getOperand().getDefiningOp<BroadcastInDimOp>();
  if (!broadcastInDimOp) {
    return failure();
  }
  DenseIntElementsAttr broadcastDimensions =
      broadcastInDimOp.getBroadcastDimensions();
  DenseIntElementsAttr permutation = op.getPermutation();
  SmallVector<int64_t> newBroadcastDimensions;
  for (auto dimension : broadcastDimensions.getValues<int64_t>()) {
    int64_t index = 0;
    for (auto p : permutation.getValues<int64_t>()) {
      if (p == dimension) {
        newBroadcastDimensions.push_back(index);
        break;
      }
      index++;
    }
  }
  rewriter.replaceOpWithNewOp<BroadcastInDimOp>(
      op, op->getResultTypes(), broadcastInDimOp.getOperand(),
      rewriter.getI64TensorAttr(newBroadcastDimensions));
  return success();
}

// simplify Transpose: replace Transpose with Reshape if they are equivalent
static LogicalResult simplifyTranspose(TransposeOp op,
                                       PatternRewriter& rewriter) {
  auto operandType = op.getOperand().getType().dyn_cast<RankedTensorType>();
  auto resultType = op.getResult().getType().dyn_cast<RankedTensorType>();
  if (!operandType || !resultType) {
    return failure();
  }
  // Not support dynamic shape a.t.m. BTW, when it's dynamic shape,
  // maybe Transpose should be replaced by DynamicReshape.
  if (!operandType.hasStaticShape() || !resultType.hasStaticShape()) {
    return failure();
  }
  auto permutation = op.getPermutation().getValues<int64_t>();
  llvm::SmallVector<int64_t> sortedPermutation;
  for (int64_t i = 0, e = resultType.getRank(); i < e; i++) {
    if (resultType.getDimSize(i) != 1) {
      sortedPermutation.push_back(permutation[i]);
    }
  }
  if (llvm::is_sorted(sortedPermutation)) {
    rewriter.replaceOpWithNewOp<ReshapeOp>(op, op.getType(), op.getOperand());
    return success();
  }
  return failure();
}

void TransposeOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                              MLIRContext* /*context*/) {
  results.add(eliminateRedundantTranspse);
  results.add(eliminateBroadcastInDimTranspose);
  results.add(simplifyTranspose);
}

LogicalResult TransposeOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  TransposeOp::Adaptor adaptor(operands);
  Value operand = adaptor.getOperand();

  auto operandType = operand.getType().dyn_cast<RankedTensorType>();
  // Not support unranked type a.t.m.
  if (!operandType) return failure();

  Location loc = this->getLoc();
  SmallVector<int64_t, 4> permutation(
      this->getPermutation().getValues<int64_t>());
  SmallVector<Value, 4> shapeValues(permutation.size());

  Type shapeScalarType = builder.getIndexType();
  auto toShapeScalarType = [&](Value v) {
    return maybeCastTo(builder, loc, v, shapeScalarType);
  };

  for (const auto& element : llvm::enumerate(operandType.getShape())) {
    int64_t idx = element.index();
    auto* it = std::find(permutation.begin(), permutation.end(), idx);
    Value valueDim = toShapeScalarType(
        builder.createOrFold<tensor::DimOp>(loc, operand, element.index()));
    shapeValues[std::distance(permutation.begin(), it)] = valueDim;
  }

  Value outputShape = builder.create<tensor::FromElementsOp>(
      loc,
      RankedTensorType::get({static_cast<int64_t>(shapeValues.size())},
                            shapeScalarType),
      shapeValues);
  reifiedReturnShapes.push_back(outputShape);

  return success();
}

LogicalResult TransposeOp::inferReturnTypes(
    MLIRContext*, Optional<Location> loc, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  TransposeOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferTransposeOp(loc, adaptor.getOperand(),
                               adaptor.getPermutation(), inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// TriangularSolveOp
//===----------------------------------------------------------------------===//

LogicalResult TriangularSolveOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  TriangularSolveOp::Adaptor adaptor(operands, attributes, regions);
  bool isTransposeAInvalid =
      (adaptor.getTransposeA() == Transpose::TRANSPOSE_INVALID);
  return hlo::inferTriangularSolveOp(location, adaptor.getA(), adaptor.getB(),
                                     adaptor.getLeftSide(), isTransposeAInvalid,
                                     inferredReturnShapes);
}

//===----------------------------------------------------------------------===//
// GetTupleElementOp
//===----------------------------------------------------------------------===//

OpFoldResult GetTupleElementOp::fold(FoldAdaptor /*adaptor*/) {
  if (auto tupleOp = getOperand().getDefiningOp<mhlo::TupleOp>()) {
    return tupleOp.getOperand(getIndex());
  }

  return {};
}

LogicalResult GetTupleElementOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  GetTupleElementOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferGetTupleElementOp(location, adaptor.getOperand(),
                                     adaptor.getIndex(), inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// TupleOp
//===----------------------------------------------------------------------===//

LogicalResult TupleOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  TupleOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferTupleOp(context, location, adaptor.getVal(),
                           inferredReturnTypes);
}

//===----------------------------------------------------------------------===//
// UnaryEinsumOp
//===----------------------------------------------------------------------===//

void UnaryEinsumOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                                MLIRContext* context) {
  results.add<UnaryEinsumToEinsum>(context);
}

//===----------------------------------------------------------------------===//
// CompareOp
//===----------------------------------------------------------------------===//

void CompareOp::build(OpBuilder& builder, OperationState& result, Value lhs,
                      Value rhs, ComparisonDirection comparisonDirection,
                      ComparisonType compareType) {
  build(builder, result, lhs, rhs,
        ComparisonDirectionAttr::get(builder.getContext(), comparisonDirection),
        ComparisonTypeAttr::get(builder.getContext(), compareType));
}

LogicalResult CompareOp::inferReturnTypeComponents(
    MLIRContext* context, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  CompareOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferCompareOp(context, location, adaptor.getLhs(),
                             inferredReturnShapes);
}

LogicalResult CompareOp::reifyReturnTypeShapes(
    OpBuilder& builder, ValueRange operands,
    SmallVectorImpl<Value>& reifiedReturnShapes) {
  return hlo::deriveShapeFromOperand(&builder, getOperation(), operands.front(),
                                     &reifiedReturnShapes);
}

template <typename Op, typename ElementType, typename SrcType, typename Convert>
static Attribute CompareFolder(CompareOp op, ArrayRef<Attribute> attrs) {
  if (!attrs[0] || !attrs[1]) return {};

  DenseElementsAttr lhs = attrs[0].dyn_cast<DenseElementsAttr>();
  DenseElementsAttr rhs = attrs[1].dyn_cast<DenseElementsAttr>();
  if (!lhs || !rhs) return {};

  ShapedType operandType =
      op.getOperand(0).getType().template cast<ShapedType>();
  if (!operandType.hasStaticShape()) {
    return {};
  }

  auto etype = operandType.getElementType();
  if (!etype.isa<ElementType>()) {
    return {};
  }

  // Prevent folding if the result is too large.
  if (lhs.getNumElements() > kFoldOpEltLimit) return {};

  SmallVector<bool, 6> values;
  values.reserve(lhs.getNumElements());
  for (const auto zip :
       llvm::zip(lhs.getValues<SrcType>(), rhs.getValues<SrcType>())) {
    values.push_back(
        Convert()(addSign(std::get<0>(zip), lhs.getElementType()),
                  addSign(std::get<1>(zip), rhs.getElementType())));
  }

  auto resultTy = op.getType().cast<ShapedType>();
  return DenseElementsAttr::get(resultTy, values);
}

OpFoldResult CompareOp::fold(FoldAdaptor adaptor) {
  auto operands = adaptor.getOperands();
  auto resultTy = getType().cast<ShapedType>();
  if (!resultTy.hasStaticShape()) return {};

  auto direction = getComparisonDirection();
  auto lhsTy = getElementTypeOrSelf(getLhs());
  if (getLhs() == getRhs() && !lhsTy.isa<FloatType>() &&
      (!lhsTy.isa<ComplexType>() ||
       !lhsTy.cast<ComplexType>().getElementType().isa<FloatType>())) {
    if (direction == ComparisonDirection::LE ||
        direction == ComparisonDirection::EQ ||
        direction == ComparisonDirection::GE) {
      return DenseIntElementsAttr::get(resultTy, {true});
    }
    return DenseIntElementsAttr::get(resultTy, {false});
  }

  auto opElType = getLhs().getType().cast<ShapedType>().getElementType();
  // Fold tensor<*xi1> != false to just return tensor<*xi1>
  if (direction == ComparisonDirection::NE && opElType.isInteger(1)) {
    DenseIntElementsAttr cstAttr;
    if (matchPattern(getLhs(), m_Constant(&cstAttr))) {
      if (cstAttr.isSplat() && !cstAttr.getSplatValue<bool>()) {
        return getRhs();
      }
    }

    if (matchPattern(getRhs(), m_Constant(&cstAttr))) {
      if (cstAttr.isSplat() && !cstAttr.getSplatValue<bool>()) {
        return getLhs();
      }
    }
  }

  // Fold tensor<*xi1> == True to just return tensor<*xi1>
  if (direction == ComparisonDirection::EQ && opElType.isInteger(1)) {
    DenseIntElementsAttr cstAttr;
    if (matchPattern(getLhs(), m_Constant(&cstAttr))) {
      if (cstAttr.isSplat() && cstAttr.getSplatValue<bool>()) {
        return getRhs();
      }
    }

    if (matchPattern(getRhs(), m_Constant(&cstAttr))) {
      if (cstAttr.isSplat() && cstAttr.getSplatValue<bool>()) {
        return getLhs();
      }
    }
  }

  if (!operands[0] || !operands[1]) {
    return {};
  }

#define COMPARE_FOLDER(Op, comparison, Func)                                \
  if (direction == comparison) {                                            \
    if (auto folded = CompareFolder<Op, FloatType, APFloat, Func<APFloat>>( \
            *this, operands))                                               \
      return folded;                                                        \
    if (auto folded = CompareFolder<Op, IntegerType, APInt, Func<APSInt>>(  \
            *this, operands))                                               \
      return folded;                                                        \
  }

  COMPARE_FOLDER(CompareOp, ComparisonDirection::EQ, std::equal_to);
  COMPARE_FOLDER(CompareOp, ComparisonDirection::NE, std::not_equal_to);
  COMPARE_FOLDER(CompareOp, ComparisonDirection::LT, std::less);
  COMPARE_FOLDER(CompareOp, ComparisonDirection::LE, std::less_equal);
  COMPARE_FOLDER(CompareOp, ComparisonDirection::GT, std::greater);
  COMPARE_FOLDER(CompareOp, ComparisonDirection::GE, std::greater_equal);
#undef COMPARE_FOLDER

  return {};
}

//===----------------------------------------------------------------------===//
// SelectAndScatterOp
//===----------------------------------------------------------------------===//

LogicalResult SelectAndScatterOp::inferReturnTypes(
    MLIRContext*, Optional<Location>, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  SelectAndScatterOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferSelectAndScatterOp(adaptor.getOperand(),
                                      inferredReturnTypes);
}

LogicalResult SelectAndScatterOp::verify() {
  return hlo::verifySelectAndScatterOp(getLoc(), getOperand(), getSource(),
                                       getInitValue(), getWindowDimensions(),
                                       getWindowStrides(), getPadding(),
                                       getSelect(), getScatter());
}

//===----------------------------------------------------------------------===//
// ScatterOp
//===----------------------------------------------------------------------===//

LogicalResult ScatterOp::inferReturnTypes(
    MLIRContext*, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  ScatterOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferScatterOp(location, adaptor.getInputs(),
                             inferredReturnTypes);
}

LogicalResult ScatterOp::verify() {
  return hlo::verifyScatterOp(
      getLoc(), getInputs(), getScatterIndices(), getUpdates(),
      getScatterDimensionNumbers().getUpdateWindowDims(),
      getScatterDimensionNumbers().getInsertedWindowDims(),
      getScatterDimensionNumbers().getScatterDimsToOperandDims(),
      getScatterDimensionNumbers().getIndexVectorDim(), getUpdateComputation());
}

llvm::SmallVector<Attribute, 4> evaluateMhloRegion(Region& region,
                                                   ArrayRef<Attribute> inputs) {
  if (region.getNumArguments() != inputs.size()) return {};

  llvm::DenseMap<Value, Attribute> values;
  values.reserve(region.getNumArguments());
  for (auto it : llvm::zip(region.getArguments(), inputs)) {
    values.try_emplace(std::get<0>(it), std::get<1>(it));
  }

  for (auto& op : region.getOps()) {
    llvm::SmallVector<Attribute, 4> inputs;
    for (auto& operand : op.getOpOperands()) {
      inputs.push_back(values.lookup(operand.get()));
    }
    if (isa<ReturnOp>(op)) return inputs;

    llvm::SmallVector<OpFoldResult, 4> results;
    if (failed(op.fold(inputs, results))) return {};
    for (auto it : llvm::zip(op.getResults(), results)) {
      if (!std::get<1>(it).is<Attribute>()) return {};
      values.insert({std::get<0>(it), std::get<1>(it).get<Attribute>()});
    }
  }
  return {};
}

LogicalResult ScatterOp::fold(
    FoldAdaptor adaptor, llvm::SmallVectorImpl<OpFoldResult>& foldResults) {
  auto args = adaptor.getOperands();
  // Variadic Scatter not yet implemented
  if (getInputs().size() != 1 || getUpdates().size() != 1) return failure();
  auto index = args[1].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!index) return failure();

  auto baseType = getInputs().getTypes()[0].dyn_cast<RankedTensorType>();
  auto updateType = getUpdates().getTypes()[0].dyn_cast<RankedTensorType>();
  auto indexType = index.getType().cast<RankedTensorType>();
  if (!baseType || !indexType || !updateType) return failure();

  // TODO(b/228310289): Work around canonicalization crash for complex types.
  // Remove after upstream MLIR has been fixed.
  if (baseType.getElementType().isa<ComplexType>()) return failure();

  // Catch a trivial full replacement of base with update, this does not require
  // these to be constant: just that we know the type.
  if (updateType == baseType && updateType.hasStaticShape() &&
      baseType.hasStaticShape() && index.isSplat() &&
      index.getSplatValue<uint32_t>() == 0 &&
      llvm::hasSingleElement(getUpdateComputation().front())) {
    foldResults.push_back(getUpdates()[0]);
    return success();
  }
  auto base = args[0].dyn_cast_or_null<DenseElementsAttr>();
  auto update = args[2].dyn_cast_or_null<DenseElementsAttr>();
  if (!base || !update) return failure();

  // Add the virtual trailing dimension of size 1 if indexVectorDim equals to
  // indexType.rank.
  const int64_t indexVectorDim =
      getScatterDimensionNumbers().getIndexVectorDim();
  if (indexVectorDim == indexType.getRank()) {
    auto indexShape = indexType.getShape().vec();
    indexShape.push_back(1);
    indexType = RankedTensorType::get(indexShape, indexType.getElementType());
    index = reshape(index, indexType).cast<DenseIntElementsAttr>();
  }

  // Increment the multi-dimensional index vector based on the limits for each
  // dimension specified by shape and returns false if the index rolled around
  // with true otherwise.
  auto nextIndex = [](llvm::SmallVector<uint64_t, 8>& index,
                      llvm::ArrayRef<int64_t> shape) {
    for (int64_t i = index.size() - 1; i >= 0; --i) {
      ++index[i];
      if (index[i] < static_cast<unsigned long>(shape[i])) return true;
      index[i] = 0;
    }
    return false;
  };

  // Prevent folding if the result is too large.
  if (base.getNumElements() > kFoldOpEltLimit) return failure();

  // Iterate over all elements of the update tensor, then find the corresponding
  // value in the indices tensor to determine which location we have to update
  // in the base/result tensor.
  llvm::SmallVector<Attribute, 8> results(base.getValues<Attribute>());
  llvm::SmallVector<uint64_t, 8> updateIndex(updateType.getRank(), 0);
  llvm::SmallVector<uint64_t, 8> indexIndex;
  indexIndex.reserve(indexType.getRank());
  llvm::SmallVector<int64_t, 8> baseIndex;
  baseIndex.reserve(baseType.getRank());
  do {
    // Compute the index for the slice of the indices tensor for this update
    // value.
    indexIndex.clear();
    if (indexVectorDim == 0) indexIndex.push_back(0);
    for (int64_t i = 0; i < static_cast<int64_t>(updateIndex.size()); ++i) {
      if (llvm::count(getScatterDimensionNumbers().getUpdateWindowDims(), i) ==
          0)
        indexIndex.push_back(updateIndex[i]);
      if (static_cast<int64_t>(indexIndex.size()) == indexVectorDim)
        indexIndex.push_back(0);
    }

    // Compute the index for the given update value in the base tensor.
    baseIndex.assign(baseType.getRank(), 0);
    uint64_t indexCount = indexType.getShape()[indexVectorDim];
    for (uint64_t i = 0; i < indexCount; ++i) {
      uint64_t operandDim =
          getScatterDimensionNumbers().getScatterDimsToOperandDims()[i];
      indexIndex[indexVectorDim] = i;
      baseIndex[operandDim] +=
          index.getValues<APInt>()[indexIndex].getSExtValue();
    }
    uint64_t updateWindowDimIndex = 0;
    auto insertedWindowDims =
        getScatterDimensionNumbers().getInsertedWindowDims();
    auto updateWindowDims = getScatterDimensionNumbers().getUpdateWindowDims();
    for (uint64_t i = 0; i < baseIndex.size(); ++i) {
      if (llvm::count(insertedWindowDims, i)) continue;
      baseIndex[i] += updateIndex[updateWindowDims[updateWindowDimIndex]];
      updateWindowDimIndex++;
    }

    // Compute the linear index for the index into the base tensor.
    int64_t linearBaseIndex = 0;
    int64_t linearBaseIndexMultiplyer = 1;
    for (int64_t i = baseIndex.size() - 1; i >= 0; --i) {
      // Out of bound index have backend specific behaviour so avoid folding it.
      if (baseIndex[i] < 0 || baseIndex[i] >= baseType.getShape()[i])
        return failure();
      linearBaseIndex += baseIndex[i] * linearBaseIndexMultiplyer;
      linearBaseIndexMultiplyer *= baseType.getShape()[i];
    }

    // Evaluate update computation and update the value with the newly computed
    // attribute in the base tensor.
    auto lhs = DenseElementsAttr::get(
        RankedTensorType::get({}, baseType.getElementType()),
        results[linearBaseIndex]);
    auto rhs = DenseElementsAttr::get(
        RankedTensorType::get({}, baseType.getElementType()),
        update.getValues<Attribute>()[updateIndex]);
    auto newValue = evaluateMhloRegion(getUpdateComputation(), {lhs, rhs});
    if (newValue.size() != 1 || !newValue[0]) return failure();
    results[linearBaseIndex] =
        newValue[0].cast<DenseElementsAttr>().getValues<Attribute>()[0];
  } while (nextIndex(updateIndex, updateType.getShape()));

  foldResults.push_back(DenseElementsAttr::get(baseType, results));
  return success();
}

// Replace mhlo.scatter overwriting the entire input with mhlo.map.
struct ScatterFullReplace : public OpRewritePattern<ScatterOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(ScatterOp scatter,
                                PatternRewriter& rewriter) const override {
    // Variadic Scatter not yet implemented
    if (scatter.getInputs().size() != 1 || scatter.getUpdates().size() != 1)
      return failure();

    auto baseType =
        scatter.getInputs().getTypes()[0].dyn_cast<RankedTensorType>();
    auto updateType =
        scatter.getUpdates().getTypes()[0].dyn_cast<RankedTensorType>();
    auto indexType =
        scatter.getScatterIndices().getType().dyn_cast<RankedTensorType>();
    if (!baseType || !indexType || !updateType) return failure();

    // If updates is an empty shape, scatter overwrites the entire tensor.
    // Transform it into a map with the combiner function.
    if (!indexType.hasStaticShape() || indexType.getNumElements() > 0)
      return failure();

    // Require the same shape for base and updates. This isn't strictly
    // necessary, but handling other cases would require turning scatter options
    // into the appropriate reshapes and transposes.
    if (!baseType.hasStaticShape() || !updateType.hasStaticShape() ||
        baseType != updateType)
      return failure();

    auto dimensions =
        llvm::to_vector(llvm::seq<int64_t>(0, baseType.getRank()));
    auto map = rewriter.create<mhlo::MapOp>(
        scatter.getLoc(), scatter->getResultTypes(),
        ValueRange{scatter.getOperands()[0], scatter.getUpdates()[0]},
        rewriter.getI64TensorAttr(dimensions));
    rewriter.inlineRegionBefore(scatter.getRegion(), map.getRegion(),
                                map.getRegion().begin());
    rewriter.replaceOp(scatter, map->getResults());
    return success();
  }
};

void ScatterOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                            MLIRContext* context) {
  results.add<ScatterFullReplace>(context);
}

//===----------------------------------------------------------------------===//
// WhileOp
//===----------------------------------------------------------------------===//

LogicalResult WhileOp::inferReturnTypes(
    MLIRContext* context, Optional<Location> location, ValueRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<Type>& inferredReturnTypes) {
  WhileOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferWhileOp(location, adaptor.getOperand(), inferredReturnTypes);
}

LogicalResult WhileOp::verify() {
  return hlo::verifyWhileOp(getLoc(), getOperand(), getCond(), getBody());
}

/// Print a `while` op.
///
/// op ::= `mhlo.while` `(` assignment-list `)` `:` types attribute-dict
///         `cond` region
///         `do` region
/// assignment-list ::= assignment | assignment `,` assignment-list
/// assignment ::= ssa-value `=` ssa-value
void WhileOp::print(OpAsmPrinter& p) {
  p << '(';
  llvm::interleaveComma(
      llvm::zip(SingleBlock::getBody()->getArguments(), getOperands()), p,
      [&](auto zip) {
        p.printOperand(std::get<0>(zip));
        p << " = ";
        p.printOperand(std::get<1>(zip));
      });
  p << ")";
  if (getNumOperands()) {
    p << " : ";
    llvm::interleaveComma(getOperandTypes(), p);
  }
  p.printOptionalAttrDictWithKeyword(getOperation()->getAttrs());
  p.printNewline();
  p << " cond ";
  p.printRegion(getRegion(0), /*printEntryBlockArgs=*/false);
  p << " do ";
  p.printRegion(getRegion(1), /*printEntryBlockArgs=*/false);
}

ParseResult WhileOp::parse(OpAsmParser& parser, OperationState& result) {
  llvm::SMLoc loc = parser.getCurrentLocation();
  // Parse the operands of the while: these are of the form:
  //   %iter_arg = %init_val
  // where %iter_arg is the name of the block argument in the cond/body blocks
  // and %init_val is the actual operand.
  SmallVector<OpAsmParser::UnresolvedOperand> operands;
  SmallVector<OpAsmParser::UnresolvedOperand> iterArgs;
  if (parser.parseLParen()) return failure();
  do {
    if (succeeded(parser.parseOptionalRParen())) break;
    OpAsmParser::UnresolvedOperand operand, iterArg;
    if (parser.parseOperand(iterArg) || parser.parseEqual() ||
        parser.parseOperand(operand))
      return failure();
    iterArgs.push_back(iterArg);
    operands.push_back(operand);
    if (succeeded(parser.parseOptionalRParen())) break;
    if (failed(parser.parseComma())) return failure();
  } while (true);
  if (!operands.empty()) {
    if (parser.parseColon() || parser.parseTypeList(result.types))
      return failure();
  }

  SmallVector<OpAsmParser::Argument> args;
  createArgs(iterArgs, result.types, args);
  if (parser.resolveOperands(operands, result.types, loc, result.operands) ||
      parser.parseOptionalAttrDictWithKeyword(result.attributes) ||
      parser.parseKeyword("cond") ||
      parser.parseRegion(*result.addRegion(), args) ||
      parser.parseKeyword("do") ||
      parser.parseRegion(*result.addRegion(), args))
    return failure();
  return success();
}

LogicalResult WhileOp::fold(FoldAdaptor /*adaptor*/,
                            SmallVectorImpl<OpFoldResult>& results) {
  DenseIntElementsAttr condValue;
  // TODO: This folder is executed on invalid mhlo.while ops during
  // LegalizeMhlo, mlir_hlo/tosa/tests/unary.mlir. Broken pattern?
  auto condReturnOp = dyn_cast<ReturnOp>(getCond().front().back());
  if (!condReturnOp) return failure();
  if (!matchPattern(condReturnOp.getOperand(0), m_Constant(&condValue)))
    return failure();
  if (condValue.getSplatValue<BoolAttr>().getValue())
    return failure();  // TODO(mhlo): this is an infinite loop, should we fold?

  results.append(getOperands().begin(), getOperands().end());
  return success();
}

static LogicalResult whileCanonicalization(WhileOp whileOp,
                                           PatternRewriter& rewriter) {
  // Turn loop invariant values into implicit capture.
  // Check if there is at least one value is forwarded from one iteration to the
  // next, or one of the yielded value is an implicit capture already. Otherwise
  // there is nothing to do here.
  Block* cond = whileOp.SingleBlock::getBody(0);
  Block* body = whileOp.SingleBlock::getBody(1);
  auto bodyReturnOp = cast<ReturnOp>(body->getTerminator());
  if (!llvm::any_of(llvm::zip(whileOp->getOperands(), body->getArguments(),
                              bodyReturnOp->getOperands()),
                    [&](auto zip) {
                      return (std::get<0>(zip) == std::get<2>(zip) ||
                              std::get<1>(zip) == std::get<2>(zip));
                    }))
    return rewriter.notifyMatchFailure(whileOp, "no loop invariant found");

  SmallVector<Value> newOperands, resultsToReplace;
  SmallVector<unsigned> invariantArgIdxs;
  BitVector invariantArgIdxBitVector(cond->getNumArguments());
  for (const auto& enumeratedOperands : llvm::enumerate(llvm::zip(
           whileOp.getOperands(), cond->getArguments(), body->getArguments(),
           bodyReturnOp->getOperands(), whileOp->getResults()))) {
    const auto& operands = enumeratedOperands.value();
    Value whileOperand = std::get<0>(operands);
    BlockArgument condBlockArg = std::get<1>(operands);
    BlockArgument bodyBlockArg = std::get<2>(operands);
    Value bodyReturnOperand = std::get<3>(operands);
    Value whileResult = std::get<4>(operands);

    bool forwarded = (whileOperand == bodyReturnOperand ||
                      bodyBlockArg == bodyReturnOperand);
    if (forwarded) {
      invariantArgIdxs.push_back(enumeratedOperands.index());
      invariantArgIdxBitVector.set(enumeratedOperands.index());
      condBlockArg.replaceAllUsesWith(whileOperand);
      bodyBlockArg.replaceAllUsesWith(whileOperand);
      whileResult.replaceAllUsesWith(whileOperand);
      continue;
    }
    newOperands.push_back(whileOperand);
    resultsToReplace.push_back(whileResult);
  }
  cond->eraseArguments(invariantArgIdxBitVector);
  body->eraseArguments(invariantArgIdxBitVector);
  for (int idx : llvm::reverse(invariantArgIdxs))
    bodyReturnOp->eraseOperand(idx);

  WhileOp newWhileOp = rewriter.create<WhileOp>(
      whileOp.getLoc(), bodyReturnOp->getOperandTypes(), newOperands);
  newWhileOp.getBodyRegion(0).takeBody(whileOp.getBodyRegion(0));
  newWhileOp.getBodyRegion(1).takeBody(whileOp.getBodyRegion(1));
  for (auto results : llvm::zip(resultsToReplace, newWhileOp->getResults()))
    std::get<0>(results).replaceAllUsesWith(std::get<1>(results));
  rewriter.eraseOp(whileOp);
  return success();
}

void WhileOp::getCanonicalizationPatterns(RewritePatternSet& results,
                                          MLIRContext* context) {
  results.add(&whileCanonicalization);
}

LogicalResult UniformDequantizeOp::inferReturnTypeComponents(
    MLIRContext*, Optional<Location> location, ValueShapeRange operands,
    DictionaryAttr attributes, RegionRange regions,
    SmallVectorImpl<ShapedTypeComponents>& inferredReturnShapes) {
  UniformDequantizeOp::Adaptor adaptor(operands, attributes, regions);
  return hlo::inferUniformDequantizeOp(location, adaptor.getOperand(),
                                       inferredReturnShapes);
}

using mlir::hlo::parseWindowAttributes;
using mlir::hlo::printWindowAttributes;

}  // namespace mhlo
}  // namespace mlir

using mlir::hlo::parseComplexOpType;
using mlir::hlo::parseCustomCallTarget;
using mlir::hlo::parseDenseI64Array;
using mlir::hlo::parseExponentMantissa;
using mlir::hlo::parsePairwiseOpType;
using mlir::hlo::parseSameOperandsAndResultType;
using mlir::hlo::parseSelectOpType;
using mlir::hlo::parseTupleOpType;
using mlir::hlo::parseVariadicOperandWithAttribute;
using mlir::hlo::parseVariadicSameOperandsAndResultType;
using mlir::hlo::printComplexOpType;
using mlir::hlo::printCustomCallTarget;
using mlir::hlo::printDenseI64Array;
using mlir::hlo::printExponentMantissa;
using mlir::hlo::printPairwiseOpType;
using mlir::hlo::printSameOperandsAndResultType;
using mlir::hlo::printSelectOpType;
using mlir::hlo::printTupleOpType;
using mlir::hlo::printVariadicOperandWithAttribute;
using mlir::hlo::printVariadicSameOperandsAndResultType;

#define GET_OP_CLASSES
#include "mhlo/IR/hlo_ops.cc.inc"

namespace mlir {
namespace mhlo {

//===----------------------------------------------------------------------===//
// mhlo Dialect Interfaces
//===----------------------------------------------------------------------===//

namespace {
struct MhloDialectInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;

  // Allow all call operations to be inlined.
  bool isLegalToInline(Operation* call, Operation* callable,
                       bool wouldBeCloned) const final {
    return true;
  }
  // We don't have any special restrictions on what can be inlined into
  // destination regions (e.g. while/conditional bodies). Always allow it.
  bool isLegalToInline(Region* dest, Region* src, bool wouldBeCloned,
                       IRMapping& valueMapping) const final {
    return true;
  }
  // Operations in mhlo dialect are always legal to inline since they are
  // pure.
  bool isLegalToInline(Operation*, Region*, bool,
                       IRMapping&) const final {
    return true;
  }
};

struct MhloHloDialectInterface : public hlo::HloDialectInterface {
  using HloDialectInterface::HloDialectInterface;

  Type createTokenType() const override {
    return TokenType::get(getDialect()->getContext());
  }

  bool isTokenType(Type type) const override { return type.isa<TokenType>(); }

  Attribute createTypeExtensions(ArrayRef<int64_t> bounds) const override {
    return TypeExtensionsAttr::get(getDialect()->getContext(), bounds);
  }
};
}  // end anonymous namespace

//===----------------------------------------------------------------------===//
// mhlo Dialect Constructor
//===----------------------------------------------------------------------===//

MhloDialect::MhloDialect(MLIRContext* context)
    : Dialect(getDialectNamespace(), context, TypeID::get<MhloDialect>()) {
  addOperations<
#define GET_OP_LIST
#include "mhlo/IR/hlo_ops.cc.inc"
      >();
  addInterfaces<MhloHloDialectInterface>();
  addInterfaces<MhloDialectInlinerInterface>();
  addBytecodeInterface(this);
  addTypes<TokenType, AsyncBundleType>();
  addAttributes<
#define GET_ATTRDEF_LIST
#include "mhlo/IR/hlo_ops_attrs.cc.inc"
      >();
  context->loadDialect<tensor::TensorDialect>();
}

Type MhloDialect::parseType(DialectAsmParser& parser) const {
  StringRef mnemonic;
  Type parsedType;
  auto parseResult = generatedTypeParser(parser, &mnemonic, parsedType);
  if (parseResult.has_value()) return parsedType;
  if (mnemonic == "token") return TokenType::get(getContext());
  parser.emitError(parser.getNameLoc()) << "unknown mhlo type: " << mnemonic;
  return nullptr;
}

void MhloDialect::printType(Type type, DialectAsmPrinter& os) const {
  if (type.isa<TokenType>()) {
    os << "token";
    return;
  }
  if (succeeded(generatedTypePrinter(type, os))) return;
  os << "<unknown mhlo type>";
}

// Entry point for Attribute parsing, TableGen generated code will handle the
// dispatch to the individual classes.
Attribute MhloDialect::parseAttribute(DialectAsmParser& parser,
                                      Type type) const {
  StringRef attrTag;
  Attribute attr;
  auto parseResult = generatedAttributeParser(parser, &attrTag, type, attr);
  if (parseResult.has_value()) return attr;
  parser.emitError(parser.getNameLoc(), "unknown mhlo attribute");
  return Attribute();
}

// Entry point for Attribute printing, TableGen generated code will handle the
// dispatch to the individual classes.
void MhloDialect::printAttribute(Attribute attr, DialectAsmPrinter& os) const {
  LogicalResult result = generatedAttributePrinter(attr, os);
  (void)result;
  assert(succeeded(result));
}

/// Helpers for attributes parsing.
static ParseResult parseDims(AsmParser& parser,
                             SmallVector<int64_t>& dimSizes) {
  dimSizes.clear();
  auto failOrDims = parseDimSizes(parser);
  if (failed(failOrDims)) {
    return failure();
  }
  dimSizes = std::move(*failOrDims);
  return success();
}

static ParseResult parseDimsWithMinimumElements(AsmParser& parser,
                                                SmallVector<int64_t>& dimSizes,
                                                int minElements) {
  if (failed(parseDims(parser, dimSizes))) return failure();
  if (static_cast<int64_t>(dimSizes.size()) < minElements)
    return parser.emitError(parser.getCurrentLocation())
           << "expected at least " << minElements << " element(s), found "
           << dimSizes.size();
  return success();
}

/// Parse a custom attribute that resembles a struct of the form
/// <
///   foo = something_parsed_by_custom_parser,
///   bar = something_parsed_by_different_custom_parser,
///   baz something_parsed_by_another_custom_parser
/// >
/// The optional argument `parse_equal` array can be used to denote if
/// '=' follows the keyword (see baz in the example above) for a field. If
/// not provided, all fields must be followed by a '='.
static ParseResult parseStruct(
    AsmParser& parser, ArrayRef<StringRef> keywords,
    ArrayRef<llvm::function_ref<ParseResult()>> parseFuncs,
    ArrayRef<bool> parseEqual = {}) {
  assert(keywords.size() == parseFuncs.size());
  assert(parseEqual.empty() || parseEqual.size() == keywords.size());
  SmallVector<bool> seen(keywords.size(), false);
  while (failed(parser.parseOptionalGreater())) {
    bool foundOne = false;
    for (const auto& it : llvm::enumerate(keywords)) {
      size_t index = it.index();
      StringRef keyword = it.value();
      if (succeeded(parser.parseOptionalKeyword(keyword))) {
        if (seen[index]) {
          return parser.emitError(parser.getCurrentLocation())
                 << "duplicated `" << keyword << "` entry";
        }
        if (parseEqual.empty() || parseEqual[index]) {
          if (failed(parser.parseEqual())) return failure();
        }
        if (failed(parseFuncs[index]())) return failure();
        if (failed(parser.parseOptionalComma())) return parser.parseGreater();
        seen[index] = true;
        foundOne = true;
      }
    }
    if (!foundOne) {
      auto parseError = parser.emitError(parser.getCurrentLocation())
                        << "expected one of: ";
      llvm::interleaveComma(keywords, parseError, [&](StringRef kw) {
        parseError << '`' << kw << '`';
      });
      return parseError;
    }
  }
  return success();
}

// Helpers to print an optional array or integer field, to simplify writing
// attribute printers.
template <typename T>
static void printField(AsmPrinter& printer, StringRef name, T field,
                       StringRef& separator) {
  if (field != 0) {
    printer << separator << name << " = " << field;
    separator = ", ";
  }
}
template <typename T>
static void printField(AsmPrinter& printer, StringRef name, ArrayRef<T> field,
                       StringRef& separator) {
  if (!field.empty()) {
    printer << separator << name << " = [";
    llvm::interleaveComma(field, printer);
    printer << "]";
    separator = ", ";
  }
}

template <typename... Ts>
static void printStruct(AsmPrinter& printer, StringRef name,
                        Ts... printFields) {
  printer << "<";
  StringRef separator = "";
  // Fold expression to print each entry in the parameter pack.
  // TODO(mhlo-team): this can be simplified when TF moves to C++17.
  using unused = int[];
  (void)unused{0, (printField(printer, std::get<0>(printFields),
                              std::get<1>(printFields), separator),
                   0)...};
  printer << ">";
}

// Custom printer and parser for ScatterDimensionNumbersAttr.
void ScatterDimensionNumbersAttr::print(AsmPrinter& printer) const {
  printStruct(printer, "scatter",
              std::make_pair("update_window_dims", getUpdateWindowDims()),
              std::make_pair("inserted_window_dims", getInsertedWindowDims()),
              std::make_pair("scatter_dims_to_operand_dims",
                             getScatterDimsToOperandDims()),
              std::make_pair("index_vector_dim", getIndexVectorDim()));
}
Attribute ScatterDimensionNumbersAttr::parse(AsmParser& parser, Type type) {
  if (failed(parser.parseLess())) return {};
  SmallVector<int64_t> updateWindowDims;
  SmallVector<int64_t> insertedWindowDims;
  SmallVector<int64_t> scatterDimsToOperandDims;
  int64_t indexVectorDim = 0;

  if (failed(parseStruct(
          parser,
          {"update_window_dims", "inserted_window_dims",
           "scatter_dims_to_operand_dims", "index_vector_dim"},
          {[&]() { return parseDims(parser, updateWindowDims); },
           [&]() { return parseDims(parser, insertedWindowDims); },
           [&]() { return parseDims(parser, scatterDimsToOperandDims); },
           [&]() { return parser.parseInteger(indexVectorDim); }}))) {
    parser.emitError(parser.getCurrentLocation())
        << "failed parsing scatter dimension numbers attribute";
    return {};
  }

  return ScatterDimensionNumbersAttr::get(
      parser.getContext(), updateWindowDims, insertedWindowDims,
      scatterDimsToOperandDims, indexVectorDim);
}

// Custom printer and parser for GatherDimensionNumbersAttr.
void GatherDimensionNumbersAttr::print(AsmPrinter& printer) const {
  printStruct(printer, "gather", std::make_pair("offset_dims", getOffsetDims()),
              std::make_pair("collapsed_slice_dims", getCollapsedSliceDims()),
              std::make_pair("start_index_map", getStartIndexMap()),
              std::make_pair("index_vector_dim", getIndexVectorDim()));
}

Attribute GatherDimensionNumbersAttr::parse(AsmParser& parser, Type type) {
  if (failed(parser.parseLess())) return {};

  SmallVector<int64_t> offsetDims;
  SmallVector<int64_t> collapsedSliceDims;
  SmallVector<int64_t> startIndexMap;
  int64_t indexVectorDim = 0;

  if (failed(parseStruct(
          parser,
          {"offset_dims", "collapsed_slice_dims", "start_index_map",
           "index_vector_dim"},
          {[&]() { return parseDims(parser, offsetDims); },
           [&]() { return parseDims(parser, collapsedSliceDims); },
           [&]() { return parseDims(parser, startIndexMap); },
           [&]() { return parser.parseInteger(indexVectorDim); }}))) {
    parser.emitError(parser.getCurrentLocation())
        << "failed parsing gather dimension numbers attribute";
    return {};
  }

  return GatherDimensionNumbersAttr::get(parser.getContext(), offsetDims,
                                         collapsedSliceDims, startIndexMap,
                                         indexVectorDim);
}

// Custom printer and parser for DotDimensionNumbersAttr.
void DotDimensionNumbersAttr::print(AsmPrinter& printer) const {
  printStruct(
      printer, "dot",
      std::make_pair("lhs_batching_dimensions", getLhsBatchingDimensions()),
      std::make_pair("rhs_batching_dimensions", getRhsBatchingDimensions()),
      std::make_pair("lhs_contracting_dimensions",
                     getLhsContractingDimensions()),
      std::make_pair("rhs_contracting_dimensions",
                     getRhsContractingDimensions()));
}

Attribute DotDimensionNumbersAttr::parse(AsmParser& parser, Type type) {
  if (failed(parser.parseLess())) return {};

  SmallVector<int64_t> lhsBatchingDimensions;
  SmallVector<int64_t> rhsBatchingDimensions;
  SmallVector<int64_t> lhsContractingDimensions;
  SmallVector<int64_t> rhsContractingDimensions;

  if (failed(parseStruct(
          parser,
          {"lhs_batching_dimensions", "rhs_batching_dimensions",
           "lhs_contracting_dimensions", "rhs_contracting_dimensions"},
          {[&]() { return parseDims(parser, lhsBatchingDimensions); },
           [&]() { return parseDims(parser, rhsBatchingDimensions); },
           [&]() { return parseDims(parser, lhsContractingDimensions); },
           [&]() { return parseDims(parser, rhsContractingDimensions); }}))) {
    parser.emitError(parser.getCurrentLocation())
        << "failed parsing dot dimension numbers attribute";
    return {};
  }
  return DotDimensionNumbersAttr::get(
      parser.getContext(), lhsBatchingDimensions, rhsBatchingDimensions,
      lhsContractingDimensions, rhsContractingDimensions);
}

namespace {
enum NonSpatialDim : int64_t {
  IOBatch = -1,    // Input or output batch dimension
  IOFeature = -2,  // Input or output feature dimension
  KIFeature = -3,  // Kernel input feature dimension
  KOFeature = -4,  // Kernel output feature dimensions.
};

struct DenseMapInfoNonSpatialDim {
  static inline NonSpatialDim getEmptyKey() {
    return NonSpatialDim(DenseMapInfo<int64_t>::getEmptyKey());
  }

  static inline NonSpatialDim getTombstoneKey() {
    return NonSpatialDim(DenseMapInfo<int64_t>::getTombstoneKey());
  }

  static unsigned getHashValue(const NonSpatialDim& key) {
    return DenseMapInfo<int64_t>::getHashValue(key);
  }

  static bool isEqual(const NonSpatialDim& lhs, const NonSpatialDim& rhs) {
    return lhs == rhs;
  }
};

char nonSpatialDimToString(NonSpatialDim dim) {
  switch (dim) {
    case IOBatch:
      return 'b';
    case IOFeature:
      return 'f';
    case KIFeature:
      return 'i';
    case KOFeature:
      return 'o';
  }
  llvm_unreachable("Unknown NonSpatialDim");
}
}  // namespace

// Custom printer and parser for convolution attribute.
void printConvolutionDimensions(AsmPrinter& p, ConvDimensionNumbersAttr dnums) {
  // TODO(b/202040055): we should check the attribute invariant and print the
  // "raw" form if they are violated, otherwise we'll crash here.
  constexpr int64_t kUnknownDim = std::numeric_limits<int64_t>::min();
  auto printDim =
      [&](ArrayRef<int64_t> spatialDims,
          ArrayRef<std::pair<int64_t, NonSpatialDim>> nonSpatialDims) {
        int64_t numDims = 0;
        if (!spatialDims.empty()) {
          numDims =
              *std::max_element(spatialDims.begin(), spatialDims.end()) + 1;
        }
        for (const auto& dim : nonSpatialDims) {
          numDims = std::max(numDims, dim.first + 1);
        }

        llvm::SmallVector<int64_t> dims(numDims, kUnknownDim);
        // Fill each element of dims with a (< 0) NonSpatialDim enum or a (>=0)
        // spatial dimension index.
        for (const std::pair<int64_t, NonSpatialDim>& nonSpatialDim :
             nonSpatialDims) {
          dims[nonSpatialDim.first] = nonSpatialDim.second;
        }
        for (const auto& spatialDim : llvm::enumerate(spatialDims)) {
          dims[spatialDim.value()] = static_cast<int64_t>(spatialDim.index());
        }

        // Each dimension numbers will be printed as a comma separated list
        // surrounded by square brackets, e.g., [b, 0, 1, 2, f]
        p << '[';
        llvm::interleaveComma(dims, p, [&](int64_t dim) {
          if (dim == kUnknownDim) {
            p << "?";
          } else if (dim >= 0) {
            p << dim;
          } else {
            p << nonSpatialDimToString(static_cast<NonSpatialDim>(dim));
          }
        });
        p << ']';
      };

  printDim(dnums.getInputSpatialDimensions(),
           {{dnums.getInputBatchDimension(), IOBatch},
            {dnums.getInputFeatureDimension(), IOFeature}});
  p << "x";
  printDim(dnums.getKernelSpatialDimensions(),
           {{dnums.getKernelInputFeatureDimension(), KIFeature},
            {dnums.getKernelOutputFeatureDimension(), KOFeature}});
  p << "->";
  printDim(dnums.getOutputSpatialDimensions(),
           {{dnums.getOutputBatchDimension(), IOBatch},
            {dnums.getOutputFeatureDimension(), IOFeature}});
}

void printConvolutionDimensions(AsmPrinter& p, Operation*,
                                ConvDimensionNumbersAttr dnums) {
  printConvolutionDimensions(p, dnums);
}

// Custom printer and parser for ConvDimensionNumbersAttr.
void ConvDimensionNumbersAttr::print(AsmPrinter& printer) const {
  printer << "<";
  printConvolutionDimensions(printer, *this);
  printer << ">";
}

// If the attribute is written with `#mhlo.conv raw<`, we parse it as a struct
// instead of the compressed format. This enables writing tests covering
// impossible/invalid internal representation for the attribute.
static ParseResult parseConvolutionDimensionsRaw(
    AsmParser& parser, ConvDimensionNumbersAttr& dnums) {
  int64_t inputBatchDimension = 0;
  int64_t inputFeatureDimension = 0;
  SmallVector<int64_t> inputSpatialDimensions;
  int64_t kernelInputFeatureDimension = 0;
  int64_t kernelOutputFeatureDimension = 0;
  SmallVector<int64_t> kernelSpatialDimensions;
  int64_t outBatchDimension = 0;
  int64_t outputFeatureDimension = 0;
  SmallVector<int64_t> outputSpatialDimensions;
  if (failed(parseStruct(
          parser,
          {"input_batch_dimension", "input_feature_dimension",
           "input_spatial_dimensions", "kernel_input_feature_dimension",
           "kernel_output_feature_dimension", "kernel_spatial_dimensions",
           "output_batch_dimension", "output_feature_dimension",
           "output_spatial_dimensions"},
          {
              [&]() { return parser.parseInteger(inputBatchDimension); },
              [&]() { return parser.parseInteger(inputFeatureDimension); },
              [&]() { return parseDims(parser, inputSpatialDimensions); },
              [&]() {
                return parser.parseInteger(kernelInputFeatureDimension);
              },
              [&]() {
                return parser.parseInteger(kernelOutputFeatureDimension);
              },
              [&]() { return parseDims(parser, kernelSpatialDimensions); },
              [&]() { return parser.parseInteger(outBatchDimension); },
              [&]() { return parser.parseInteger(outputFeatureDimension); },
              [&]() { return parseDims(parser, outputSpatialDimensions); },
          }))) {
    parser.emitError(parser.getCurrentLocation())
        << "failed parsing dot dimension numbers attribute";
    return failure();
  }
  dnums = ConvDimensionNumbersAttr::get(
      parser.getBuilder().getContext(), inputBatchDimension,
      inputFeatureDimension, inputSpatialDimensions,
      kernelInputFeatureDimension, kernelOutputFeatureDimension,
      kernelSpatialDimensions, outBatchDimension, outputFeatureDimension,
      outputSpatialDimensions);
  return success();
}

ParseResult parseConvolutionDimensions(AsmParser& parser,
                                       ConvDimensionNumbersAttr& dnums) {
  // Parsing a single set of dim numbers gives the spatial dimensions as a
  // single ArrayRef<int64_t> and a list of non-spatial dimensions as
  // IntegerAttrs (indexed by the NonSpatialDim enum).
  using parse_dim_result_t =
      std::pair<llvm::SmallVector<int64_t>,
                llvm::SmallDenseMap<NonSpatialDim, int64_t, 4,
                                    DenseMapInfoNonSpatialDim>>;

  // Note that the allowed_non_spatial_dims is a set (as opposed to unordered
  // set) because its used to print a list of allowed non spatial dims in the
  // error messages, so making it a set keeps the error messages deterministic.
  auto parseDims =
      [&](std::set<NonSpatialDim, std::greater<>> allowedNonSpatialDims,
          parse_dim_result_t& parsedDims) -> ParseResult {
    auto& spatialDims = std::get<0>(parsedDims);
    auto& nonSpatialDims = std::get<1>(parsedDims);
    spatialDims.clear();
    nonSpatialDims.clear();

    // Parse the starting [
    if (parser.parseLSquare()) {
      return failure();
    }

    llvm::SmallDenseMap<int64_t, int64_t> spatialDimsMap;
    constexpr int64_t kInvalidDimension = -1;
    // Keep track of the maximum spatial dimension parsed as we expect to see
    // all the dimensions from 0 to maximum dimension parsed.
    int64_t maxParsedSpatialDim = kInvalidDimension;

    int64_t index = 0;
    do {
      int64_t spatialDim;
      auto dimLocation = parser.getCurrentLocation();
      OptionalParseResult parseResult = parser.parseOptionalInteger(spatialDim);
      if (parseResult.has_value()) {
        if (parseResult.value().failed()) {
          return failure();
        }
        // We were successful in parsing an integer. Check if it is a valid
        // dimension (non-negative and no duplicate) and add its index to the
        // spatial dims map.
        if (spatialDim < 0)
          return parser.emitError(dimLocation)
                 << "Unexpected dimension " << spatialDim;
        if (!spatialDimsMap
                 .insert(std::pair<int64_t, int64_t>(spatialDim, index))
                 .second)
          return parser.emitError(dimLocation)
                 << "Duplicate entries for spatial dimension " << spatialDim;
        maxParsedSpatialDim = std::max(spatialDim, maxParsedSpatialDim);
      } else if (!parser.parseOptionalQuestion()) {
        // Do nothing other than increment `index` at the bottom of the loop;
        // '?' means "unknown dimension", and it's not represented in the
        // return value of this function.
      } else {
        // We did not parse an integer or question mark. We expect a keyword
        // token.
        StringRef keyword;
        if (parser.parseKeyword(&keyword)) {
          return failure();
        }
        if (keyword.size() != 1 || allowedNonSpatialDims.empty()) {
          return parser.emitError(dimLocation, "Unexpected keyword ")
                 << keyword;
        }
        // Check if the keyword matches one of the allowed non-spatial dims.
        // If so, add it to the non_spatial dims and remove it from the
        // allowed set so that it won't be allowed again.
        bool isAllowed = false;
        for (NonSpatialDim allowed : allowedNonSpatialDims) {
          if (keyword[0] == nonSpatialDimToString(allowed)) {
            nonSpatialDims.insert({allowed, index});
            allowedNonSpatialDims.erase(allowed);
            isAllowed = true;
            break;
          }
        }

        if (!isAllowed) {
          mlir::InFlightDiagnostic diag =
              parser.emitError(dimLocation, "Unexpected dimension ");
          diag << keyword << ", expecting ";
          llvm::interleaveComma(
              allowedNonSpatialDims, diag,
              [&](NonSpatialDim dim) { diag << nonSpatialDimToString(dim); });
          return diag;
        }
      }
      index++;
    } while (parser.parseOptionalComma().succeeded());

    // Make sure all expected non-spatial dimensions are parsed.
    if (!allowedNonSpatialDims.empty()) {
      mlir::InFlightDiagnostic diag =
          parser.emitError(parser.getCurrentLocation(), "Expected dimensions ");
      llvm::interleaveComma(
          allowedNonSpatialDims, diag,
          [&](NonSpatialDim dim) { diag << nonSpatialDimToString(dim); });
      diag << " not specified";
      return diag;
    }

    // parse ending ]
    if (parser.parseRSquare()) {
      return failure();
    }

    // Number of expected spatial dimensions is one more than the maximum parsed
    // spatial dimension. For example, if we parse [0, 3, 2, b, i, 1], then the
    // maximum parsed spatial dimension is 3 and the number of expected spatial
    // dimensions is 4.
    int64_t numSpatialDimensions = maxParsedSpatialDim + 1;
    spatialDims.resize(numSpatialDimensions);
    // Store spatial dimensions in a vector which maps spatial dim (vector
    // index) -> index in the tensor dimensions. For example, for parsed
    // dimension numbers [0, 3, 2, b, i, 1] the spatial dimension vector would
    // be [0, 5, 2, 1].
    //
    // Get all the unspecified spatial dimensions to throw a more descriptive
    // error later.
    llvm::SmallVector<int64_t> unspecifiedSpatialDims;
    constexpr int kPrintUnspecifiedDimsMax = 10;
    for (int dim = 0; dim < numSpatialDimensions; ++dim) {
      auto it = spatialDimsMap.find(dim);
      if (it == spatialDimsMap.end()) {
        // Have an upper bound on the number of unspecified dimensions to print
        // in the error message.
        if (unspecifiedSpatialDims.size() < kPrintUnspecifiedDimsMax)
          unspecifiedSpatialDims.push_back(dim);
        continue;
      }
      spatialDims[dim] = it->second;
    }

    // Verify that we got all spatial dimensions between 0 and maximum parsed
    // spatial dimension.
    if (!unspecifiedSpatialDims.empty()) {
      mlir::InFlightDiagnostic diag = parser.emitError(
          parser.getCurrentLocation(), "Expected spatial dimensions ");
      llvm::interleaveComma(unspecifiedSpatialDims, diag);
      diag << " not specified";
      return diag;
    }

    return success();
  };

  parse_dim_result_t parsedDims;
  if (parseDims({IOBatch, IOFeature}, parsedDims)) {
    return failure();
  }
  llvm::SmallVector<int64_t> inputSpatialDimensions = parsedDims.first;
  int64_t inputBatchDimension = parsedDims.second[IOBatch];
  int64_t inputFeatureDimension = parsedDims.second[IOFeature];
  if (parser.parseKeyword("x")) return failure();
  if (parseDims({KIFeature, KOFeature}, parsedDims)) {
    return failure();
  }
  llvm::SmallVector<int64_t> kernelSpatialDimensions = parsedDims.first;
  int64_t kernelInputFeatureDimension = parsedDims.second[KIFeature];
  int64_t kernelOutputFeatureDimension = parsedDims.second[KOFeature];
  if (parser.parseArrow()) {
    return failure();
  }
  if (parseDims({IOBatch, IOFeature}, parsedDims)) {
    return failure();
  }
  llvm::SmallVector<int64_t> outputSpatialDimensions = parsedDims.first;
  const int64_t outBatchDimension = parsedDims.second[IOBatch];
  const int64_t outputFeatureDimension = parsedDims.second[IOFeature];
  dnums = ConvDimensionNumbersAttr::get(
      parser.getBuilder().getContext(), inputBatchDimension,
      inputFeatureDimension, inputSpatialDimensions,
      kernelInputFeatureDimension, kernelOutputFeatureDimension,
      kernelSpatialDimensions, outBatchDimension, outputFeatureDimension,
      outputSpatialDimensions);

  return success();
}

Attribute ConvDimensionNumbersAttr::parse(AsmParser& parser, Type type) {
  if (failed(parser.parseLess())) return {};
  ConvDimensionNumbersAttr dnums;
  if (succeeded(parser.parseOptionalKeyword("raw"))) {
    if (failed(parseConvolutionDimensionsRaw(parser, dnums))) return {};
    return dnums;
  }
  if (failed(parseConvolutionDimensions(parser, dnums))) return {};
  if (failed(parser.parseGreater())) return {};
  return dnums;
}

// Custom printer and parser for ArgResultAliasAttr.
constexpr char kMustAlias[] = "must_alias";
constexpr char kResult[] = "result_index";
constexpr char kArgTupleIndices[] = "tuple_indices";

void ArgResultAliasAttr::print(AsmPrinter& printer) const {
  printer << "<";

  // The attribute can have empty tuple indices. Only print argument tuple
  // indices if they are non-empty.
  if (!getArgTupleIndices().empty())
    printer << kArgTupleIndices << " = [" << getArgTupleIndices() << "], ";

  // Print the result index followed by any result tuple indices if present.
  printer << kResult << " = [";
  printer << getResultIndex();
  if (!getResultTupleIndices().empty()) {
    printer << ", " << getResultTupleIndices();
  }
  printer << "]";

  // Print the "must_alias" keyword if this is a must alias, otherwise skip.
  if (getIsMustAlias()) printer << ", " << kMustAlias;

  printer << ">";
}

Attribute ArgResultAliasAttr::parse(AsmParser& parser, Type type) {
  if (failed(parser.parseLess())) return {};
  llvm::SmallVector<int64_t> argTupleIndices;
  // The first element of result indices holds the aliased result index and the
  // remaining elements are the result tuple indices.
  llvm::SmallVector<int64_t> resultIndices;
  bool isMustAlias = false;

  // This conveys to parseStruct that keyword "must_alias" (3rd field) is not
  // followed by a "=", but other fields are.
  llvm::SmallVector<bool, 3> parseEqual = {true, true, false};

  if (failed(parseStruct(parser, {kArgTupleIndices, kResult, kMustAlias},
                         {[&]() { return parseDims(parser, argTupleIndices); },
                          [&]() {
                            // Since the first element is the index of result,
                            // at least one element is expected.
                            return parseDimsWithMinimumElements(
                                parser, resultIndices, /*minElements=*/1);
                          },
                          [&]() {
                            // always succeeds if the keyword "must_alias" was
                            // parsed
                            isMustAlias = true;
                            return success();
                          }},
                         parseEqual))) {
    parser.emitError(parser.getCurrentLocation())
        << "failed parsing argument-result alias attribute";
    return {};
  }

  int64_t resultIndex = resultIndices[0];
  auto resultTupleIndices =
      ArrayRef<int64_t>{resultIndices.begin() + 1, resultIndices.end()};

  return ArgResultAliasAttr::get(parser.getContext(), argTupleIndices,
                                 resultIndex, resultTupleIndices, isMustAlias);
}

// Returns the element type pointed to by `indices` in type `t`. If the indices
// are invalid, returns nullptr.
static Type getTypeFromTupleIndices(Type type, ArrayRef<int64_t> indices) {
  Type current = type;
  for (auto index : indices) {
    TupleType tupleType = current.dyn_cast<TupleType>();
    if (!tupleType || index >= static_cast<int64_t>(tupleType.size()))
      return {};
    current = tupleType.getType(index);
  }
  return current;
}

static LogicalResult verifyArgResultAliasAttr(StringAttr attrName,
                                              ArgResultAliasAttr aliasAttr,
                                              unsigned argIndex,
                                              Operation* op) {
  // The attribute can only be applied to function-like operations.
  if (!isa<mlir::FunctionOpInterface>(op))
    return op->emitOpError() << "attribute " << attrName
                             << " can only be used on function-like operations";

  // Verify there are no negative indices.
  auto tupleIndices = llvm::concat<const int64_t>(
      aliasAttr.getArgTupleIndices(), aliasAttr.getResultTupleIndices());
  if (llvm::any_of(tupleIndices, [](const int64_t val) { return val < 0; }) ||
      aliasAttr.getResultIndex() < 0)
    return op->emitOpError()
           << "attribute " << attrName
           << " expects all argument and result indices to be >= 0";

  // Verify that the result index is not out of range. Since the attribute is a
  // function argument attribute, the argument index is always correct when this
  // verifier is called.
  FunctionOpInterface funcOp = cast<FunctionOpInterface>(op);
  ArrayRef<Type> argTypes = funcOp.getArgumentTypes();
  ArrayRef<Type> resultTypes = funcOp.getResultTypes();
  if (aliasAttr.getResultIndex() >= static_cast<int64_t>(resultTypes.size()))
    return op->emitOpError()
           << "attribute " << attrName
           << " result index is out of range, must be <" << resultTypes.size();

  // Verify that argument and result types pointed to by the indices are valid
  // and compatible.
  Type argType = getTypeFromTupleIndices(argTypes[argIndex],
                                         aliasAttr.getArgTupleIndices());
  if (!argType)
    return op->emitOpError()
           << "attribute " << attrName << " argument tuple indices are invalid";
  Type resultType =
      getTypeFromTupleIndices(resultTypes[aliasAttr.getResultIndex()],
                              aliasAttr.getResultTupleIndices());
  if (!resultType)
    return op->emitOpError()
           << "attribute " << attrName << " result tuple indices are invalid";

  if (failed(mlir::verifyCompatibleShape(argType, resultType)) ||
      getElementTypeOrSelf(argType) != getElementTypeOrSelf(resultType))
    return op->emitOpError() << "attribute " << attrName
                             << " aliases do not have compatible types, "
                             << argType << " vs. " << resultType;
  return success();
}

// Each CrossProgramPrefetchAttr specifies a parameter and a ShapeIndex
// (1) the parameter must be valid
// (2) there must be a subshape at the given indices
LogicalResult verifyCrossProgramPrefetchAttr(CrossProgramPrefetchAttr cpp,
                                             ModuleOp module) {
  func::FuncOp main = module.lookupSymbol<func::FuncOp>("main");
  if (cpp.getParameter() >= main.getNumArguments())
    return module->emitOpError()
           << "cross_program_prefetch: parameter " << cpp.getParameter()
           << " out of range. main has only " << main.getNumArguments()
           << " arguments";
  auto type = getTypeFromTupleIndices(main.getArgument(cpp.getParameter())
                                          .getType()
                                          .dyn_cast_or_null<TupleType>(),
                                      cpp.getIndices());
  if (!type)
    return module->emitOpError()
           << "cross_program_prefetch: no subshape at given index: "
           << cpp.getIndices();
  return success();
}

// Each DynamicParameterBinding specifies a dynamic parameter, a target
// parameter, a shape index of each and a target dimension.
// (1) the parameters must be valid
// (2) there must be a subshape at the given ShapeIndex for each parameter
// (3) the given subshape for the dynamic parameter must be of type tensor<i32>
// (4) there must be a dimension at the given dimension number for the given
// subshape of the target parameter
// (5) that dimension is dynamic
LogicalResult verifyDynamicParameterBinding(DynamicParameterBindingAttr bind,
                                            ModuleOp module) {
  func::FuncOp main = module.lookupSymbol<func::FuncOp>("main");

  // (1)
  if (bind.getDynamicParamNum() >= main.getNumArguments() ||
      bind.getTargetParamNum() >= main.getNumArguments())
    return module->emitOpError()
           << "dynamic_parameter_binding: parameters "
           << bind.getDynamicParamNum() << " and " << bind.getTargetParamNum()
           << " out of range. main has only " << main.getNumArguments()
           << " arguments";

  // (2)
  auto dynamicParamSubshape =
      getTypeFromTupleIndices(
          main.getArgument(bind.getDynamicParamNum()).getType(),
          bind.getDynamicParamIndices())
          .dyn_cast_or_null<RankedTensorType>();
  if (!dynamicParamSubshape)
    return module->emitOpError() << "dynamic_parameter_binding: no ranked "
                                    "tensor type at dynamic_param_indices: "
                                 << bind.getDynamicParamIndices();
  // (3)
  if (dynamicParamSubshape.getRank() != 0 ||
      !dynamicParamSubshape.getElementType().isInteger(32))
    return module->emitOpError()
           << "dynamic_parameter_binding: dynamic size must be tensor<i32>";

  // (2)
  auto targetParamSubshape =
      getTypeFromTupleIndices(
          main.getArgument(bind.getTargetParamNum()).getType(),
          bind.getTargetParamIndices())
          .dyn_cast_or_null<RankedTensorType>();
  if (!targetParamSubshape)
    return module->emitOpError() << "dynamic_parameter_binding: no ranked "
                                    "tensor type at target_param_indices: "
                                 << bind.getTargetParamIndices();
  // (4)
  if (targetParamSubshape.getRank() <= bind.getTargetParamDimNum())
    return module->emitOpError()
           << "dynamic_parameter_binding: no dimension number "
           << bind.getTargetParamDimNum() << " in target subshape "
           << targetParamSubshape;

  // (5)
  if (!targetParamSubshape.isDynamicDim(bind.getTargetParamDimNum()))
    return module->emitOpError()
           << "dynamic_parameter_binding: dimension number "
           << bind.getTargetParamDimNum() << " in target subshape "
           << targetParamSubshape << " is not dynamic";

  return success();
}

//===----------------------------------------------------------------------===//
// Builder utilities
//===----------------------------------------------------------------------===//

// Builds the region `body` for mhlo.sort's comparator: for each type in
// `element_types`, create two block arguments, one for lhs and one for rhs, and
// generates mhlo.compare op to compare them with the given `direction`.
//
// Note that this right now only does comparision on the first pair of block
// arguments.
static void buildSortComparisonBody(llvm::ArrayRef<Type> elementTypes,
                                    ComparisonDirection direction,
                                    llvm::Optional<StringRef> compareType,
                                    Region* body, OpBuilder* builder) {
  OpBuilder::InsertionGuard insertionPointGurad(*builder);

  Location loc = body->getLoc();
  Block* block = builder->createBlock(body);
  // Add two arguments for each element type.
  for (Type elementType : elementTypes) {
    TensorType tensorType = RankedTensorType::get({}, elementType);
    block->addArguments({tensorType, tensorType},
                        SmallVector<Location, 2>(2, loc));
  }

  ComparisonType typeAttr;
  if (compareType)
    typeAttr = symbolizeComparisonType(*compareType).value();
  else
    typeAttr = ComparisonType::NOTYPE;
  Value compare = builder->create<mhlo::CompareOp>(
      loc, block->getArgument(0), block->getArgument(1), direction, typeAttr);

  builder->create<mhlo::ReturnOp>(loc, compare);
}

SortOp createSortOp(PatternRewriter* rewriter, const Location& loc,
                    const llvm::ArrayRef<Value>& operands,
                    const llvm::ArrayRef<Type>& elementTypes, int64_t dimension,
                    bool isStable, ComparisonDirection direction) {
  assert(!operands.empty() && "No operands to sort");
  // Create the sort op.
  auto sortOp =
      rewriter->create<mhlo::SortOp>(loc, operands, dimension, isStable);

  // Use TOTALORDER comparison type instead of the default comparison if the
  // element type is of type float.
  llvm::Optional<StringRef> compareType = std::nullopt;
  for (auto const& elementType : elementTypes)
    if (elementType.isa<FloatType>()) {
      compareType.emplace("TOTALORDER");
      break;
    }
  buildSortComparisonBody(elementTypes, direction, compareType,
                          &sortOp.getComparator(), rewriter);
  return sortOp;
}

//===----------------------------------------------------------------------===//
// MHLO Dialect Hooks
//===----------------------------------------------------------------------===//

Operation* MhloDialect::materializeConstant(OpBuilder& builder, Attribute value,
                                            Type type, Location loc) {
  auto elementsAttr = value.dyn_cast<ElementsAttr>();
  // HLO dialect constants only support ElementsAttr unlike standard dialect
  // constant which supports all attributes.
  if (!elementsAttr) return nullptr;
  // HLO dialect constants require the type of value and result to match.
  if (type != elementsAttr.getType()) return nullptr;

  return builder.create<mhlo::ConstantOp>(loc, type, elementsAttr);
}

int64_t getNumLeafBuffers(Type type) {
  if (auto tuple = type.dyn_cast<TupleType>()) {
    auto ans = 0;
    for (auto type : tuple.getTypes()) ans += getNumLeafBuffers(type);
    return ans;
  } else {
    return 1;
  }
}

LogicalResult MhloDialect::verifyRegionArgAttribute(Operation* op,
                                                    unsigned /*regionIndex*/,
                                                    unsigned argIndex,
                                                    NamedAttribute attr) {
  if (auto aliasAttr = attr.getValue().dyn_cast<ArgResultAliasAttr>()) {
    if (failed(
            verifyArgResultAliasAttr(attr.getName(), aliasAttr, argIndex, op)))
      return failure();
  }
  if (attr.getName() == "mhlo.parameter_replication") {
    auto arrayAttr = attr.getValue().dyn_cast<ArrayAttr>();
    if (!arrayAttr)
      return op->emitOpError() << "parameter_replication: must be an array";
    auto func = dyn_cast<mlir::FunctionOpInterface>(op);
    if (!func) {
      return op->emitOpError()
             << "has parameter_replication but is not a function";
    }
    // parameter_replication = [] or [false] is equivalent to
    // [false,...,false] and parameter_replication = [true] means
    // [true,...,true]
    if (arrayAttr.size() == 0 || arrayAttr.size() == 1) return success();
    auto num_leaf_buffers =
        getNumLeafBuffers(func.getArgumentTypes()[argIndex]);
    if ((size_t)num_leaf_buffers != arrayAttr.size())
      return op->emitOpError()
             << "parameter_replication: arg " << argIndex << " has "
             << num_leaf_buffers << " leaf_buffers, but parameter_replication"
             << " expects " << arrayAttr.size();
  }
  return success();
}

LogicalResult MhloDialect::verifyOperationAttribute(Operation* op,
                                                    NamedAttribute attr) {
  if (auto aliasAttr = attr.getValue().dyn_cast<ArgResultAliasAttr>()) {
    if (!isa<mlir::FunctionOpInterface>(op))
      return op->emitOpError()
             << "attribute " << attr.getName()
             << " can only be used on function-like operations";
  }
  if (attr.getName() == "mhlo.cross_program_prefetches") {
    auto arrayAttr = attr.getValue().dyn_cast<ArrayAttr>();
    if (!arrayAttr)
      return op->emitOpError() << "cross_program_prefetches must be an array";
    for (auto attrElt : arrayAttr) {
      auto prefetchAttr = attrElt.dyn_cast<CrossProgramPrefetchAttr>();
      if (!prefetchAttr)
        return op->emitOpError() << "cross_program_prefetches must be an array "
                                    "of cross_program_prefetch attrs";
      auto module = dyn_cast<ModuleOp>(op);
      if (!module)
        return op->emitOpError()
               << "has cross_program_prefetches but is not a module";
      auto res = verifyCrossProgramPrefetchAttr(prefetchAttr, module);
      if (failed(res)) return res;
    }
  }
  if (attr.getName() == "mhlo.dynamic_parameter_bindings") {
    auto arrayAttr = attr.getValue().dyn_cast<ArrayAttr>();
    if (!arrayAttr)
      return op->emitOpError() << "dynamic_parameter_bindings must be an array";
    auto module = dyn_cast<ModuleOp>(op);
    if (!module)
      return op->emitOpError()
             << "has dynamic_parameter_bindings but is not a module";
    for (auto attrElt : arrayAttr) {
      auto bindingAttr = attrElt.dyn_cast<DynamicParameterBindingAttr>();
      if (!bindingAttr)
        return op->emitOpError() << "dynamic_parameter_bindings must be an "
                                    "array of dynamic_parameter_binding attrs";
      auto res = verifyDynamicParameterBinding(bindingAttr, module);
      if (failed(res)) return res;
    }
  }
  if (attr.getName() == "mhlo.spmd_parameters_sharding") {
    auto arrayAttr = attr.getValue().dyn_cast<ArrayAttr>();
    if (!arrayAttr)
      return op->emitOpError() << "spmd_parameters_sharding: must be an array";
    auto module = dyn_cast<ModuleOp>(op);
    if (!module)
      return op->emitOpError()
             << "has spmd_paramters_sharding but is not a module";
    // Check that the "main" function exists:
    auto main = module.lookupSymbol<mlir::func::FuncOp>("main");
    if (!main)
      return module.emitOpError() << "spmd_parameters_sharding: main not found";
    if (main.getNumArguments() != arrayAttr.size())
      return module.emitOpError()
             << "spmd_parameters_sharding: main has " << main.getNumArguments()
             << " arguments, but spmd_parameters_sharding expects "
             << arrayAttr.size();
  }
  return success();
}

}  // namespace mhlo
}  // namespace mlir
