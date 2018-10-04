//===- AffineMap.cpp - MLIR Affine Map Classes ----------------------------===//
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

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/Support/MathExtras.h"
#include "llvm/ADT/StringRef.h"

using namespace mlir;

AffineMap::AffineMap(unsigned numDims, unsigned numSymbols, unsigned numResults,
                     ArrayRef<AffineExprRef> results,
                     ArrayRef<AffineExprRef> rangeSizes)
    : numDims(numDims), numSymbols(numSymbols), numResults(numResults),
      results(results), rangeSizes(rangeSizes) {}

/// Returns a single constant result affine map.
AffineMap *AffineMap::getConstantMap(int64_t val, MLIRContext *context) {
  return get(/*dimCount=*/0, /*symbolCount=*/0,
             {AffineConstantExpr::get(val, context)}, {}, context);
}

bool AffineMap::isIdentity() {
  if (getNumDims() != getNumResults())
    return false;
  ArrayRef<AffineExprRef> results = getResults();
  for (unsigned i = 0, numDims = getNumDims(); i < numDims; ++i) {
    auto *expr =
        const_cast<AffineDimExpr *>(dyn_cast<AffineDimExpr>(results[i]));
    if (!expr || expr->getPosition() != i)
      return false;
  }
  return true;
}

bool AffineMap::isSingleConstant() {
  return getNumResults() == 1 && isa<AffineConstantExpr>(getResult(0));
}

int64_t AffineMap::getSingleConstantResult() {
  assert(isSingleConstant() && "map must have a single constant result");
  return const_cast<AffineConstantExpr *>(
             cast<AffineConstantExpr>(getResult(0)))
      ->getValue();
}

AffineExprRef AffineMap::getResult(unsigned idx) { return results[idx]; }

/// Simplify add expression. Return nullptr if it can't be simplified.
AffineExprRef AffineBinaryOpExpr::simplifyAdd(AffineExprRef lhs,
                                              AffineExprRef rhs,
                                              MLIRContext *context) {
  auto *lhsConst = dyn_cast<AffineConstantExpr>(lhs);
  auto *rhsConst = dyn_cast<AffineConstantExpr>(rhs);

  // Fold if both LHS, RHS are a constant.
  if (lhsConst && rhsConst)
    return AffineConstantExpr::get(lhsConst->getValue() + rhsConst->getValue(),
                                   context);

  // Canonicalize so that only the RHS is a constant. (4 + d0 becomes d0 + 4).
  // If only one of them is a symbolic expressions, make it the RHS.
  if (isa<AffineConstantExpr>(lhs) ||
      (lhs->isSymbolicOrConstant() && !rhs->isSymbolicOrConstant())) {
    return AffineBinaryOpExpr::getAdd(rhs, lhs, context);
  }

  // At this point, if there was a constant, it would be on the right.

  // Addition with a zero is a noop, return the other input.
  if (rhsConst) {
    if (rhsConst->getValue() == 0)
      return lhs;
  }
  // Fold successive additions like (d0 + 2) + 3 into d0 + 5.
  auto *lBin =
      const_cast<AffineBinaryOpExpr *>(dyn_cast<AffineBinaryOpExpr>(lhs));
  if (lBin && rhsConst && lBin->getKind() == Kind::Add) {
    if (auto *lrhs = const_cast<AffineConstantExpr *>(
            dyn_cast<AffineConstantExpr>(lBin->getRHS())))
      return lBin->getLHS() + (lrhs->getValue() + rhsConst->getValue());
  }

  // When doing successive additions, bring constant to the right: turn (d0 + 2)
  // + d1 into (d0 + d1) + 2.
  if (lBin && lBin->getKind() == Kind::Add) {
    if (auto *lrhs = const_cast<AffineConstantExpr *>(
            dyn_cast<AffineConstantExpr>(lBin->getRHS()))) {
      return lBin->getLHS() + rhs + lrhs;
    }
  }

  return nullptr;
}

/// Simplify a multiply expression. Return nullptr if it can't be simplified.
AffineExprRef AffineBinaryOpExpr::simplifyMul(AffineExprRef lhs,
                                              AffineExprRef rhs,
                                              MLIRContext *context) {
  auto *lhsConst = dyn_cast<AffineConstantExpr>(lhs);
  auto *rhsConst = dyn_cast<AffineConstantExpr>(rhs);

  if (lhsConst && rhsConst)
    return AffineConstantExpr::get(lhsConst->getValue() * rhsConst->getValue(),
                                   context);

  assert(lhs->isSymbolicOrConstant() || rhs->isSymbolicOrConstant());

  // Canonicalize the mul expression so that the constant/symbolic term is the
  // RHS. If both the lhs and rhs are symbolic, swap them if the lhs is a
  // constant. (Note that a constant is trivially symbolic).
  if (!rhs->isSymbolicOrConstant() || isa<AffineConstantExpr>(lhs)) {
    // At least one of them has to be symbolic.
    return AffineBinaryOpExpr::getMul(rhs, lhs, context);
  }

  // At this point, if there was a constant, it would be on the right.

  // Multiplication with a one is a noop, return the other input.
  if (rhsConst) {
    if (rhsConst->getValue() == 1)
      return lhs;
    // Multiplication with zero.
    if (rhsConst->getValue() == 0)
      return rhsConst;
  }

  // Fold successive multiplications: eg: (d0 * 2) * 3 into d0 * 6.
  auto *lBin =
      const_cast<AffineBinaryOpExpr *>(dyn_cast<AffineBinaryOpExpr>(lhs));
  if (lBin && rhsConst && lBin->getKind() == Kind::Mul) {
    if (auto *lrhs = const_cast<AffineConstantExpr *>(
            dyn_cast<AffineConstantExpr>(lBin->getRHS())))
      return lBin->getLHS() * (lrhs->getValue() * rhsConst->getValue());
  }

  // When doing successive multiplication, bring constant to the right: turn (d0
  // * 2) * d1 into (d0 * d1) * 2.
  if (lBin && lBin->getKind() == Kind::Mul) {
    if (auto *lrhs = const_cast<AffineConstantExpr *>(
            dyn_cast<AffineConstantExpr>(lBin->getRHS()))) {
      return (lBin->getLHS() * rhs) * lrhs;
    }
  }

  return nullptr;
}

