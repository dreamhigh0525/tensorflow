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
"""The OneHotCategorical distribution class."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.contrib.distributions.python.ops import distribution
from tensorflow.contrib.distributions.python.ops import distribution_util
from tensorflow.contrib.distributions.python.ops import kullback_leibler
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import check_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import nn_ops
from tensorflow.python.ops import random_ops


class _OneHotCategorical(distribution.Distribution):
  """OneHotCategorical distribution.

  The categorical distribution is parameterized by the log-probabilities
  of a set of classes. The difference between OneHotCategorical and Categorical
  distributions is that OneHotCategorical is a discrete distribution over
  one-hot bit vectors whereas Categorical is a discrete distribution over
  positive integers.

  This class provides methods to create indexed batches of OneHotCategorical
  distributions.  If the provided `logits` or `probs` is rank 2 or higher, for
  every fixed set of leading dimensions, the last dimension represents one
  single OneHotCategorical distribution.  When calling distribution
  functions (e.g. `dist.prob(x)`), `logits` and `x` are broadcast to the
  same shape (if possible).  In all cases, the last dimension of `logits,x`
  represents single OneHotCategorical distributions.

  #### Examples

  Creates a 3-class distiribution, with the 2nd class, the most likely to be
  drawn from.

  ```python
  p = [0.1, 0.5, 0.4]
  dist = OneHotCategorical(probs=p)
  ```

  Creates a 3-class distiribution, with the 2nd class the most likely to be
  drawn from, using logits.

  ```python
  logits = [-2, 2, 0]
  dist = OneHotCategorical(logits=logits)
  ```

  Creates a 3-class distribution, with the 3rd class is most likely to be drawn.

  ```python
  # counts is a scalar.
  p = [0.1, 0.4, 0.5]
  dist = OneHotCategorical(probs=p)
  dist.pmf([0,1,0])  # Shape []

  # p will be broadcast to [[0.1, 0.4, 0.5], [0.1, 0.4, 0.5]] to match.
  samples = [[0,1,0], [1,0,0]]
  dist.pmf(samples)  # Shape [2]
  ```

  """

  def __init__(
      self,
      logits=None,
      probs=None,
      dtype=dtypes.int32,
      validate_args=False,
      allow_nan_stats=True,
      name="OneHotCategorical"):
    """Initialize OneHotCategorical distributions using class log-probabilities.

    Args:
      logits: An N-D `Tensor`, `N >= 1`, representing the log probabilities of a
        set of Categorical distributions. The first `N - 1` dimensions index
        into a batch of independent distributions and the last dimension
        represents a vector of logits for each class. Only one of `logits` or
        `probs` should be passed in.
      probs: An N-D `Tensor`, `N >= 1`, representing the probabilities of a set
        of Categorical distributions. The first `N - 1` dimensions index into a
        batch of independent distributions and the last dimension represents a
        vector of probabilities for each class. Only one of `logits` or `probs`
        should be passed in.
      dtype: The type of the event samples (default: int32).
      validate_args: Python `Boolean`, default `False`. When `True` distribution
        parameters are checked for validity despite possibly degrading runtime
        performance. When `False` invalid inputs may silently render incorrect
        outputs.
      allow_nan_stats: Python `Boolean`, default `True`. When `True`, statistics
        (e.g., mean, mode, variance) use the value "`NaN`" to indicate the
        result is undefined.  When `False`, an exception is raised if one or
        more of the statistic's batch members are undefined.
      name: `String` name prefixed to Ops created by this class.
    """
    parameters = locals()
    parameters.pop("self")
    with ops.name_scope(name, values=[logits, probs]) as ns:
      self._logits, self._probs = distribution_util.get_logits_and_probs(
          name=name, logits=logits, probs=probs, validate_args=validate_args,
          multidimensional=True)

      logits_shape_static = self._logits.get_shape().with_rank_at_least(1)
      if logits_shape_static.ndims is not None:
        self._batch_rank = ops.convert_to_tensor(
            logits_shape_static.ndims - 1,
            dtype=dtypes.int32,
            name="batch_rank")
      else:
        with ops.name_scope(name="batch_rank"):
          self._batch_rank = array_ops.rank(self._logits) - 1

      logits_shape = array_ops.shape(self._logits, name="logits_shape")

      with ops.name_scope(name="event_size"):
        self._event_size = logits_shape[-1]

      with ops.name_scope(name="batch_shape"):
        self._batch_shape_val = logits_shape[:-1]

    super(_OneHotCategorical, self).__init__(
        dtype=dtype,
        is_continuous=False,
        reparameterization_type=distribution.NOT_REPARAMETERIZED,
        validate_args=validate_args,
        allow_nan_stats=allow_nan_stats,
        parameters=parameters,
        graph_parents=[self._logits,
                       self._probs],
        name=ns)

  @property
  def event_size(self):
    """Scalar `int32` tensor: the number of classes."""
    return self._event_size

  @property
  def logits(self):
    """Vector of coordinatewise logits."""
    return self._logits

  @property
  def probs(self):
    """Vector of probabilities summing to one.

    Each element is the probability of drawing that coordinate."""
    return self._probs

  def _batch_shape(self):
    # Use identity to inherit callers "name".
    return array_ops.identity(self._batch_shape_val)

  def _get_batch_shape(self):
    return self.logits.get_shape()[:-1]

  def _event_shape(self):
    return array_ops.shape(self.logits)[-1]

  def _get_event_shape(self):
    return self.logits.get_shape().with_rank_at_least(1)[-1:]

  def _sample_n(self, n, seed=None):
    sample_shape = array_ops.concat(([n], array_ops.shape(self.logits)), 0)
    logits = self.logits
    if logits.get_shape().ndims == 2:
      logits_2d = logits
    else:
      logits_2d = array_ops.reshape(logits, [-1, self.event_size])
    samples = random_ops.multinomial(logits_2d, n, seed=seed)
    samples = array_ops.transpose(samples)
    samples = array_ops.one_hot(samples, self.event_size, dtype=self.dtype)
    ret = array_ops.reshape(samples, sample_shape)
    return ret

  def _log_prob(self, x):
    x = self._assert_valid_sample(x)
    # broadcast logits or x if need be.
    logits = self.logits
    if (not x.get_shape().is_fully_defined() or
        not logits.get_shape().is_fully_defined() or
        x.get_shape() != logits.get_shape()):
      logits = array_ops.ones_like(x, dtype=logits.dtype) * logits
      x = array_ops.ones_like(logits, dtype=x.dtype) * x

    logits_shape = array_ops.shape(logits)
    if logits.get_shape().ndims == 2:
      logits_2d = logits
      x_2d = x
    else:
      logits_2d = array_ops.reshape(logits, [-1, self.event_size])
      x_2d = array_ops.reshape(x, [-1, self.event_size])
    ret = -nn_ops.softmax_cross_entropy_with_logits(labels=x_2d,
                                                    logits=logits_2d)
    ret = array_ops.reshape(ret, logits_shape)
    return ret

  def _prob(self, x):
    return math_ops.exp(self._log_prob(x))

  def _entropy(self):
    if self.logits.get_shape().ndims == 2:
      logits_2d = self.logits
    else:
      logits_2d = array_ops.reshape(self.logits, [-1, self.event_size])
    histogram_2d = nn_ops.softmax(logits_2d)
    ret = array_ops.reshape(
        nn_ops.softmax_cross_entropy_with_logits(labels=histogram_2d,
                                                 logits=logits_2d),
        self.batch_shape())
    ret.set_shape(self.get_batch_shape())
    return ret

  def _mode(self):
    ret = math_ops.argmax(self.logits, axis=self._batch_rank)
    ret = array_ops.one_hot(ret, self.event_size, dtype=self.dtype)
    ret.set_shape(self.logits.get_shape())
    return ret

  def _assert_valid_sample(self, x):
    if not self.validate_args:
      return x
    return control_flow_ops.with_dependencies([
        check_ops.assert_non_positive(x),
        distribution_util.assert_close(
            array_ops.zeros((), dtype=self.dtype),
            math_ops.reduce_logsumexp(x, reduction_indices=[-1])),
    ], x)


@kullback_leibler.RegisterKL(_OneHotCategorical, _OneHotCategorical)
def _kl_categorical_categorical(a, b, name=None):
  """Calculate the batched KL divergence KL(a || b) with a, b OneHotCategorical.

  Args:
    a: instance of a OneHotCategorical distribution object.
    b: instance of a OneHotCategorical distribution object.
    name: (optional) Name to use for created operations.
      default is "kl_categorical_categorical".

  Returns:
    Batchwise KL(a || b)
  """
  with ops.name_scope(name, "kl_categorical_categorical", values=[
      a.logits, b.logits]):
    # sum(p ln(p / q))
    return math_ops.reduce_sum(
        nn_ops.softmax(a.logits) * (nn_ops.log_softmax(a.logits)
                                    - nn_ops.log_softmax(b.logits)),
        axis=-1)
