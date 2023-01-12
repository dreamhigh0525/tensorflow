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

#include <memory>
#include <string>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/SymbolTable.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/analysis/resource_alias_analysis.h"
#include "tensorflow/compiler/mlir/tensorflow/analysis/side_effect_analysis.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_executor.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_remaining_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/attribute_utils.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/verify_suitable_for_graph_export.h"

namespace mlir {
namespace tf_executor {
namespace {

// Comparator for `OpsInProgramOrder`.
struct isBeforeInBlock {
  bool operator()(Operation* op, Operation* other_op) const {
    // This function has an average complexity of O(1).
    return op->isBeforeInBlock(other_op);
  }
};

// Maps group IDs to branch IDs.
using GroupIdToBranchIdMap = absl::flat_hash_map<std::string, std::string>;
// Maps an op to parallel execution IDs.
using OpToParallelIdsMap =
    absl::flat_hash_map<Operation*, GroupIdToBranchIdMap>;
// Maps an op to a vector of ops.
using OpToOpsMap =
    absl::flat_hash_map<Operation*, llvm::SmallVector<Operation*, 8>>;
// Represents a set of ops in program order.
using OpsInProgramOrder = absl::btree_set<Operation*, isBeforeInBlock>;

#define GEN_PASS_DEF_EXECUTORUPDATECONTROLDEPENDENCIESPASS
#include "tensorflow/compiler/mlir/tensorflow/transforms/tf_passes.h.inc"

class UpdateControlDependenciesPass
    : public impl::ExecutorUpdateControlDependenciesPassBase<
          UpdateControlDependenciesPass> {
 public:
  void runOnOperation() override;
};

const GroupIdToBranchIdMap& EmptyGroupIdToBranchIdMap() {
  // clang-format off
  static auto* empty_map = new absl::flat_hash_map<std::string, std::string>{};
  return *empty_map;
}

// Returns map whose elements are the (group ID,branch ID) pairs for `op`.
const GroupIdToBranchIdMap& GetGroupIdToBranchIdMap(
    Operation* op, const OpToParallelIdsMap& op_to_parallel_ids_map) {
  auto iter = op_to_parallel_ids_map.find(op);
  if (iter == op_to_parallel_ids_map.end()) return EmptyGroupIdToBranchIdMap();
  return iter->second;
}

// Returns true iff we should keep a control dependency between both ops,
// depending on their parallel execution IDs.
bool ShouldKeepDependency(Operation* op, Operation* other_op,
                          OpToParallelIdsMap& op_to_parallel_ids_map) {
  const GroupIdToBranchIdMap& parallel_ids_map =
      GetGroupIdToBranchIdMap(op, op_to_parallel_ids_map);
  const GroupIdToBranchIdMap& other_parallel_ids_map =
      GetGroupIdToBranchIdMap(other_op, op_to_parallel_ids_map);

  for (auto [group_id, branch_id] : parallel_ids_map) {
    auto iter = other_parallel_ids_map.find(group_id);
    // `other_op` has same group which `op` has, with different branch ID.
    if (iter != other_parallel_ids_map.end() && iter->second != branch_id) {
      return false;
    }
  }
  // The ops don't share a common group with different branch IDs.
  return true;
}

// Returns true iff `op` is dominated by `other_op`, that means,
// `ShouldKeepDependency(op, other_op, ...)` is true, and for every op `C` for
// which `ShouldKeepDependency(op, C, ...)` is true,
// `ShouldKeepDependency(other_op, C, ...)` is also true.
// We need to propagate ops that are not dominated to make sure that we keep all
// valid transitive dependencies.
bool IsDominatedBy(Operation* op, Operation* other_op,
                   const OpToParallelIdsMap& op_to_parallel_ids_map) {
  const GroupIdToBranchIdMap& parallel_ids_map =
      GetGroupIdToBranchIdMap(op, op_to_parallel_ids_map);
  const GroupIdToBranchIdMap& other_parallel_ids_map =
      GetGroupIdToBranchIdMap(other_op, op_to_parallel_ids_map);

  for (auto [other_group_id, other_branch_id] : other_parallel_ids_map) {
    auto iter = parallel_ids_map.find(other_group_id);
    // `other_op` has some group which `op` doesn't have.
    if (iter == parallel_ids_map.end()) return false;
    auto branch_id = iter->second;
    // `other_op` has some group which `op` has with different branch ID.
    if (branch_id != other_branch_id) return false;
  }
  // `op` has all groups that `other_op` has, with same branch IDs.
  return true;
}

void ClearControlInputs(Operation* op, int& num_control_inputs_removed) {
  // We only call this function for island or fetch ops. The second pair of
  // parentheses is needed for successful compilation.
  assert((isa<IslandOp, FetchOp>(op)));
  if (auto island = dyn_cast<IslandOp>(op)) {
    num_control_inputs_removed += island.getControlInputs().size();
    island.getControlInputsMutable().clear();
  } else if (auto fetch = dyn_cast<FetchOp>(op)) {
    GraphOp graph = fetch->getParentOfType<GraphOp>();
    int num_control_fetches = fetch.getNumOperands() - graph.getNumResults();
    if (num_control_fetches > 0) {
      fetch.getFetchesMutable().erase(graph.getNumResults(),
                                      num_control_fetches);
      num_control_inputs_removed += num_control_fetches;
    }
  }
}

void SetControlInputs(Operation* op, const OpsInProgramOrder& control_preds,
                      int& num_control_inputs_added) {
  // We only call this function for island or fetch ops. The second pair of
  // parentheses is needed for successful compilation.
  assert((isa<IslandOp, FetchOp>(op)));
  mlir::MutableOperandRange mutable_control_inputs =
      isa<IslandOp>(op) ? cast<IslandOp>(op).getControlInputsMutable()
                        : cast<FetchOp>(op).getFetchesMutable();
  for (Operation* control_pred : control_preds) {
    if (auto control_pred_island =
            dyn_cast<mlir::tf_executor::IslandOp>(control_pred)) {
      mutable_control_inputs.append(control_pred_island.getControl());
    }
  }
  num_control_inputs_added += control_preds.size();
}

// Fills `op_to_parallel_ids_map` from parallel execution attributes in `graph`.
// Returns `failure` iff any attribute is malformed.
LogicalResult FillOpToParallelIdsMap(
    GraphOp graph, OpToParallelIdsMap& op_to_parallel_ids_map) {
  for (Operation& op : graph.GetBody()) {
    auto island = dyn_cast<IslandOp>(&op);
    if (!island) continue;

    // We call `VerifyExportSuitable` in the beginning of the pass, so every
    // island wraps a single op.
    Operation& wrapped_op = island.GetBody().front();
    TF::ParallelExecutionIdPairs id_pairs;
    if (failed(TF::ParseParallelExecutionIds(&wrapped_op, id_pairs))) {
      wrapped_op.emitError()
          << "Malformed " << TF::kParallelExecAnnotation << " attribute";
      return failure();
    }
    if (id_pairs.empty()) continue;

    GroupIdToBranchIdMap& ids_map = op_to_parallel_ids_map[island];
    for (auto [group_id, branch_id] : id_pairs) ids_map[group_id] = branch_id;
  }
  return success();
}

// This function updates all control dependencies in `func`, represented as
// control inputs for island and fetch ops of the graph body in `func`.
// Ideally, we would purely rely on side effect analysis here and propagate
// the queried dependencies to the island and fetch ops. However, this is
// currently not in line with execution semantics in case of replication and
// parallel executes: If two ops originated from different branches of a
// `tf_device.replicate` or `tf_device.parallel_execute` op, then there should
// be no control dependency between them irrespective of side effects, even if
// this could cause a race condition (see b/262304795).
// Because of this, we need to keep track of the origin of such ops which we do
// via `kParallelExecAnnotation` attributes that are interpreted in this pass.
//
// NOTE: This pass does not guarantee the minimum number of control inputs.
// In other words, if we interpret all ops and control dependencies as a DAG,
// then we don't guarantee to find the transitive reduction of the graph
// (see https://en.wikipedia.org/wiki/Transitive_reduction).
// If necessary, the transitive reduction can be computed in a post-processing
// step (time complexity: O(nm)).
LogicalResult UpdateAllControlDependencies(
    func::FuncOp func, const TF::SideEffectAnalysis::Info& analysis_for_func) {
  int num_control_inputs_removed = 0;
  int num_control_inputs_added = 0;

  // Maps island ops to parallel IDs of the wrapped ops.
  OpToParallelIdsMap op_to_parallel_ids_map;
  // For each `op`, stores transitive control predecessors that could be
  // relevant for control successors of `op` (including `op` itself).
  OpToOpsMap candidate_control_preds;
  // Stores control predecessors in program order.
  OpsInProgramOrder control_preds;

  // We call `VerifyExportSuitable` in the beginning of the pass, so every
  // function has a single graph op.
  auto graph = cast<GraphOp>(func.front().front());
  if (failed(FillOpToParallelIdsMap(graph, op_to_parallel_ids_map))) {
    return failure();
  }

  for (Operation& op_ref : graph.GetBody()) {
    Operation* op = &op_ref;
    // We only represent control dependencies between island and fetch ops.
    if (!isa<IslandOp, FetchOp>(op)) continue;

    // Remove all existing control inputs.
    ClearControlInputs(op, num_control_inputs_removed);
    // Determine control predecessors and update candidates.
    control_preds.clear();
    candidate_control_preds[op].push_back(op);
    for (Operation* direct_control_pred :
         analysis_for_func.DirectControlPredecessors(op)) {
      for (Operation* candidate_control_pred :
           candidate_control_preds[direct_control_pred]) {
        // Only take the candidate if the dependency should be kept.
        if (ShouldKeepDependency(candidate_control_pred, op,
                                 op_to_parallel_ids_map)) {
          control_preds.insert(candidate_control_pred);
        }
        // We need to propagate candidates that are not dominated by `op`
        // because we could encounter some op later that depends on such a
        // candidate but not on `op`.
        if (!IsDominatedBy(candidate_control_pred, op,
                           op_to_parallel_ids_map)) {
          candidate_control_preds[op].push_back(candidate_control_pred);
        }
      }
    }
    // Set new control inputs based on control predecessors.
    SetControlInputs(op, control_preds, num_control_inputs_added);
  }
  VLOG(2) << "Number of control inputs removed: " << num_control_inputs_removed;
  VLOG(2) << "Number of control inputs added: " << num_control_inputs_added;
  return success();
}

void UpdateControlDependenciesPass::runOnOperation() {
  ModuleOp module = getOperation();
  // This pass assumes that all functions are suitable for export, i.e., each
  // function has a single tf_executor.graph op and all islands wrap single
  // ops.
  if (failed(tensorflow::VerifyExportSuitable(module))) {
    signalPassFailure();
    return;
  }
  TF::SideEffectAnalysis side_effect_analysis(module);
  for (auto func : module.getOps<func::FuncOp>()) {
    if (func.isExternal()) continue;
    const auto& analysis_for_func =
        side_effect_analysis.GetAnalysisForFunc(func);
    if (failed(UpdateAllControlDependencies(func, analysis_for_func))) {
      signalPassFailure();
      return;
    }
  }
}

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>>
CreateTFExecutorUpdateControlDependenciesPass() {
  return std::make_unique<UpdateControlDependenciesPass>();
}

}  // namespace tf_executor
}  // namespace mlir