AffineExprRef AffineBinaryOpExpr::simplifyFloorDiv(AffineExprRef lhs,
                                                   AffineExprRef rhs,
                                                   MLIRContext *context) {
  auto *lhsConst = dyn_cast<AffineConstantExpr>(lhs);
  auto *rhsConst = dyn_cast<AffineConstantExpr>(rhs);

  if (lhsConst && rhsConst)
    return AffineConstantExpr::get(
        floorDiv(lhsConst->getValue(), rhsConst->getValue()), context);

  // Fold floordiv of a multiply with a constant that is a multiple of the
  // divisor. Eg: (i * 128) floordiv 64 = i * 2.
  if (rhsConst) {
    if (rhsConst->getValue() == 1)
      return lhs;

    auto *lBin =
        const_cast<AffineBinaryOpExpr *>(dyn_cast<AffineBinaryOpExpr>(lhs));
    if (lBin && lBin->getKind() == Kind::Mul) {
      if (auto *lrhs = const_cast<AffineConstantExpr *>(
              dyn_cast<AffineConstantExpr>(lBin->getRHS()))) {
        // rhsConst is known to be positive if a constant.
        if (lrhs->getValue() % rhsConst->getValue() == 0)
          return lBin->getLHS() * (lrhs->getValue() / rhsConst->getValue());
      }
    }
  }

  return nullptr;
}

AffineExprRef AffineBinaryOpExpr::simplifyCeilDiv(AffineExprRef lhs,
                                                  AffineExprRef rhs,
                                                  MLIRContext *context) {
  auto *lhsConst = dyn_cast<AffineConstantExpr>(lhs);
  auto *rhsConst = dyn_cast<AffineConstantExpr>(rhs);

  if (lhsConst && rhsConst)
    return AffineConstantExpr::get(
        ceilDiv(lhsConst->getValue(), rhsConst->getValue()), context);

  // Fold ceildiv of a multiply with a constant that is a multiple of the
  // divisor. Eg: (i * 128) ceildiv 64 = i * 2.
  if (rhsConst) {
    if (rhsConst->getValue() == 1)
      return lhs;

    auto *lBin =
        const_cast<AffineBinaryOpExpr *>(dyn_cast<AffineBinaryOpExpr>(lhs));
    if (lBin && lBin->getKind() == Kind::Mul) {
      if (auto *lrhs = const_cast<AffineConstantExpr *>(
              dyn_cast<AffineConstantExpr>(lBin->getRHS()))) {
        // rhsConst is known to be positive if a constant.
        if (lrhs->getValue() % rhsConst->getValue() == 0)
          return lBin->getLHS() * (lrhs->getValue() / rhsConst->getValue());
      }
    }
  }

  return nullptr;
}

AffineExprRef AffineBinaryOpExpr::simplifyMod(AffineExprRef lhs,
                                              AffineExprRef rhs,
                                              MLIRContext *context) {
  auto *lhsConst = dyn_cast<AffineConstantExpr>(lhs);
  auto *rhsConst = dyn_cast<AffineConstantExpr>(rhs);

  if (lhsConst && rhsConst)
    return AffineConstantExpr::get(
        mod(lhsConst->getValue(), rhsConst->getValue()), context);

  // Fold modulo of an expression that is known to be a multiple of a constant
  // to zero if that constant is a multiple of the modulo factor. Eg: (i * 128)
  // mod 64 is folded to 0, and less trivially, (i*(j*4*(k*32))) mod 128 = 0.
  if (rhsConst) {
    // rhsConst is known to be positive if a constant.
    if (lhs->getLargestKnownDivisor() % rhsConst->getValue() == 0)
      return AffineConstantExpr::get(0, context);
  }

  return nullptr;
  // TODO(bondhugula): In general, this can be simplified more by using the GCD
  // test, or in general using quantifier elimination (add two new variables q
  // and r, and eliminate all variables from the linear system other than r. All
  // of this can be done through mlir/Analysis/'s FlatAffineConstraints.
}
