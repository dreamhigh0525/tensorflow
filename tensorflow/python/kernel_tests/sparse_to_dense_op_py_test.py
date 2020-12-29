# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for tensorflow.kernels.sparse_op."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import sparse_ops
from tensorflow.python.platform import test


class SparseToDenseTest(test.TestCase):

  def testInt(self):
    tf_ans = sparse_ops.sparse_to_dense([1, 3], [5], 1, 0)
    np_ans = np.array([0, 1, 0, 1, 0]).astype(np.int32)
    self.assertAllClose(np_ans, tf_ans)

  def testFloat(self):
    tf_ans = sparse_ops.sparse_to_dense([1, 3], [5], 1.0, 0.0)
    np_ans = np.array([0, 1, 0, 1, 0]).astype(np.float32)
    self.assertAllClose(np_ans, tf_ans)

  def testString(self):
    tf_ans = sparse_ops.sparse_to_dense([1, 3], [5], "a", "b")
    np_ans = np.array(["b", "a", "b", "a", "b"]).astype(np.string_)
    self.assertAllEqual(np_ans, tf_ans)

  def testSetValue(self):
    tf_ans = sparse_ops.sparse_to_dense([1, 3], [5], [1, 2], -1)
    np_ans = np.array([-1, 1, -1, 2, -1]).astype(np.int32)
    self.assertAllClose(np_ans, tf_ans)

  def testSetSingleValue(self):
    tf_ans = sparse_ops.sparse_to_dense([1, 3], [5], 1, -1)
    np_ans = np.array([-1, 1, -1, 1, -1]).astype(np.int32)
    self.assertAllClose(np_ans, tf_ans)

  def test2d(self):
    tf_ans = sparse_ops.sparse_to_dense([[1, 3], [2, 0]], [3, 4], 1, -1)
    np_ans = np.array([[-1, -1, -1, -1],
                       [-1, -1, -1, 1],
                       [1, -1, -1, -1]]).astype(np.int32)
    self.assertAllClose(np_ans, tf_ans)

  def testZeroDefault(self):
    x = sparse_ops.sparse_to_dense(2, [4], 7)
    self.assertAllEqual(x, [0, 0, 7, 0])

  def test3d(self):
    tf_ans = sparse_ops.sparse_to_dense([[1, 3, 0], [2, 0, 1]], [3, 4, 2], 1,
                                        -1)
    np_ans = np.ones((3, 4, 2), dtype=np.int32) * -1
    np_ans[1, 3, 0] = 1
    np_ans[2, 0, 1] = 1
    self.assertAllClose(np_ans, tf_ans)

  def testBadShape(self):
    with self.assertRaisesRegex((ValueError, errors.InvalidArgumentError),
                                "must be rank 1"):
      sparse_ops.sparse_to_dense([1, 3], [[5], [3]], 1, -1)

  def testBadValue(self):
    with self.assertRaisesRegex((ValueError, errors.InvalidArgumentError),
                                r"sparse_values has incorrect shape \[2,1\], "
                                r"should be \[\] or \[2\]"):
      self.evaluate(sparse_ops.sparse_to_dense([1, 3], [5], [[5], [3]], -1))

  def testBadNumValues(self):
    with self.assertRaisesRegex(
        (ValueError, errors.InvalidArgumentError),
        r"sparse_values has incorrect shape \[3\], should be \[\] or \[2\]"):
      self.evaluate(sparse_ops.sparse_to_dense([1, 3], [5], [1, 2, 3], -1))

  def testBadDefault(self):
    with self.assertRaisesRegex((ValueError, errors.InvalidArgumentError),
                                "default_value should be a scalar"):
      self.evaluate(sparse_ops.sparse_to_dense([1, 3], [5], [1, 2], [0]))

  def testOutOfBoundsIndicesWithWithoutValidation(self):
    with self.assertRaisesRegex(
        (ValueError, errors.InvalidArgumentError),
        r"indices\[1\] = \[10\] is out of bounds: need 0 <= index < \[5\]"):
      self.evaluate(
          sparse_ops.sparse_to_dense([[1], [10]], [5], [1.0, 1.0], 0.0))
    # Disable checks, the allocation should still fail.
    with self.assertRaisesRegex((ValueError, errors.InvalidArgumentError),
                                "out of bounds"):
      self.evaluate(
          sparse_ops.sparse_to_dense([[1], [10]], [5], [-1.0, 1.0],
                                     0.0,
                                     validate_indices=False))

  def testRepeatingIndicesWithWithoutValidation(self):
    with self.assertRaisesRegex((ValueError, errors.InvalidArgumentError),
                                r"indices\[1\] = \[1\] is repeated"):
      self.evaluate(
          sparse_ops.sparse_to_dense([[1], [1]], [5], [-1.0, 1.0], 0.0))
    # Disable checks
    self.evaluate(
        sparse_ops.sparse_to_dense([[1], [1]], [5], [-1.0, 1.0],
                                   0.0,
                                   validate_indices=False))

  def testUnsortedIndicesWithWithoutValidation(self):
    with self.assertRaisesRegex((ValueError, errors.InvalidArgumentError),
                                r"indices\[1\] = \[1\] is out of order"):
      self.evaluate(
          sparse_ops.sparse_to_dense([[2], [1]], [5], [-1.0, 1.0], 0.0))
    # Disable checks
    self.evaluate(
        sparse_ops.sparse_to_dense([[2], [1]], [5], [-1.0, 1.0],
                                   0.0,
                                   validate_indices=False))

  def testShapeInferenceKnownShape(self):
    with ops.Graph().as_default():
      indices = array_ops.placeholder(dtypes.int64)

      shape = [4, 5, 6]
      output = sparse_ops.sparse_to_dense(indices, shape, 1, 0)
      self.assertEqual(output.get_shape(), [4, 5, 6])

      shape = array_ops.placeholder(dtypes.int64, shape=(3,))
      output = sparse_ops.sparse_to_dense(indices, shape, 1, 0)
      self.assertEqual(output.get_shape().as_list(), [None, None, None])

  def testShapeInferenceUnknownShape(self):
    with ops.Graph().as_default():
      indices = array_ops.placeholder(dtypes.int64)
      shape = array_ops.placeholder(dtypes.int64)
      output = sparse_ops.sparse_to_dense(indices, shape, 1, 0)
      self.assertIsNone(output.get_shape().ndims)


if __name__ == "__main__":
  test.main()
