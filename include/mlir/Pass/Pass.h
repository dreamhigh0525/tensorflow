//===- Pass.h - Base classes for compiler passes ----------------*- C++ -*-===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#ifndef MLIR_PASS_PASS_H
#define MLIR_PASS_PASS_H

#include "mlir/Pass/AnalysisManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "llvm/ADT/PointerIntPair.h"

namespace mlir {
/// The abstract base pass class. This class contains information describing the
/// derived pass object, e.g its kind and abstract PassInfo.
class Pass {
public:
  enum class Kind { FunctionPass, ModulePass };

  virtual ~Pass() = default;

  /// Returns the unique identifier that corresponds to this pass.
  const PassID *getPassID() const { return passIDAndKind.getPointer(); }

  /// Returns the pass info for the specified pass class or null if unknown.
  static const PassInfo *lookupPassInfo(const PassID *passID);
  template <typename PassT> static const PassInfo *lookupPassInfo() {
    return lookupPassInfo(PassID::getID<PassT>());
  }

  /// Returns the pass info for this pass.
  const PassInfo *lookupPassInfo() const { return lookupPassInfo(getPassID()); }

  /// Return the kind of this pass.
  Kind getKind() const { return passIDAndKind.getInt(); }

protected:
  Pass(const PassID *passID, Kind kind) : passIDAndKind(passID, kind) {}

private:
  /// Out of line virtual method to ensure vtables and metadata are emitted to a
  /// single .o file.
  virtual void anchor();

  /// Represents a unique identifier for the pass and its kind.
  llvm::PointerIntPair<const PassID *, 1, Kind> passIDAndKind;
};

namespace detail {
class FunctionPassExecutor;
class ModulePassExecutor;

/// The state for a single execution of a pass. This provides a unified
/// interface for accessing and initializing necessary state for pass execution.
template <typename IRUnitT, typename AnalysisManagerT>
struct PassExecutionState {
  PassExecutionState(IRUnitT *ir, AnalysisManagerT &analysisManager)
      : irAndPassFailed(ir, false), analysisManager(analysisManager) {}

  /// The current IR unit being transformed and a bool for if the pass signaled
  /// a failure.
  llvm::PointerIntPair<IRUnitT *, 1, bool> irAndPassFailed;

  /// The analysis manager for the IR unit.
  AnalysisManagerT &analysisManager;

  /// The set of preserved analyses for the current execution.
  detail::PreservedAnalyses preservedAnalyses;
};
} // namespace detail

/// Pass to transform a specific function within a module. Derived passes should
/// not inherit from this class directly, and instead should use the CRTP
/// FunctionPass class.
class FunctionPassBase : public Pass {
  using PassStateT =
      detail::PassExecutionState<Function, FunctionAnalysisManager>;

public:
  static bool classof(const Pass *pass) {
    return pass->getKind() == Kind::FunctionPass;
  }

protected:
  explicit FunctionPassBase(const PassID *id) : Pass(id, Kind::FunctionPass) {}

  /// The polymorphic API that runs the pass over the currently held function.
  virtual void runOnFunction() = 0;

  /// Return the current function being transformed.
  Function &getFunction() {
    return *getPassState().irAndPassFailed.getPointer();
  }

  /// Returns the current pass state.
  PassStateT &getPassState() {
    assert(passState && "pass state was never initialized");
    return *passState;
  }

  /// Returns the current analysis manager.
  FunctionAnalysisManager &getAnalysisManager() {
    return getPassState().analysisManager;
  }

private:
  /// Forwarding function to execute this pass. Returns false if the pass
  /// execution failed, true otherwise.
  LLVM_NODISCARD
  bool run(Function *fn, FunctionAnalysisManager &fam);

  /// The current execution state for the pass.
  llvm::Optional<PassStateT> passState;

