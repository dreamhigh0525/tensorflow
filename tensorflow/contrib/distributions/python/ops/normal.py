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
"""The Normal (Gaussian) distribution class."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import math

from tensorflow.contrib.distributions.python.ops.distribution import ContinuousDistribution  # pylint: disable=line-too-long
from tensorflow.contrib.framework.python.framework import tensor_util as contrib_tensor_util  # pylint: disable=line-too-long
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_shape
from tensorflow.python.framework import tensor_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import constant_op
from tensorflow.python.ops import logging_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import random_ops


# TODO(ebrevdo): Use asserts contrib module when ready
def _assert_all_positive(x):
  return logging_ops.Assert(
      math_ops.reduce_all(x > 0),
      ["Tensor %s should contain only positive values: " % x.name, x])


class Normal(ContinuousDistribution):
  """The scalar Normal distribution with mean and stddev parameters mu, sigma.

  #### Mathematical details

  The PDF of this distribution is:

  ```f(x) = sqrt(1/(2*pi*sigma^2)) exp(-(x-mu)^2/(2*sigma^2))```

  #### Examples

  Examples of initialization of one or a batch of distributions.

  ```python
  # Define a single scalar Normal distribution.
  dist = tf.contrib.distributions.Normal(mu=0, sigma=3)

  # Evaluate the cdf at 1, returning a scalar.
  dist.cdf(1)

  # Define a batch of two scalar valued Normals.
  # The first has mean 1 and standard deviation 11, the second 2 and 22.
  dist = tf.contrib.distributions.Normal(mu=[1, 2.], sigma=[11, 22.])

  # Evaluate the pdf of the first distribution on 0, and the second on 1.5,
  # returning a length two tensor.
  dist.pdf([0, 1.5])

  # Get 3 samples, returning a 3 x 2 tensor.
  dist.sample(3)
  ```

  Arguments are broadcast when possible.

  ```python
  # Define a batch of two scalar valued Normals.
  # Both have mean 1, but different standard deviations.
  dist = tf.contrib.distributions.Normal(mu=1, sigma=[11, 22.])

  # Evaluate the pdf of both distributions on the same point, 3.0,
  # returning a length 2 tensor.
  dist.pdf(3.0)
  ```

  """

  def __init__(self, mu, sigma, name="Normal"):
    """Construct Normal distributions with mean and stddev `mu` and `sigma`.

    The parameters `mu` and `sigma` must be shaped in a way that supports
    broadcasting (e.g. `mu + sigma` is a valid operation).

    Args:
      mu: `float` or `double` tensor, the means of the distribution(s).
      sigma: `float` or `double` tensor, the stddevs of the distribution(s).
        sigma must contain only positive values.
      name: The name to give Ops created by the initializer.

    Raises:
      TypeError: if mu and sigma are different dtypes.
    """
    with ops.op_scope([mu, sigma], name):
      mu = ops.convert_to_tensor(mu)
      sigma = ops.convert_to_tensor(sigma)
      with ops.control_dependencies([_assert_all_positive(sigma)]):
        self._name = name
        self._mu = array_ops.identity(mu, name="mu")
        self._sigma = array_ops.identity(sigma, name="sigma")
        self._batch_shape = self._ones().get_shape()
        self._event_shape = tensor_shape.TensorShape([])

    contrib_tensor_util.assert_same_float_dtype((mu, sigma))

  @property
  def name(self):
    return self._name

  @property
  def dtype(self):
    return self._mu.dtype

  def batch_shape(self, name="batch_shape"):
    """Batch dimensions of this instance as a 1-D int32 `Tensor`.

    The product of the dimensions of the `batch_shape` is the number of
    independent distributions of this kind the instance represents.

    Args:
      name: name to give to the op.

    Returns:
      `Tensor` `batch_shape`
    """
    with ops.name_scope(self._name):
      return array_ops.shape(self._ones(), name=name)

  def get_batch_shape(self):
    """`TensorShape` available at graph construction time.

    Same meaning as `batch_shape`. May be only partially defined.

    Returns:
      batch shape
    """
    return self._batch_shape

  def event_shape(self, name="event_shape"):
    """Shape of a sample from a single distribution as a 1-D int32 `Tensor`.

    Args:
      name: name to give to the op.

    Returns:
      `Tensor` `event_shape`
    """
    with ops.name_scope(self.name):
      return constant_op.constant([], name=name, dtype=dtypes.int32)

  def get_event_shape(self):
    """`TensorShape` available at graph construction time.

    Same meaning as `event_shape`. May be only partially defined.

    Returns:
      event shape
    """
    return self._event_shape

  @property
  def mu(self):
    return self._mu

  @property
  def sigma(self):
    return self._sigma

  @property
  def mean(self):
    return self._mu * array_ops.ones_like(self._sigma)

  def log_pdf(self, x, name=None):
    """Log pdf of observations in `x` under these Normal distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `mu` and `sigma`.
      name: The name to give this op.

    Returns:
      log_pdf: tensor of dtype `dtype`, the log-PDFs of `x`.
    """
    with ops.op_scope([self._mu, self._sigma, x], name, "NormalLogPdf"):
      x = ops.convert_to_tensor(x)
      if x.dtype != self.dtype:
        raise TypeError("Input x dtype does not match dtype: %s vs. %s"
                        % (x.dtype, self.dtype))
      log_2_pi = constant_op.constant(math.log(2 * math.pi), dtype=self.dtype)
      return (-0.5*log_2_pi - math_ops.log(self._sigma)
              -0.5*math_ops.square((x - self._mu) / self._sigma))

  def cdf(self, x, name=None):
    """CDF of observations in `x` under these Normal distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `mu` and `sigma`.
      name: The name to give this op.

    Returns:
      cdf: tensor of dtype `dtype`, the CDFs of `x`.
    """
    with ops.op_scope([self._mu, self._sigma, x], name, "NormalCdf"):
      x = ops.convert_to_tensor(x)
      if x.dtype != self.dtype:
        raise TypeError("Input x dtype does not match dtype: %s vs. %s"
                        % (x.dtype, self.dtype))
      return (0.5 + 0.5*math_ops.erf(
          1.0/(math.sqrt(2.0) * self._sigma)*(x - self._mu)))

  def log_cdf(self, x, name=None):
    """Log CDF of observations `x` under these Normal distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `mu` and `sigma`.
      name: The name to give this op.

    Returns:
      log_cdf: tensor of dtype `dtype`, the log-CDFs of `x`.
    """
    with ops.op_scope([self._mu, self._sigma, x], name, "NormalLogCdf"):
      return math_ops.log(self.cdf(x))

  def pdf(self, x, name="pdf"):
    """The PDF of observations in `x` under these Normal distribution(s).

    Args:
      x: tensor of dtype `dtype`, must be broadcastable with `mu` and `sigma`.
      name: The name to give this op.

    Returns:
      pdf: tensor of dtype `dtype`, the pdf values of `x`.
    """
    return super(Normal, self).pdf(x, name=name)

  def entropy(self, name=None):
    """The entropy of Normal distribution(s).

    Args:
      name: The name to give this op.

    Returns:
      entropy: tensor of dtype `dtype`, the entropy.
    """
    with ops.op_scope([self._mu, self._sigma], name, "NormalEntropy"):
      two_pi_e1 = constant_op.constant(
          2 * math.pi * math.exp(1), dtype=self.dtype)
      # Use broadcasting rules to calculate the full broadcast sigma.
      sigma = self._sigma * array_ops.ones_like(self._mu)
      return 0.5 * math_ops.log(two_pi_e1 * math_ops.square(sigma))

  def sample(self, n, seed=None, name=None):
    """Sample `n` observations from the Normal Distributions.

    Args:
      n: `Scalar`, type int32, the number of observations to sample.
      seed: Python integer, the random seed.
      name: The name to give this op.

    Returns:
      samples: `[n, ...]`, a `Tensor` of `n` samples for each
        of the distributions determined by broadcasting the hyperparameters.
    """
    with ops.op_scope([self._mu, self._sigma, n], name, "NormalSample"):
      broadcast_shape = (self._mu + self._sigma).get_shape()
      n = ops.convert_to_tensor(n)
      shape = array_ops.concat(
          0, [array_ops.pack([n]), array_ops.shape(self.mean)])
      sampled = random_ops.random_normal(
          shape=shape, mean=0, stddev=1, dtype=self._mu.dtype, seed=seed)

      # Provide some hints to shape inference
      n_val = tensor_util.constant_value(n)
      final_shape = tensor_shape.vector(n_val).concatenate(broadcast_shape)
      sampled.set_shape(final_shape)

      return sampled * self._sigma + self._mu

  @property
  def is_reparameterized(self):
    return True

  def _ones(self):
    return array_ops.ones_like(self._mu + self._sigma)

  def _zeros(self):
    return array_ops.zeros_like(self._mu + self._sigma)
