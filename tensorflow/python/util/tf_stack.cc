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

// We extract stack traces in Python using the logic in tf_stack.cc, which
// stores a list of PyCodeObject*. Such stack trace extraction is really fast.
//
// We store the retrieved stack trace within the Node object directly. Then
// whenever the graph is instantiated/copies, we copy the stack trace with it.
// Since the graph instantiation goes through the protobuf roundtrip, we store
// the original stack traces mapping attached in FunctionLibraryDefinition.

#include <Python.h>
#include <frameobject.h>

#include <algorithm>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/hash/hash.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"
#include "pybind11/stl_bind.h"
#include "tensorflow/c/c_api_internal.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/python/util/stack_trace.h"

struct StackFrame;  // Forward declaration.
struct StackTrace;

PYBIND11_MAKE_OPAQUE(std::vector<StackFrame>);
PYBIND11_MAKE_OPAQUE(StackTrace);

namespace tensorflow {

namespace {

namespace py = pybind11;

// Returns contents of the line corresponding to the given frame.
//
// Precondition: must be holding Python GIL.
py::str LineContents(const StackFrame& frame) {
  DCheckPyGilStateForStackTrace();
  static const auto* linecache =
      new py::module(py::module::import("linecache"));
  const auto& checkcache = linecache->attr("checkcache");
  const auto& getline = linecache->attr("getline");
  checkcache(py::str(frame.file_name));
  return py::cast<py::str>(
      getline(py::str(frame.file_name), py::int_(frame.line_number))
          .attr("strip")());
}

// Ignores the frames containing this substring for common prefix calculation.
static const char* kFilenameToIgnorePrefix = "<embedded";

// Converts the given stack frame to string, according to options defined in
// `opts`.
std::string StackFrameToString(
    const StackFrame& frame,
    const AbstractStackTrace::TracePrintingOptions& opts,
    int shared_prefix_size = 0) {
  std::string out = absl::StrFormat(
      "File \"%s\", line %d, in %s",
      absl::StrContains(frame.file_name, kFilenameToIgnorePrefix)
          ? frame.file_name
          : frame.file_name.substr(shared_prefix_size),
      frame.line_number, frame.function_name);

  if (opts.show_line_contents) {
    PyGILState_STATE state = PyGILState_Ensure();
    std::string line_contents = std::string(LineContents(frame));
    PyGILState_Release(state);
    if (!line_contents.empty()) {
      absl::StrAppend(&out, "\n  ", line_contents);
    }
  }
  return out;
}

class StackTraceWrapper : public AbstractStackTrace {
 public:
  StackTraceWrapper(StackTrace&& captured, const py::dict& source_map,
                    const py::set& filtered_filenames)
      : captured_(std::move(captured)),
        source_map_(source_map),
        filtered_filenames_(filtered_filenames) {}

  explicit StackTraceWrapper(absl::Span<StackFrame const> stack_frames)
      : stack_frames_cache_(std::vector<StackFrame>(stack_frames.begin(),
                                                    stack_frames.end())) {}

  static StackTraceWrapper ExtractStack(const py::object& limit,
                                        const py::list& mappers,
                                        const py::list& filters) {
    // In Python 3.X ``traceback.extract_stack`` allows ``limit`` to
    // either be None or -1.
    int casted_limit = limit.is_none() ? -1 : py::cast<ssize_t>(limit);

    // Raise limit by one since we are dropping the last frame.
    if (casted_limit != -1) casted_limit++;

    const py::dict& source_map =
        mappers.empty()
            ? py::dict()
            : mappers[mappers.size() - 1].attr("get_effective_source_map")();
    const py::set& filtered_filenames =
        filters.empty()
            ? py::set()
            : filters[filters.size() - 1].attr("get_filtered_filenames")();
    return StackTraceWrapper{StackTrace::Capture(casted_limit), source_map,
                             filtered_filenames};
  }

  absl::Span<StackFrame const> ToFrames() const override {
    GenerateCache();
    return *stack_frames_cache_;
  }

  StackFrame LastUserFrame() const override {
    GenerateLastFrameCache();
    return *last_stack_frame_cache_;
  }

