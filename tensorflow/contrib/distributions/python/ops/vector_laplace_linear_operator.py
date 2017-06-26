# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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
"""Vectorized Laplace distribution class, directly using LinearOperator."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib import linalg
from tensorflow.contrib.distributions.python.ops import bijectors
from tensorflow.contrib.distributions.python.ops import distribution_util
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops.distributions import laplace
from tensorflow.python.ops.distributions import transformed_distribution


__all__ = [
    "VectorLaplaceLinearOperator"
]

_mvn_sample_note = """
`value` is a batch vector with compatible shape if `value` is a `Tensor` whose
shape can be broadcast up to either:

```python
self.batch_shape + self.event_shape
```

or

```python
[M1, ..., Mm] + self.batch_shape + self.event_shape
```

"""


class VectorLaplaceLinearOperator(
    transformed_distribution.TransformedDistribution):
  """The vectorization of the Laplace distribution on `R^k`.

  The vector laplace distribution is defined over `R^k`, and parameterized by
  a (batch of) length-`k` `loc` vector (the means) and a (batch of) `k x k`
  `scale` matrix:  `covariance = 2 * scale @ scale.T`, where `@` denotes
  matrix-multiplication.

  #### Mathematical Details

  The probability density function (pdf) is,

  ```none
  pdf(x; loc, scale) = exp(-||y||_1) / Z,
  y = inv(scale) @ (x - loc),
  Z = 2**k |det(scale)|,
  ```

  where:

  * `loc` is a vector in `R^k`,
  * `scale` is a linear operator in `R^{k x k}`, `cov = scale @ scale.T`,
  * `Z` denotes the normalization constant, and,
  * `||y||_1` denotes the `l1` norm of `y`, `sum_i |y_i|.

  The VectorLaplace distribution is a member of the [location-scale
  family](https://en.wikipedia.org/wiki/Location-scale_family), i.e., it can be
  constructed as,

  ```none
  X = (X_1, ..., X_k), each X_i ~ Laplace(loc=0, scale=1)
  Y = (Y_1, ...,Y_k) = scale @ X + loc
  ```

  #### About `VectorLaplace` and `Vector` distributions in TensorFlow.

  The `VectorLaplace` is a non-standard distribution that has useful properties.

  The marginals `Y_1, ..., Y_k` are *not* Laplace random variables, due to
  the fact that the sum of Laplace random variables is not Laplace.

  Instead, `Y` is a vector whose components are linear combinations of Laplace
  random variables.  Thus, `Y` lives in the vector space generated by `vectors`
  of Laplace distributions.  This allows the user to decide the mean and
  covariance (by setting `loc` and `scale`), while preserving some properties of
  the Laplace distribution.  In particular, the tails of `Y_i` will be (up to
  polynomial factors) exponentially decaying.

  To see this last statement, note that the pdf of `Y_i` is the convolution of
  the pdf of `k` independent Laplace random variables.  One can then show by
  induction that distributions with exponential (up to polynomial factors) tails
  are closed under convolution.


  #### Examples

  ```python
  ds = tf.contrib.distributions
  la = tf.contrib.linalg

  # Initialize a single 3-variate VectorLaplace with some desired covariance.
  mu = [1., 2, 3]
  cov = [[ 0.36,  0.12,  0.06],
         [ 0.12,  0.29, -0.13],
         [ 0.06, -0.13,  0.26]]

  scale = tf.cholesky(cov)
  # ==> [[ 0.6,  0. ,  0. ],
  #      [ 0.2,  0.5,  0. ],
  #      [ 0.1, -0.3,  0.4]])

  # Divide scale by sqrt(2) so that the final covariance will be what we want.
  vla = ds.VectorLaplaceLinearOperator(
      loc=mu,
      scale=la.LinearOperatorTriL(scale / tf.sqrt(2)))

  # Covariance agrees with cholesky(cov) parameterization.
  vla.covariance().eval()
  # ==> [[ 0.36,  0.12,  0.06],
  #      [ 0.12,  0.29, -0.13],
  #      [ 0.06, -0.13,  0.26]]

  # Compute the pdf of an`R^3` observation; return a scalar.
  vla.prob([-1., 0, 1]).eval()  # shape: []

  # Initialize a 2-batch of 3-variate Vector Laplace's.
  mu = [[1., 2, 3],
        [11, 22, 33]]              # shape: [2, 3]
  scale_diag = [[1., 2, 3],
                [0.5, 1, 1.5]]     # shape: [2, 3]

  vla = ds.VectorLaplaceLinearOperator(
      loc=mu,
      scale=la.LinearOperatorDiag(scale_diag))

  # Compute the pdf of two `R^3` observations; return a length-2 vector.
  x = [[-0.9, 0, 0.1],
       [-10, 0, 9]]     # shape: [2, 3]
  vla.prob(x).eval()    # shape: [2]
  ```

  """

  def __init__(self,
               loc=None,
               scale=None,
               validate_args=False,
               allow_nan_stats=True,
               name="VectorLaplaceLinearOperator"):
    """Construct Vector Laplace distribution on `R^k`.

    The `batch_shape` is the broadcast shape between `loc` and `scale`
    arguments.

    The `event_shape` is given by last dimension of the matrix implied by
    `scale`. The last dimension of `loc` (if provided) must broadcast with this.

    Recall that `covariance = 2 * scale @ scale.T`.

    Additional leading dimensions (if any) will index batches.

    Args:
      loc: Floating-point `Tensor`. If this is set to `None`, `loc` is
        implicitly `0`. When specified, may have shape `[B1, ..., Bb, k]` where
        `b >= 0` and `k` is the event size.
      scale: Instance of `LinearOperator` with same `dtype` as `loc` and shape
        `[B1, ..., Bb, k, k]`.
      validate_args: Python `bool`, default `False`. Whether to validate input
        with asserts. If `validate_args` is `False`, and the inputs are
        invalid, correct behavior is not guaranteed.
      allow_nan_stats: Python `bool`, default `True`. If `False`, raise an
        exception if a statistic (e.g. mean/mode/etc...) is undefined for any
        batch member If `True`, batch members with valid parameters leading to
        undefined statistics will return NaN for this statistic.
      name: The name to give Ops created by the initializer.

    Raises:
      ValueError: if `scale` is unspecified.
      TypeError: if not `scale.dtype.is_floating`
    """
    parameters = locals()
    if scale is None:
      raise ValueError("Missing required `scale` parameter.")
    if not scale.dtype.is_floating:
      raise TypeError("`scale` parameter must have floating-point dtype.")

    with ops.name_scope(name, values=[loc] + scale.graph_parents):
      # Since expand_dims doesn't preserve constant-ness, we obtain the
      # non-dynamic value if possible.
      loc = ops.convert_to_tensor(loc, name="loc") if loc is not None else loc
      batch_shape, event_shape = distribution_util.shapes_from_loc_and_scale(
          loc, scale)

      super(VectorLaplaceLinearOperator, self).__init__(
          distribution=laplace.Laplace(
              loc=array_ops.zeros([], dtype=scale.dtype),
              scale=array_ops.ones([], dtype=scale.dtype)),
          bijector=bijectors.AffineLinearOperator(
              shift=loc, scale=scale, validate_args=validate_args),
          batch_shape=batch_shape,
          event_shape=event_shape,
          validate_args=validate_args,
          name=name)
      self._parameters = parameters

  @property
  def loc(self):
    """The `loc` `Tensor` in `Y = scale @ X + loc`."""
    return self.bijector.shift

  @property
  def scale(self):
    """The `scale` `LinearOperator` in `Y = scale @ X + loc`."""
    return self.bijector.scale

  @distribution_util.AppendDocstring(_mvn_sample_note)
  def _log_prob(self, x):
    return super(VectorLaplaceLinearOperator, self)._log_prob(x)

  @distribution_util.AppendDocstring(_mvn_sample_note)
  def _prob(self, x):
    return super(VectorLaplaceLinearOperator, self)._prob(x)

  def _mean(self):
    shape = self.batch_shape.concatenate(self.event_shape)
    has_static_shape = shape.is_fully_defined()
    if not has_static_shape:
      shape = array_ops.concat([
          self.batch_shape_tensor(),
          self.event_shape_tensor(),
      ], 0)

    if self.loc is None:
      return array_ops.zeros(shape, self.dtype)

    if has_static_shape and shape == self.loc.get_shape():
      return array_ops.identity(self.loc)

    # Add dummy tensor of zeros to broadcast.  This is only necessary if shape
    # != self.loc.shape, but we could not determine if this is the case.
    return array_ops.identity(self.loc) + array_ops.zeros(shape, self.dtype)

  def _covariance(self):
    # Let
    #   W = (w1,...,wk), with wj ~ iid Laplace(0, 1).
    # Then this distribution is
    #   X = loc + LW,
    # and since E[X] = loc,
    #   Cov(X) = E[LW W^T L^T] = L E[W W^T] L^T.
    # Since E[wi wj] = 0 if i != j, and 2 if i == j, we have
    #   Cov(X) = 2 LL^T
    if distribution_util.is_diagonal_scale(self.scale):
      return 2. * array_ops.matrix_diag(math_ops.square(self.scale.diag_part()))
    else:
      return 2. * self.scale.matmul(self.scale.to_dense(), adjoint_arg=True)

  def _variance(self):
    if distribution_util.is_diagonal_scale(self.scale):
      return 2. * math_ops.square(self.scale.diag_part())
    elif (isinstance(self.scale, linalg.LinearOperatorUDVHUpdate)
          and self.scale.is_self_adjoint):
      return array_ops.matrix_diag_part(
          2. * self.scale.matmul(self.scale.to_dense()))
    else:
      return 2. * array_ops.matrix_diag_part(
          self.scale.matmul(self.scale.to_dense(), adjoint_arg=True))

  def _stddev(self):
    if distribution_util.is_diagonal_scale(self.scale):
      return np.sqrt(2) * math_ops.abs(self.scale.diag_part())
    elif (isinstance(self.scale, linalg.LinearOperatorUDVHUpdate)
          and self.scale.is_self_adjoint):
      return np.sqrt(2) * math_ops.sqrt(array_ops.matrix_diag_part(
          self.scale.matmul(self.scale.to_dense())))
    else:
      return np.sqrt(2) * math_ops.sqrt(array_ops.matrix_diag_part(
          self.scale.matmul(self.scale.to_dense(), adjoint_arg=True)))

  def _mode(self):
    return self._mean()
