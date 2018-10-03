//===- AffineMap.h - MLIR Affine Map Class ----------------------*- C++ -*-===//
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

namespace mlir {

class MLIRContext;

/// A one-dimensional affine expression.
/// AffineExpression's are immutable (like Type's)
class AffineExpr {
public:
  enum class Kind {
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

  /// Return the classification for this type.
  Kind getKind() const { return kind; }

  void print(raw_ostream &os) const;
  void dump() const;

  /// Returns true if this expression is made out of only symbols and
  /// constants, i.e., it does not involve dimensional identifiers.
  bool isSymbolicOrConstant() const;

  /// Returns true if this is a pure affine expression, i.e., multiplication,
  /// floordiv, ceildiv, and mod is only allowed w.r.t constants.
  bool isPureAffine() const;

  /// Returns the greatest known integral divisor of this affine expression.
  uint64_t getLargestKnownDivisor() const;

  /// Return true if the affine expression is a multiple of 'factor'.
  bool isMultipleOf(int64_t factor) const;

  MLIRContext *getContext() const;

protected:
  explicit AffineExpr(Kind kind, MLIRContext *context)
      : kind(kind), context(context) {}
  ~AffineExpr() {}

private:
  AffineExpr(const AffineExpr &) = delete;
  void operator=(const AffineExpr &) = delete;

  /// Classification of the subclass
  const Kind kind;
  MLIRContext *context;
};

inline raw_ostream &operator<<(raw_ostream &os, const AffineExpr &expr) {
  expr.print(os);
  return os;
}

// Helper structure to build AffineExpr with intuitive operators in order to
// operate on chainable, lightweight value types instead of pointer types.
struct AffineExprWrap {
  /* implicit */ AffineExprWrap(mlir::AffineExpr *expr) : expr(expr) {}

  AffineExprWrap(const AffineExprWrap &other) : expr(other.expr){};
  AffineExprWrap &operator=(AffineExprWrap other) {
    expr = other.expr;
    return *this;
  };
  bool operator==(AffineExprWrap other) { return expr == other.expr; };
  mlir::AffineExpr *operator->() { return expr; }
  const mlir::AffineExpr *operator->() const { return expr; }
  /* implicit */ operator mlir::AffineExpr *() { return expr; }

  bool operator!() { return expr == nullptr; }

  AffineExprWrap operator+(int64_t v) const;
  AffineExprWrap operator+(AffineExprWrap other) const;
  AffineExprWrap operator-() const;
  AffineExprWrap operator-(int64_t v) const;
  AffineExprWrap operator-(AffineExprWrap other) const;
  AffineExprWrap operator*(int64_t v) const;
  AffineExprWrap operator*(AffineExprWrap other) const;
  AffineExprWrap floorDiv(uint64_t v) const;
  AffineExprWrap floorDiv(AffineExprWrap other) const;
  AffineExprWrap ceilDiv(uint64_t v) const;
  AffineExprWrap ceilDiv(AffineExprWrap other) const;
  AffineExprWrap operator%(uint64_t v) const;
  AffineExprWrap operator%(AffineExprWrap other) const;

  AffineExpr *expr;
};

/// Affine binary operation expression. An affine binary operation could be an
/// add, mul, floordiv, ceildiv, or a modulo operation. (Subtraction is
/// represented through a multiply by -1 and add.) These expressions are always
/// constructed in a simplified form. For eg., the LHS and RHS operands can't
/// both be constants. There are additional canonicalizing rules depending on
/// the op type: see checks in the constructor.
class AffineBinaryOpExpr : public AffineExpr {
public:
  static AffineExprWrap get(Kind kind, AffineExprWrap lhs, AffineExprWrap rhs,
                            MLIRContext *context);
  static AffineExprWrap getAdd(AffineExprWrap lhs, AffineExprWrap rhs,
                               MLIRContext *context) {
    return get(AffineExpr::Kind::Add, lhs, rhs, context);
  }
  static AffineExprWrap getAdd(AffineExprWrap expr, int64_t rhs,
                               MLIRContext *context);
  static AffineExprWrap getSub(AffineExprWrap lhs, AffineExprWrap rhs,
                               MLIRContext *context);