  /// Allow access to 'run'.
  friend detail::FunctionPassExecutor;
};

/// Pass to transform a module. Derived passes should not inherit from this
/// class directly, and instead should use the CRTP ModulePass class.
class ModulePassBase : public Pass {
  using PassStateT = detail::PassExecutionState<Module, ModuleAnalysisManager>;

public:
  static bool classof(const Pass *pass) {
    return pass->getKind() == Kind::ModulePass;
  }

protected:
  explicit ModulePassBase(const PassID *id) : Pass(id, Kind::ModulePass) {}

  /// The polymorphic API that runs the pass over the currently held module.
  virtual void runOnModule() = 0;

  /// Return the current module being transformed.
  Module &getModule() { return *getPassState().irAndPassFailed.getPointer(); }

  /// Returns the current pass state.
  PassStateT &getPassState() {
    assert(passState && "pass state was never initialized");
    return *passState;
  }

  /// Returns the current analysis manager.
  ModuleAnalysisManager &getAnalysisManager() {
    return getPassState().analysisManager;
  }

private:
  /// Forwarding function to execute this pass. Returns false if the pass
  /// execution failed, true otherwise.
  LLVM_NODISCARD
  bool run(Module *module, ModuleAnalysisManager &mam);

  /// The current execution state for the pass.
  llvm::Optional<PassStateT> passState;

  /// Allow access to 'run'.
  friend detail::ModulePassExecutor;
};

//===----------------------------------------------------------------------===//
// Pass Model Definitions
//===----------------------------------------------------------------------===//
namespace detail {
/// The opaque CRTP model of a pass. This class provides utilities for derived
/// pass execution and handles all of the necessary polymorphic API.
template <typename IRUnitT, typename PassT, typename BasePassT>
class PassModel : public BasePassT {
protected:
  PassModel() : BasePassT(PassID::getID<PassT>()) {}

  /// TODO(riverriddle) Provide additional utilities for cloning, getting the
  /// derived class name, etc.

  /// Signal that some invariant was broken when running. The IR is allowed to
  /// be in an invalid state.
  void signalPassFailure() {
    this->getPassState().irAndPassFailed.setInt(true);
  }

  /// Query the result of an analysis for the current ir unit.
  template <typename AnalysisT> AnalysisT &getAnalysisResult() {
    return this->getAnalysisManager().template getResult<AnalysisT>();
  }

  /// Query the cached result of an analysis for the current ir unit if one
  /// exists.
  template <typename AnalysisT>
  llvm::Optional<std::reference_wrapper<AnalysisT>> getCachedAnalysisResult() {
    return this->getAnalysisManager().template getCachedResult<AnalysisT>();
  }

  /// Mark all analyses as preserved.
  void markAllAnalysesPreserved() {
    this->getPassState().preservedAnalyses.preserveAll();
  }
};
} // end namespace detail

/// A model for providing function pass specific utilities.
///
/// Function passes must not:
///   - read or modify any other functions within the parent module, as
///     other threads may be manipulating them concurrently.
///   - modify any state within the parent module, this includes adding
///     additional functions.
///
/// Derived function passes are expected to provide the following:
///   - A 'void runOnFunction()' method.
template <typename T>
struct FunctionPass : public detail::PassModel<Function, T, FunctionPassBase> {
  /// Returns the analysis result for the parent module if it exists.
  template <typename AnalysisT>
  llvm::Optional<std::reference_wrapper<AnalysisT>>
  getCachedModuleAnalysisResult() {
    return this->getAnalysisManager()
        .template getCachedModuleResult<AnalysisT>();
  }
};

/// A model for providing module pass specific utilities.
///
/// Derived module passes are expected to provide the following:
///   - A 'void runOnModule()' method.
template <typename T>
struct ModulePass : public detail::PassModel<Module, T, ModulePassBase> {
  /// Returns the analysis result for a child function.
  template <typename AnalysisT>
  AnalysisT &getFunctionAnalysisResult(Function *f) {
    return this->getAnalysisManager().template getFunctionResult<AnalysisT>(f);
  }
};
} // end namespace mlir

#endif // MLIR_PASS_PASS_H
