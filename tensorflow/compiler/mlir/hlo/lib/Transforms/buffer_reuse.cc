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

#include "mlir-hlo/Analysis/userange_analysis.h"
#include "mlir-hlo/Transforms/PassDetail.h"
#include "mlir-hlo/Transforms/passes.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Operation.h"
#include "mlir/Interfaces/LoopLikeInterface.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/BufferUtils.h"

namespace mlir {

namespace {

/// Reuses already allocated buffer to save allocation operations.
class BufferReuse : BufferPlacementTransformationBase {
public:
  BufferReuse(Operation *op)
      : BufferPlacementTransformationBase(op), dominators(op),
        postDominators(op), userange(op, allocs, aliases) {}

  /// Reuses already allocated buffers to save allocation operations.
  void reuse(Operation *operation) {
    // Create a list of values that can potentially be replaced for each value
    // in the useRangeMap. The potentialReuseMap maps each value to the
    // respective list.
    llvm::MapVector<Value, SmallVector<Value, 4>> potentialReuseMap;
    for (BufferPlacementAllocs::AllocEntry entry : allocs) {
      Value itemA = std::get<0>(entry);
      SmallVector<Value, 4> potReuseVector;
      for (BufferPlacementAllocs::AllocEntry entry : allocs) {
        Value itemB = std::get<0>(entry);
        // Do not compare an item to itself and make sure that the value of item
        // B is not a BlockArgument. BlockArguments cannot be reused. Also
        // perform a reuse compatibility check.
        if (itemA == itemB || !checkReuseCompatibility(itemA, itemB))
          continue;

        // Check if itemA interferes with itemB. If this is the case no reuse is
        // possible.
        if (userange.rangesInterfere(itemA, itemB))
          continue;

        // Get the defining block of itemA.
        Block *defOpBlock = itemA.isa<BlockArgument>()
                                ? itemA.getParentBlock()
                                : itemA.getDefiningOp()->getBlock();

        // The defining block of itemA has to dominate all uses of itemB.
        if (!dominatesAllUses(defOpBlock, itemB))
          continue;

        // Insert itemB into the right place of the potReuseVector. The order of
        // the vector is defined via the program order of the first use of each
        // item.
        auto insertionPoint = potReuseVector.begin();
        while (insertionPoint != potReuseVector.end()) {
          if (userange.getFirstUseIndex(itemB) <
              userange.getFirstUseIndex(*insertionPoint))
            break;
          ++insertionPoint;
        }
        potReuseVector.insert(insertionPoint, itemB);
      }

      potentialReuseMap.insert(
          std::pair<Value, SmallVector<Value, 4>>(itemA, potReuseVector));
    }

    // Replace all uses of the value that is replaced and
    // delete the DefiningOp.
    for (auto &reuse : computeActualReuse(potentialReuseMap)) {
      for (Value reuseValue : reuse.second) {
        reuseValue.replaceAllUsesWith(reuse.first);
        reuseValue.getDefiningOp()->erase();
      }
    }
  }

private:
  /// Check if all uses of item are dominated by the given block.
  bool dominatesAllUses(Block *block, Value item) {
    for (OpOperand &operand : item.getUses()) {
      if (!dominators.dominates(block, operand.getOwner()->getBlock()))
        return false;
    }
    return true;
  }

  /// Checks if there is a transitive interference between potReuseValue and the
  /// value that may replace it, we call this value V. potReuses is the vector
  /// of all values that can potentially be replaced by V. If potReuseValue
  /// already replaces any other value that is not part of the potReuses vector
  /// it cannot be replaced by V anymore.
  bool transitiveInterference(
      Value potReuseValue, SmallVector<Value, 4> &potReuses,
      llvm::MapVector<Value, DenseSet<Value>> &actualReuseMap) {
    return actualReuseMap.find(potReuseValue) != actualReuseMap.end() &&
           llvm::any_of(actualReuseMap[potReuseValue], [&](Value vReuse) {
             return !std::count(potReuses.begin(), potReuses.end(), vReuse);
           });
  }

  /// Checks if the types of the given values are compatible for a
  /// replacement.
  bool checkReuseCompatibility(Value a, Value b) {
    auto shapedA = a.getType().cast<ShapedType>();
    auto shapedB = b.getType().cast<ShapedType>();

    // If both types are shaped we can check for equality.
    if (shapedA.hasStaticShape() && shapedB.hasStaticShape())
      return a.getType() == b.getType();
    // If only one of the types is shaped we cannot detect compatibility since
    // we do not know how the allocation operation behaves on its operands.
    if (shapedA.hasStaticShape() != shapedB.hasStaticShape())
      return false;

    // We need the actual alloc operation of both types. For aliases we need
    // to check for the defining OP of the alias' origin.
    Operation *defOpA = a.getDefiningOp();
    Operation *defOpB = b.getDefiningOp();

    // If the alloc method or the number of operands is not the same the types
    // might not be compatible.
    if (defOpA->getName() != defOpB->getName() ||
        defOpA->getNumOperands() != defOpB->getNumOperands())
      return false;

    // If all operands are equal the types are compatible.
    for (auto const &pair :
         llvm::zip(defOpA->getOperands(), defOpB->getOperands())) {
      if (std::get<0>(pair) != std::get<1>(pair))
        return false;
    }
    return true;
  }

