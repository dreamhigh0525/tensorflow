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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import tensorflow as tf

distributions = tf.contrib.distributions


def softplus(x):
  return np.log(1 + np.exp(x))


class OperatorPDCholeskyTest(tf.test.TestCase):

  def setUp(self):
    self._rng = np.random.RandomState(42)

  def _random_cholesky_array(self, shape):
    mat = self._rng.rand(*shape)
    chol = distributions.batch_matrix_diag_transform(mat,
                                                     transform=tf.nn.softplus)
    # Zero the upper triangle because we're using this as a true Cholesky factor
    # in our tests.
    return tf.batch_matrix_band_part(chol, -1, 0).eval()

  def _numpy_inv_quadratic_form(self, chol, x):
    # Numpy works with batches now (calls them "stacks").
    x_expanded = np.expand_dims(x, -1)
    whitened = np.linalg.solve(chol, x_expanded)
    return (whitened**2).sum(axis=-1).sum(axis=-1)

  def test_inv_quadratic_form_x_rank_same_as_broadcast_rank(self):
    with self.test_session():
      for batch_shape in [(), (2,)]:
        for k in [1, 3]:

          x_shape = batch_shape + (k,)
          x = self._rng.randn(*x_shape)

          chol_shape = batch_shape + (k, k)
          chol = self._random_cholesky_array(chol_shape)
          operator = distributions.OperatorPDCholesky(chol)
          qf = operator.inv_quadratic_form(x)

          self.assertEqual(batch_shape, qf.get_shape())

          numpy_qf = self._numpy_inv_quadratic_form(chol, x)
          self.assertAllClose(numpy_qf, qf.eval())

  def test_inv_quadratic_form_x_and_chol_batch_shape_dont_match(self):
    # In this case, chol will have to be stretched to match x.
    with self.test_session():
      k = 3
      x_shape = (2, k)
      chol_shape = (1, k, k)
      broadcast_batch_shape = (2,)

      x = self._rng.randn(*x_shape)
      chol = self._random_cholesky_array(chol_shape)

      operator = distributions.OperatorPDCholesky(chol)
      qf = operator.inv_quadratic_form(x)

      self.assertEqual(broadcast_batch_shape, qf.get_shape())

      numpy_qf = self._numpy_inv_quadratic_form(chol, x)
      self.assertAllClose(numpy_qf, qf.eval())

  def test_inv_quadratic_form_x_rank_less_than_broadcast_rank(self):
    with self.test_session():
      for batch_shape in [(2,), (2, 3)]:
        for k in [1, 4]:

          # x will not have the leading dimension.
          x_shape = batch_shape[1:] + (k,)
          x = self._rng.randn(*x_shape)

          chol_shape = batch_shape + (k, k)
          chol = self._random_cholesky_array(chol_shape)
          operator = distributions.OperatorPDCholesky(chol)
          qf = operator.inv_quadratic_form(x)

          self.assertEqual(batch_shape, qf.get_shape())

          x_upshaped = x + np.zeros(chol.shape[:-1])
          numpy_qf = self._numpy_inv_quadratic_form(chol, x_upshaped)
          numpy_qf = numpy_qf.reshape(batch_shape)
          self.assertAllClose(numpy_qf, qf.eval())

  def test_inv_quadratic_form_x_rank_greater_than_broadcast_rank(self):
    with self.test_session():
      for batch_shape in [(2,), (2, 3)]:
        for k in [1, 4]:

          x_shape = batch_shape + (k,)
          x = self._rng.randn(*x_shape)

          # chol will not have the leading dimension.
          chol_shape = batch_shape[1:] + (k, k)
          chol = self._random_cholesky_array(chol_shape)
          operator = distributions.OperatorPDCholesky(chol)
          qf = operator.inv_quadratic_form(x)
          numpy_qf = self._numpy_inv_quadratic_form(chol, x)

          self.assertEqual(batch_shape, qf.get_shape())
          self.assertAllClose(numpy_qf, qf.eval())

  def test_inv_quadratic_form_x_rank_two_greater_than_broadcast_rank(self):
    with self.test_session():
      for batch_shape in [(2, 3), (2, 3, 4), (2, 3, 4, 5)]:
        for k in [1, 4]:

          x_shape = batch_shape + (k,)
          x = self._rng.randn(*x_shape)

          # chol will not have the leading two dimensions.
          chol_shape = batch_shape[2:] + (k, k)
          chol = self._random_cholesky_array(chol_shape)
          operator = distributions.OperatorPDCholesky(chol)
          qf = operator.inv_quadratic_form(x)
          numpy_qf = self._numpy_inv_quadratic_form(chol, x)

          self.assertEqual(batch_shape, qf.get_shape())
          self.assertAllClose(numpy_qf, qf.eval())

  def test_log_det(self):
    with self.test_session():
      batch_shape = ()
      for k in [1, 4]:
        chol_shape = batch_shape + (k, k)
        chol = self._random_cholesky_array(chol_shape)
        operator = distributions.OperatorPDCholesky(chol)
        log_det = operator.log_det()
        expected_log_det = np.log(np.prod(np.diag(chol))**2)

        self.assertEqual(batch_shape, log_det.get_shape())
        self.assertAllClose(expected_log_det, log_det.eval())

  def test_log_det_batch_matrix(self):
    with self.test_session():
      batch_shape = (2, 3)
      for k in [1, 4]:
        chol_shape = batch_shape + (k, k)
        chol = self._random_cholesky_array(chol_shape)
        operator = distributions.OperatorPDCholesky(chol)
        log_det = operator.log_det()

        self.assertEqual(batch_shape, log_det.get_shape())

        # Test the log-determinant of the [1, 1] matrix.
        chol_11 = chol[1, 1, :, :]
        expected_log_det = np.log(np.prod(np.diag(chol_11))**2)
        self.assertAllClose(expected_log_det, log_det.eval()[1, 1])

  def test_sqrt_matmul_single_matrix(self):
    with self.test_session():
      batch_shape = ()
      for k in [1, 4]:
        x_shape = batch_shape + (k, 3)
        x = self._rng.rand(*x_shape)
        chol_shape = batch_shape + (k, k)
        chol = self._random_cholesky_array(chol_shape)

        operator = distributions.OperatorPDCholesky(chol)

        sqrt_operator_times_x = operator.sqrt_matmul(x)
        expected = tf.batch_matmul(chol, x)

        self.assertEqual(expected.get_shape(),
                         sqrt_operator_times_x.get_shape())
        self.assertAllClose(expected.eval(), sqrt_operator_times_x.eval())

  def test_sqrt_matmul_batch_matrix(self):
    with self.test_session():
      batch_shape = (2, 3)
      for k in [1, 4]:
        x_shape = batch_shape + (k, 5)
        x = self._rng.rand(*x_shape)
        chol_shape = batch_shape + (k, k)
        chol = self._random_cholesky_array(chol_shape)

        operator = distributions.OperatorPDCholesky(chol)

        sqrt_operator_times_x = operator.sqrt_matmul(x)
        expected = tf.batch_matmul(chol, x)

        self.assertEqual(expected.get_shape(),
                         sqrt_operator_times_x.get_shape())
        self.assertAllClose(expected.eval(), sqrt_operator_times_x.eval())

  def test_matmul_batch_matrix(self):
    with self.test_session():
      batch_shape = (2, 3)
      for k in [1, 4]:
        x_shape = batch_shape + (k, 5)
        x = self._rng.rand(*x_shape)
        chol_shape = batch_shape + (k, k)
        chol = self._random_cholesky_array(chol_shape)

        operator = distributions.OperatorPDCholesky(chol)

        chol_times_x = tf.batch_matmul(chol, x, adj_x=True)
        expected = tf.batch_matmul(chol, chol_times_x)

        self.assertEqual(expected.get_shape(), operator.matmul(x).get_shape())
        self.assertAllClose(expected.eval(), operator.matmul(x).eval())

  def test_shape(self):
    # All other shapes are defined by the abstractmethod shape, so we only need
    # to test this.
    with self.test_session():
      for shape in [(3, 3), (2, 3, 3), (1, 2, 3, 3)]:
        chol = self._random_cholesky_array(shape)
        operator = distributions.OperatorPDCholesky(chol)
        self.assertAllEqual(shape, operator.shape().eval())

  def test_to_dense(self):
    with self.test_session():
      chol = self._random_cholesky_array((3, 3))
      operator = distributions.OperatorPDCholesky(chol)
      self.assertAllClose(chol.dot(chol.T), operator.to_dense().eval())

  def test_to_dense_sqrt(self):
    with self.test_session():
      chol = self._random_cholesky_array((2, 3, 3))
      operator = distributions.OperatorPDCholesky(chol)
      self.assertAllClose(chol, operator.to_dense_sqrt().eval())

  def test_non_positive_definite_matrix_raises(self):
    # Singlular matrix with one positive eigenvalue and one zero eigenvalue.
    with self.test_session():
      lower_mat = [[1.0, 0.0], [2.0, 0.0]]
      operator = distributions.OperatorPDCholesky(lower_mat)
      with self.assertRaisesOpError('x > 0 did not hold'):
        operator.to_dense().eval()

  def test_non_positive_definite_matrix_does_not_raise_if_not_verify_pd(self):
    # Singlular matrix with one positive eigenvalue and one zero eigenvalue.
    with self.test_session():
      lower_mat = [[1.0, 0.0], [2.0, 0.0]]
      operator = distributions.OperatorPDCholesky(lower_mat, verify_pd=False)
      operator.to_dense().eval()  # Should not raise.

  def test_not_having_two_identical_last_dims_raises(self):
    # Unless the last two dims are equal, this cannot represent a matrix, and it
    # should raise.
    with self.test_session():
      batch_vec = [[1.0], [2.0]]  # shape 2 x 1
      with self.assertRaisesRegexp(ValueError, '.*Dimensions.*'):
        operator = distributions.OperatorPDCholesky(batch_vec)
        operator.to_dense().eval()