  std::string ToString(const TracePrintingOptions& opts) const override {
    GenerateCache();
    std::vector<std::string> files_to_find_prefix;
    for (const StackFrame& frame : *stack_frames_cache_) {
      if (!absl::StrContains(frame.file_name, kFilenameToIgnorePrefix)) {
        files_to_find_prefix.push_back(frame.file_name);
      }
    }
    int shared_prefix_size =
        opts.filter_common_prefix
            ? io::CommonPathPrefix(files_to_find_prefix).size()
            : 0;

    if (!opts.drop_internal_frames) {
      return ToStringHelper(*stack_frames_cache_, opts, shared_prefix_size);
    }

    std::vector<StackFrame> filtered_frames;
    for (const StackFrame& frame : *stack_frames_cache_) {
      if (!IsInternalFrameForFilename(frame.file_name)) {
        filtered_frames.push_back(frame);
      }
    }
    return ToStringHelper(filtered_frames, opts, shared_prefix_size);
  }

  bool IsCacheGenerated() const { return stack_frames_cache_.has_value(); }

  void GenerateCache() const {
    if (stack_frames_cache_) {
      return;
    }

    // Grabbing the GIL solves two purposes: 1) makes the class thread-safe, and
    // 2) ToStackFrames and LineContents actually need it.
    PyGILState_STATE state = PyGILState_Ensure();

    stack_frames_cache_ = captured_.ToStackFrames(
        [&](std::pair<const char*, int> p) { return StackTraceMapping(p); },
        [&](const char* f) { return StackTraceFiltering(f); });
    stack_frames_cache_->pop_back();  // Drop last stack frame.
    PyGILState_Release(state);
  }

  void GenerateLastFrameCache() const {
    if (last_stack_frame_cache_) {
      return;
    }

    PyGILState_STATE state = PyGILState_Ensure();
    auto f = [&](const char* file_name) -> bool {
      return StackTraceFiltering(file_name) ||
             IsInternalFrameForFilename(file_name);
    };

    std::vector<StackFrame> last_frame = captured_.ToStackFrames(
        [&](std::pair<const char*, int> p) { return StackTraceMapping(p); }, f,
        /*reverse_traversal=*/true,
        /*limit=*/1);

    if (last_frame.empty()) {
      last_stack_frame_cache_ = StackFrame{};
    } else {
      DCHECK(last_frame.size() == 1);
      last_stack_frame_cache_ = last_frame[0];
    }
    PyGILState_Release(state);
  }

  StackTraceWrapper(StackTraceWrapper&&) = default;
  ~StackTraceWrapper() override {
    PyGILState_STATE state = PyGILState_Ensure();
    captured_.Clear();
    source_map_.reset();
    filtered_filenames_.reset();
    PyGILState_Release(state);
  }

 private:
  static std::string ToStringHelper(absl::Span<StackFrame const> stack_frames,
                                    const TracePrintingOptions& opts,
                                    int shared_prefix_size) {
    return absl::StrJoin(
        stack_frames, "\n", [&](std::string* out, const StackFrame& frame) {
          absl::StrAppend(out,
                          StackFrameToString(frame, opts, shared_prefix_size));
        });
  }

  static bool IsInternalFrameForFilename(absl::string_view file_name) {
    // Use a simple heuristic for now.
    // TODO(cheshire): Build a more sophisticated mechanism, rely on @tf.export.
    return absl::StrContains(file_name, "tensorflow/python") &&
           !absl::StrContains(file_name, "keras") &&
           !absl::StrContains(file_name, "test.py");
  }

  absl::optional<StackFrame> StackTraceMapping(
      std::pair<const char*, int> p) const {
    if (source_map_->empty()) {
      return absl::nullopt;
    }

    auto key = py::make_tuple(py::str(p.first), py::int_(p.second));

    if (source_map_->contains(key)) {
      const py::tuple& value = (*source_map_)[key];
      return StackFrame{std::string(py::cast<py::str>(value[0])),
                        py::cast<py::int_>(value[1]),
                        std::string(py::cast<py::str>(value[2]))};
    }

    return absl::nullopt;
  }

  bool StackTraceFiltering(const char* file_name) const {
    return filtered_filenames_->contains(file_name);
  }

  StackTrace captured_;
  // Using optional to force destruction while we hold a GIL.
  absl::optional<py::dict> source_map_;
  absl::optional<py::set> filtered_filenames_;