  /// A Fixpoint iteration over the potential reuses to compute the actual
  /// reuses.
  llvm::MapVector<Value, DenseSet<Value>> computeActualReuse(
      llvm::MapVector<Value, SmallVector<Value, 4>> &potentialReuseMap) {

    // The replacedSet contains all values that are going to be replaced.
    DenseSet<Value> replacedSet;

    // The currentReuserSet contains all values that are replacing another
    // value in the current iteration. Note: This is necessary because the
    // replacing property is not transitive.
    DenseSet<Value> currentReuserSet;

    /// Maps a value to the set of values that it replaces.
    llvm::MapVector<Value, DenseSet<Value>> actualReuseMap;

    for (;;) {
      // Clear the currentReuserSet for this iteration.
      currentReuserSet.clear();
      // Step 1 of the fixpoint iteration: Choose a value to be replaced for
      // each value in the potentialReuseMap.
      choosePotentialReuses(replacedSet, currentReuserSet, potentialReuseMap,
                            actualReuseMap);

      // If the currentReuseSet is empty we can terminate the fixpoint
      // iteration.
      if (currentReuserSet.empty())
        break;

      // Step 2 of the fixpoint iteration: Update the potentialReuseVectors for
      // each value in the potentialReuseMap. Due to the chosen replacements in
      // step 1 some values might not be replaceable anymore. Also remove all
      // replaced values from the potentialReuseMap.
      updatePotentialReuses(replacedSet, potentialReuseMap, actualReuseMap);
    }
    return actualReuseMap;
  }

  /// For each value in the potentialReuseMap, check if another value tries to
  /// reuse it or if it is already replaced by another value. If neither is the
  /// case add the value and its reuses (if any) to the actualReuseMap.
  void choosePotentialReuses(
      DenseSet<Value> &replacedSet, DenseSet<Value> &currentReuserSet,
      llvm::MapVector<Value, SmallVector<Value, 4>> &potentialReuseMap,
      llvm::MapVector<Value, DenseSet<Value>> &actualReuseMap) {
    for (auto &potReuser : potentialReuseMap) {
      Value item = potReuser.first;
      SmallVector<Value, 4> potReuses = potReuser.second;

      // If the current value is replaced already we have to skip it.
      if (replacedSet.contains(item))
        continue;

      // Find a value that can be reused. If the value is already in the
      // currentReuserSet then we have to break. Due to the order of the
      // values we must not skip it, because it can potentially be replaced in
      // the next iteration. However, we may skip the value if it is replaced
      // by another value.
      for (Value v : potReuses) {
        if (currentReuserSet.contains(v))
          break;
        if (replacedSet.contains(v))
          continue;

        // Update the actualReuseMap.
        actualReuseMap[item].insert(v);

        // Check if the replaced value already replaces other values and also
        // add them to the reused set.
        auto alreadyReplaced = actualReuseMap.find(v);
        if (alreadyReplaced != actualReuseMap.end()) {
          actualReuseMap[item].insert(alreadyReplaced->second.begin(),
                                      alreadyReplaced->second.end());
          actualReuseMap.erase(v);
        }

        // Merge the userange of v into the userange of item.
        userange.unionRanges(item, v);

        currentReuserSet.insert(item);
        replacedSet.insert(v);
        break;
      }
    }
  }

  /// Update the potentialReuseVectors for each value in the potentialReuseMap.
  void updatePotentialReuses(
      DenseSet<Value> &replacedSet,
      llvm::MapVector<Value, SmallVector<Value, 4>> &potentialReuseMap,
      llvm::MapVector<Value, DenseSet<Value>> &actualReuseMap) {
    for (auto itReuseMap = potentialReuseMap.begin();
         itReuseMap != potentialReuseMap.end();) {
      Value item = itReuseMap->first;
      SmallVector<Value, 4> &potReuses = itReuseMap->second;

      // If the item is already reused, we can remove it from the
      // potentialReuseMap.
      if (replacedSet.contains(item)) {
        potentialReuseMap.erase(itReuseMap);
        continue;
      }

      // Remove all potential reuses that cannot be reused for this value.
      potReuses.erase(
          std::remove_if(potReuses.begin(), potReuses.end(),
                         [&](Value potReuseValue) {
                           return replacedSet.contains(potReuseValue) ||
                                  transitiveInterference(potReuseValue,
                                                         potReuses,
                                                         actualReuseMap) ||
                                  userange.rangesInterfere(item, potReuseValue);
                         }),
          potReuses.end());
      ++itReuseMap;
    }
  }

  /// The current dominance info.
  DominanceInfo dominators;

  /// The current postdominance info.
  PostDominanceInfo postDominators;

  /// The current userange info.
  UserangeAnalysis userange;
};

/// The buffer reuse pass that uses already allocated buffers if all critera
/// are met.
struct BufferReusePass : BufferReuseBase<BufferReusePass> {
  void runOnFunction() override {
    // Reuse allocated buffer instead of new allocation.
    Operation *funcOp = getFunction();
    BufferReuse optimizer(funcOp);
    optimizer.reuse(funcOp);
  }
};

} // end namespace

std::unique_ptr<FunctionPass> createBufferReusePass() {
  return std::make_unique<BufferReusePass>();
}

} // end namespace mlir
