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
"""The Kumaraswamy distribution class."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.python.ops import array_ops
from tensorflow.python.ops import check_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import random_ops
from tensorflow.python.ops import special_math_ops
from tensorflow.python.ops.distributions import beta
from tensorflow.python.ops.distributions import distribution
from tensorflow.python.ops.distributions import util as distribution_util
from tensorflow.python.util.tf_export import tf_export

__all__ = [
    "Kumaraswamy",
]

_kumaraswamy_sample_note = """Note: `x` must have dtype `self.dtype` and be in
`[0, 1].` It must have a shape compatible with `self.batch_shape()`."""


def _harmonic_number(x):
  """Compute the harmonic number from its analytic continuation.

  Derivation from [1] and Euler's constant [2].
  [1] -
  https://en.wikipedia.org/wiki/Digamma_function#Relation_to_harmonic_numbers
  [2] - https://en.wikipedia.org/wiki/Euler%E2%80%93Mascheroni_constant


  Args:
    x: input float.

  Returns:
    z: The analytic continuation of the harmonic number for the input.

  """
  one = array_ops.ones([], dtype=x.dtype)
  return math_ops.digamma(x + one) - math_ops.digamma(one)


@tf_export("distributions.Kumaraswamy")
class Kumaraswamy(beta.Beta):
  """Kumaraswamy distribution.

  The Kumaraswamy distribution is defined over the `(0, 1)` interval using
  parameters
  `concentration1` (aka "alpha") and `concentration0` (aka "beta").  It has a
  shape similar to the Beta distribution, but is reparameterizeable.

  #### Mathematical Details

  The probability density function (pdf) is,

  ```none
  pdf(x; alpha, beta) = alpha * beta * x**(alpha - 1) * (1 - x**alpha)**(beta -
  1)
  ```

  where:

  * `concentration1 = alpha`,
  * `concentration0 = beta`,

  Distribution parameters are automatically broadcast in all functions; see
  examples for details.

  #### Examples

  ```python
  # Create a batch of three Kumaraswamy distributions.
  alpha = [1, 2, 3]
  beta = [1, 2, 3]
  dist = Kumaraswamy(alpha, beta)

  dist.sample([4, 5])  # Shape [4, 5, 3]

  # `x` has three batch entries, each with two samples.
  x = [[.1, .4, .5],
       [.2, .3, .5]]
  # Calculate the probability of each pair of samples under the corresponding
  # distribution in `dist`.
  dist.prob(x)         # Shape [2, 3]
  ```

  ```python
  # Create batch_shape=[2, 3] via parameter broadcast:
  alpha = [[1.], [2]]      # Shape [2, 1]
  beta = [3., 4, 5]        # Shape [3]
  dist = Kumaraswamy(alpha, beta)

  # alpha broadcast as: [[1., 1, 1,],
  #                      [2, 2, 2]]
  # beta broadcast as:  [[3., 4, 5],
  #                      [3, 4, 5]]
  # batch_Shape [2, 3]
  dist.sample([4, 5])  # Shape [4, 5, 2, 3]

  x = [.2, .3, .5]
  # x will be broadcast as [[.2, .3, .5],
  #                         [.2, .3, .5]],
  # thus matching batch_shape [2, 3].
  dist.prob(x)         # Shape [2, 3]
  ```

  """

  def __init__(self,
               concentration1=None,
               concentration0=None,
               validate_args=False,
               allow_nan_stats=True,
               name="Kumaraswamy"):
    """Initialize a batch of Kumaraswamy distributions.

    Args:
      concentration1: Positive floating-point `Tensor` indicating mean
        number of successes; aka "alpha". Implies `self.dtype` and
        `self.batch_shape`, i.e.,
        `concentration1.shape = [N1, N2, ..., Nm] = self.batch_shape`.
      concentration0: Positive floating-point `Tensor` indicating mean
        number of failures; aka "beta". Otherwise has same semantics as
        `concentration1`.
      validate_args: Python `bool`, default `False`. When `True` distribution
        parameters are checked for validity despite possibly degrading runtime
        performance. When `False` invalid inputs may silently render incorrect
        outputs.
      allow_nan_stats: Python `bool`, default `True`. When `True`, statistics
        (e.g., mean, mode, variance) use the value "`NaN`" to indicate the
        result is undefined. When `False`, an exception is raised if one or
        more of the statistic's batch members are undefined.
      name: Python `str` name prefixed to Ops created by this class.
    """
    super(Kumaraswamy, self).__init__(
        concentration1=concentration1,
        concentration0=concentration0,
        validate_args=validate_args,
        allow_nan_stats=allow_nan_stats,
        name=name)
    self._reparameterization_type = distribution.FULLY_REPARAMETERIZED

  def _sample_n(self, n, seed=None):
    expanded_concentration1 = array_ops.ones_like(
        self.total_concentration, dtype=self.dtype) * self.concentration1
    expanded_concentration0 = array_ops.ones_like(
        self.total_concentration, dtype=self.dtype) * self.concentration0
    shape = array_ops.concat([[n], self.batch_shape_tensor()], 0)
    uniform_sample = random_ops.random_uniform(
        shape=shape, minval=0.0, maxval=1.0, dtype=self.dtype, seed=seed)

    kumaraswamy_sample = (1 - uniform_sample**(1. / expanded_concentration0))**(
        1. / expanded_concentration1)
    return kumaraswamy_sample

  @distribution_util.AppendDocstring(_kumaraswamy_sample_note)
  def _log_cdf(self, x):
    a = self.concentration1
    b = self.concentration0
    return math_ops.log1p(-(1 - x**a)**b)

  @distribution_util.AppendDocstring(_kumaraswamy_sample_note)
  def _cdf(self, x):
    a = self.concentration1
    b = self.concentration0
    return 1 - (1 - x**a)**b

  def _survival_function(self, x):
    a = self.concentration1
    b = self.concentration0
    return (1 - x**a)**b

  def _log_survival_function(self, x):
    a = self.concentration1
    b = self.concentration0
    return b * math_ops.log1p(-x**a)

  def _log_unnormalized_prob(self, x):
    x = self._maybe_assert_valid_sample(x)
    a = self.concentration1
    b = self.concentration0
    return (a - 1) * math_ops.log(x) + (b - 1) * math_ops.log1p(-x**a)

  def _log_normalization(self):
    a = self.concentration1
    b = self.concentration0
    return -(math_ops.log(a) + math_ops.log(b))

  def _entropy(self):
    a = self.concentration1
    b = self.concentration0
    return (1 - 1. / a) + (
        1 - 1. / b) * _harmonic_number(b) + math_ops.log(a) + math_ops.log(b)

  def _moment(self, n):
    """Compute the n'th (uncentered) moment."""
    expanded_concentration1 = array_ops.ones_like(
        self.total_concentration, dtype=self.dtype) * self.concentration1
    expanded_concentration0 = array_ops.ones_like(
        self.total_concentration, dtype=self.dtype) * self.concentration0
    beta_arg0 = 1 + n / expanded_concentration1
    beta_arg = array_ops.stack([beta_arg0, expanded_concentration0], -1)
    log_moment = math_ops.log(expanded_concentration0) + special_math_ops.lbeta(
        beta_arg)
    return math_ops.exp(log_moment)

  def _mean(self):
    return self._moment(1)

  def _variance(self):
    # TODO(b/72696533): Investigate a more numerically stable version.
    return self._moment(2) - math_ops.square(self._moment(1))

  @distribution_util.AppendDocstring(
      """Note: The mode is undefined when `concentration1 <= 1` or
      `concentration0 <= 1`. If `self.allow_nan_stats` is `True`, `NaN`
      is used for undefined modes. If `self.allow_nan_stats` is `False` an
      exception is raised when one or more modes are undefined.""")
  def _mode(self):
    a = self.concentration1
    b = self.concentration0
    mode = ((a - 1) / (a * b - 1))**(1. / a)
    if self.allow_nan_stats:
      nan = array_ops.fill(
          self.batch_shape_tensor(),
          np.array(np.nan, dtype=self.dtype.as_numpy_dtype),
          name="nan")
      is_defined = (self.concentration1 > 1.) & (self.concentration0 > 1.)
      return array_ops.where(is_defined, mode, nan)
    return control_flow_ops.with_dependencies([
        check_ops.assert_less(
            array_ops.ones([], dtype=self.dtype),
            self.concentration1,
            message="Mode undefined for concentration1 <= 1."),
        check_ops.assert_less(
            array_ops.ones([], dtype=self.dtype),
            self.concentration0,
            message="Mode undefined for concentration0 <= 1.")
    ], mode)
