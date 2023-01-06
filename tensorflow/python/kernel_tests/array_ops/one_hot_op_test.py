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
"""Tests for tensorflow.ops.one_hot_op."""

from absl.testing import parameterized
import numpy as np

from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.ops import array_ops
from tensorflow.python.platform import test


class OneHotTest(test.TestCase, parameterized.TestCase):

  def _testOneHot(self,
                  truth,
                  use_gpu=False,
                  expected_err_re=None,
                  raises=None,
                  dtype=None,
                  **inputs):
    with self.cached_session(use_gpu=use_gpu):
      if raises is not None:
        with self.assertRaises(raises):
          array_ops.one_hot(dtype=dtype, **inputs)
      else:
        ans = array_ops.one_hot(dtype=dtype, **inputs)
        if expected_err_re is None:
          tf_ans = self.evaluate(ans)
          self.assertAllEqual(tf_ans, truth)
          if dtype:
            self.assertEqual(tf_ans.dtype, dtype)
          self.assertEqual(tf_ans.shape, ans.get_shape())
        else:
          with self.assertRaisesOpError(expected_err_re):
            self.evaluate(ans)

  def _testBothOneHot(self, truth, expected_err_re=None, raises=None, **inputs):
    self._testOneHot(truth, True, expected_err_re, raises, **inputs)
    self._testOneHot(truth, False, expected_err_re, raises, **inputs)

  def _testBasic(self, dtype):
    indices = np.asarray([0, 2, -1, 1], dtype=np.int64)
    depth = 3
    on_value = np.asarray(1.0, dtype=dtype)
    off_value = np.asarray(-1.0, dtype=dtype)

    truth = np.asarray(
        [[1.0, -1.0, -1.0], [-1.0, -1.0, 1.0], [-1.0, -1.0, -1.0],
         [-1.0, 1.0, -1.0]],
        dtype=dtype)

    # axis == -1
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        dtype=dtype,
        truth=truth)

    # axis == 0
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        axis=0,
        dtype=dtype,
        truth=truth.T)  # Output is transpose version in this case

  def _testDefaultBasic(self, dtype):
    indices = np.asarray([0, 2, -1, 1], dtype=np.int64)
    depth = 3

    truth = np.asarray(
        [[1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, 0.0, 0.0], [0.0, 1.0, 0.0]],
        dtype=dtype)

    # axis == -1
    self._testBothOneHot(indices=indices, depth=depth, dtype=dtype, truth=truth)

    # axis == 0
    self._testBothOneHot(
        indices=indices, depth=depth, axis=0, dtype=dtype,
        truth=truth.T)  # Output is transpose version in this case

  def testDefaultNoDtype(self):
    self._testDefaultBasic(None)

  @parameterized.parameters(np.float16, dtypes.bfloat16.as_numpy_dtype,
                            np.float32, np.float64, np.int8, np.int32,
                            np.int64, np.complex64, np.complex128)
  def testBasic(self, dtype):
    self._testBasic(dtype)
    self._testDefaultBasic(dtype)

  def _testBatch(self, dtype):
    indices = np.asarray([[0, 2, -1, 1], [1, 0, 1, -1]], dtype=np.int64)
    depth = 3
    on_value = np.asarray(1.0, dtype=dtype)
    off_value = np.asarray(-1.0, dtype=dtype)

    truth = np.asarray(
        [[[1.0, -1.0, -1.0], [-1.0, -1.0, 1.0], [-1.0, -1.0, -1.0],
          [-1.0, 1.0, -1.0]], [[-1.0, 1.0, -1.0], [1.0, -1.0, -1.0],
                               [-1.0, 1.0, -1.0], [-1.0, -1.0, -1.0]]],
        dtype=dtype)

    # axis == -1
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        dtype=dtype,
        truth=truth)

    # axis == 1
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        axis=1,
        dtype=dtype,
        truth=[truth[0].T, truth[1].T])  # Do not transpose the batch

  def _testDefaultValuesBatch(self, dtype):
    indices = np.asarray([[0, 2, -1, 1], [1, 0, 1, -1]], dtype=np.int64)
    depth = 3

    truth = np.asarray(
        [[[1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, 0.0, 0.0], [0.0, 1.0, 0.0]],
         [[0.0, 1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 0.0]]],
        dtype=dtype)

    # axis == -1
    self._testBothOneHot(indices=indices, depth=depth, dtype=dtype, truth=truth)

    # axis == 1
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        axis=1,
        dtype=dtype,
        truth=[truth[0].T, truth[1].T])  # Do not transpose the batch

  def _testValueTypeBatch(self, dtype):
    indices = np.asarray([[0, 2, -1, 1], [1, 0, 1, -1]], dtype=np.int64)
    depth = 3

    on_value = np.asarray(1.0, dtype=dtype)
    off_value = np.asarray(-1.0, dtype=dtype)

    truth = np.asarray(
        [[[1.0, -1.0, -1.0], [-1.0, -1.0, 1.0], [-1.0, -1.0, -1.0],
          [-1.0, 1.0, -1.0]], [[-1.0, 1.0, -1.0], [1.0, -1.0, -1.0],
                               [-1.0, 1.0, -1.0], [-1.0, -1.0, -1.0]]],
        dtype=dtype)

    # axis == -1
    self._testBothOneHot(
        indices=indices,
        on_value=on_value,
        off_value=off_value,
        depth=depth,
        dtype=dtype,
        truth=truth)

    # axis == 1
    self._testBothOneHot(
        indices=indices,
        on_value=on_value,
        off_value=off_value,
        depth=depth,
        axis=1,
        dtype=dtype,
        truth=[truth[0].T, truth[1].T])  # Do not transpose the batch

  def _testEmpty(self, dtype):
    indices = np.zeros((0, 16), dtype=np.int64)
    depth = 3
    on_value = np.asarray(1.0, dtype=dtype)
    off_value = np.asarray(-1.0, dtype=dtype)
    truth = np.empty((0, 16, 3), dtype=dtype)

    # axis == -1
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        dtype=dtype,
        truth=truth)

  @parameterized.parameters(np.float16, dtypes.bfloat16.as_numpy_dtype,
                            np.float32, np.float64, np.int8, np.int32,
                            np.int64, np.complex64)
  def testBatch(self, dtype):
    self._testEmpty(dtype)
    self._testBatch(dtype)
    if dtype != np.complex64:
      self._testDefaultValuesBatch(dtype)
    self._testValueTypeBatch(dtype)

  def testSimpleCases(self):
    indices = [0, 1, 2]
    depth = 3
    truth = np.asarray(
        [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]], dtype=np.float32)
    self._testBothOneHot(indices=indices, depth=depth, truth=truth)

    indices = [0, 1, 2]
    depth = 3
    truth = np.asarray([[1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.int32)
    self._testBothOneHot(
        indices=indices, depth=depth, dtype=np.int32, truth=truth)

    indices = [0, 1, 2]
    depth = 3
    truth = np.asarray([[1, -1, -1], [-1, 1, -1], [-1, -1, 1]], dtype=np.int32)
    self._testBothOneHot(
        indices=indices, depth=depth, on_value=1, off_value=-1, truth=truth)

  def testSingleValueGiven(self):
    # Only on_value provided
    indices = [0, 1, 2]
    depth = 3
    truth = np.asarray([[1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.int32)
    self._testBothOneHot(indices=indices, depth=depth, on_value=1, truth=truth)

    # Only off_value provided
    indices = [0, 1, 2]
    depth = 3
    truth = np.asarray([[1, 0, 0], [0, 1, 0], [0, 0, 1]], dtype=np.float32)
    self._testBothOneHot(
        indices=indices, depth=depth, off_value=0.0, truth=truth)

  def testString(self):
    indices = [0, 1, 2]
    depth = 3
    truth = np.asarray([[b"1.0", b"0.0", b"0.0"], [b"0.0", b"1.0", b"0.0"],
                        [b"0.0", b"0.0", b"1.0"]])
    on_value = np.asarray(b"1.0")
    off_value = np.asarray(b"0.0")

    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        truth=truth)

    on_value = constant_op.constant(b"1.0")
    off_value = constant_op.constant(b"0.0")
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        truth=truth)

    on_value = b"1.0"
    off_value = b"0.0"
    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        truth=truth)

  def testIndicesTypes(self):
    tf_types = [dtypes.uint8, dtypes.int32, dtypes.int64]
    np_types = [np.int32, np.int64]
    for itype in tf_types + np_types:
      # Note: to keep the tests simple in the case of uint8 the index -1 below
      # maps to 255 which is out of the depth range, just like -1.
      if itype in tf_types:
        indices = constant_op.constant(
            [[0, 2, -1, 1], [1, 0, 1, -1]], dtype=itype)
      elif itype in np_types:
        indices = np.asarray([[0, 2, -1, 1], [1, 0, 1, -1]], dtype=itype)
      depth = 3

      on_value = np.asarray(1.0, dtype=np.float32)
      off_value = np.asarray(-1.0, dtype=np.float32)

      truth = np.asarray(
          [[[1.0, -1.0, -1.0], [-1.0, -1.0, 1.0], [-1.0, -1.0, -1.0],
            [-1.0, 1.0, -1.0]], [[-1.0, 1.0, -1.0], [1.0, -1.0, -1.0],
                                 [-1.0, 1.0, -1.0], [-1.0, -1.0, -1.0]]],
          dtype=np.float32)

      # axis == -1
      self._testBothOneHot(
          indices=indices,
          on_value=on_value,
          off_value=off_value,
          depth=depth,
          truth=truth)

      # axis == 1
      self._testBothOneHot(
          indices=indices,
          on_value=on_value,
          off_value=off_value,
          depth=depth,
          axis=1,
          truth=[truth[0].T, truth[1].T])  # Do not transpose the batch

  def testPrefixDimOverflow(self):
    for itype in [dtypes.int32, dtypes.int64, dtypes.uint8]:
      prefix_dim_size = 65536
      depth = 2
      x = [i % depth for i in range(prefix_dim_size)]
      indices = constant_op.constant(x, dtype=itype)

      truth = np.zeros((prefix_dim_size, depth), np.float32)
      for i in range(prefix_dim_size):
        truth[i, x[i]] = 1.0

      self._testBothOneHot(
          indices=indices,
          depth=depth,
          on_value=1.0,
          off_value=0.0,
          truth=truth)

  def testOnOffMismatchTypeError(self):
    indices = [0, 1, 2]
    depth = 3
    on_value = np.asarray(1.0, np.float64)
    off_value = np.asarray(0.0, np.float32)

    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        off_value=off_value,
        truth=None,
        raises=TypeError)

  def testDtypeMismatchTypeError(self):
    indices = [0, 1, 2]
    depth = 3
    on_value = constant_op.constant(1.0, dtypes.float32)
    off_value = constant_op.constant(0.0, dtypes.float32)
    dtype = np.int32

    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=on_value,
        dtype=dtype,
        truth=None,
        raises=TypeError)

    self._testBothOneHot(
        indices=indices,
        depth=depth,
        on_value=off_value,
        dtype=dtype,
        truth=None,
        raises=TypeError)

  def testConvertToTensorOfCorrectDtype(self):
    indices = [0, 1, 2]
    depth = 3
    dtype = np.float16
    truth = np.asarray([[1, 0, 0],
                        [0, 1, 0],
                        [0, 0, 1]])
    self._testBothOneHot(
        truth=truth,
        indices=indices,
        depth=depth,
        on_value=1.0,
        off_value=constant_op.constant(0.0, dtype),
        dtype=dtype)

    self._testBothOneHot(
        truth=truth,
        indices=indices,
        depth=depth,
        on_value=constant_op.constant(1.0, dtype),
        off_value=0.,
        dtype=dtype)

    self._testBothOneHot(
        truth=truth,
        indices=indices,
        depth=depth,
        on_value=1.0,
        off_value=0.,
        dtype=dtype)

  def testOneHotUint8WithLargeArray(self):
    with self.cached_session(use_gpu=False) as sess:
      matrix = np.random.rand(256) * 10
      tensor = constant_op.constant(matrix, dtypes.uint8, shape=matrix.shape)
      tensor_one_hot = array_ops.one_hot(tensor, depth=10, axis=0)
      self.assertEqual(sess.run(tensor_one_hot).shape, (10, 256))


if __name__ == "__main__":
  test.main()
