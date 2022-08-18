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

#include "tensorflow/compiler/xla/runtime/arguments.h"

#include <cstddef>
#include <string>
#include <type_traits>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Casting.h"
#include "tensorflow/compiler/xla/runtime/types.h"
#include "tfrt/support/error_util.h"  // from @tf_runtime

namespace xla {
namespace runtime {

using llvm::dyn_cast;
using llvm::isa;

using llvm::ArrayRef;
using llvm::Error;
using llvm::MutableArrayRef;
using llvm::Optional;
using llvm::raw_ostream;

using tfrt::DType;
using tfrt::MakeStringError;

raw_ostream& OpaqueArg::print(raw_ostream& os) const {
  return os << "OpaqueArg: ptr=" << ptr_;
}

raw_ostream& MemrefDesc::print(raw_ostream& os) const {
  auto print_arr = [&](llvm::StringRef name, ArrayRef<int64_t> arr) {
    os << " " << name << ": [";
    if (!arr.empty()) {
      os << arr[0];
      for (int i = 1; i < arr.size(); ++i) os << ", " << arr[i];
    }
    os << "]";
  };

  os << "MemrefDesc: dtype: " << dtype() << " offset: " << offset();
  print_arr("sizes", sizes());
  print_arr("strides", strides());

  return os;
}

//===----------------------------------------------------------------------===//
// OpaqueArg.
//===----------------------------------------------------------------------===//

Error OpaqueArg::Verify(const Type& type) const {
  if (isa<AsyncTokenType>(type)) return Error::success();
  return MakeStringError("unsupported opaque argument type: ", type);
}

size_t OpaqueArg::Pack(MutableArrayRef<void*> args, size_t offset) const {
  args[offset] = ptr_;
  return ++offset;
}

//===----------------------------------------------------------------------===//
// MemrefDesc.
//===----------------------------------------------------------------------===//

static bool AreCompatibleTypes(DType type1, DType type2) {
  auto compatible = [&](DType fromType, DType toType) {
    return (type1 == fromType && type2 == toType) ||
           (type1 == toType && type2 == fromType);
  };
  // I1 and I8 types are compatible since they both are 1-byte size at runtime.
  if (compatible(DType::I1, DType::I8)) return true;

  // Signed and unsigned integers of the same size are compatible in memory.
  if (compatible(DType::I8, DType::UI8) ||
      compatible(DType::I16, DType::UI16) ||
      compatible(DType::I32, DType::UI32) ||
      compatible(DType::I64, DType::UI64))
    return true;

  return type1 == type2;
}

static Error VerifyMemrefArgument(DType element_type,
                                  Optional<ArrayRef<int64_t>> sizes,
                                  const MemrefDesc& memref) {
  // Format memref argument and expected type for user-friendly error messages.
  auto pretty_print = [&]() -> std::string {
    std::string err;
    llvm::raw_string_ostream os(err);

    auto dim = [](int64_t d) -> std::string {
      return d == MemrefType::kDynamicSize ? "?" : std::to_string(d);
    };

    auto print_shaped = [&](Optional<ArrayRef<int64_t>> dims, DType dtype) {
      if (!dims.has_value()) {
        os << "[*x" << dtype << "]";
        return;
      }

      if (dims->empty()) {
        os << "[" << dtype << "]";
        return;
      }

      os << "[" << dim((*dims)[0]);
      for (int i = 1; i < dims->size(); ++i) os << "x" << dim((*dims)[i]);
      os << "x" << dtype << "]";
    };

    os << "got ";
    print_shaped({memref.sizes()}, memref.dtype());
    os << " vs expected ";
    print_shaped(sizes, element_type);

    return err;
  };

  // Check that memref data type is compatible with the expected element type.
  if (LLVM_UNLIKELY(!AreCompatibleTypes(element_type, memref.dtype()))) {
    return MakeStringError(
        "type is not compatible with the expected element type: ",
        memref.dtype(), " vs ", element_type, " (", pretty_print(), ")");
  }

  // Skip sizes verification if they are not available (unranked tensor or
  // memref type is compatible with run-time arguments of any shape).
  if (!sizes.has_value()) return Error::success();

  // Check that memref rank is the same as the expected rank.
  if (LLVM_UNLIKELY(memref.rank() != sizes->size()))
    return MakeStringError(
        "rank does not match expected input rank: ", memref.rank(), " vs ",
        sizes->size(), " (", pretty_print(), ")");

  // Check that all statically known dimensions matches the memref dimensions.
  for (const auto& pair : llvm::enumerate(llvm::zip(memref.sizes(), *sizes))) {
    int64_t argument_dim = std::get<0>(pair.value());
    int64_t expected_dim = std::get<1>(pair.value());

    bool is_dynamic_dim = MemrefType::IsDynamic(expected_dim);

    if (LLVM_UNLIKELY(argument_dim != expected_dim && !is_dynamic_dim))
      return MakeStringError(
          "dimension #", pair.index(),
          " does not match expected input dimension: ", argument_dim, " vs ",
          expected_dim, " (", pretty_print(), ")");
  }

  return Error::success();
}

Error MemrefDesc::Verify(const Type& type) const {
  // Only ranked memrefs have a defined ABI and can be passed as an argument.
  if (auto* memref = dyn_cast<MemrefType>(&type))
    return VerifyMemrefArgument(memref->element_type(), memref->sizes(), *this);
  return MakeStringError("unsupported memref type: ", type);
}

size_t MemrefDesc::Pack(MutableArrayRef<void*> args, size_t offset) const {
  // Write into the arguments data starting from the given offset.
  void** storage = &args[offset];

  auto cast = [](const void* p) { return const_cast<void*>(p); };

  // Packs memref with a rank not known at compile time.
  auto pack_memref = [&](int64_t rank) -> size_t {
    storage[0] = cast(&data_);  // memref.basePtr
    storage[1] = cast(&data_);  // memref.data
    storage[2] = cast(&offset_);
    for (int64_t d = 0; d < rank; ++d) {
      storage[3 + d] = cast(&sizes_and_strides_[d]);
      storage[3 + rank + d] = cast(&sizes_and_strides_[rank_ + d]);
    }

    // Move offsets to the next argument position.
    return offset + 3 + rank * 2;
  };

  // Packs memref with a rank known at compile time.
  auto pack_ranked_memref = [&](auto rank_tag) -> size_t {
    static constexpr int64_t rank = decltype(rank_tag)::value;
    return pack_memref(rank);
  };

  // Dispatch to lambda with a statically known rank parameter for the most
  // common ranks. It allows to inline the nested lambda, and generate better
  // code without for loops on a hot path.
  switch (rank_) {
    case 0:
      return pack_ranked_memref(std::integral_constant<int64_t, 0>{});
    case 1:
      return pack_ranked_memref(std::integral_constant<int64_t, 1>{});
    case 2:
      return pack_ranked_memref(std::integral_constant<int64_t, 2>{});
    case 3:
      return pack_ranked_memref(std::integral_constant<int64_t, 3>{});
    case 4:
      return pack_ranked_memref(std::integral_constant<int64_t, 4>{});
    default:
      return pack_memref(rank_);
  }
}

//===----------------------------------------------------------------------===//
// Verify that argument type is compatible with the run-time memref argument.
//===----------------------------------------------------------------------===//

static Error VerifyMemrefArgument(const Type& type, const MemrefDesc& arg) {
  if (auto* memref = dyn_cast<MemrefType>(&type))
    return VerifyMemrefArgument(memref->element_type(), memref->sizes(), arg);
  if (auto* memref = dyn_cast<UnrankedMemrefType>(&type))
    return VerifyMemrefArgument(memref->element_type(), llvm::None, arg);

  if (auto* tensor = dyn_cast<RankedTensorType>(&type))
    return VerifyMemrefArgument(tensor->element_type(), tensor->sizes(), arg);
  if (auto* tensor = dyn_cast<UnrankedTensorType>(&type))
    return VerifyMemrefArgument(tensor->element_type(), llvm::None, arg);

  return MakeStringError("unsupported memref type: ", type);
}

Error VerifyMemrefArgument(unsigned index, const Type& type,
                           const MemrefDesc& arg) {
  if (auto err = VerifyMemrefArgument(type, arg))
    return MakeStringError("argument #", index, " ", err);
  return Error::success();
}

}  // namespace runtime
}  // namespace xla
