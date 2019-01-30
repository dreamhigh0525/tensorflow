# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
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
"""Various classes representing distributed inputs."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.data.experimental.ops import batching
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.data.ops import multi_device_iterator_ops
from tensorflow.python.distribute import device_util
from tensorflow.python.distribute import distribution_strategy_context
from tensorflow.python.distribute import input_ops
from tensorflow.python.distribute import values
from tensorflow.python.eager import context
from tensorflow.python.framework import device as tf_device
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_util


class InputWorkers(object):
  """A 1-to-many mapping from input worker devices to compute devices."""

  def __init__(self, device_map, worker_device_pairs=None, logical_device=0):
    """Initialize an `InputWorkers` object.

    Args:
      device_map: A `DeviceMap` with the computation devices fed by the
        input workers.
      worker_device_pairs: A sequence of pairs:
        `(input device, a tuple of compute devices fed by that input device)`.
      logical_device: The logical device of `device_map` to feed.
    """
    self._device_map = device_map
    self._logical_device = logical_device
    if worker_device_pairs is None:
      worker_device_pairs = ((
          device_util.canonicalize("/device:CPU:0"),
          device_map.logical_to_actual_devices(logical_device)),)
    self._input_worker_devices = tuple(d for d, _ in worker_device_pairs)
    self._fed_devices = tuple(tuple(device_util.canonicalize(d) for d in f)
                              for _, f in worker_device_pairs)
    flattened = tuple(d for l in self._fed_devices for d in l)
    assert (flattened ==
            device_map.logical_to_actual_devices(logical_device)), (
                "flattened: %s logical device %d: %s" %
                (flattened, logical_device,
                 device_map.logical_to_actual_devices(logical_device)))

  @property
  def device_map(self):
    return self._device_map

  @property
  def logical_device(self):
    return self._logical_device

  @property
  def num_workers(self):
    return len(self._input_worker_devices)

  @property
  def worker_devices(self):
    return self._input_worker_devices

  def compute_devices_for_worker(self, worker_index):
    return self._fed_devices[worker_index]

  def __repr__(self):
    devices = self.worker_devices
    debug_repr = ",\n".join("  %d %s: %s" %
                            (i, devices[i], self._fed_devices[i])
                            for i in range(len(devices)))
    return "%s:{\n%s\n  device_map: %s}" % (
        self.__class__.__name__, debug_repr, self._device_map)


class InputIterator(object):
  """An input iterator, intended to be passed to `DistributionStrategy.run`."""

  def get_next(self):
    """Returns the next inputs for all replicas."""
    raise NotImplementedError("must be implemented in descendants")

  def initialize(self):
    """Initialize the underlying input dataset, when applicable.

    In eager mode, this will create a new iterator and return it.
    In graph mode, this will initialize the same underlying iterator(s).

    Users are required to call this if
    - This iterator was returned from a call to `make_input_fn_iterator` with an
      input function that returns a dataset.
    - Or this iterator was returned from a call to `make_dataset_iterator`.

    Returns:
      A list of initialization ops to be executed.
    """
    raise NotImplementedError("must be implemented in descendants")


class InputIteratorImpl(InputIterator):
  """Common implementation for all input iterators."""

  def __init__(self, input_workers, iterators):
    assert isinstance(input_workers, InputWorkers)
    if not input_workers.worker_devices:
      raise ValueError("Should have at least one worker for input iterator.")

    self._iterators = iterators
    self._input_workers = input_workers

  def get_next(self, name=None):
    """Returns the next input from the iterator for all replicas."""
    replicas = []
    for i, worker in enumerate(self._input_workers.worker_devices):
      if name is not None:
        d = tf_device.DeviceSpec.from_string(worker)
        new_name = "%s_%s_%d" % (name, d.job, d.task)
      else:
        new_name = None
      with ops.device(worker):
        # Make `replicas` a flat list of values across all replicas.
        replicas.extend(self._iterators[i].get_next_as_list(new_name))

    return values.regroup(self._input_workers.device_map, replicas)

  def initialize(self):
    """Initialze underlying iterators.

    Returns:
      A list of any initializer ops that should be run.
    """
    init_ops = []
    for it in self._iterators:
      init_ops.extend(it.initialize())
    return init_ops

  # TODO(priyag): Remove when we switch to using `MultiDeviceIterator` for TPUs.
  @property
  def output_classes(self):
    return self._iterators[0].output_classes

  # TODO(priyag): Remove when we switch to using `MultiDeviceIterator` for TPUs.
  @property
  def output_shapes(self):
    return self._iterators[0].output_shapes

  # TODO(priyag): Remove when we switch to using `MultiDeviceIterator` for TPUs.
  @property
  def output_types(self):
    return self._iterators[0].output_types

  # TODO(priyag): Remove when we switch to using `MultiDeviceIterator` for TPUs.
  def get_iterator(self, worker):
    for i, w in enumerate(self._input_workers.worker_devices):
      if worker == w:
        return self._iterators[i]
    return None


class InputFunctionIterator(InputIteratorImpl):
  """Iterator created from input function."""

  def __init__(self, input_fn, input_workers, input_contexts):
    """Make an iterator for input provided via an input function.

    Currently implements PER_WORKER mode, in which the `input_fn` is called
    once on each worker.

    TODO(priyag): Add other replication modes.

    Args:
      input_fn: Input function that returns a `tf.data.Dataset` object.
      input_workers: an `InputWorkers` object.
      input_contexts: A list of `InputContext` instances to be passed to call(s)
        to `input_fn`. Length and order should match worker order in
        `worker_device_pairs`.
    """
    assert isinstance(input_workers, InputWorkers)
    if input_workers.num_workers != len(input_contexts):
      raise ValueError(
          "Number of input workers (%d) is not same as number of "
          "input_contexts (%d)" %
          (input_workers.num_workers, len(input_contexts)))

    iterators = []
    for i, ctx in enumerate(input_contexts):
      worker = input_workers.worker_devices[i]
      with ops.device(worker):
        result = input_fn(ctx)
        devices = input_workers.compute_devices_for_worker(i)
        if isinstance(result, dataset_ops.DatasetV2):
          iterator = _SingleWorkerDatasetIterator(result, worker, devices)
        elif callable(result):
          iterator = _SingleWorkerCallableIterator(result, worker, devices)
        else:
          raise ValueError(
              "input_fn must return a tf.data.Dataset or a callable.")
        iterators.append(iterator)

    super(InputFunctionIterator, self).__init__(input_workers, iterators)


class DatasetIterator(InputIteratorImpl):
  """Iterator created from input dataset."""

  def __init__(self, dataset, input_workers, split_batch_by=None):
    """Make an iterator for the dataset on given devices.

    If `split_batch_by` is not None, we "split" each batch of the
    dataset by `split_batch_by` value. To achieve this, we first unbatch the
    input dataset and then rebatch it with the per replica batch size that is
    calculated using `global_batch_size // split_batch_by`.
    The currently supported datasets are as follows:
    `dataset.batch()` is the last operation on the dataset OR
    `dataset.apply(map_and_batch)` is the last operation on the dataset OR
    `dataset.batch().prefetch()` are the last 2 operations on the dataset OR
    `dataset.apply(map_and_batch).prefetch()` are the last 2 operations.

    TODO(priyag): Support multi worker / host cases properly by cloning
    and sharding the dataset on each worker. Current setup will only work in
    some cases, such as in-graph multi worker GPU case. If the input pipeline
    has random shuffling (with a different seed on each worker), each worker
    will see random input from the same overall dataset in each step. Otherwise,
    each worker will see the same input in each step.

    Args:
      dataset: `tf.data.Dataset` that will be used as the input source.
      input_workers: an `InputWorkers` object.
      split_batch_by: Optional integer. If present, we "split" each batch of the
        dataset by `split_batch_by` value.
    """
    assert isinstance(input_workers, InputWorkers)
    if split_batch_by:
      dataset = _split_dataset_batch(dataset, split_batch_by)

    iterators = []
    for i, worker in enumerate(input_workers.worker_devices):
      with ops.device(worker):
        worker_devices = input_workers.compute_devices_for_worker(i)
        cloned_dataset = dataset
        if not context.executing_eagerly():
          cloned_dataset = input_ops._clone_dataset(dataset)  # pylint: disable=protected-access
        iterator = _SingleWorkerDatasetIterator(cloned_dataset, worker,
                                                worker_devices)
        iterators.append(iterator)

    super(DatasetIterator, self).__init__(input_workers, iterators)


class _SingleWorkerDatasetIterator(object):
  """Iterator for a single `tf.data.Dataset`."""

  def __init__(self, dataset, worker, devices):
    """Create iterator for the `dataset` to fetch data to worker's `devices` .

    `MultiDeviceIterator` is used to prefetch input to the devices on the
    given worker.

    Args:
      dataset: A `tf.data.Dataset` instance.
      worker: Worker on which ops should be created.
      devices: Distribute data from `dataset` to these devices.
    """
    self._dataset = dataset
    self._worker = worker
    self._devices = devices
    self._make_iterator()

  def _make_iterator(self):
    """Make appropriate iterator on the dataset."""
    with ops.device(self._worker):
      self._iterator = multi_device_iterator_ops.MultiDeviceIterator(
          self._dataset, self._devices)

  def get_next_as_list(self, name=None):
    """Get next element from the underlying iterator."""
    del name
    with ops.device(self._worker):
      data_list = self._iterator.get_next()
      return data_list

  def initialize(self):
    """Initialze underlying iterator.

    In eager execution, this simply recreates the underlying iterator.
    In graph execution, it returns the initializer ops for the underlying
    iterator.

    Returns:
      A list of any initializer ops that should be run.
    """
    if context.executing_eagerly():
      self._make_iterator()
      return []
    else:
      return [self._iterator.initializer]

  @property
  def output_classes(self):
    return self._iterator.output_classes

  @property
  def output_shapes(self):
    return self._iterator.output_shapes

  @property
  def output_types(self):
    return self._iterator.output_types


class _SingleWorkerCallableIterator(object):
  """Iterator for a single tensor-returning callable."""

  def __init__(self, fn, worker, devices):
    self._fn = fn
    self._worker = worker
    self._devices = devices

  def get_next_as_list(self, name=None):
    """Get next element from the callable."""
    del name
    with ops.device(self._worker):
      data_list = [self._fn() for _ in self._devices]
      return data_list

  def initialize(self):
    # TODO(petebu) Should this throw an exception instead?
    return []


# TODO(sourabhbajaj): Remove this in lieu of distributed datasets
def _get_batched_dataset(d):
  """Get the underlying batch dataset from the dataset object."""
  # pylint: disable=protected-access
  if isinstance(d, dataset_ops.DatasetV1Adapter):
    d = d._dataset

  if isinstance(d, (dataset_ops.BatchDataset, batching._MapAndBatchDataset)):
    return d
  elif isinstance(d, dataset_ops.PrefetchDataset):
    return _get_batched_dataset(d._input_dataset)

  raise ValueError(
      "Unable to get batched dataset from the input dataset. `batch` "
      "`map_and_batch` need to be the last operations on the dataset. "
      "The batch operations can be followed by a prefetch.")


def _get_batched_dataset_attributes(dataset):
  """Get `batch_size`, `drop_remainder`, and `prefetch_buffer` of dataset."""
  # pylint: disable=protected-access
  assert isinstance(dataset,
                    (dataset_ops.BatchDataset, batching._MapAndBatchDataset))
  if isinstance(dataset, dataset_ops.BatchDataset):
    batch_size = dataset._batch_size
    drop_remainder = dataset._drop_remainder
  elif isinstance(dataset, batching._MapAndBatchDataset):
    batch_size = dataset._batch_size_t
    drop_remainder = dataset._drop_remainder_t

  prefetch_buffer = None
  if isinstance(dataset, dataset_ops.PrefetchDataset):
    prefetch_buffer = dataset._buffer_size
  elif (isinstance(dataset, dataset_ops.DatasetV1Adapter)
        and isinstance(dataset._dataset, dataset_ops.PrefetchDataset)):
    prefetch_buffer = dataset._dataset._buffer_size
  # pylint: enable=protected-access

  if tensor_util.is_tensor(batch_size):
    batch_size = tensor_util.constant_value(batch_size)

  if tensor_util.is_tensor(drop_remainder):
    drop_remainder = tensor_util.constant_value(drop_remainder)

  return batch_size, drop_remainder, prefetch_buffer


def _split_dataset_batch(dataset, split_batch_by):
  """Divide a batch-ed dataset's batches into smaller batches."""
  batched_dataset = _get_batched_dataset(dataset)
  batch_size, drop_remainder, prefetch_buffer = (
      _get_batched_dataset_attributes(batched_dataset))

  if batch_size % split_batch_by:
    raise ValueError(
        "Batch size %s cannot be sharded evenly across replicas %s" % (
            batch_size, split_batch_by))
  new_batch_size = batch_size // split_batch_by

  dataset = dataset.apply(batching.unbatch())
  dataset = dataset.batch(new_batch_size, drop_remainder=drop_remainder)
  if prefetch_buffer is not None:
    dataset = dataset.prefetch(prefetch_buffer)
  return dataset


