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

"""Strategies for placing variables on parameter servers.
"""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np


class GreedyLoadBalancingStrategy(object):
  """Returns the least-loaded ps task for op placement.

  The load is calculated by a user-specified load function passed in at
  construction.  There are no units for load, and the load function is
  responsible for providing an internally consistent measure.

  Note that this strategy is very sensitive to the exact order in which
  ps ops (typically variables) are created, as it greedily places ops
  on the least-loaded ps at the point each op is processed.

  One reasonable heuristic is the `variable_size_load_fn`, which
  estimates load as the number of bytes that would be used to store and
  transmit the entire variable.  More advanced load functions
  could consider the difference in access patterns across ops, or trade
  off CPU-intensive ops with RAM-intensive ops with network bandwidth.

  This class is intended to be used as a `ps_strategy` in
  `tf.replica_device_setter`.
  """

  def __init__(self, num_tasks, load_fn):
    """Create a new `LoadBalancingStrategy`.

    Args:
      num_tasks: Number of ps tasks to cycle among.
      load_fn: A callable that takes an `Operation` and returns a
        numeric load value for that op.
    """
    self._num_tasks = num_tasks
    self._load_fn = load_fn
    self._ps_loads = np.zeros(num_tasks)

  def __call__(self, op):
    """Choose a ps task index for the given `Operation`.

    Args:
      op: A `Operation` to be placed on ps.

    Returns:
      The next ps task index to use for the `Operation`. Greedily
      places the op on the least-loaded ps task so far, as determined
      by the load function.
    """
    task = np.argmin(self._ps_loads)
    self._ps_loads[task] += self._load_fn(op)
    return task


def byte_size_load_fn(op):
  """Load function that computes the byte size of a single-output `Operation`.

  This is intended to be used with `"Variable"` ops, which have a single
  `Tensor` output with the contents of the variable.  However, it can also be
  used for calculating the size of any op that has a single output.

  Intended to be used with `GreedyLoadBalancingStrategy`.

  Args:
    op: An `Operation` with a single output, typically a "Variable" op.

  Returns:
    The number of bytes in the output `Tensor`.

  Raises:
    ValueError: if `op` does not have a single output, or if the shape of the
      single output is not fully-defined.
  """
  if len(op.outputs) != 1:
    raise ValueError("Op %s must have a single output" % op)
  output = op.outputs[0]
  output.get_shape().assert_is_fully_defined()
  num_elems = output.get_shape().num_elements()
  elem_size = output.dtype.size
  return num_elems * elem_size
