//===- TypeSwitch.h - Switch functionality for RTTI casting -*- C++ -*-----===//
//
// Part of the MLIR Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements the TypeSwitch template, which mimics a switch()
//  statement whose cases are type names.
//
//===-----------------------------------------------------------------------===/

#ifndef MLIR_SUPPORT_TYPESWITCH_H
#define MLIR_SUPPORT_TYPESWITCH_H

#include "mlir/Support/LLVM.h"
#include "mlir/Support/STLExtras.h"
#include "llvm/ADT/Optional.h"

namespace mlir {
namespace detail {

template <typename DerivedT, typename T> class TypeSwitchBase {
public:
  TypeSwitchBase(const T &value) : value(value) {}
  TypeSwitchBase(TypeSwitchBase &&other) : value(other.value) {}
  ~TypeSwitchBase() = default;

  /// TypeSwitchBase is not copyable.
  TypeSwitchBase(const TypeSwitchBase &) = delete;
  void operator=(const TypeSwitchBase &) = delete;
  void operator=(TypeSwitchBase &&other) = delete;

  /// Invoke a case on the derived class with multiple case types.
  template <typename CaseT, typename CaseT2, typename... CaseTs,
            typename CallableT>
  DerivedT &Case(CallableT &&caseFn) {
    DerivedT &derived = static_cast<DerivedT &>(*this);
    return derived.template Case<CaseT>(caseFn)
        .template Case<CaseT2, CaseTs...>(caseFn);
  }

  /// Invoke a case on the derived class, inferring the type of the Case from
  /// the first input of the given callable.
  /// Note: This inference rules for this overload are very simple: strip
  ///       pointers and references.
  template <typename CallableT> DerivedT &Case(CallableT &&caseFn) {
    using Traits = FunctionTraits<std::decay_t<CallableT>>;
    using CaseT = std::remove_cv_t<std::remove_pointer_t<
        std::remove_reference_t<typename Traits::template arg_t<0>>>>;

    DerivedT &derived = static_cast<DerivedT &>(*this);
    return derived.template Case<CaseT>(std::forward<CallableT>(caseFn));
  }

protected:
  /// Trait to check whether `ValueT` provides a 'dyn_cast' method with type
  /// `CastT`.
  template <typename ValueT, typename CastT>
  using has_dyn_cast_t =
      decltype(std::declval<ValueT &>().template dyn_cast<CastT>());

  /// Attempt to dyn_cast the given `value` to `CastT`. This overload is
  /// selected if `value` already has a suitable dyn_cast method.
  template <typename CastT, typename ValueT>
  static auto castValue(
      ValueT value,
      typename std::enable_if_t<
          is_detected<has_dyn_cast_t, ValueT, CastT>::value> * = nullptr) {
    return value.template dyn_cast<CastT>();
  }

  /// Attempt to dyn_cast the given `value` to `CastT`. This overload is
  /// selected if llvm::dyn_cast should be used.
  template <typename CastT, typename ValueT>
  static auto castValue(
      ValueT value,
      typename std::enable_if_t<
          !is_detected<has_dyn_cast_t, ValueT, CastT>::value> * = nullptr) {
    return dyn_cast<CastT>(value);
  }

  /// The root value we are switching on.
  const T value;
};
} // end namespace detail

/// This class implements a switch-like dispatch statement for a value of 'T'
/// using dyn_cast functionality. Each `Case<T>` takes a callable to be invoked
/// if the root value isa<T>, the callable is invoked with the result of
/// dyn_cast<T>() as a parameter.
///
/// Example:
///  Operation *op = ...;
///  LogicalResult result = TypeSwitch<Operation *, LogicalResult>(op)
///    .Case<ConstantOp>([](ConstantOp op) { ... })
///    .Default([](Operation *op) { ... });
///
template <typename T, typename ResultT = void>
class TypeSwitch : public detail::TypeSwitchBase<TypeSwitch<T, ResultT>, T> {
public:
  using BaseT = detail::TypeSwitchBase<TypeSwitch<T, ResultT>, T>;
  using BaseT::BaseT;
  using BaseT::Case;
  TypeSwitch(TypeSwitch &&other) = default;

  /// Add a case on the given type.
  template <typename CaseT, typename CallableT>
  TypeSwitch<T, ResultT> &Case(CallableT &&caseFn) {
    if (result)
      return *this;

    // Check to see if CaseT applies to 'value'.
    if (auto caseValue = BaseT::template castValue<CaseT>(this->value))
      result = caseFn(caseValue);
    return *this;
  }

  /// As a default, invoke the given callable within the root value.
  template <typename CallableT>
  LLVM_NODISCARD ResultT Default(CallableT &&defaultFn) {
    if (result)
      return std::move(*result);
    return defaultFn(this->value);
  }

  LLVM_NODISCARD
  operator ResultT() {
    assert(result && "Fell off the end of a type-switch");
    return std::move(*result);
  }

private:
  /// The pointer to the result of this switch statement, once known,
  /// null before that.
  Optional<ResultT> result;
};

/// Specialization of TypeSwitch for void returning callables.
template <typename T>
class TypeSwitch<T, void>
    : public detail::TypeSwitchBase<TypeSwitch<T, void>, T> {
public:
  using BaseT = detail::TypeSwitchBase<TypeSwitch<T, void>, T>;
  using BaseT::BaseT;
  using BaseT::Case;
  TypeSwitch(TypeSwitch &&other) = default;

  /// Add a case on the given type.
  template <typename CaseT, typename CallableT>
  TypeSwitch<T, void> &Case(CallableT &&caseFn) {
    if (foundMatch)
      return *this;

    // Check to see if any of the types apply to 'value'.
    if (auto caseValue = BaseT::template castValue<CaseT>(this->value)) {
      caseFn(caseValue);
      foundMatch = true;
    }
    return *this;
  }

  /// As a default, invoke the given callable within the root value.
  template <typename CallableT> void Default(CallableT &&defaultFn) {
    if (!foundMatch)
      defaultFn(this->value);
  }

private:
  /// A flag detailing if we have already found a match.
  bool foundMatch = false;
};
} // end namespace mlir

#endif // MLIR_SUPPORT_TYPESWITCH_H
