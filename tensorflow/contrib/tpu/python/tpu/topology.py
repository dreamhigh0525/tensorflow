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
# ======================================
"""Defines the `Topology` class, that describes a TPU fabric topology."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np

from tensorflow.contrib.tpu.proto import topology_pb2


class Topology(object):
  """Describes a set of TPU devices.

  Represents both the shape of the physical mesh, and the mapping between
  TensorFlow TPU devices to physical mesh coordinates.
  """

  def __init__(self, serialized=None, mesh_shape=None, device_coordinates=None):
    """Builds a Topology object.

    If `serialized` is not `None`, the topology is parsed from `serialized` and
    the other arguments are ignored. Otherwise, the topology is computed from
    `mesh_shape` and `device_coordinates`.

    Args:
      serialized: A serialized `TopologyProto`, or `None`. If not `None`, the
        serialized proto is parsed to discover the topology.
      mesh_shape: A sequence of 3 positive integers, or `None`. If not `None`,
        the shape of the TPU topology, in number of cores. Ignored if
        `serialized` is not `None`.
      device_coordinates: A rank 3 numpy array that describes the mapping from
        TensorFlow TPU devices to TPU fabric coordinates, or `None`. Ignored
        if `serialized is not `None`.

    Raises:
      ValueError: If `serialized` does not describe a well-formed topology.
      ValueError: If `serialized` is `None` and `mesh_shape` is not a sequence
        of 3 positive integers.
      ValueError: If `serialized` is `None` and `device_coordinates` is not a
        rank 3 numpy int32 array that describes a valid coordinate mapping.
    """

    if serialized:
      self._serialized = serialized
      self._parse_topology(serialized)
    else:
      self._mesh_shape = np.asarray(mesh_shape, dtype=np.int32)
      self._device_coordinates = np.asarray(device_coordinates, np.int32)
      if len(self._mesh_shape) != 3 or any(self._mesh_shape < 1):
        raise ValueError("`mesh_shape` must be a sequence of 3 positive "
                         "entries; got {}".format(self._mesh_shape))

      if (len(self._device_coordinates.shape) != 3 or
          self._device_coordinates.shape[2] != len(self._mesh_shape)):
        raise ValueError("`device_coordinates` must be a rank 3 int32 array "
                         "with minor dimension equal to the mesh shape rank")

  def _parse_topology(self, serialized):
    """Parses a serialized `TopologyProto` into `self`."""
    proto = topology_pb2.TopologyProto()
    proto.ParseFromString(serialized)

    self._mesh_shape = np.array(proto.mesh_shape, dtype=np.int32)
    if len(self._mesh_shape) != 3 or any(self._mesh_shape < 1):
      raise ValueError("`mesh_shape` must be a vector of size 3 with positive "
                       "entries; got {}".format(self._mesh_shape))

    if proto.num_tasks < 0:
      raise ValueError("`num_tasks` must be >= 0; got {}".format(
          proto.num_tasks))
    if proto.num_tpu_devices_per_task < 0:
      raise ValueError("`num_tpu_devices_per_task` must be >= 0; got {}".format(
          proto.num_tpu_devices_per_task))

    expected_coordinates_size = (
        proto.num_tasks * proto.num_tpu_devices_per_task * len(
            proto.mesh_shape))
    if len(proto.device_coordinates) != expected_coordinates_size:
      raise ValueError("`device_coordinates` must have shape num_tasks ({}) * "
                       "num_tpu_devices_per_task ({}) * len(mesh_shape) ({}); "
                       "got shape {}".format(proto.num_tasks,
                                             proto.num_tpu_devices_per_task,
                                             proto.mesh_shape,
                                             len(proto.device_coordinates)))

    coords = np.array(proto.device_coordinates, dtype=np.int32)
    if any(coords < 0):
      raise ValueError("`device_coordinates` must be >= 0")
    coords = coords.reshape((proto.num_tasks, proto.num_tpu_devices_per_task,
                             len(proto.mesh_shape)))
    self._device_coordinates = coords

  @property
  def mesh_shape(self):
    """A rank 1 int32 array describing the shape of the TPU topology."""
    return self._mesh_shape

  @property
  def device_coordinates(self):
    """Describes the mapping from TPU devices to topology coordinates.

    Returns:
      A rank 3 int32 array with shape `[tasks, devices, axis]`.
      `tasks` is the number of tasks in the TPU cluster, `devices` is the number
      of TPU devices per task, and `axis` is the number of axes in the TPU
      cluster topology. Each entry gives the `axis`-th coordinate in the
      topology of a task/device pair. TPU topologies are 3-dimensional, with
      dimensions `(x, y, core number)`.
    """
    return self._device_coordinates

  def serialized(self):
    """Returns the serialized form of the topology."""
    if self._serialized is None:
      proto = topology_pb2.TopologyProto()
      proto.mesh_shape[:] = list(self._mesh_shape)
      proto.num_tasks = self._device_coordinates.shape[0]
      proto.num_tpu_devices_per_task = self._device_coordinates.shape[1]
      proto.device_coordinates = list(self._device_coordinates.flatten())
      self._serialized = proto.SerializeToString()

    return self._serialized
