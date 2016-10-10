/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/framework/shape_inference_testutil.h"

#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"

namespace tensorflow {
namespace shape_inference {

using errors::Unknown;

Status ShapeInferenceTestutil::InferShapes(ShapeInferenceTestOp op,
                                           const string& ins,
                                           const string& expected_outs) {
  const OpRegistrationData* op_reg_data;
  TF_RETURN_IF_ERROR(OpRegistry::Global()->LookUp(op.name, &op_reg_data));

  std::vector<string> ins_v = str_util::Split(ins, ';');
  std::unique_ptr<const NodeDef> new_node_def;

  shape_inference::InferenceContext c(&op.node_def, op_reg_data->op_def, ins_v,
                                      op.input_tensors);
  TF_RETURN_IF_ERROR(c.construction_status());
  if (op_reg_data->shape_inference_fn == nullptr) {
    return errors::InvalidArgument(
        "No shape inference function exists for op '", op.name,
        "', did you forget to define it?");
  }

  TF_RETURN_IF_ERROR(c.Run(op_reg_data->shape_inference_fn));

  const int num_outputs = c.num_outputs();

  if (expected_outs == "e") {
    return Unknown("Shape inference should have returned error");
  }

  // Verify the output shape.
  std::vector<string> expected_outs_v = str_util::Split(expected_outs, ';');
  if (num_outputs != expected_outs_v.size()) {
    return Unknown("The expected output string lists the wrong number of ",
                   "outputs. It lists ", expected_outs_v.size(),
                   " but should list ", num_outputs);
  }
  for (int i = 0; i < num_outputs; ++i) {
    StringPiece expected(expected_outs_v[i]);
    shape_inference::ShapeHandle out = c.output(i);

    string err_prefix = strings::StrCat("Output ", i);
    string err_suffix =
        strings::StrCat(". Output shape was ", c.DebugString(out));

    int in_index = -1;
    for (int i = 0; i < c.num_inputs(); ++i) {
      if (c.input(i).SameHandle(out)) {
        in_index = i;
      }
    }

    if (expected.starts_with("in")) {
      if (in_index == -1) {
        return Unknown(err_prefix,
                       " should have matched an input shape by "
                       "handle, but matched no input shape. This means the ",
                       "shape function was expected to pass an input "
                       "ShapeHandle through for this output, but did not",
                       err_suffix);
      }
      auto v = str_util::Split(expected, '|');
      if (std::find(v.begin(), v.end(), strings::StrCat("in", in_index)) ==
          v.end()) {
        return Unknown(
            err_prefix, " matched input ", in_index,
            " by handle, but should have matched one of (", expected,
            ") instead. This means the shape function passed the ShapeHandle ",
            "for input ", in_index,
            " to the output, but should have passed a different input ",
            "ShapeHandle through", err_suffix);
      }
      continue;
    }
    if (in_index != -1) {
      return Unknown(err_prefix, " matched input ", in_index,
                     " by ShapeHandle, but was expected to not match an input ",
                     "shape by handle", err_suffix);
    }
    if (expected == "?") {
      if (c.RankKnown(out)) {
        return Unknown(err_prefix, " expected to be unknown", err_suffix);
      }
      continue;
    }

    // Verify the dimensions.
    CHECK(expected.starts_with("[") && expected.ends_with("]")) << expected;
    expected.remove_prefix(1);
    expected.remove_suffix(1);

    // Split expected as a dimension.
    auto expected_dims = str_util::Split(expected, ',');
    if (!c.RankKnown(out)) {
      return Unknown(err_prefix, " expected rank ", expected_dims.size(),
                     " but was ?", err_suffix);
    }
    if (c.Rank(out) != expected_dims.size()) {
      return Unknown(err_prefix, " expected rank ", expected_dims.size(),
                     " but was ", c.Rank(out), err_suffix);
    }
    for (int j = 0; j < expected_dims.size(); ++j) {
      err_prefix = strings::StrCat("Output dim ", i, ",", j);
      StringPiece expected_dim(expected_dims[j]);
      DimensionHandle out_dim = c.Dim(out, j);

      std::pair<int, int> in_dim_idx(-1, -1);
      for (int i = 0; i < c.num_inputs(); ++i) {
        auto in = c.input(i);
        for (int j = 0; j < c.Rank(in); ++j) {
          if (c.Dim(in, j).SameHandle(out_dim)) {
            in_dim_idx = std::make_pair(i, j);
          }
        }
      }

      if (expected_dim == "?") {
        if (in_dim_idx.first != -1) {
          return Unknown(err_prefix,
                         " expected to be an unknown but matched input d",
                         in_dim_idx.first, "_", in_dim_idx.second,
                         ". The shape function passed through ",
                         "a DimensionHandle from an input instead of making ",
                         "a new unknown dimension", err_suffix);
        } else if (c.ValueKnown(out_dim)) {
          return Unknown(err_prefix, " expected to be unknown but was ",
                         c.Value(out_dim), err_suffix);
        }
      } else if (expected_dim.starts_with("d")) {
        // Compare the dimension values.
        auto v = str_util::Split(expected_dim, '|');
        if (in_dim_idx.first == -1) {
          return Unknown(
              err_prefix, " was expected to match the dimension of an input, ",
              "but did not match any input dimension. The shape ",
              "function was expected to pass through a ",
              "DimensionHandle for an input, but did not", err_suffix);
        }
        if (std::find(v.begin(), v.end(),
                      strings::StrCat("d", in_dim_idx.first, "_",
                                      in_dim_idx.second)) == v.end()) {
          return Unknown(err_prefix, " matched input d", in_dim_idx.first, "_",
                         in_dim_idx.second,
                         ", but should have matched one of (", expected_dim,
                         "). The shape function passed through "
                         "the DimensionHandle for an input, but ",
                         "was expected to pass a different one", err_suffix);
        }
      } else {
        // Parse it as a value.
        int64 value = -1;
        if (!strings::safe_strto64(expected_dim, &value)) {
          return Unknown(err_prefix, ": the expected dimension value '",
                         expected_dim, "' failed to parse as int64",
                         err_suffix);
        }
        if (in_dim_idx.first != -1) {
          return Unknown(  //
              err_prefix, " expected to be ", value, " but matched input d",
              in_dim_idx.first, "_", in_dim_idx.second,
              ". The shape function was not expected to pass a DimensionHandle "
              "from the input to the output, but did. Note that even if the "
              "passed through output has the same dimension value as the "
              "expected value, this is considered a failure for the test; "
              "switch to using d#_# syntax if passing through the "
              "DimensionHandle should be the expected behavior",
              err_suffix);
        } else if (value != c.Value(out_dim)) {
          return Unknown(err_prefix, " expected to be ", value, " but was ",
                         c.DebugString(out_dim), err_suffix);
        }
      }
    }
  }
  return Status::OK();
}

}  // namespace shape_inference
}  // namespace tensorflow
