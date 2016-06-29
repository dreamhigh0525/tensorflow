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
"""The Gamma distribution class."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib.distributions.python.ops import distribution  # pylint: disable=line-too-long
from tensorflow.contrib.framework.python.framework import tensor_util as contrib_tensor_util  # pylint: disable=line-too-long
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_shape
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import check_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import random_ops


class Gamma(distribution.ContinuousDistribution):
  """The `Gamma` distribution with parameter alpha and beta.

  The parameters are the shape and inverse scale parameters alpha, beta.

  The PDF of this distribution is:

  ```pdf(x) = (beta^alpha)(x^(alpha-1))e^(-x*beta)/Gamma(alpha), x > 0```

  and the CDF of this distribution is:

  ```cdf(x) =  GammaInc(alpha, beta * x) / Gamma(alpha), x > 0```

  where GammaInc is the incomplete lower Gamma function.

  Examples:

  ```python
  dist = Gamma(alpha=3.0, beta=2.0)
  dist2 = Gamma(alpha=[3.0, 4.0], beta=[2.0, 3.0])
  ```

  """

  def __init__(
      self, alpha, beta, strict=True, strict_statistics=True, name="Gamma"):
    """Construct Gamma distributions with parameters `alpha` and `beta`.

    The parameters `alpha` and `beta` must be shaped in a way that supports
    broadcasting (e.g. `alpha + beta` is a valid operation).

    Args:
      alpha: `float` or `double` tensor, the shape params of the
        distribution(s).
        alpha must contain only positive values.
      beta: `float` or `double` tensor, the inverse scale params of the
        distribution(s).
        beta must contain only positive values.
      strict: Whether to assert that `a > 0, b > 0`, and that `x > 0` in the
        methods `pdf(x)` and `log_pdf(x)`.  If `strict` is False
        and the inputs are invalid, correct behavior is not guaranteed.
      strict_statistics:  Boolean, default True.  If True, raise an exception if
        a statistic (e.g. mean/mode/etc...) is undefined for any batch member.
        If False, batch members with valid parameters leading to undefined
        statistics will return NaN for this statistic.
      name: The name to prepend to all ops created by this distribution.

    Raises:
      TypeError: if `alpha` and `beta` are different dtypes.
    """
    self._strict_statistics = strict_statistics
    self._strict = strict
    with ops.op_scope([alpha, beta], name) as scope:
      self._name = scope
      with ops.control_dependencies(
          [check_ops.assert_positive(alpha), check_ops.assert_positive(beta)]
          if strict else []):
        alpha = array_ops.identity(alpha, name="alpha")
        beta = array_ops.identity(beta, name="beta")

        contrib_tensor_util.assert_same_float_dtype((alpha, beta))
        self._broadcast_tensor = alpha + beta

    self._get_batch_shape = self._broadcast_tensor.get_shape()
    self._get_event_shape = tensor_shape.TensorShape([])

    self._alpha = alpha
    self._beta = beta

  @property
  def strict_statistics(self):
    """Boolean describing behavior when a stat is undefined for batch member."""
    return self._strict_statistics

  @property
  def strict(self):
    """Boolean describing behavior on invalid input."""
    return self._strict

  @property
  def name(self):
    """Name to prepend to all ops."""
    return self._name

  @property
  def dtype(self):
    """dtype of samples from this distribution."""
    return self._alpha.dtype

  @property
  def alpha(self):
    """Shape parameter."""
    return self._alpha

  @property
  def beta(self):
    """Inverse scale parameter."""
    return self._beta

  def batch_shape(self, name="batch_shape"):
    """Batch dimensions of this instance as a 1-D int32 `Tensor`.

    The product of the dimensions of the `batch_shape` is the number of
    independent distributions of this kind the instance represents.

    Args:
      name: name to give to the op

    Returns:
      `Tensor` `batch_shape`
    """
    with ops.name_scope(self.name):
      with ops.op_scope([self._broadcast_tensor], name):
        return array_ops.shape(self._broadcast_tensor)

  def get_batch_shape(self):
    """`TensorShape` available at graph construction time.

    Same meaning as `batch_shape`. May be only partially defined.

    Returns:
      `TensorShape` object.
    """
    return self._get_batch_shape

  def event_shape(self, name="event_shape"):
    """Shape of a sample from a single distribution as a 1-D int32 `Tensor`.

    Args:
      name: name to give to the op

    Returns:
      `Tensor` `event_shape`
    """
    with ops.name_scope(self.name):
      with ops.op_scope([], name):
        return constant_op.constant([], dtype=dtypes.int32)

  def get_event_shape(self):
    """`TensorShape` available at graph construction time.

    Same meaning as `event_shape`. May be only partially defined.

    Returns:
      `TensorShape` object.
    """
    return self._get_event_shape

  def mean(self, name="mean"):
    """Mean of each batch member."""
    with ops.name_scope(self.name):
      with ops.op_scope([self._alpha, self._beta], name):
        return self._alpha / self._beta

  def mode(self, name="mode"):
    """Mode of each batch member.

    The mode of a gamma distribution is `(alpha - 1) / beta` when `alpha > 1`,
    and `NaN` otherwise.  If `self.strict_statistics` is `True`, an exception
    will be raised rather than returning `NaN`.

    Args:
      name:  A name to give this op.

    Returns:
      The mode for every batch member, a `Tensor` with same `dtype` as self.
    """
    alpha = self._alpha
    beta = self._beta
    with ops.name_scope(self.name):
      with ops.op_scope([alpha, beta], name):
        mode_if_defined = (alpha - 1.0) / beta
        if self.strict_statistics:
          one = ops.convert_to_tensor(1.0, dtype=self.dtype)
          return control_flow_ops.with_dependencies(
              [check_ops.assert_less(one, alpha)], mode_if_defined)
        else:
          alpha_ge_1 = alpha >= 1.0
          nan = np.nan * self._ones()
          return math_ops.select(alpha_ge_1, mode_if_defined, nan)

  def variance(self, name="variance"):
    """Variance of each batch member."""
    with ops.name_scope(self.name):
      with ops.op_scope([self._alpha, self._beta], name):
        return self._alpha / math_ops.square(self._beta)

  def std(self, name="std"):
    """Standard deviation of this distribution."""
    with ops.name_scope(self.name):
      with ops.op_scope([self._alpha, self._beta], name):
        return math_ops.sqrt(self._alpha) / self._beta

  def log_pdf(self, x, name="log_pdf"):
    """Log pdf of observations in `x` under these Gamma distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
      name: The name to give this op.

    Returns:
      log_pdf: tensor of dtype `dtype`, the log-PDFs of `x`.

    Raises:
      TypeError: if `x` and `alpha` are different dtypes.
    """
    with ops.name_scope(self.name):
      with ops.op_scope([self._alpha, self._beta, x], name):
        alpha = self._alpha
        beta = self._beta
        x = ops.convert_to_tensor(x)
        x = control_flow_ops.with_dependencies(
            [check_ops.assert_positive(x)] if self.strict else [],
            x)
        contrib_tensor_util.assert_same_float_dtype(tensors=[x,],
                                                    dtype=self.dtype)

        return (alpha * math_ops.log(beta) + (alpha - 1) * math_ops.log(x) -
                beta * x - math_ops.lgamma(self._alpha))

  def pdf(self, x, name="pdf"):
    """Pdf of observations in `x` under these Gamma distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
      name: The name to give this op.

    Returns:
      pdf: tensor of dtype `dtype`, the PDFs of `x`

    Raises:
      TypeError: if `x` and `alpha` are different dtypes.
    """
    with ops.name_scope(self.name):
      with ops.op_scope([], name):
        return math_ops.exp(self.log_pdf(x))

  def log_cdf(self, x, name="log_cdf"):
    """Log CDF of observations `x` under these Gamma distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
      name: The name to give this op.

    Returns:
      log_cdf: tensor of dtype `dtype`, the log-CDFs of `x`.
    """
    with ops.name_scope(self.name):
      with ops.op_scope([self._alpha, self._beta, x], name):
        x = ops.convert_to_tensor(x)
        x = control_flow_ops.with_dependencies(
            [check_ops.assert_positive(x)] if self.strict else [],
            x)
        contrib_tensor_util.assert_same_float_dtype(tensors=[x,],
                                                    dtype=self.dtype)
        # Note that igamma returns the regularized incomplete gamma function,
        # which is what we want for the CDF.
        return math_ops.log(math_ops.igamma(self._alpha, self._beta * x))

  def cdf(self, x, name="cdf"):
    """CDF of observations `x` under these Gamma distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
      name: The name to give this op.

    Returns:
      cdf: tensor of dtype `dtype`, the CDFs of `x`.
    """
    with ops.name_scope(self.name):
      with ops.op_scope([self._alpha, self._beta, x], name):
        return math_ops.igamma(self._alpha, self._beta * x)

  def entropy(self, name="entropy"):
    """The entropy of Gamma distribution(s).

    This is defined to be

    ```
    entropy = alpha - log(beta) + log(Gamma(alpha))
                 + (1-alpha)digamma(alpha)
    ```

    where digamma(alpha) is the digamma function.

    Args:
      name: The name to give this op.

    Returns:
      entropy: tensor of dtype `dtype`, the entropy.
    """
    with ops.name_scope(self.name):
      with ops.op_scope([self.alpha, self._beta], name):
        alpha = self._alpha
        beta = self._beta
        return (alpha - math_ops.log(beta) + math_ops.lgamma(alpha) +
                (1 - alpha) * math_ops.digamma(alpha))

  def sample(self, n, seed=None, name="sample"):
    """Draws `n` samples from the Gamma distribution(s).

    See the doc for tf.random_gamma for further detail.

    Args:
      n: Python integer, the number of observations to sample from each
        distribution.
      seed: Python integer, the random seed for this operation.
      name: Optional name for the operation.

    Returns:
      samples: a `Tensor` of shape `(n,) + self.batch_shape + self.event_shape`
          with values of type `self.dtype`.
    """
    with ops.op_scope([n, self.alpha, self._beta], self.name):
      return random_ops.random_gamma([n],
                                     self.alpha,
                                     beta=self._beta,
                                     dtype=self.dtype,
                                     seed=seed,
                                     name=name)

  @property
  def is_reparameterized(self):
    return False

  def _ones(self):
    return array_ops.ones_like(self._alpha + self._beta, dtype=self.dtype)
