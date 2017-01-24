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

"""Subscribe function."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import re

from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops


def _recursive_apply(tensors, apply_fn):
  """Helper method to recursively apply a function to structure of tensors.

  The structure of the tensors should take the form similar to fetches in
  `tf.Session` and includes single `Tensor`, `list`, nested `list`, `tuple`,
  `namedtuple`, or `dict`.

  Args:
    tensors: Single `Tensor`, `list`, nested `list, `tuple`,
      `namedtuple`, or `dict`.
    apply_fn: Function to apply to each `Tensor` and should return a `Tensor`.
  Returns:
    Returns the modified tensors with the same structure.
  Raises:
    `TypeError` if undefined type in the tensors structure.
  """
  tensors_type = type(tensors)
  if tensors_type is ops.Tensor:
    return apply_fn(tensors)
  elif isinstance(tensors, (list, tuple)):
    tensors = [_recursive_apply(t, apply_fn) for t in tensors]
    if tensors_type is list:
      return list(tensors)
    elif tensors_type is tuple:
      return tuple(tensors)
    return tensors_type(*tensors)  # collections.namedtuple
  elif tensors_type is dict:
    return dict([(k, _recursive_apply(v, apply_fn))
                 for k, v in tensors.iteritems()])
  else:
    raise TypeError('_recursive_apply argument %r has invalid type %r' %
                    (tensors, tensors_type))


class _ControlOutputCache(object):
  """Helper class to manage calculating and caching control_outputs in graph."""

  def __init__(self):
    self.cache = {}

  def calc_control_outputs(self, graph):
    """Returns the map of control_outputs for a given graph.

    Args:
      graph: The graph to parse.
    Returns:
      A map of the control outputs.
    """
    control_outputs = {}
    for op in graph.get_operations():
      for control_input in op.control_inputs:
        if control_input not in control_outputs:
          control_outputs[control_input] = set()
        control_outputs[control_input].add(op)
    return control_outputs

  def get_control_outputs(self, op):
    """Return the control outputs for a given op.

    Args:
      op: The op to fetch control outputs for.
    Returns:
      Iterable of control output ops.
    """
    if op.graph not in self.cache:
      control_outputs = self.calc_control_outputs(op.graph)
      self.cache[op.graph] = control_outputs
    else:
      control_outputs = self.cache[op.graph]
    return control_outputs.get(op, [])


def _subscribe_new(tensor, side_effects, control_cache):
  """Helper method that subscribes a single tensor to a list of side_effects.

  Args:
    tensor: `tf.Tensor`
    side_effects: List of side_effect functions see subscribe for details.
    control_cache: `_ControlOutputCache` helper to get control_outputs faster.
  Returns:
    The modified replacement to the passed in tensor which triggers the side
    effects.
  """
  update_input = []
  for consumer_op in list(tensor.consumers()):  # explicit copy
    update_input.append((consumer_op, list(consumer_op.inputs).index(tensor)))

  update_control_input = control_cache.get_control_outputs(tensor.op)

  # Trailing slash on name scope to replace the scope.
  name_scope = tensor.op.name + '/subscription/'
  with ops.name_scope(name_scope):
    outs = []
    for s in side_effects:
      outs += s(tensor)

    with ops.control_dependencies(outs):
      out = array_ops.identity(tensor)

  for consumer_op, index in update_input:
    consumer_op._update_input(index, out)  # pylint: disable=protected-access

  for consumer_op in update_control_input:
    consumer_op._control_inputs.remove(tensor.op)  # pylint: disable=protected-access
    consumer_op._control_inputs.append(out.op)  # pylint: disable=protected-access
    consumer_op._recompute_node_def()  # pylint: disable=protected-access

  return out


def _subscribe_extend(tensor, side_effects):
  """Helper method to extend the list of side_effects for a subscribed tensor.

  Args:
    tensor: A `tf.Tensor` as returned by subscribe().
    side_effects: List of side_effect functions, see subscribe for details.
  Returns:
    The given subscribed tensor (for API consistency).
  """
  assert len(tensor.op.inputs) == 1, 'Op {} must only have one input'.format(
      tensor.op.name)
  source_tensor = tensor.op.inputs[0]

  # Build the side effect graphs and add their outputs to the list of control
  # dependencies for the subscribed tensor.
  outs = []
  name_scope = source_tensor.op.name + '/subscription/'
  with ops.name_scope(name_scope):
    for s in side_effects:
      outs += s(source_tensor)

  for out in outs:
    out_type = type(out)
    if out_type is ops.Tensor:
      out = out.op
    tensor.op._control_inputs.append(out)  # pylint: disable=protected-access
  tensor.op._recompute_node_def()  # pylint: disable=protected-access

  return tensor


def _is_subscribed_identity(tensor):
  """Checks if the given tensor is an identity op returned by `subscribe()`.

  Args:
    tensor: A `tf.Tensor` to check.
  Returns:
    True if the given tensor matches the criteria for subscription identies:
    its op type is `Identity`, its name matches the name of its input and
    conforms to the convention for subscribed nodes.
    False otherwise.
  """
  # Subscribed tensor are assumed to be identity ops.
  if tensor.op.type != 'Identity':
    return False

  # Check that the tensor name matches the convention in place for identity ops
  # created by subscribe().
  match = re.match(
      r'(?P<prefix_name>^.*?)/subscription/Identity[^/]+', tensor.name)
  if match is None or len(match.groups()) != 1:
    return False
  prefix_name = match.group('prefix_name')

  # Get a reference to the source tensor and check that it has a matching name.
  assert len(tensor.op.inputs) == 1, 'Op {} must only have one input'.format(
      tensor.op.name)
  source_tensor = tensor.op.inputs[0]
  if prefix_name != source_tensor.op.name:
    return False

  return True


def _subscribe(tensor, side_effects, control_cache):
  """Helper method that subscribes a single tensor to a list of side_effects.

  This method will check if the given tensor has already been subscribed or if
  it's a tensor returned by a previous call to `subscribe()` and, if so, will
  reuse the existing identity op, appending the given side effects to the list
  of existing ones.

  Args:
    tensor: The `tf.Tensor` to be subscribed.
    side_effects: List of side_effect functions, see subscribe for details.
    control_cache: `_ControlOutputCache` helper to get control_outputs faster.
  Returns:
    The modified replacement to the passed in tensor which triggers the side
    effects or the given tensor, if it was already been subscribed.
  """

  if _is_subscribed_identity(tensor):
    return _subscribe_extend(tensor, side_effects)

  # Check if the given tensor has already been subscribed by inspecting its
  # outputs.
  name_scope = tensor.op.name + '/subscription/Identity'
  consumers = tensor.consumers()
  matching_ops = [op for op in consumers if op.name.startswith(name_scope)]
  assert len(matching_ops) <= 1, ('Op {} must only have one subscription '
                                  'op connected to it').format(tensor.op.name)
  if len(matching_ops) == 1:
    candidate_tensor = matching_ops[0].outputs[0]
    if _is_subscribed_identity(candidate_tensor):
      return _subscribe_extend(candidate_tensor, side_effects)

  return _subscribe_new(tensor, side_effects, control_cache)


def subscribe(tensors, side_effects):
  """Subscribe to a tensor.

  This method will attach side effect graphs to a given set
  of tensors. Set of tensors follows from session.run and supports
  single `Tensor`, `list`, nested `list`, `tuple`, `namedtuple`, or `dict`. It
  returns the tensors in the same passed in structure, but as clones with
  side effects applied. The supplied side effect graphs are specified
  as a constructor function which takes the target tensor and
  constructs a side effect graph and returns a list of ops that should
  be control dependencies on fetching the tensor. It will append
  'subscription' to the name scope of the tensor for every node in
  the side effect graph. These control dependencies are what trigger
  the side effects. Subscribe will construct the additions to your
  graph and return the created identity tensor downstream of the control
  dependencies. Use these tensors as you would normally in the rest of
  your tensorflow code. If a given tensor has already been subscribed or a
  tensor returned by a call to subscribe is passed, the previously created
  identity tensor will be reused and the side effect graphs will be added to
  the existing ones.

  Args:
    tensors: `Tensor` or set of tensors to subscribe to. Set of tensors format
      follows from `Session.run` and supports single `Tensor`, `list`, nested
      `list`, `tuple`, `namedtuple`, or `dict`.
    side_effects: Function(s) that takes a `Tensor`, construct a subgraph, and
      return a nonempty list of control dependencies. This can be a single
      function or list of functions.
  Returns:
    Subscribed tensors, which are identity copies of the passed in tensors
      in the same passed in structure, but the graph has been modified
      such that these are downstream of the control dependencies for
      the side effect graphs. Use these functionally equivelant tensors
      instead of the passed in tensors for further construction or running.
  """
  if not hasattr(side_effects, '__iter__'):
    side_effects = [side_effects]

  control_outputs = _ControlOutputCache()
  result = _recursive_apply(
      tensors, lambda t: _subscribe(t, side_effects, control_outputs))
  return result
