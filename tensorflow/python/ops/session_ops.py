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
"""## Tensor Handle Operations.

TensorFlow provides several operators that allows the user to keep tensors
"in-place" across run calls.

@@get_session_handle
@@get_session_tensor
@@delete_session_tensor
"""

# pylint: disable=g-bad-name
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.framework import device as pydev
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gen_data_flow_ops
from tensorflow.python.util import compat


class TensorHandle(object):
  """Represents a handle for a live tensor in a session."""

  def __init__(self, handle, dtype, session):
    """Constructs a new tensor handle.

    A tensor handle for a persistent tensor is a python string
    that has the form of "tensor_name;unique_id;device_name".

    Args:
      handle: A tensor handle.
      dtype: The data type of the tensor represented by `handle`.
      session: The session in which the tensor is produced.
    """
    self._handle = compat.as_str_any(handle)
    self._dtype = dtype
    self._session = session
    self._auto_gc_enabled = True

  def __del__(self):
    if self._auto_gc_enabled:
      self._session._register_dead_handle(self.handle)

  def __str__(self):
    return self._handle

  @property
  def handle(self):
    return self._handle

  def eval(self):
    """Return the value of the tensor represented by this handle."""
    holder, reader = _get_handle_reader(self._session.graph, self._handle,
                                        self._dtype)
    return self._session.run(reader, feed_dict={holder: self._handle})

  def get_raw_handle(self):
    """Return the raw handle of the tensor.

    Note that the method disables the automatic garbage collection of this
    persistent tensor. The caller is now responsible for managing the life
    time of the tensor.
    """
    self._auto_gc_enabled = False
    return self._handle

  @staticmethod
  def _get_device_name(handle):
    """The device name encoded in the handle."""
    handle_str = compat.as_str_any(handle)
    return pydev.canonical_name(handle_str.split(';')[-1])

  @staticmethod
  def _get_reader_key(handle):
    """The graph key for reader."""
    handle_parts = str(handle).split(';')
    return handle_parts[0] + ';' + handle_parts[-1]

  @staticmethod
  def _get_deleter_key(handle):
    """The graph key for deleter."""
    return str(handle).split(';')[-1]

  @staticmethod
  def _get_mover_key(feeder, handle):
    """The graph key for mover."""
    return feeder.op.name + ';' + TensorHandle._get_reader_key(handle)


def get_session_handle(data, name=None):
  """Return the handle of `data`.

  This is EXPERIMENTAL and subject to change.

  Keep `data` "in-place" in the runtime and create a handle that can be
  used to retrieve `data` in a subsequent run().

  Combined with `get_session_tensor`, we can keep a tensor produced in
  one run call in place, and use it as the input in a future run call.
  Below is a simple example:

  ```python
  c = tf.mul(a, b)
  h = tf.get_session_handle(c)
  h = sess.run(h)

  p, a = tf.get_session_tensor(tf.float32)
  b = tf.mul(a, 10)
  c = sess.run(b, feed_dict={p: h.handle})
  ```

  Args:
    data: A tensor to be stored in the session.
    name: Optional name prefix for the return tensor.

  Returns:
    A scalar string tensor representing a unique handle for `data`.

  Raises:
    TypeError: if `data` is not a Tensor.
  """
  if not isinstance(data, ops.Tensor):
    raise TypeError('`data` must be of type Tensor.')

  # Colocate this operation with data.
  with ops.colocate_with(data):
    return gen_data_flow_ops._get_session_handle(data, name=name)


def get_session_tensor(dtype, name=None):
  """Get the tensor of type `dtype` by feeding a tensor handle.

  This is EXPERIMENTAL and subject to change.

  Get the value of the tensor from a tensor handle. The tensor
  is produced in a previous run() and stored in the state of the
  session.

  Args:
    dtype: The type of the output tensor.
    name: Optional name prefix for the return tensor.

  Returns:
    A pair of tensors. The first is a placeholder for feeding a
    tensor handle and the second is the tensor in the session state
    keyed by the tensor handle.
  """
  with ops.device(None):
    # Commit the device when it is used the first time.
    holder = array_ops.placeholder(dtypes.string)
    _register_handle_feeder(holder.graph, holder, dtype)
    tensor = gen_data_flow_ops._get_session_tensor(holder, dtype, name=name)
  return (holder, tensor)


def delete_session_tensor(name=None):
  """Delete the tensor by feeding a tensor handle.

  This is EXPERIMENTAL and subject to change.

  Delete the tensor of a given tensor handle. The tensor is produced
  in a previous run() and stored in the state of the session.

  Args:
    name: Optional name prefix for the return tensor.

  Returns:
    A pair of graph elements. The first is a placeholder for feeding a
    tensor handle and the second is a deletion operation.
  """
  with ops.device(None):
    # We will commit the device at the time it is used.
    holder = array_ops.placeholder(dtypes.string)
  deleter = gen_data_flow_ops._delete_session_tensor(holder, name=name)
  return (holder, deleter)


def _register_handle_feeder(graph, feeder, dtype):
  graph._handle_feeders[feeder.op.name] = dtype


def _get_handle_feeder(graph, feeder):
  return graph._handle_feeders.get(feeder.op.name)


def _get_handle_reader(graph, handle, dtype):
  """Return a read subgraph for this handle."""
  graph_key = TensorHandle._get_reader_key(handle)
  result = graph._handle_readers.get(graph_key)
  if result is None:
    # Create reader if we haven't done it.
    handle_device = TensorHandle._get_device_name(handle)
    with graph.as_default(), graph.device(handle_device):
      holder = array_ops.placeholder(dtypes.string)
      _register_handle_feeder(holder.graph, holder, dtype)
      reader = gen_data_flow_ops._get_session_tensor(holder, dtype)
    result = (holder, reader)
    graph._handle_readers[graph_key] = result
  return result


def _get_handle_mover(graph, feeder, handle):
  """Return a move subgraph for this pair of feeder and handle."""
  dtype = _get_handle_feeder(graph, feeder)
  if dtype is None:
    return None
  handle_device = TensorHandle._get_device_name(handle)
  if not feeder.op.device:
    feeder.op._set_device(handle_device)
    feeder.consumers()[0]._set_device(handle_device)
    return None
  if feeder.op.device == handle_device:
    return None
  # Now we know we have to move the tensor.
  graph_key = TensorHandle._get_mover_key(feeder, handle)
  result = graph._handle_movers.get(graph_key)
  if result is None:
    # Create mover if we haven't done it.
    holder, reader = _get_handle_reader(graph, handle, dtype)
    with graph.as_default(), graph.device(feeder.op.device):
      mover = gen_data_flow_ops._get_session_handle(reader)
    result = (holder, mover)
    graph._handle_movers[graph_key] = result
  return result


def _get_handle_deleter(graph, handle):
  """Return a deletion subgraph for this handle."""
  graph_key = TensorHandle._get_deleter_key(handle)
  result = graph._handle_deleters.get(graph_key)
  if result is None:
    # Create deleter if we haven't done it.
    handle_device = TensorHandle._get_device_name(handle)
    with graph.as_default(), graph.device(handle_device):
      holder = array_ops.placeholder(dtypes.string)
      deleter = gen_data_flow_ops._delete_session_tensor(holder)
    result = (holder, deleter)
    graph._handle_deleters[graph_key] = result
  return result
