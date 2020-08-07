# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

"""Tests for tensorflow.python.ops.op_def_library."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from absl.testing import parameterized

import numpy as np

from tensorflow.core.framework import tensor_pb2
from tensorflow.core.framework import types_pb2
from tensorflow.python import _op_def_util
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import tensor_shape
from tensorflow.python.framework import test_util
from tensorflow.python.platform import googletest


class OpDefUtilTest(test_util.TensorFlowTestCase, parameterized.TestCase):

  @parameterized.parameters([
      ("any", "Foo", "Foo"),
      ("any", 12, 12),
      ("any", {2: 3}, {2: 3}),
      ("string", "Foo", "Foo"),
      ("string", b"Foo", b"Foo"),
      ("int", 12, 12),
      ("int", 12.3, 12),
      ("float", 12, 12.0),
      ("float", 12.3, 12.3),
      ("bool", True, True),
      ("shape", tensor_shape.TensorShape([3]), tensor_shape.TensorShape([3])),
      ("shape", [3], tensor_shape.TensorShape([3])),
      ("type", dtypes.int32, dtypes.int32),
      ("type", np.int32, dtypes.int32),
      ("type", "int32", dtypes.int32),
      ("tensor", tensor_pb2.TensorProto(dtype=types_pb2.DataType.DT_FLOAT),
       tensor_pb2.TensorProto(dtype=types_pb2.DataType.DT_FLOAT)),
      ("tensor", "dtype: DT_FLOAT",
       tensor_pb2.TensorProto(dtype=types_pb2.DataType.DT_FLOAT)),
      ("list(any)", [1, "foo", 7.3, dtypes.int32],
       [1, "foo", 7.3, dtypes.int32]),
      ("list(any)", (1, "foo"), [1, "foo"]),
      ("list(string)", ["foo", "bar"], ["foo", "bar"]),
      ("list(string)", ("foo", "bar"), ["foo", "bar"]),
      ("list(string)", iter("abcd"), ["a", "b", "c", "d"]),
      ("list(int)", (1, 2.3), [1, 2]),
      ("list(float)", (1, 2.3), [1.0, 2.3]),
      ("list(bool)", [True, False], [True, False]),
  ])
  def testConvert(self, attr_type, value, expected):
    result = _op_def_util.ConvertPyObjectToAttributeType(value, attr_type)

    # Check that we get the expected value(s).
    self.assertEqual(expected, result)

    # Check that we get the expected type(s).
    self.assertEqual(type(expected), type(result))
    if isinstance(result, list):
      for expected_item, result_item in zip(expected, result):
        self.assertEqual(type(expected_item), type(result_item))

  @parameterized.parameters([
      ("string", 12),
      ("int", "foo"),
      ("float", "foo"),
      ("bool", 1),
      ("dtype", None),
      ("shape", 12.0),
      ("tensor", [1, 2, 3]),
      ("list(any)", 12),
      ("list(int)", [1, "two"]),
      ("list(string)", [1, "two"]),
  ])
  def testConvertError(self, attr_type, value):
    with self.assertRaisesRegex(TypeError, "Failed to convert value"):
      _op_def_util.ConvertPyObjectToAttributeType(value, attr_type)

if __name__ == "__main__":
  googletest.main()

