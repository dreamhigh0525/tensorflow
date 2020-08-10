# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
"""Optimizer utilities."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.distribute import distribution_strategy_context as distribute_ctx
from tensorflow.python.distribute import reduce_util as ds_reduce_util
from tensorflow.python.platform import tf_logging as logging


def all_reduce_sum_gradients(grads_and_vars):
  """Returns all-reduced gradients aggregated via summation.

  Args:
    grads_and_vars: List of (gradient, variable) pairs.

  Returns:
    A list of all-reduced gradients.
  """
  grads_and_vars = list(grads_and_vars)
  filtered_grads_and_vars = filter_empty_gradients(grads_and_vars)
  # We switch to a cross-replica context since there is a bug which causes
  # IndexedSlices to be converted to dense tensors when all-reduced in a
  # replica context.
  # TODO(b/150507409): Do not switch to a cross-replica context once the bug
  # is fixed.
  if filtered_grads_and_vars:
    reduced = distribute_ctx.get_replica_context().merge_call(
        _all_reduce_sum_fn, args=(filtered_grads_and_vars,))
  else:
    reduced = []
  # Copy 'reduced' but add None gradients back in
  reduced_with_nones = []
  reduced_pos = 0
  for g, _ in grads_and_vars:
    if g is None:
      reduced_with_nones.append(None)
    else:
      reduced_with_nones.append(reduced[reduced_pos])
      reduced_pos += 1
  assert reduced_pos == len(reduced), "Failed to add all gradients"
  return reduced_with_nones


def filter_empty_gradients(grads_and_vars):
  """Filter out `(grad, var)` pairs that have a gradient equal to `None`."""
  grads_and_vars = tuple(grads_and_vars)
  if not grads_and_vars:
    return grads_and_vars

  filtered = []
  vars_with_empty_grads = []
  for grad, var in grads_and_vars:
    if grad is None:
      vars_with_empty_grads.append(var)
    else:
      filtered.append((grad, var))
  filtered = tuple(filtered)

  if not filtered:
    raise ValueError("No gradients provided for any variable: %s." %
                     ([v.name for _, v in grads_and_vars],))
  if vars_with_empty_grads:
    logging.warning(
        ("Gradients do not exist for variables %s when minimizing the loss."),
        ([v.name for v in vars_with_empty_grads]))
  return filtered


def _all_reduce_sum_fn(distribution, grads_and_vars):
  return distribution.extended.batch_reduce_to(ds_reduce_util.ReduceOp.SUM,
                                               grads_and_vars)
