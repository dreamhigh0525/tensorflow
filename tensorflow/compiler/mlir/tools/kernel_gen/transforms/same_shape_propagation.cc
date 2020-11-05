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

// This file contains the analysis and transformation to rewrite kernel
// functions such that they use a single set of arguments for the strides and
// sizes of operands with equal shapes.

#include <memory>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/GPU/GPUDialect.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/lhlo_ops.h"
#include "tensorflow/compiler/mlir/tools/kernel_gen/ir/tf_framework_ops.h"
#include "tensorflow/compiler/mlir/tools/kernel_gen/transforms/passes.h"

namespace {

using mlir::ArrayRef;
using mlir::SmallVector;
using mlir::Value;

/// Represents a value or constant. Used to unify operands for operations that
/// take both ssa values and attributes.
struct ValueOrConst {
  explicit ValueOrConst(Value v) : value_or_constant(v), is_constant(false) {}
  explicit ValueOrConst(int64_t c) : value_or_constant(c), is_constant(true) {}

  Value value() const {
    assert(!is_constant);
    return value_or_constant.value;
  }

  int64_t constant() const {
    assert(is_constant);
    return value_or_constant.constant;
  }

  bool isConstant() const { return is_constant; }

 private:
  union ValueOrConstStorage {
    explicit ValueOrConstStorage(Value v) : value(v) {}
    explicit ValueOrConstStorage(size_t c) : constant(c) {}

    Value value;
    int64_t constant;
  } value_or_constant;

  bool is_constant;
};

llvm::hash_code hash_value(ValueOrConst value) {
  return value.isConstant() ? static_cast<llvm::hash_code>(value.constant())
                            : mlir::hash_value(value.value());
}

bool operator==(ValueOrConst lhs, ValueOrConst rhs) {
  if (lhs.isConstant()) {
    return rhs.isConstant() && lhs.constant() == rhs.constant();
  } else {
    return !rhs.isConstant() && lhs.value() == rhs.value();
  }
}

/// Represents a shape, as either a single ssa value that represents the entire
/// shape vector or as a vector of ssa values representing scalars.
struct ShapeValue {
  explicit ShapeValue(Value vector)
      : shape({ValueOrConst{vector}}), is_vector(true) {}
  explicit ShapeValue(ValueOrConst vector) : shape({vector}), is_vector(true) {
    assert(!vector.isConstant());
  }
  template <typename T>
  explicit ShapeValue(T values)
      : shape(values.begin(), values.end()), is_vector(false) {}

  ValueOrConst vector() const {
    assert(is_vector);
    return shape.front();
  }

  ArrayRef<ValueOrConst> scalars() const {
    assert(!is_vector);
    return llvm::makeArrayRef(shape);
  }

  bool isVector() const { return is_vector; }

 private:
  SmallVector<ValueOrConst, 4> shape;
  bool is_vector;
};

llvm::hash_code hash_value(ShapeValue shape) {
  return shape.isVector() ? hash_value(shape.vector())
                          : hash_value(shape.scalars());
}

bool operator==(ShapeValue lhs, ShapeValue rhs) {
  if (lhs.isVector()) {
    return rhs.isVector() && lhs.vector() == rhs.vector();
  } else {
    return !rhs.isVector() && lhs.scalars() == rhs.scalars();
  }
}

}  // namespace

namespace llvm {

template <>
struct DenseMapInfo<ShapeValue> {
  static ShapeValue getEmptyKey() {
    return ShapeValue(DenseMapInfo<mlir::Value>::getEmptyKey());
  }
  static ShapeValue getTombstoneKey() {
    return ShapeValue(DenseMapInfo<mlir::Value>::getTombstoneKey());
  }
  static unsigned getHashValue(ShapeValue shape) { return hash_value(shape); }
  static bool isEqual(ShapeValue LHS, ShapeValue RHS) { return LHS == RHS; }
};

}  // namespace llvm

namespace mlir {
namespace kernel_gen {
namespace transforms {

namespace {

#define GEN_PASS_CLASSES
#include "tensorflow/compiler/mlir/tools/kernel_gen/transforms/kernel_gen_passes.h.inc"

// A basic shape equality inference. This should be superceeded by a proper
// inference once available. Until then, we just build this out to the needs of
// the kernel generator project.
class ShapeEqualityKnowledge {
 public:
  /// Checks all operations for potential shape equality of their respective
  /// results.
  void build(FuncOp function) {
    function.walk([&](Operation *op) {
      if (auto reshape = dyn_cast<lmhlo::ReshapeMemRefCastOp>(op)) {
        registerAssociation(ShapeValue{reshape.operand()}, reshape.result());
        return;
      }
      if (auto alloc = dyn_cast<AllocOp>(op)) {
        SmallVector<ValueOrConst, 4> shape;
        ShapedType type = alloc.getResult().getType().cast<ShapedType>();
        fillShapeFromAllocLike(alloc.getDynamicSizes(), type, shape);
        registerAssociation(ShapeValue{shape}, alloc.getResult());
        return;
      }
      if (auto alloc = dyn_cast<tf_framework::TFAllocOp>(op)) {
        // Construct a symbol representing the allocated shape.
        SmallVector<ValueOrConst, 4> shape;
        ShapedType type = alloc.getResult().getType().cast<ShapedType>();
        fillShapeFromAllocLike(alloc.dyn_sizes(), type, shape);
        registerAssociation(ShapeValue{shape}, alloc.getResult());
        return;
      }
    });
  }

