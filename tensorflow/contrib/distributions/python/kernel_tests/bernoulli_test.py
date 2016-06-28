# Copyright 2016 Google Inc. All Rights Reserved.
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
"""Tests for the Bernoulli distribution."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import tensorflow as tf


def make_bernoulli(batch_shape, dtype=tf.int32):
  p = np.random.uniform(size=list(batch_shape))
  p = tf.constant(p, dtype=tf.float32)
  return tf.contrib.distributions.Bernoulli(p, dtype=dtype)


def entropy(p):
  q = 1. - p
  return -q * np.log(q) - p * np.log(p)


class BernoulliTest(tf.test.TestCase):

  def testP(self):
    p = [0.2, 0.4]
    dist = tf.contrib.distributions.Bernoulli(p)
    with self.test_session():
      self.assertAllClose(p, dist.p.eval())

  def testInvalidP(self):
    invalid_ps = [1.01, -0.01, 2., -3.]
    for p in invalid_ps:
      with self.test_session():
        with self.assertRaisesOpError("x <= y"):
          dist = tf.contrib.distributions.Bernoulli(p)
          dist.p.eval()

    valid_ps = [0.0, 0.5, 1.0]
    for p in valid_ps:
      with self.test_session():
        dist = tf.contrib.distributions.Bernoulli(p)
        self.assertEqual(p, dist.p.eval())  # Should not fail

  def testShapes(self):
    with self.test_session():
      for batch_shape in ([], [1], [2, 3, 4]):
        dist = make_bernoulli(batch_shape)
        self.assertAllEqual(batch_shape, dist.get_batch_shape().as_list())
        self.assertAllEqual(batch_shape, dist.batch_shape().eval())
        self.assertAllEqual([], dist.get_event_shape().as_list())
        self.assertAllEqual([], dist.event_shape().eval())

  def testDtype(self):
    dist = make_bernoulli([])
    self.assertEqual(dist.dtype, tf.int32)
    self.assertEqual(dist.dtype, dist.sample(5).dtype)
    self.assertEqual(dist.dtype, dist.mode().dtype)
    self.assertEqual(dist.p.dtype, dist.mean().dtype)
    self.assertEqual(dist.p.dtype, dist.variance().dtype)
    self.assertEqual(dist.p.dtype, dist.std().dtype)
    self.assertEqual(dist.p.dtype, dist.entropy().dtype)
    self.assertEqual(dist.p.dtype, dist.pmf(0).dtype)
    self.assertEqual(dist.p.dtype, dist.log_pmf(0).dtype)

    dist64 = make_bernoulli([], tf.int64)
    self.assertEqual(dist64.dtype, tf.int64)
    self.assertEqual(dist64.dtype, dist64.sample(5).dtype)
    self.assertEqual(dist64.dtype, dist64.mode().dtype)

  def testPmf(self):
    p = [[0.2, 0.4], [0.3, 0.6]]
    dist = tf.contrib.distributions.Bernoulli(p)
    with self.test_session():
      # pylint: disable=bad-continuation
      xs = [
          0,
          [1],
          [1, 0],
          [[1, 0]],
          [[1, 0], [1, 1]],
      ]
      expected_pmfs = [
          [[0.8, 0.6], [0.7, 0.4]],
          [[0.2, 0.4], [0.3, 0.6]],
          [[0.2, 0.6], [0.3, 0.4]],
          [[0.2, 0.6], [0.3, 0.4]],
          [[0.2, 0.6], [0.3, 0.6]],
      ]
      # pylint: enable=bad-continuation

      for x, expected_pmf in zip(xs, expected_pmfs):
        self.assertAllClose(dist.pmf(x).eval(), expected_pmf)
        self.assertAllClose(dist.log_pmf(x).eval(), np.log(expected_pmf))

  def testBoundaryConditions(self):
    with self.test_session():
      dist = tf.contrib.distributions.Bernoulli(1.0)
      self.assertEqual(-np.inf, dist.log_pmf(0).eval())
      self.assertAllClose([0.0], [dist.log_pmf(1).eval()])

  def testEntropyNoBatch(self):
    p = 0.2
    dist = tf.contrib.distributions.Bernoulli(p)
    with self.test_session():
      self.assertAllClose(dist.entropy().eval(), entropy(p))

  def testEntropyWithBatch(self):
    p = [[0.0, 0.7], [1.0, 0.6]]
    dist = tf.contrib.distributions.Bernoulli(p, strict=False)
    with self.test_session():
      self.assertAllClose(dist.entropy().eval(), [[0.0, entropy(0.7)],
                                                  [0.0, entropy(0.6)]])

  def testSample(self):
    with self.test_session():
      p = [0.2, 0.6]
      dist = tf.contrib.distributions.Bernoulli(p)
      n = 1000
      samples = dist.sample(n, seed=123)
      samples.set_shape([n, 2])
      self.assertEqual(samples.dtype, tf.int32)
      sample_values = samples.eval()
      self.assertFalse(np.any(sample_values < 0))
      self.assertFalse(np.any(sample_values > 1))
      self.assertAllClose(p, np.mean(sample_values == 1, axis=0), atol=1e-2)
      self.assertEqual(set([0, 1]), set(sample_values.flatten()))

  def testMean(self):
    with self.test_session():
      p = np.array([[0.2, 0.7], [0.5, 0.4]], dtype=np.float32)
      dist = tf.contrib.distributions.Bernoulli(p)
      self.assertAllEqual(dist.mean().eval(), p)

  def testVarianceAndStd(self):
    var = lambda p: p * (1. - p)
    with self.test_session():
      p = [[0.2, 0.7], [0.5, 0.4]]
      dist = tf.contrib.distributions.Bernoulli(p)
      self.assertAllClose(dist.variance().eval(),
                          np.array([[var(0.2), var(0.7)], [var(0.5), var(0.4)]],
                                   dtype=np.float32))
      self.assertAllClose(dist.std().eval(),
                          np.array([[np.sqrt(var(0.2)), np.sqrt(var(0.7))],
                                    [np.sqrt(var(0.5)), np.sqrt(var(0.4))]],
                                   dtype=np.float32))


if __name__ == "__main__":
  tf.test.main()
