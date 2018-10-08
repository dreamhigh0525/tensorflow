//===- AffineExpr.h - MLIR Affine Expr Class --------------------*- C++ -*-===//
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
//
// An affine expression is an affine combination of dimension identifiers and
// symbols, including ceildiv/floordiv/mod by a constant integer.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_AFFINE_EXPR_H
#define MLIR_IR_AFFINE_EXPR_H

#include "mlir/Support/LLVM.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/Support/Casting.h"
#include <type_traits>

namespace mlir {

class MLIRContext;

namespace detail {

class AffineExpr;
class AffineBinaryOpExpr;
class AffineDimExpr;
class AffineSymbolExpr;
class AffineConstantExpr;

} // namespace detail

enum class AffineExprKind {
  Add,
  // RHS of mul is always a constant or a symbolic expression.
  Mul,
  // RHS of mod is always a constant or a symbolic expression.
  Mod,
  // RHS of floordiv is always a constant or a symbolic expression.
  FloorDiv,
  // RHS of ceildiv is always a constant or a symbolic expression.
  CeilDiv,

  /// This is a marker for the last affine binary op. The range of binary
  /// op's is expected to be this element and earlier.
  LAST_AFFINE_BINARY_OP = CeilDiv,

  // Constant integer.
  Constant,
  // Dimensional identifier.
  DimId,
  // Symbolic identifier.
  SymbolId,
};

/// Helper structure to build AffineExpr with intuitive operators in order to
/// operate on chainable, lightweight, immutable value types instead of pointer
/// types.
/// TODO(ntv): Remove all redundant MLIRContext* arguments through the API
/// TODO(ntv): Add extra out-of-class operators for int op AffineExprBaseRef
/// TODO(ntv): Rename
/// TODO(ntv): pointer pair
template <typename AffineExprType> class AffineExprBaseRef {
public:
  typedef AffineExprBaseRef TemplateType;
  typedef AffineExprType ImplType;

  AffineExprBaseRef() : expr(nullptr) {}
  /* implicit */ AffineExprBaseRef(const AffineExprType *expr)
      : expr(const_cast<AffineExprType *>(expr)) {}

  AffineExprBaseRef(const AffineExprBaseRef &other) : expr(other.expr) {}
  AffineExprBaseRef &operator=(AffineExprBaseRef other) {
    expr = other.expr;
    return *this;
  }

  bool operator==(AffineExprBaseRef other) const { return expr == other.expr; }

  explicit operator AffineExprType *() const {
    return const_cast<AffineExprType *>(expr);
  }
  /* implicit */ operator AffineExprBaseRef<detail::AffineExpr>() const {
    return const_cast<detail::AffineExpr *>(
        static_cast<const detail::AffineExpr *>(expr));
  }
  explicit operator bool() const { return expr; }

  bool operator!() const { return expr == nullptr; }
  AffineExprType *operator->() const { return expr; }

  template <typename U> bool isa() const {
    using PtrType = typename U::ImplType;
    return llvm::isa<PtrType>(const_cast<AffineExprType *>(this->expr));
  }
  template <typename U> U dyn_cast() const {
    using PtrType = typename U::ImplType;
    return U(llvm::dyn_cast<PtrType>(const_cast<AffineExprType *>(this->expr)));
  }
  template <typename U> U cast() const {
    using PtrType = typename U::ImplType;
    return U(llvm::cast<PtrType>(const_cast<AffineExprType *>(this->expr)));
  }

  AffineExprBaseRef operator+(int64_t v) const;
  AffineExprBaseRef operator+(AffineExprBaseRef other) const;
  AffineExprBaseRef operator-() const;
  AffineExprBaseRef operator-(int64_t v) const;
  AffineExprBaseRef operator-(AffineExprBaseRef other) const;
  AffineExprBaseRef operator*(int64_t v) const;
  AffineExprBaseRef operator*(AffineExprBaseRef other) const;
  AffineExprBaseRef floorDiv(uint64_t v) const;
  AffineExprBaseRef floorDiv(AffineExprBaseRef other) const;
  AffineExprBaseRef ceilDiv(uint64_t v) const;
  AffineExprBaseRef ceilDiv(AffineExprBaseRef other) const;
  AffineExprBaseRef operator%(uint64_t v) const;
  AffineExprBaseRef operator%(AffineExprBaseRef other) const;

  friend ::llvm::hash_code hash_value(AffineExprBaseRef arg);

private:
  AffineExprType *expr;
};

using AffineExprRef = AffineExprBaseRef<detail::AffineExpr>;
using AffineBinaryOpExprRef = AffineExprBaseRef<detail::AffineBinaryOpExpr>;
using AffineDimExprRef = AffineExprBaseRef<detail::AffineDimExpr>;
using AffineSymbolExprRef = AffineExprBaseRef<detail::AffineSymbolExpr>;
using AffineConstantExprRef = AffineExprBaseRef<detail::AffineConstantExpr>;

// Make AffineExprRef hashable.
inline ::llvm::hash_code hash_value(AffineExprRef arg) {
  return ::llvm::hash_value(static_cast<detail::AffineExpr *>(arg.expr));
}

// These free functions allow clients of the API to not use classes in detail.
AffineExprRef getAffineDimExpr(unsigned position, MLIRContext *context);
AffineExprRef getAffineSymbolExpr(unsigned position, MLIRContext *context);
AffineExprRef getAffineConstantExpr(int64_t constant, MLIRContext *context);

namespace detail {

/// A one-dimensional affine expression.
/// AffineExpression's are immutable (like Type's)
class AffineExpr {
public:
  /// Return the classification for this type.
  AffineExprKind getKind() { return kind; }

