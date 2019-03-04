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

#include "tensorflow/core/lib/bfloat16/bfloat16.h"

#include "third_party/eigen3/Eigen/Core"

namespace tensorflow {

const uint16_t bfloat16::NAN_VALUE;
const uint16_t bfloat16::ZERO_VALUE;

B16_DEVICE_FUNC bfloat16::operator Eigen::half() const {
  return static_cast<Eigen::half>(float(*this));
}
}  // end namespace tensorflow
