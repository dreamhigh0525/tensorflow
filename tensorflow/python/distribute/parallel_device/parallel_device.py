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
"""Utility for eagerly executing operations in parallel on multiple devices."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import threading

from tensorflow.python import _pywrap_parallel_device
from tensorflow.python.distribute import device_util
from tensorflow.python.distribute.parallel_device import gen_parallel_device_ops
from tensorflow.python.distribute.parallel_device import saving
from tensorflow.python.eager import context
from tensorflow.python.framework import load_library
from tensorflow.python.framework import ops
from tensorflow.python.platform import resource_loader
from tensorflow.python.tpu.ops import tpu_ops

load_library.load_op_library(
    resource_loader.get_path_to_datafile("_parallel_device_ops.so"))

_next_device_number = 0
_next_device_number_lock = threading.Lock()


# TODO(allenl): Expand this docstring once things like getting components on and
# off the device are stable.
class ParallelDevice(object):
  """A device which executes operations in parallel."""

  def __init__(self, components):
    """Creates a device which executes operations in parallel on `components`.

    Args:
      components: A list of device names. Each operation executed on the
        returned device executes on these component devices.

    Returns:
      A string with the name of the newly created device.
    """
    global _next_device_number, _next_device_number_lock
    self.components = tuple(device_util.canonicalize(d) for d in components)
    ctx = context.context()
    with _next_device_number_lock:
      # TODO(allenl): Better names for parallel devices (right now "CUSTOM" is
      # special-cased).
      self._name = "{}/device:CUSTOM:{}".format(ctx.host_address_space(),
                                                _next_device_number)
      _next_device_number += 1
    device, device_info = _pywrap_parallel_device.GetParallelDeviceCapsules(
        self._name, self.components)
    context.register_custom_device(device, self._name, device_info)
    with ops.device(self._name):
      self._device_ids = gen_parallel_device_ops.device_id()
    self._device_scope = None
    self._saving_scope = None

  def pack(self, tensors):
    """Create a tensor on the parallel device from a sequence of tensors.

    Args:
      tensors: A flat list of tensors, one per device in `self.components`.

    Returns:
      A single tensor placed on the ParallelDevice.
    """
    self._assert_eager()
    with ops.device(self._name):
      return tpu_ops.tpu_replicated_input(inputs=tensors)

  def unpack(self, parallel_tensor):
    """Unpack a parallel tensor into its components.

    Args:
      parallel_tensor: A tensor placed on the ParallelDevice.

    Returns:
      A flat list of tensors, one per `self.components`.
    """
    self._assert_eager()
    with ops.device(self._name):
      return tpu_ops.tpu_replicated_output(
          parallel_tensor, num_replicas=len(self.components))

  @property
  def device_ids(self):
    """A parallel tensor with scalar integers numbering component devices.

    Each device ID is placed on its corresponding device, in the same order as
    the `components` constructor argument.

    Returns:
      A parallel tensor containing 0 on the first device, 1 on the second, etc.
    """
    return self._device_ids

  def _assert_eager(self):
    """Verifies that tracing is not active."""
    if not context.executing_eagerly():
      raise NotImplementedError(
          "ParallelDevice is currently not supported inside `tf.function`. It "
          "can however run calls to a `tf.function` in parallel:\n\n"
          "with ParallelDevice() as p:\n  f()")

  def __enter__(self):
    """Runs ops in parallel, makes variables which save independent buffers."""
    if (self._device_scope is not None or self._saving_scope is not None):
      raise AssertionError(
          "Re-entered a ParallelDevice scope without first exiting it.")
    self._assert_eager()
    self._device_scope = ops.device(self._name)
    self._saving_scope = saving.independent_buffers(self)
    self._device_scope.__enter__()
    # TODO(allenl): Fixing saving in Python is a bit odd. One alternative would
    # be to provide a hook for the custom device to create save specs/etc., then
    # call that hook from the default variable implementation if the variable is
    # on a custom device. We'll likely want similar hooks for repr() and such.
    self._saving_scope.__enter__()
    return self

  def __exit__(self, typ, exc, tb):
    self._device_scope.__exit__(typ, exc, tb)
    self._saving_scope.__exit__(typ, exc, tb)
    self._device_scope = None
    self._saving_scope = None