class BatchMatrixDiagTransformTest(tf.test.TestCase):

  def setUp(self):
    self._rng = np.random.RandomState(0)

  def check_off_diagonal_same(self, m1, m2):
    """Check the lower triangular part, not upper or diag."""
    self.assertAllClose(np.tril(m1, k=-1), np.tril(m2, k=-1))
    self.assertAllClose(np.triu(m1, k=1), np.triu(m2, k=1))

  def test_non_batch_matrix_with_transform(self):
    mat = self._rng.rand(4, 4)
    with self.test_session():
      chol = distributions.batch_matrix_diag_transform(mat,
                                                       transform=tf.nn.softplus)
      self.assertEqual((4, 4), chol.get_shape())

      self.check_off_diagonal_same(mat, chol.eval())
      self.assertAllClose(softplus(np.diag(mat)), np.diag(chol.eval()))

  def test_non_batch_matrix_no_transform(self):
    mat = self._rng.rand(4, 4)
    with self.test_session():
      # Default is no transform.
      chol = distributions.batch_matrix_diag_transform(mat)
      self.assertEqual((4, 4), chol.get_shape())
      self.assertAllClose(mat, chol.eval())

  def test_batch_matrix_with_transform(self):
    mat = self._rng.rand(2, 4, 4)
    mat_0 = mat[0, :, :]
    with self.test_session():
      chol = distributions.batch_matrix_diag_transform(mat,
                                                       transform=tf.nn.softplus)

      self.assertEqual((2, 4, 4), chol.get_shape())

      chol_0 = chol.eval()[0, :, :]

      self.check_off_diagonal_same(mat_0, chol_0)
      self.assertAllClose(softplus(np.diag(mat_0)), np.diag(chol_0))

      self.check_off_diagonal_same(mat_0, chol_0)
      self.assertAllClose(softplus(np.diag(mat_0)), np.diag(chol_0))

  def test_batch_matrix_no_transform(self):
    mat = self._rng.rand(2, 4, 4)
    with self.test_session():
      # Default is no transform.
      chol = distributions.batch_matrix_diag_transform(mat)

      self.assertEqual((2, 4, 4), chol.get_shape())
      self.assertAllClose(mat, chol.eval())


if __name__ == '__main__':
  tf.test.main()