  void print(raw_ostream &os);
  void dump();

  /// Returns true if this expression is made out of only symbols and
  /// constants, i.e., it does not involve dimensional identifiers.
  bool isSymbolicOrConstant();

  /// Returns true if this is a pure affine expression, i.e., multiplication,
  /// floordiv, ceildiv, and mod is only allowed w.r.t constants.
  bool isPureAffine();

  /// Returns the greatest known integral divisor of this affine expression.
  uint64_t getLargestKnownDivisor();

  /// Return true if the affine expression is a multiple of 'factor'.
  bool isMultipleOf(int64_t factor);

  MLIRContext *getContext();

protected:
  explicit AffineExpr(AffineExprKind kind, MLIRContext *context)
      : kind(kind), context(context) {}
  ~AffineExpr() {}

private:
  AffineExpr(const AffineExpr &) = delete;
  void operator=(const AffineExpr &) = delete;

  /// Classification of the subclass
  const AffineExprKind kind;
  MLIRContext *context;
};

inline raw_ostream &operator<<(raw_ostream &os, AffineExprRef &expr) {
  expr->print(os);
  return os;
}

/// Affine binary operation expression. An affine binary operation could be an
/// add, mul, floordiv, ceildiv, or a modulo operation. (Subtraction is
/// represented through a multiply by -1 and add.) These expressions are always
/// constructed in a simplified form. For eg., the LHS and RHS operands can't
/// both be constants. There are additional canonicalizing rules depending on
/// the op type: see checks in the constructor.
class AffineBinaryOpExpr : public AffineExpr {
public:
  static AffineExprRef get(AffineExprKind kind, AffineExprRef lhs,
                           AffineExprRef rhs, MLIRContext *context);
  static AffineExprRef getAdd(AffineExprRef lhs, AffineExprRef rhs,
                              MLIRContext *context) {
    return get(AffineExprKind::Add, lhs, rhs, context);
  }
  static AffineExprRef getAdd(AffineExprRef expr, int64_t rhs,
                              MLIRContext *context);
  static AffineExprRef getSub(AffineExprRef lhs, AffineExprRef rhs,
                              MLIRContext *context);

  static AffineExprRef getMul(AffineExprRef lhs, AffineExprRef rhs,
                              MLIRContext *context) {
    return get(AffineExprKind::Mul, lhs, rhs, context);
  }
  static AffineExprRef getMul(AffineExprRef expr, int64_t rhs,
                              MLIRContext *context);
  static AffineExprRef getFloorDiv(AffineExprRef lhs, AffineExprRef rhs,
                                   MLIRContext *context) {
    return get(AffineExprKind::FloorDiv, lhs, rhs, context);
  }
  static AffineExprRef getFloorDiv(AffineExprRef lhs, uint64_t rhs,
                                   MLIRContext *context);
  static AffineExprRef getCeilDiv(AffineExprRef lhs, AffineExprRef rhs,
                                  MLIRContext *context) {
    return get(AffineExprKind::CeilDiv, lhs, rhs, context);
  }
  static AffineExprRef getCeilDiv(AffineExprRef lhs, uint64_t rhs,
                                  MLIRContext *context);
  static AffineExprRef getMod(AffineExprRef lhs, AffineExprRef rhs,
                              MLIRContext *context) {
    return get(AffineExprKind::Mod, lhs, rhs, context);
  }
  static AffineExprRef getMod(AffineExprRef lhs, uint64_t rhs,
                              MLIRContext *context);