class MultiStepContext(object):
  """A context object that can be used to capture things when running steps.

  This context object is useful when running multiple steps at a time using the
  `experimental_run_steps_on_iterator` API. For e.g. it allows the user's step
  function to specify which outputs to emit at what frequency. Currently it
  supports capturing output from the last step, as well as capturing non tensor
  outputs.  In the future it will be augmented to support other use cases such
  as output each N steps.
  """

  def __init__(self):
    """Initialize an output context.

    Returns:
      A context object.
    """
    self._last_step_outputs = {}
    self._last_step_outputs_reduce_ops = {}
    self._non_tensor_outputs = {}

  @property
  def last_step_outputs(self):
    """A dictionary consisting of outputs to be captured on last step.

    Keys in the dictionary are names of tensors to be captured, as specified
    when `set_last_step_output` is called.
    Values in the dictionary are the tensors themselves. If
    `set_last_step_output` was called with a `reduce_op` for this output,
    then the value is the reduced value.

    Returns:
      A dictionary with last step outputs.
    """
    return self._last_step_outputs

  def _set_last_step_outputs(self, outputs):
    """Replace the entire dictionary of last step outputs."""
    if not isinstance(outputs, dict):
      raise ValueError("Need a dictionary to set last_step_outputs.")
    self._last_step_outputs = outputs

  def set_last_step_output(self, name, output, reduce_op=None):
    """Set `output` with `name` to be outputted from the last step.

    Args:
      name: String, name to identify the output. Doesn't need to match tensor
        name.
      output: The tensors that should be outputted with `name`. See below for
        actual types supported.
      reduce_op: Reduction method to use to reduce outputs from multiple
        replicas. Required if `set_last_step_output` is called in a replica
        context. Optional in cross_replica_context.
        When present, the outputs from all the replicas are reduced using the
        current distribution strategy's `reduce` method. Hence, the type of
        `output` must be what's supported by the corresponding `reduce` method.
        For e.g. if using MirroredStrategy and reduction is set, output
        must be a `PerReplica` value.
        The reduce method is also recorded in a dictionary
        `_last_step_outputs_reduce_ops` for later interpreting of the
        outputs as already reduced or not.
    """
    if distribution_strategy_context.in_cross_replica_context():
      self._last_step_outputs_reduce_ops[name] = reduce_op
      if reduce_op is None:
        self._last_step_outputs[name] = output
      else:
        distribution = distribution_strategy_context.get_strategy()
        self._last_step_outputs[name] = distribution.reduce(reduce_op, output)
    else:
      assert reduce_op is not None
      def merge_fn(distribution, value):
        self._last_step_outputs[name] = distribution.reduce(reduce_op, value)
        # Setting this inside the `merge_fn` because all replicas share the same
        # context object, so it's more robust to set it only once (even if all
        # the replicas are trying to set the same value).
        self._last_step_outputs_reduce_ops[name] = reduce_op

      distribution_strategy_context.get_replica_context().merge_call(
          merge_fn, args=(output,))

  @property
  def non_tensor_outputs(self):
    """A dictionary consisting of any non tensor outputs to be captured."""
    return self._non_tensor_outputs

  def set_non_tensor_output(self, name, output):
    """Set `output` with `name` to be captured as a non tensor output."""
    if distribution_strategy_context.in_cross_replica_context():
      self._non_tensor_outputs[name] = output
    else:
      def merge_fn(distribution, value):
        # NOTE(priyag): For non tensor outputs, we simply return all the values
        # in a list as reduction doesn't make sense on non tensors.
        self._non_tensor_outputs[name] = distribution.unwrap(value)
      distribution_strategy_context.get_replica_context().merge_call(
          merge_fn, args=(output,))
