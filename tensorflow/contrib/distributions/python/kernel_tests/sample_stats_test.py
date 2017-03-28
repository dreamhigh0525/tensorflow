# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for Sample Stats Ops."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib.distributions.python.ops import sample_stats
from tensorflow.python.framework import dtypes
from tensorflow.python.ops import array_ops
from tensorflow.python.platform import test

rng = np.random.RandomState(0)


class PercentileTestWithLowerInterpolation(test.TestCase):

  _interpolation = "lower"

  def test_one_dim_odd_input(self):
    x = [1., 5., 3., 2., 4.]
    for q in [0, 10, 25, 49.9, 50, 50.01, 90, 95, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation, axis=0)
      with self.test_session():
        pct = sample_stats.percentile(
            x, q=q, interpolation=self._interpolation, axis=[0])
        self.assertAllEqual((), pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_one_dim_even_input(self):
    x = [1., 5., 3., 2., 4., 5.]
    for q in [0, 10, 25, 49.9, 50, 50.01, 90, 95, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation)
      with self.test_session():
        pct = sample_stats.percentile(x, q=q, interpolation=self._interpolation)
        self.assertAllEqual((), pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_two_dim_odd_input_axis_0(self):
    x = np.array([[-1., 50., -3.5, 2., -1], [0., 0., 3., 2., 4.]]).T
    for q in [0, 10, 25, 49.9, 50, 50.01, 90, 95, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation, axis=0)
      with self.test_session():
        # Get dim 1 with negative and positive indices.
        pct_neg_index = sample_stats.percentile(
            x, q=q, interpolation=self._interpolation, axis=[0])
        pct_pos_index = sample_stats.percentile(
            x, q=q, interpolation=self._interpolation, axis=[0])
        self.assertAllEqual((2,), pct_neg_index.get_shape())
        self.assertAllEqual((2,), pct_pos_index.get_shape())
        self.assertAllClose(expected_percentile, pct_neg_index.eval())
        self.assertAllClose(expected_percentile, pct_pos_index.eval())

  def test_two_dim_even_axis_0(self):
    x = np.array([[1., 2., 4., 50.], [1., 2., -4., 5.]]).T
    for q in [0, 10, 25, 49.9, 50, 50.01, 90, 95, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation, axis=0)
      with self.test_session():
        pct = sample_stats.percentile(
            x, q=q, interpolation=self._interpolation, axis=[0])
        self.assertAllEqual((2,), pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_two_dim_even_input_and_keep_dims_true(self):
    x = np.array([[1., 2., 4., 50.], [1., 2., -4., 5.]]).T
    for q in [0, 10, 25, 49.9, 50, 50.01, 90, 95, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation, keepdims=True, axis=0)
      with self.test_session():
        pct = sample_stats.percentile(
            x,
            q=q,
            interpolation=self._interpolation,
            keep_dims=True,
            axis=[0])
        self.assertAllEqual((1, 2), pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_four_dimensional_input(self):
    x = rng.rand(2, 3, 4, 5)
    for axis in [None, 0, 1, -2, (0,), (-1,), (-1, 1), (3, 1), (-3, 0)]:
      expected_percentile = np.percentile(
          x, q=0.77, interpolation=self._interpolation, axis=axis)
      with self.test_session():
        pct = sample_stats.percentile(
            x,
            q=0.77,
            interpolation=self._interpolation,
            axis=axis)
        self.assertAllEqual(expected_percentile.shape, pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_four_dimensional_input_and_keepdims(self):
    x = rng.rand(2, 3, 4, 5)
    for axis in [None, 0, 1, -2, (0,), (-1,), (-1, 1), (3, 1), (-3, 0)]:
      expected_percentile = np.percentile(
          x,
          q=0.77,
          interpolation=self._interpolation,
          axis=axis,
          keepdims=True)
      with self.test_session():
        pct = sample_stats.percentile(
            x,
            q=0.77,
            interpolation=self._interpolation,
            axis=axis,
            keep_dims=True)
        self.assertAllEqual(expected_percentile.shape, pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_four_dimensional_input_x_static_ndims_but_dynamic_sizes(self):
    x = rng.rand(2, 3, 4, 5)
    x_ph = array_ops.placeholder(dtypes.float64, shape=[None, None, None, None])
    for axis in [None, 0, 1, -2, (0,), (-1,), (-1, 1), (3, 1), (-3, 0)]:
      expected_percentile = np.percentile(
          x, q=0.77, interpolation=self._interpolation, axis=axis)
      with self.test_session():
        pct = sample_stats.percentile(
            x_ph,
            q=0.77,
            interpolation=self._interpolation,
            axis=axis)
        self.assertAllClose(expected_percentile, pct.eval(feed_dict={x_ph: x}))

  def test_four_dimensional_input_and_keepdims_x_static_ndims_dynamic_sz(self):
    x = rng.rand(2, 3, 4, 5)
    x_ph = array_ops.placeholder(dtypes.float64, shape=[None, None, None, None])
    for axis in [None, 0, 1, -2, (0,), (-1,), (-1, 1), (3, 1), (-3, 0)]:
      expected_percentile = np.percentile(
          x,
          q=0.77,
          interpolation=self._interpolation,
          axis=axis,
          keepdims=True)
      with self.test_session():
        pct = sample_stats.percentile(
            x_ph,
            q=0.77,
            interpolation=self._interpolation,
            axis=axis,
            keep_dims=True)
        self.assertAllClose(expected_percentile, pct.eval(feed_dict={x_ph: x}))

  def test_with_integer_dtype(self):
    x = [1, 5, 3, 2, 4]
    for q in [0, 10, 25, 49.9, 50, 50.01, 90, 95, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation)
      with self.test_session():
        pct = sample_stats.percentile(x, q=q, interpolation=self._interpolation)
        self.assertEqual(dtypes.int32, pct.dtype)
        self.assertAllEqual((), pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())


class PercentileTestWithHigherInterpolation(
    PercentileTestWithLowerInterpolation):

  _interpolation = "higher"


class PercentileTestWithNearestInterpolation(test.TestCase):
  """Test separately because np.round and tf.round make different choices."""

  _interpolation = "nearest"

  def test_one_dim_odd_input(self):
    x = [1., 5., 3., 2., 4.]
    for q in [0, 10.1, 25.1, 49.9, 50.1, 50.01, 89, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation)
      with self.test_session():
        pct = sample_stats.percentile(x, q=q, interpolation=self._interpolation)
        self.assertAllEqual((), pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_one_dim_even_input(self):
    x = [1., 5., 3., 2., 4., 5.]
    for q in [0, 10.1, 25.1, 49.9, 50.1, 50.01, 89, 100]:
      expected_percentile = np.percentile(
          x, q=q, interpolation=self._interpolation)
      with self.test_session():
        pct = sample_stats.percentile(x, q=q, interpolation=self._interpolation)
        self.assertAllEqual((), pct.get_shape())
        self.assertAllClose(expected_percentile, pct.eval())

  def test_invalid_interpolation_raises(self):
    x = [1., 5., 3., 2., 4.]
    with self.assertRaisesRegexp(ValueError, "interpolation"):
      sample_stats.percentile(x, q=0.5, interpolation="bad")

  def test_vector_q_raises_static(self):
    x = [1., 5., 3., 2., 4.]
    with self.assertRaisesRegexp(ValueError, "Expected.*ndims"):
      sample_stats.percentile(x, q=[0.5])

  def test_vector_q_raises_dynamic(self):
    x = [1., 5., 3., 2., 4.]
    q_ph = array_ops.placeholder(dtypes.float32)
    pct = sample_stats.percentile(x, q=q_ph, validate_args=True)
    with self.test_session():
      with self.assertRaisesOpError("rank"):
        pct.eval(feed_dict={q_ph: [0.5]})


if __name__ == "__main__":
  test.main()