  AffineExprRef getLHS() { return lhs; }
  AffineExprRef getRHS() { return rhs; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return const_cast<AffineExpr *>(expr)->getKind() <=
           AffineExprKind::LAST_AFFINE_BINARY_OP;
  }

protected:
  explicit AffineBinaryOpExpr(AffineExprKind kind, AffineExprRef lhs,
                              AffineExprRef rhs, MLIRContext *context);

  const AffineExprRef lhs;
  const AffineExprRef rhs;

private:
  ~AffineBinaryOpExpr() = delete;
};

/// A dimensional identifier appearing in an affine expression.
///
/// This is a POD type of int size; so it should be passed around by
/// value.  The underlying data is owned by MLIRContext and is thus immortal for
/// almost all clients.
class AffineDimExpr : public AffineExpr {
public:
  static AffineExprBaseRef<AffineExpr> get(unsigned position,
                                           MLIRContext *context);

  unsigned getPosition() { return position; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return const_cast<AffineExpr *>(expr)->getKind() == AffineExprKind::DimId;
  }

  friend AffineExprRef mlir::getAffineDimExpr(unsigned position,
                                              MLIRContext *context);

private:
  ~AffineDimExpr() = delete;
  explicit AffineDimExpr(unsigned position, MLIRContext *context)
      : AffineExpr(AffineExprKind::DimId, context), position(position) {}

  /// Position of this identifier in the argument list.
  unsigned position;
};

/// A symbolic identifier appearing in an affine expression.
//
/// This is a POD type of int size, so it should be passed around by
/// value.  The underlying data is owned by MLIRContext and is thus immortal for
/// almost all clients.
class AffineSymbolExpr : public AffineExpr {
public:
  static AffineExprBaseRef<AffineExpr> get(unsigned position,
                                           MLIRContext *context);

  unsigned getPosition() { return position; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return const_cast<AffineExpr *>(expr)->getKind() ==
           AffineExprKind::SymbolId;
  }

  friend AffineExprRef mlir::getAffineSymbolExpr(unsigned position,
                                                 MLIRContext *context);

private:
  ~AffineSymbolExpr() = delete;
  explicit AffineSymbolExpr(unsigned position, MLIRContext *context)
      : AffineExpr(AffineExprKind::SymbolId, context), position(position) {}

  /// Position of this identifier in the symbol list.
  unsigned position;
};

/// An integer constant appearing in affine expression.
class AffineConstantExpr : public AffineExpr {
public:
  static AffineExprBaseRef<AffineExpr> get(int64_t constant,
                                           MLIRContext *context);

  int64_t getValue() { return constant; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return const_cast<AffineExpr *>(expr)->getKind() ==
           AffineExprKind::Constant;
  }

  friend AffineExprRef mlir::getAffineConstantExpr(int64_t constant,
                                                   MLIRContext *context);

private:
  ~AffineConstantExpr() = delete;
  explicit AffineConstantExpr(int64_t constant, MLIRContext *context)
      : AffineExpr(AffineExprKind::Constant, context), constant(constant) {}

  // The constant.
  int64_t constant;
};

} // end namespace detail
} // namespace mlir

namespace llvm {

// AffineExprRef hash just like pointers
template <> struct DenseMapInfo<mlir::AffineExprRef> {
  static mlir::AffineExprRef getEmptyKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getEmptyKey();
    return mlir::AffineExprRef(
        static_cast<mlir::AffineExprRef::ImplType *>(pointer));
  }
  static mlir::AffineExprRef getTombstoneKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getTombstoneKey();
    return mlir::AffineExprRef(
        static_cast<mlir::AffineExprRef::ImplType *>(pointer));
  }
  static unsigned getHashValue(mlir::AffineExprRef val) {
    return mlir::hash_value(val);
  }
  static bool isEqual(mlir::AffineExprRef LHS, mlir::AffineExprRef RHS) {
    return LHS == RHS;
  }
};

} // namespace llvm

#endif // MLIR_IR_AFFINE_EXPR_H
