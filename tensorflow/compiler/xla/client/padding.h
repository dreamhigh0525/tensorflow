/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_CLIENT_PADDING_H_
#define TENSORFLOW_COMPILER_XLA_CLIENT_PADDING_H_

#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/core/lib/gtl/array_slice.h"

namespace xla {

// Describes the padding applied for a windowed operation like
// convolution, where a window is placed inside a base area.
enum class Padding {
  // Make the output have the same dimensions as the base area. For
  // example, for a 3x3 base area and a 2x2 window, the output will be
  // 3x3, so that requires padding the 3x3 base area to 4x4.
  kSame,

  // Use no padding. For example, for a 4x4 base area and a 2x2
  // window, the output will be 3x3.
  kValid,
};

// Validates that the slices are acceptable for determining padding -- this can
// be used to check the preconditions of MakePadding below to produce an error
// message that can be returned to the user.
Status ValidatePaddingValues(absl::Span<const int64> input_dimensions,
                             absl::Span<const int64> window_dimensions,
                             absl::Span<const int64> window_strides);

// Returns the padding needed for the base area, given the base area dimensions,
// window dimensions, strides, and the type of padding.
//
// If v is the returned vector, then for each dimension number i,
// v[i].first is the padding to the left (i.e. in the direction of
// lower indices) and v[i].second is the padding to the right (i.e. in
// the direction of higher indices).
//
// Precondition: The number of dimensions (i.e., rank) in input_dimensions,
// window_dimensions, and strides must match, which is equal to the number
// of elements in the result vector.
std::vector<std::pair<int64, int64>> MakePadding(
    absl::Span<const int64> input_dimensions,
    absl::Span<const int64> window_dimensions,
    absl::Span<const int64> window_strides, Padding padding);

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_CLIENT_PADDING_H_