  static AffineExprWrap getMul(AffineExprWrap lhs, AffineExprWrap rhs,
                               MLIRContext *context) {
    return get(AffineExpr::Kind::Mul, lhs, rhs, context);
  }
  static AffineExprWrap getMul(AffineExprWrap expr, int64_t rhs,
                               MLIRContext *context);
  static AffineExprWrap getFloorDiv(AffineExprWrap lhs, AffineExprWrap rhs,
                                    MLIRContext *context) {
    return get(AffineExpr::Kind::FloorDiv, lhs, rhs, context);
  }
  static AffineExprWrap getFloorDiv(AffineExprWrap lhs, uint64_t rhs,
                                    MLIRContext *context);
  static AffineExprWrap getCeilDiv(AffineExprWrap lhs, AffineExprWrap rhs,
                                   MLIRContext *context) {
    return get(AffineExpr::Kind::CeilDiv, lhs, rhs, context);
  }
  static AffineExprWrap getCeilDiv(AffineExprWrap lhs, uint64_t rhs,
                                   MLIRContext *context);
  static AffineExprWrap getMod(AffineExprWrap lhs, AffineExprWrap rhs,
                               MLIRContext *context) {
    return get(AffineExpr::Kind::Mod, lhs, rhs, context);
  }
  static AffineExprWrap getMod(AffineExprWrap lhs, uint64_t rhs,
                               MLIRContext *context);

  AffineExprWrap getLHS() const { return lhs; }
  AffineExprWrap getRHS() const { return rhs; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return expr->getKind() <= Kind::LAST_AFFINE_BINARY_OP;
  }

protected:
  explicit AffineBinaryOpExpr(Kind kind, AffineExpr *lhs, AffineExpr *rhs,
                              MLIRContext *context);

  AffineExpr *const lhs;
  AffineExpr *const rhs;

private:
  ~AffineBinaryOpExpr() = delete;
  // Simplification prior to construction of binary affine op expressions.
  static AffineExpr *simplifyAdd(AffineExpr *lhs, AffineExpr *rhs,
                                 MLIRContext *context);
  static AffineExpr *simplifyMul(AffineExpr *lhs, AffineExpr *rhs,
                                 MLIRContext *context);
  static AffineExpr *simplifyFloorDiv(AffineExpr *lhs, AffineExpr *rhs,
                                      MLIRContext *context);
  static AffineExpr *simplifyCeilDiv(AffineExpr *lhs, AffineExpr *rhs,
                                     MLIRContext *context);
  static AffineExpr *simplifyMod(AffineExpr *lhs, AffineExpr *rhs,
                                 MLIRContext *context);
};

/// A dimensional identifier appearing in an affine expression.
///
/// This is a POD type of int size; so it should be passed around by
/// value.  The underlying data is owned by MLIRContext and is thus immortal for
/// almost all clients.
class AffineDimExpr : public AffineExpr {
public:
  static AffineExprWrap get(unsigned position, MLIRContext *context);

  unsigned getPosition() const { return position; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return expr->getKind() == Kind::DimId;
  }

private:
  ~AffineDimExpr() = delete;
  explicit AffineDimExpr(unsigned position, MLIRContext *context)
      : AffineExpr(Kind::DimId, context), position(position) {}

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
  static AffineExprWrap get(unsigned position, MLIRContext *context);

  unsigned getPosition() const { return position; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return expr->getKind() == Kind::SymbolId;
  }

private:
  ~AffineSymbolExpr() = delete;
  explicit AffineSymbolExpr(unsigned position, MLIRContext *context)
      : AffineExpr(Kind::SymbolId, context), position(position) {}

  /// Position of this identifier in the symbol list.
  unsigned position;
};

/// An integer constant appearing in affine expression.
class AffineConstantExpr : public AffineExpr {
public:
  static AffineExprWrap get(int64_t constant, MLIRContext *context);

  int64_t getValue() const { return constant; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AffineExpr *expr) {
    return expr->getKind() == Kind::Constant;
  }

private:
  ~AffineConstantExpr() = delete;
  explicit AffineConstantExpr(int64_t constant, MLIRContext *context)
      : AffineExpr(Kind::Constant, context), constant(constant) {}

  // The constant.
  int64_t constant;
};

} // end namespace mlir

#endif // MLIR_IR_AFFINE_EXPR_H
