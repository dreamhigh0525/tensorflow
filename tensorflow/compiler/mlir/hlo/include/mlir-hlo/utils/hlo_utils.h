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

#ifndef TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_UTILS_HLO_UTILS_H_
#define TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_UTILS_HLO_UTILS_H_

#include <set>
#include <string>
#include <unordered_map>

#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/TypeUtilities.h"

namespace mlir {
namespace hlo {

// Attrs for placement
constexpr llvm::StringRef kPlaceTyAttr = "mhlo_place_type";
constexpr llvm::StringRef kTypeHost = "host";
constexpr llvm::StringRef kTypeDevice = "device";
enum class PlacementType {
  kHost,
  kDevice,
};

// Input & output placement attr
const std::string kInputPlacementAttr = "input_placements";
const std::string kOutputPlacementAttr = "output_placements";

// for rule based placement strategy, the placement of the op in the list
// is up to the placement of the dominant operand
const std::unordered_map<std::string, /*dominant operand index*/ int>
    kPlaceRuleMap = {{"mhlo.dynamic_gather", /*operand*/ 0},
                     {"mhlo.gather", /*operand*/ 0}};

const std::unordered_map<std::string, std::set<int>> kShapeCalcOperandMap = {
    {"mhlo.real_dynamic_slice",
     {/*start_indices*/ 1, /*limit_indices*/ 2, /*strides*/ 3}},
    {"mhlo.dynamic_pad",
     {/*edge_padding_low*/ 2, /*edge_padding_high*/ 3, /*interior_padding*/ 4}},
    {"mhlo.dynamic_reshape", {/*shape*/ 1}},
    {"mhlo.dynamic_iota", {/*shape*/ 0}},
    {"mhlo.dynamic_broadcast_in_dim", {/*out_dim_size*/ 1}},
    {"mhlo.dynamic_gather", {/*slice_sizes*/ 2}},
    {"mhlo.dynamic_conv", {/*paddings*/ 2}},
    {"mhlo.if", {/*pred*/ 0}},
    {"mhlo.dynamic_rng_uniform", {/*start*/ 0, /*limit*/ 1, /*shape*/ 2}}};

// Computes the broadcast dimensions attr for an elementwise binary operator
// between two ranked tensors.
// If `allow_empty` is true, then null can be returned to mean that the
// broadcast is an "identity".
mlir::DenseIntElementsAttr getBroadcastDimensionsAttr(mlir::Builder* b,
                                                      mlir::Value x,
                                                      mlir::Value y,
                                                      bool allow_empty = true);

// Get a constant splat for the given value of type. Requires value to be of
// type static shaped RankedTensorType.
template <typename T>
static ElementsAttr getSplat(Builder* b, RankedTensorType ty, T constant) {
  Type element_ty = getElementTypeOrSelf(ty);

  if (element_ty.isSignlessInteger())
    return DenseElementsAttr::get(ty, b->getIntegerAttr(element_ty, constant));

  if (element_ty.isa<FloatType>())
    return DenseElementsAttr::get(ty, b->getFloatAttr(element_ty, constant));

  if (auto complex_ty = element_ty.dyn_cast<ComplexType>()) {
    auto complex_element_ty = complex_ty.getElementType();
    if (complex_element_ty.isF32())
      return DenseElementsAttr::get(ty,
                                    static_cast<std::complex<float>>(constant));
    if (complex_element_ty.isF64())
      return DenseElementsAttr::get(
          ty, static_cast<std::complex<double>>(constant));
  }
  llvm_unreachable("unhandled element type");
}

template <typename T>
static ElementsAttr getSplat(Builder* b, Value val, T constant) {
  return getSplat(b, val.getType().cast<RankedTensorType>(), constant);
}

// Returns DenseElementsAttr of rank zero with the given element type and the
// value.
// Requires `ty` to be either FloatType, IntegerType, or ComplexType.
DenseElementsAttr GetScalarOfType(Type ty, int64_t raw_value);

// Enum type used to specify scalar argument to GetScalarLimitOfType.
enum ScalarLimit {
  kLowest,          // The scalar corresponding to numeric_limits<T>::lowest.
  kInfinityLowest,  // Like kLowest, but returns -infinity where available.
  kMax,             // The scalar corresponding to numeric_limits<T>::max.
  kInfinityMax,     // Like kMax, but returns infinity where available.
};

// Returns a scalar limit value for the given type.
//
// The argument 'limit' describes which scalar value to return.
//
// Requires `ty` to be either FloatType or IntegerType.
DenseElementsAttr GetScalarLimitOfType(Type ty, ScalarLimit limit);

// Given `op_name` from LMHLO, returns the corresponding op name in MHLO.
// Returns empty string if no such op exists.
std::string LmhloToMhloOpName(llvm::StringRef op_name,
                              mlir::MLIRContext* context);

// Return true if Attr has values [0, 1, ...].
bool IsSequenceStartingWith0(DenseIntElementsAttr attr);

int64_t getArgumentIndex(mlir::FuncOp op, Value value);

PlacementType getInputPlacement(Value arg);

}  // namespace hlo
}  // namespace mlir

#endif  // TENSORFLOW_COMPILER_MLIR_HLO_INCLUDE_MLIR_HLO_UTILS_HLO_UTILS_H_
