# Copyright 2015 Google Inc. All Rights Reserved.
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

"""Tests for tensorflow.ops.math_ops.matmul."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import tensorflow.python.platform

import numpy as np
import tensorflow as tf


class MatMulTest(tf.test.TestCase):

  def _testCpuMatmul(self, x, y, transpose_x=False, transpose_y=False):
    x_mat = np.matrix(x).T if transpose_x else np.matrix(x)
    y_mat = np.matrix(y).T if transpose_y else np.matrix(y)
    np_ans = x_mat * y_mat
    with self.test_session(use_gpu=False):
      tf_ans = tf.matmul(x, y, transpose_x, transpose_y).eval()
    self.assertAllClose(np_ans, tf_ans)
    self.assertAllEqual(np_ans.shape, tf_ans.shape)

  def _testGpuMatmul(self, x, y, transpose_x=False, transpose_y=False):
    x_mat = np.matrix(x).T if transpose_x else np.matrix(x)
    y_mat = np.matrix(y).T if transpose_y else np.matrix(y)
    np_ans = x_mat * y_mat
    with self.test_session(use_gpu=True):
      tf_ans = tf.matmul(x, y, transpose_x, transpose_y).eval()
    self.assertAllClose(np_ans, tf_ans)
    self.assertAllEqual(np_ans.shape, tf_ans.shape)

  def _randMatrix(self, rows, cols, dtype):
    if dtype is np.complex64:
      real = self._randMatrix(rows, cols, np.float32)
      imag = self._randMatrix(rows, cols, np.float32)
      return real + np.complex(0, 1) * imag
    else:
      return np.random.uniform(low=1.0, high=100.0, size=rows * cols).reshape(
          [rows, cols]).astype(dtype)

  # Basic test:
  #   [ [1],
  #     [2],
  #     [3],   *  [1, 2]
  #     [4] ]
  def testFloatBasic(self):
    x = np.arange(1., 5.).reshape([4, 1]).astype(np.float32)
    y = np.arange(1., 3.).reshape([1, 2]).astype(np.float32)
    self._testCpuMatmul(x, y)
    self._testGpuMatmul(x, y)

  def testDoubleBasic(self):
    x = np.arange(1., 5.).reshape([4, 1]).astype(np.float64)
    y = np.arange(1., 3.).reshape([1, 2]).astype(np.float64)
    self._testCpuMatmul(x, y)

  def testInt32Basic(self):
    x = np.arange(1., 5.).reshape([4, 1]).astype(np.int32)
    y = np.arange(1., 3.).reshape([1, 2]).astype(np.int32)
    self._testCpuMatmul(x, y)

  def testSComplexBasic(self):
    x = np.arange(1., 5.).reshape([4, 1]).astype(np.complex64)
    y = np.arange(1., 3.).reshape([1, 2]).astype(np.complex64)
    self._testCpuMatmul(x, y)

  def testSComplexBasic(self):
    x = np.arange(1., 5.).reshape([4, 1]).astype(np.complex64)
    y = np.arange(1., 3.).reshape([1, 2]).astype(np.complex64)
    self._testCpuMatmul(x, y)

  # Tests testing random sized matrices.
  def testFloatRandom(self):
    for _ in range(10):
      n, k, m = np.random.randint(1, 100, size=3)
      x = self._randMatrix(n, k, np.float32)
      y = self._randMatrix(k, m, np.float32)
      self._testCpuMatmul(x, y)
      self._testGpuMatmul(x, y)

  def testDoubleRandom(self):
    for _ in range(10):
      n, k, m = np.random.randint(1, 100, size=3)
      x = self._randMatrix(n, k, np.float64)
      y = self._randMatrix(k, m, np.float64)
      self._testCpuMatmul(x, y)

  def testInt32Random(self):
    for _ in range(10):
      n, k, m = np.random.randint(1, 100, size=3)
      x = self._randMatrix(n, k, np.int32)
      y = self._randMatrix(k, m, np.int32)
      self._testCpuMatmul(x, y)

  def testSComplexRandom(self):
    for _ in range(10):
      n, k, m = np.random.randint(1, 100, size=3)
      x = self._randMatrix(n, k, np.complex64)
      y = self._randMatrix(k, m, np.complex64)
      self._testCpuMatmul(x, y)

  # Test the cases that transpose the matrices before multiplying.
  # NOTE(keveman): The cases where only one of the inputs is
  # transposed are covered by tf.matmul's gradient function.
  def testFloatRandomTransposeBoth(self):
    for _ in range(10):
      n, k, m = np.random.randint(1, 100, size=3)
      x = self._randMatrix(k, n, np.float32)
      y = self._randMatrix(m, k, np.float32)
      self._testCpuMatmul(x, y, True, True)
      self._testGpuMatmul(x, y, True, True)

  def testDoubleRandomTranposeBoth(self):
    for _ in range(10):
      n, k, m = np.random.randint(1, 100, size=3)
      x = self._randMatrix(k, n, np.float64)
      y = self._randMatrix(m, k, np.float64)
      self._testCpuMatmul(x, y, True, True)

  def testMatMul_OutEmpty_A(self):
    n, k, m = 0, 8, 3
    x = self._randMatrix(n, k, np.float32)
    y = self._randMatrix(k, m, np.float32)
    self._testCpuMatmul(x, y)
    self._testGpuMatmul(x, y)

  def testMatMul_OutEmpty_B(self):
    n, k, m = 3, 8, 0
    x = self._randMatrix(n, k, np.float32)
    y = self._randMatrix(k, m, np.float32)
    self._testCpuMatmul(x, y)
    self._testGpuMatmul(x, y)

  def testMatMul_Inputs_Empty(self):
    n, k, m = 3, 0, 4
    x = self._randMatrix(n, k, np.float32)
    y = self._randMatrix(k, m, np.float32)
    self._testCpuMatmul(x, y)
    self._testGpuMatmul(x, y)

# TODO(zhifengc): Figures out how to test matmul gradients on GPU.
class MatMulGradientTest(tf.test.TestCase):

  def testGradientInput0(self):
    with self.test_session(use_gpu=False):
      x = tf.constant([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], shape=[3, 2],
                      dtype=tf.float64, name="x")
      y = tf.constant([1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7],
                      shape=[2, 4], dtype=tf.float64, name="y")
      m = tf.matmul(x, y, name="matmul")
      err = tf.test.compute_gradient_error(x, [3, 2], m, [3, 4])
    print("matmul input0 gradient err = ", err)
    self.assertLess(err, 1e-10)

  def testGradientInput1(self):
    with self.test_session(use_gpu=False):
      x = tf.constant([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], shape=[3, 2],
                      dtype=tf.float64, name="x")
      y = tf.constant([1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7],
                      shape=[2, 4], dtype=tf.float64, name="y")
      m = tf.matmul(x, y, name="matmul")
      err = tf.test.compute_gradient_error(y, [2, 4], m, [3, 4])
    print("matmul input1 gradient err = ", err)
    self.assertLess(err, 1e-10)

  def _VerifyInput0(self, transpose_a, transpose_b):
    shape_x = [3, 2]
    shape_y = [2, 4]
    if transpose_a:
      shape_x = list(reversed(shape_x))
    if transpose_b:
      shape_y = list(reversed(shape_y))
    with self.test_session(use_gpu=False):
      x = tf.constant([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], shape=shape_x,
                      dtype=tf.float64, name="x")
      y = tf.constant([1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7],
                      shape=shape_y, dtype=tf.float64, name="y")
      m = tf.matmul(x, y, transpose_a, transpose_b, name="matmul")
      err = tf.test.compute_gradient_error(x, shape_x, m, [3, 4])
    print("matmul input0 gradient err = ", err)
    self.assertLess(err, 1e-10)

  def testGradientInput0WithTranspose(self):
    self._VerifyInput0(transpose_a=True, transpose_b=False)
    self._VerifyInput0(transpose_a=False, transpose_b=True)
    self._VerifyInput0(transpose_a=True, transpose_b=True)

  def _VerifyInput1(self, transpose_a, transpose_b):
    shape_x = [3, 2]
    shape_y = [2, 4]
    if transpose_a:
      shape_x = list(reversed(shape_x))
    if transpose_b:
      shape_y = list(reversed(shape_y))
    with self.test_session(use_gpu=False):
      x = tf.constant([1.0, 2.0, 3.0, 4.0, 5.0, 6.0], shape=shape_x,
                      dtype=tf.float64, name="x")
      y = tf.constant([1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7],
                      shape=shape_y, dtype=tf.float64, name="y")
      m = tf.matmul(x, y, transpose_a, transpose_b, name="matmul")
      err = tf.test.compute_gradient_error(y, shape_y, m, [3, 4])
    print("matmul input1 gradient err = ", err)
    self.assertLess(err, 1e-10)

  def testGradientInput1WithTranspose(self):
    self._VerifyInput1(transpose_a=True, transpose_b=False)
    self._VerifyInput1(transpose_a=False, transpose_b=True)
    self._VerifyInput1(transpose_a=True, transpose_b=True)

def randGauss(shape, dtype):
  """Generate a Gaussian i.i.d. numpy matrix of the given shape and type"""
  if dtype is tf.complex64:
    return (np.random.normal(size=shape).astype('float32') +
            1j * np.random.normal(size=shape).astype('float32'))
  else:
    return np.random.normal(size=shape).astype('float32')

def FrobeniusNormSquared(Z):
  """Frobenius norm of the given tensor, squared"""
  if Z.dtype is tf.complex64:
    return tf.reduce_sum(tf.square(tf.real(Z)) + tf.square(tf.imag(Z)))
  else:
    return tf.reduce_sum(tf.square(Z))


def numGrad(sess, func, var, feed_dict, eps=1e-2):
  """Calculate the numerical gradient estimate for scalar-valued function
  tensorflow node func with respect to a tensorflow node var.
  Note: feed_dict[var] must exist and indicates the point at which
  the numerical gradient is estimated."""
  shp = var.get_shape()
  grad = np.zeros(shp)
  dx = np.zeros(shp)
  if var.dtype is tf.complex64:
    grad = np.complex64(np.zeros(shp))
    dx = np.complex64(np.zeros(shp))

  varval = feed_dict[var]
  for k0 in range(shp[0]):
    for k1 in range(shp[1]):
      # implement a two-sided gradient estimate
      dx[k0][k1] = eps * .5
      fd = dict(feed_dict)
      fd[var] = varval + dx
      zHi = sess.run(func, feed_dict=fd)
      fd[var] = varval - dx
      zLo = sess.run(func, feed_dict=fd)
      derivReal = (zHi - zLo) / eps
      if var.dtype is tf.complex64:
        dx = 1j * dx
        fd[var] = varval + dx
        zHi = sess.run(func, feed_dict=fd)
        fd[var] = varval - dx
        zLo = sess.run(func, feed_dict=fd)
        derivImag = 1j * (zHi - zLo) / eps
      else:
        derivImag = 0
      dx[k0][k1] = 0  # clear
      grad[k0][k1] = derivReal + derivImag
  return grad

class CpxMatMulGradientTest(tf.test.TestCase):
  """
  Testing autogradient of quadratic loss = .5*FrobeniusNorm(X*Y - B)^2
  for matrices (X, Y, B), with respect to (X, Y)
  """

  def testAll(self):
        # dimensions of left, middle, right of matrix product X*Y
    leftN = 3
    middleN = 4
    rightN = 5

    for dtype in (tf.float32, tf.complex64):
      for transX in (False, True):
        for transY in (False, True):
          print('Testing TranX=%s TranY=%s %s' % (transX, transY, dtype))

          # use a common quadratic loss function
          # and check the gradients wrt (X, Y)
          X = tf.placeholder(dtype, shape=(middleN, leftN)
                             if transX else (leftN, middleN))
          Y = tf.placeholder(dtype, shape=(rightN, middleN)
                             if transY else(middleN, rightN))
          C = tf.constant(randGauss((leftN, rightN), dtype))
          loss = .5 * FrobeniusNormSquared(
              tf.matmul(X, Y, transpose_a=transX, transpose_b=transY) - C)
          (gX, gY) = tf.gradients(loss, [X, Y])

          init = tf.initialize_all_variables()
          with tf.Session() as sess:
            sess.run(init)
            # Populate the feed_dict with random (X, Y) tensors to evaluate the
            # autogradients and numerical gradient estimates
            feed_dict = {}
            for node in (X, Y):
              feed_dict[node] = randGauss(node.get_shape(), node.dtype)

            for func, gfunc in ((X, gX), (Y, gY)):
              gAuto = sess.run(gfunc, feed_dict)
              gNum = numGrad(sess, loss, func, feed_dict)
              self.assertLess(
                  (np.linalg.norm(gAuto - gNum) / np.linalg.norm(gNum)), 1e-3)

if __name__ == "__main__":
  tf.test.main()
