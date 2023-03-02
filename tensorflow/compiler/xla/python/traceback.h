/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_PYTHON_TRACEBACK_H_
#define TENSORFLOW_COMPILER_XLA_PYTHON_TRACEBACK_H_

#include <memory>
#include <string>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "pybind11/pybind11.h"  // from @pybind11
#include "pybind11/stl.h"  // from @pybind11

namespace xla {

// The representation of frame->f_lasti changed from bytes to words in Python
// 3.10, see https://docs.python.org/3/whatsnew/3.10.html#changes-in-the-c-api
// This should match sizeof(_Py_CODEUNIT) which is unfortunately private.
static constexpr int kLastiWordBytes = (PY_VERSION_HEX < 0x030a0000) ? 1 : 2;

// Represents a Python traceback.
class Traceback {
 public:
  // Require GIL. Creates a Traceback object that requires destructor to be
  // invoked with GIL held as well.
  static std::shared_ptr<Traceback> Get();

  // Safely destroy the traceback object regardless of whether GIL is held or
  // not.
  static void SafeDestroy(Traceback traceback);

  // Require GIL.
  static bool enabled() { return enabled_; }
  // Require GIL.
  static void SetEnabled(bool enabled);

  // Require GIL.
  Traceback();
  // Require GIL.
  ~Traceback();

  Traceback(const Traceback&) = delete;
  Traceback(Traceback&& other);
  Traceback& operator=(const Traceback&) = delete;
  Traceback& operator=(Traceback&&) = delete;

  // Requires the GIL be held.
  std::string ToString() const;

  struct Frame {
    pybind11::str file_name;
    pybind11::str function_name;
    int function_start_line;
    int line_num;

    std::string ToString() const;
  };
  std::vector<Frame> Frames() const;

  const absl::InlinedVector<std::pair<PyCodeObject*, int>, 32>& raw_frames()
      const {
    return frames_;
  }

  // Returns the traceback as a fake Python Traceback object, suitable for
  // using as an exception traceback.
  pybind11::object AsPythonTraceback() const;

  bool operator==(const Traceback& other) const {
    return frames_ == other.frames_;
  }
  bool operator!=(const Traceback& other) const {
    return frames_ != other.frames_;
  }

 private:
  absl::InlinedVector<std::pair<PyCodeObject*, int>, 32> frames_;

  // Protected by GIL.
  static bool enabled_;
};

template <typename H>
H AbslHashValue(H h, const Traceback& traceback) {
  h = H::combine(std::move(h), traceback.raw_frames());
  return h;
}

void BuildTracebackSubmodule(pybind11::module& m);

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_PYTHON_TRACEBACK_H_