  /// Checks whether `one` and `other` are known to have the same shape and
  /// strides.
  bool haveSameShape(Value one, Value other) {
    return equal_shapes_.isEquivalent(one.getAsOpaquePointer(),
                                      other.getAsOpaquePointer());
  }

 private:
  static void fillShapeFromAllocLike(mlir::OperandRange operands,
                                     ShapedType type,
                                     SmallVectorImpl<ValueOrConst> &shape) {
    assert(type.hasRank());
    auto dynamic_sizes = operands.begin();
    for (auto extent : type.getShape()) {
      shape.push_back(ShapedType::isDynamic(extent)
                          ? ValueOrConst{*(dynamic_sizes++)}
                          : ValueOrConst{extent});
    }
  }

  /// Registers the value `value` to have the shape represented by `shape`. If
  /// `shape` has been registered before, place `value` into the same
  /// equivalence class. Otherwise register `value` as an equivalence class of
  /// its own.
  void registerAssociation(ShapeValue shape, Value value) {
    auto insert_symbolic = symbolic_shapes_.insert({shape, value});
    if (insert_symbolic.second) {
      equal_shapes_.insert(value.getAsOpaquePointer());
      // We have seen this symbolic shape for the first time. Try to match it
      // with a vector or shape we already know and alias classes if possible.
      // This could be based on shape dialect if we weren't late in the
      // lowering.
      tryEvaluateShapeToRoot(shape, value);
    } else {
      equal_shapes_.unionSets(
          insert_symbolic.first->second.getAsOpaquePointer(),
          value.getAsOpaquePointer());
    }
  }

  /// Follows the definition chains of the ShapeValue `shape` to identify cases
  /// where `shape` is derived from some other value's shape. In such case, the
  /// equivalence classes of that other value and `value` are unioned.
  void tryEvaluateShapeToRoot(ShapeValue shape, Value value) {
    // Just some pattern matching for common cases here.
    if (!shape.isVector()) {
      // Patterns that revolve around scalars.
      // Check whether the scalars are all dim operations for some other memref.
      // TODO(herhut): Use pattern match infra here.
      Value candidate;
      for (auto extent : llvm::enumerate(shape.scalars())) {
        if (extent.value().isConstant()) {
          candidate = {};
          break;
        }
        if (auto dimOp = extent.value().value().getDefiningOp<mlir::DimOp>()) {
          auto dimIndex = dimOp.getConstantIndex();
          if (!dimIndex.hasValue() || (dimIndex.getValue() != extent.index())) {
            candidate = {};
            break;
          }
          if (candidate && candidate != dimOp.memrefOrTensor()) {
            candidate = {};
            break;
          }
          candidate = dimOp.memrefOrTensor();
        }
      }
      if (candidate) {
        equal_shapes_.unionSets(candidate.getAsOpaquePointer(),
                                value.getAsOpaquePointer());
      }
    } else {
      // Patterns that revovlve around vector representation.
    }
  }

  // These are values with identical shapes (or rather their opaque pointers).
  llvm::EquivalenceClasses<void *> equal_shapes_;
  // A map from a value that encodes a shape to a value that has this shape.
  llvm::DenseMap<ShapeValue, Value> symbolic_shapes_;
};

/// For arguments to kernels that have the same shape, use the stride and
/// shape information of the left-most argument inside of the kernel function.
/// That way, llvm can CSE index computations on same-shaped inputs.
struct PropagateShapeKnowledgeToKernels
    : public PropagateShapeKnowledgeToKernelsBase<
          PropagateShapeKnowledgeToKernels> {
  void runOnFunction() override {
    ShapeEqualityKnowledge knowledge;

    knowledge.build(getFunction());

    getFunction().walk([&](gpu::LaunchFuncOp launch) {
      auto module = launch.getParentOfType<ModuleOp>();
      auto kernel = module.lookupSymbol<LLVM::LLVMFuncOp>(launch.kernel());

      if (!kernel || kernel.isExternal()) return;

      llvm::SmallVector<std::pair<Value, int>, 4> seen_memrefs;
      int kernel_p = 0;
      for (auto operand : launch.operands()) {
        auto memref = operand.getType().dyn_cast<MemRefType>();
        if (!memref) {
          // Scalar argument, advance kernel position by one.
          kernel_p++;
          continue;
        }
        for (auto previous : seen_memrefs) {
          if (!knowledge.haveSameShape(operand, previous.first)) {
            continue;
          }
          // We use the first equality found and replace uses of corresponding
          // size and stride information here.
          // TODO(herhut): This is not safe if we had a cast operation
          //     inbetween that changes stride information. The current
          //     analysis above would not consider this equal.
          // We need to replace sizes and strides.
          auto args_to_replace = memref.getRank() * 2;
          int previous_args_pos = previous.second;
          auto previous_args = kernel.getArguments()
                                   .drop_front(previous_args_pos + 3)
                                   .take_front(args_to_replace);
          auto current_args = kernel.getArguments()
                                  .drop_front(kernel_p + 3)
                                  .take_back(args_to_replace);
          for (auto pair : llvm::zip(previous_args, current_args)) {
            std::get<1>(pair).replaceAllUsesWith(std::get<0>(pair));
          }
          break;
        }
        seen_memrefs.push_back({operand, kernel_p});
        // Advance base, aligned, offset, strides and sizes many arguments.
        kernel_p += memref.getRank() * 2 + 3;
      }
    });
  }
};

}  // namespace

std::unique_ptr<FunctionPass> CreatePropagateShapeKnowledgeToKernels() {
  return std::make_unique<PropagateShapeKnowledgeToKernels>();
}

}  // namespace transforms
}  // namespace kernel_gen
}  // namespace mlir