  mutable absl::optional<std::vector<StackFrame>> stack_frames_cache_;
  mutable absl::optional<StackFrame> last_stack_frame_cache_;
};

}  // namespace

PYBIND11_MODULE(_tf_stack, m) {
  py::class_<StackFrame>(m, "StackFrame")
      .def_property_readonly(
          "filename",
          [](const StackFrame& self) { return py::str(self.file_name); })
      .def_property_readonly(
          "lineno",
          [](const StackFrame& self) { return py::int_(self.line_number); })
      .def_property_readonly(
          "name",
          [](const StackFrame& self) { return py::str(self.function_name); })
      .def_property_readonly(
          "line", [](const StackFrame& self) { return LineContents(self); })

      // For compatibility with the traceback module.
      .def("__eq__", &StackFrame::operator==)
      .def("__ne__", &StackFrame::operator!=)
      .def("__hash__",
           [](const StackFrame& self) {
             return absl::Hash<std::tuple<std::string, int, std::string>>()(
                 std::make_tuple(self.file_name, self.line_number,
                                 self.function_name));
           })
      .def("__getitem__",
           [](const StackFrame& self, const py::object& index) -> py::object {
             return py::make_tuple(
                 py::str(self.file_name), py::int_(self.line_number),
                 py::str(self.function_name), LineContents(self))[index];
           })
      .def("__iter__",
           [](const StackFrame& self) {
             return py::iter(py::make_tuple(
                 py::str(self.file_name), py::int_(self.line_number),
                 py::str(self.function_name), LineContents(self))

             );
           })
      .def("__repr__",
           [](const StackFrame& self) { return StackFrameToString(self, {}); })
      .def("__len__", [](const StackFrame&) { return 4; });

  py::class_<StackTraceWrapper>(m, "StackTraceWrapper", py::module_local(true))
      // TODO(slebedev): upstream negative indexing support into pybind11.
      .def(
          "__getitem__",
          [](const StackTraceWrapper& self, ssize_t index) {
            absl::Span<StackFrame const> frames = self.ToFrames();
            const size_t eff_index =
                index < 0 ? frames.size() + index : static_cast<size_t>(index);
            if (eff_index >= frames.size()) {
              throw py::index_error();
            }
            return frames[eff_index];
          },
          py::return_value_policy::reference_internal)
      .def(
          "__getitem__",
          [](const StackTraceWrapper& self, py::slice slice) {
            absl::Span<StackFrame const> frames = self.ToFrames();
            py::ssize_t start, stop, step, slicelength;
            if (!slice.compute(frames.size(), &start, &stop, &step,
                               &slicelength)) {
              throw py::error_already_set();
            }
            if (step == 1) {
              return StackTraceWrapper{frames.subspan(start, slicelength)};
            }
            // TODO(cheshire): Cleanup, use Python slicing logic directly
            // instead.
            std::vector<StackFrame> out;
            out.reserve(slicelength);
            // Python slices allow negative indexing.
            for (int i = start; i != stop; i += step) {
              out.push_back(frames[i]);
            }
            return StackTraceWrapper{out};
          },
          py::return_value_policy::reference_internal)
      .def("__len__",
           [](const StackTraceWrapper& self) { return self.ToFrames().size(); })
      .def("__eq__",
           [](const StackTraceWrapper& self, const StackTraceWrapper& other) {
             return self.ToFrames() == other.ToFrames();
           })
      .def("__hash__",
           [](const StackTraceWrapper& self) {
             self.GenerateCache();
             return py::hash(py::str(self.ToString({})));
           })
      .def("__repr__",
           [](const StackTraceWrapper& self) {
             self.GenerateCache();
             return py::str(self.ToString({}));
           })
      .def("last_user_frame",
           [](const StackTraceWrapper& self) { return self.LastUserFrame(); });

  m.def(
      "extract_stack_for_node",
      [](const py::object& limit, const py::list& mappers,
         const py::list& filters,
         TF_Operation* op) -> const AbstractStackTrace& {
        Node* node = reinterpret_cast<Node*>(op);
        DCHECK(!node->GetStackTrace()) << "Should not reset the stack trace";
        node->SetStackTrace(std::make_shared<StackTraceWrapper>(
            StackTraceWrapper::ExtractStack(limit, mappers, filters)));
        return *node->GetStackTrace();
      },
      py::return_value_policy::reference);

  m.def(
      "extract_stack",
      [](const py::object& limit, const py::list& mappers,
         const py::list& filters) {
        return StackTraceWrapper::ExtractStack(limit, mappers, filters);
      },
      py::return_value_policy::move);
}

}  // namespace tensorflow
