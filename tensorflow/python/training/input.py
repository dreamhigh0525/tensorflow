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

"""Input pipeline.

Please see the [reading data how-to](../../how_tos/reading_data/index.md)
for context.
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections

from six.moves import xrange  # pylint: disable=redefined-builtin

from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import tensor_shape
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import constant_op
from tensorflow.python.ops import data_flow_ops
from tensorflow.python.ops import io_ops
from tensorflow.python.ops import logging_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import random_ops
from tensorflow.python.ops import sparse_ops
from tensorflow.python.ops import variables
from tensorflow.python.training import queue_runner


def match_filenames_once(pattern, name=None):
  """Save the list of files matching pattern, so it is only computed once.

  Args:
    pattern: A file pattern (glob).
    name: A name for the operations (optional).

  Returns:
    A variable that is initialized to the list of files matching pattern.
  """
  with ops.op_scope([pattern], name, "matching_filenames") as name:
    return variables.Variable(io_ops.matching_files(pattern), trainable=False,
                              name=name, validate_shape=False)


def limit_epochs(tensor, num_epochs=None, name=None):
  """Returns tensor `num_epochs` times and then raises an `OutOfRange` error.

  Args:
    tensor: Any `Tensor`.
    num_epochs: A positive integer (optional).  If specified, limits the number
      of steps the output tensor may be evaluated.
    name: A name for the operations (optional).

  Returns:
    tensor or `OutOfRange`.

  Raises:
    ValueError: if `num_epochs` is invalid.
  """
  if num_epochs is None:
    return tensor
  if num_epochs <= 0:
    raise ValueError("num_epochs must be > 0 not %d." % num_epochs)
  with ops.op_scope([tensor], name, "limit_epochs") as name:
    zero64 = constant_op.constant(0, dtype=dtypes.int64)
    epochs = variables.Variable(zero64, name="epochs", trainable=False)
    counter = epochs.count_up_to(num_epochs)
    with ops.control_dependencies([counter]):
      return array_ops.identity(tensor, name=name)


def input_producer(input_tensor, element_shape=None, num_epochs=None,
                   shuffle=True, seed=None, capacity=32, shared_name=None,
                   summary_name=None, name=None):
  """Output the rows of `input_tensor` to a queue for an input pipeline.

  Args:
    input_tensor: A tensor with the rows to produce. Must be at
      one-dimensional. Must either have a fully-defined shape, or
      `element_shape` must be defined.
    element_shape: (Optional.) A `TensorShape` representing the shape of a
      row of `input_tensor`, if it cannot be inferred.
    num_epochs: (Optional.) An integer. If specified `input_producer` produces
      each row of `input_tensor` `num_epochs` times before generating an
      `OutOfRange` error. If not specified, `input_producer` can cycle through
      the rows of `input_tensor` an unlimited number of times.
    shuffle: (Optional.) A boolean. If true, the rows are randomly shuffled
      within each eopch.
    seed: (Optional.) An integer. The seed to use if `shuffle` is true.
    capacity: (Optional.) The capacity of the queue to be used for buffering
      the input.
    shared_name: (Optional.) If set, this queue will be shared under the given
      name across multiple sessions.
    summary_name: (Optional.) If set, a scalar summary for the current queue
      size will be generated, using this name as part of the tag.
    name: (Optional.) A name for queue.

  Returns:
    A queue with the output rows.  A `QueueRunner` for the queue is
    added to the current `QUEUE_RUNNER` collection of the current
    graph.

  Raises:
    ValueError: If the shape of the input cannot be inferred from the arguments.
  """
  with ops.op_scope([input_tensor], name, "input_producer"):
    input_tensor = ops.convert_to_tensor(input_tensor, name="input_tensor")
    element_shape = input_tensor.get_shape()[1:].merge_with(element_shape)
    if not element_shape.is_fully_defined():
      raise ValueError("Either `input_tensor` must have a fully defined shape "
                       "or `element_shape` must be specified")

    if shuffle:
      input_tensor = random_ops.random_shuffle(input_tensor, seed=seed)

    input_tensor = limit_epochs(input_tensor, num_epochs)

    q = data_flow_ops.FIFOQueue(capacity=capacity,
                                dtypes=[input_tensor.dtype.base_dtype],
                                shapes=[element_shape],
                                shared_name=shared_name, name=name)
    enq = q.enqueue_many([input_tensor])
    queue_runner.add_queue_runner(queue_runner.QueueRunner(q, [enq]))
    if summary_name is not None:
      logging_ops.scalar_summary("queue/%s/%s" % (q.name, summary_name),
                                 math_ops.cast(q.size(), dtypes.float32) *
                                 (1. / capacity))
    return q


def string_input_producer(string_tensor, num_epochs=None, shuffle=True,
                          seed=None, capacity=32, shared_name=None, name=None):
  """Output strings (e.g. filenames) to a queue for an input pipeline.

  Args:
    string_tensor: A 1-D string tensor with the strings to produce.
    num_epochs: An integer (optional). If specified, `string_input_producer`
      produces each string from `string_tensor` `num_epochs` times before
      generating an `OutOfRange` error. If not specified,
      `string_input_producer` can cycle through the strings in `string_tensor`
      an unlimited number of times.
    shuffle: Boolean. If true, the strings are randomly shuffled within each
      epoch.
    seed: An integer (optional). Seed used if shuffle == True.
    capacity: An integer. Sets the queue capacity.
    shared_name: (optional). If set, this queue will be shared under the given
      name across multiple sessions.
    name: A name for the operations (optional).

  Returns:
    A queue with the output strings.  A `QueueRunner` for the Queue
    is added to the current `Graph`'s `QUEUE_RUNNER` collection.

  Raises:
    ValueError: If the string_tensor is a null Python list.  At runtime,
    will fail with an assertion if string_tensor becomes a null tensor.
  """
  not_null_err = "string_input_producer requires a non-null input tensor"
  if not isinstance(string_tensor, ops.Tensor) and not string_tensor:
    raise ValueError(not_null_err)

  with ops.op_scope([string_tensor], name, "input_producer") as name:
    string_tensor = ops.convert_to_tensor(string_tensor, dtype=dtypes.string)
    with ops.control_dependencies([
        logging_ops.Assert(math_ops.greater(array_ops.size(string_tensor), 0),
                           [not_null_err])]):
      string_tensor = array_ops.identity(string_tensor)
    return input_producer(
        input_tensor=string_tensor,
        element_shape=[],
        num_epochs=num_epochs,
        shuffle=shuffle,
        seed=seed,
        capacity=capacity,
        shared_name=shared_name,
        name=name,
        summary_name="fraction_of_%d_full" % capacity)


def range_input_producer(limit, num_epochs=None, shuffle=True, seed=None,
                         capacity=32, shared_name=None, name=None):
  """Produces the integers from 0 to limit-1 in a queue.

  Args:
    limit: An int32 scalar tensor.
    num_epochs: An integer (optional). If specified, `range_input_producer`
      produces each integer `num_epochs` times before generating an
      OutOfRange error. If not specified, `range_input_producer` can cycle
      through the integers an unlimited number of times.
    shuffle: Boolean. If true, the integers are randomly shuffled within each
      epoch.
    seed: An integer (optional). Seed used if shuffle == True.
    capacity: An integer. Sets the queue capacity.
    shared_name: (optional). If set, this queue will be shared under the given
      name across multiple sessions.
    name: A name for the operations (optional).

  Returns:
    A Queue with the output integers.  A `QueueRunner` for the Queue
    is added to the current `Graph`'s `QUEUE_RUNNER` collection.
  """
  with ops.op_scope([limit], name, "input_producer") as name:
    range_tensor = math_ops.range(limit)
    return input_producer(
        range_tensor, [], num_epochs, shuffle, seed, capacity,
        shared_name, name, "fraction_of_%d_full" % capacity)


def slice_input_producer(tensor_list, num_epochs=None, shuffle=True, seed=None,
                         capacity=32, shared_name=None, name=None):
  """Produces a slice of each `Tensor` in `tensor_list`.

  Implemented using a Queue -- a `QueueRunner` for the Queue
  is added to the current `Graph`'s `QUEUE_RUNNER` collection.

  Args:
    tensor_list: A list of `Tensor` objects. Every `Tensor` in
      `tensor_list` must have the same size in the first dimension.
    num_epochs: An integer (optional). If specified, `slice_input_producer`
      produces each slice `num_epochs` times before generating
      an `OutOfRange` error. If not specified, `slice_input_producer` can cycle
      through the slices an unlimited number of times.
    shuffle: Boolean. If true, the integers are randomly shuffled within each
      epoch.
    seed: An integer (optional). Seed used if shuffle == True.
    capacity: An integer. Sets the queue capacity.
    shared_name: (optional). If set, this queue will be shared under the given
      name across multiple sessions.
    name: A name for the operations (optional).

  Returns:
    A list of tensors, one for each element of `tensor_list`.  If the tensor
    in `tensor_list` has shape `[N, a, b, .., z]`, then the corresponding output
    tensor will have shape `[a, b, ..., z]`.

  Raises:
    ValueError: if `slice_input_producer` produces nothing from `tensor_list`.
  """
  with ops.op_scope(tensor_list, name, "input_producer"):
    tensor_list = ops.convert_n_to_tensor_or_indexed_slices(tensor_list)
    if not tensor_list:
      raise ValueError(
          "Expected at least one tensor in slice_input_producer().")
    range_size = array_ops.shape(tensor_list[0])[0]
    # TODO(josh11b): Add an assertion that the first dimension of
    # everything in TensorList matches. Maybe just check the inferred shapes?
    queue = range_input_producer(range_size, num_epochs=num_epochs,
                                 shuffle=shuffle, seed=seed, capacity=capacity,
                                 shared_name=shared_name)
    index = queue.dequeue()
    output = [array_ops.gather(t, index) for t in tensor_list]
    return output


# Helpers for the batching functions ------------------------------------------


def _flatten(tensor_list_list):
  return [tensor for tensor_list in tensor_list_list for tensor in tensor_list]


class _SparseMetaData(object):
  """Store information about the Tensor: Is it sparse?, dtype, and rank."""

  def __init__(self, sparse, dtype, rank):
    self._sparse = sparse
    self._dtype = dtype
    self._rank = rank

  def __eq__(self, other):
    if self.sparse != other.sparse:
      return False
    if not self.sparse:
      return True
    if self.dtype != other.dtype:
      return False
    if not self.rank.is_compatible_with(other.rank):
      return False
    return True

  def __ne__(self, other):
    return not self.__eq__(other)

  def __str__(self):
    return "[SparseMetaData(%s, %s, %s)]" % (self.sparse, self.dtype, self.rank)

  def merge_with(self, other):
    if self != other:
      raise ValueError("SparseMetaData objects are incompatible: %s vs. %s"
                       % (self, other))
    if self.sparse:
      self.rank.merge_with(other.rank)
    return self

  @property
  def dtype(self):
    return self._dtype

  @property
  def sparse(self):
    return self._sparse

  @property
  def rank(self):
    return self._rank


def _serialize_sparse_tensors(tensor_list, enqueue_many):
  """Serialize SparseTensors for feeding into batch, etc."""
  sparse_info_list = [
      _SparseMetaData(sparse=True,
                      dtype=t.dtype,
                      rank=t.shape.get_shape().with_rank(1)[0])
      if isinstance(t, ops.SparseTensor)
      else _SparseMetaData(False, None, None)
      for t in tensor_list]

  def _maybe_serialize(t, sparse):
    if not sparse:
      return t
    return (sparse_ops.serialize_many_sparse(t) if enqueue_many
            else sparse_ops.serialize_sparse(t))

  serialized_list = [
      _maybe_serialize(t, info.sparse) for (t, info)
      in zip(tensor_list, sparse_info_list)]

  return serialized_list, sparse_info_list


def _serialize_sparse_tensors_join(tensor_list_list, enqueue_many):
  """Serialize SparseTensors for feeding into batch_join, etc."""
  (s0, sparse_info_list) = _serialize_sparse_tensors(
      tensor_list_list[0], enqueue_many)
  serialized_list_list = [s0]
  for tensor_list in tensor_list_list[1:]:
    s, sparse_info_candidate = _serialize_sparse_tensors(
        tensor_list, enqueue_many)
    if sparse_info_list != sparse_info_candidate:
      raise ValueError("Inconsistent SparseTensors list: %s vs. %s"
                       % (tensor_list_list[0], tensor_list))
    sparse_info_list = [
        info.merge_with(candidate)
        for (info, candidate) in zip(sparse_info_list, sparse_info_candidate)]
    serialized_list_list.append(s)

  return (serialized_list_list, sparse_info_list)


def _deserialize_sparse_tensors(serialized_list, sparse_info_list):
  """Deserialize SparseTensors after dequeue in batch, batch_join, etc."""
  received_sequence = isinstance(serialized_list, collections.Sequence)
  if not received_sequence:
    serialized_list = (serialized_list,)
  tensors = [
      sparse_ops.deserialize_many_sparse(s, info.dtype, info.rank.value)
      if info.sparse else s
      for (s, info)
      in zip(serialized_list, sparse_info_list)]
  return tensors if received_sequence else tensors[0]


def _validate(tensor_list):
  tensor_list = ops.convert_n_to_tensor_or_indexed_slices(tensor_list)
  if not tensor_list:
    raise ValueError("Expected at least one tensor in batch().")
  return tensor_list


def _validate_join(tensor_list_list):
  tensor_list_list = [ops.convert_n_to_tensor_or_indexed_slices(tl)
                      for tl in tensor_list_list]
  if not tensor_list_list:
    raise ValueError("Expected at least one input in batch_join().")
  return tensor_list_list


def _dtypes(tensor_list_list):
  all_types = [[t.dtype for t in tl] for tl in tensor_list_list]
  types = all_types[0]
  for other_types in all_types[1:]:
    if other_types != types:
      raise TypeError("Expected types to be consistent: %s vs. %s." %
                      (", ".join(x.name for x in types),
                       ", ".join(x.name for x in other_types)))
  return types


def _merge_shapes(shape_list, enqueue_many):
  shape_list = [tensor_shape.as_shape(s) for s in shape_list]
  if enqueue_many:
    # We want the shapes without the leading batch dimension.
    shape_list = [s.with_rank_at_least(1)[1:] for s in shape_list]
  merged_shape = shape_list[0]
  for s in shape_list[1:]:
    merged_shape.merge_with(s)
  return merged_shape.as_list()


def _shapes(tensor_list_list, shapes, enqueue_many):
  if shapes is None:
    l = len(tensor_list_list[0])
    shapes = [_merge_shapes(
        [tl[i].get_shape().as_list() for tl in tensor_list_list], enqueue_many)
              for i in xrange(l)]
  return shapes


def _enqueue_join(queue, tensor_list_list, enqueue_many):
  if enqueue_many:
    enqueue_ops = [queue.enqueue_many(tl) for tl in tensor_list_list]
  else:
    enqueue_ops = [queue.enqueue(tl) for tl in tensor_list_list]
  queue_runner.add_queue_runner(queue_runner.QueueRunner(queue, enqueue_ops))


def _enqueue(queue, tensor_list, threads, enqueue_many):
  if enqueue_many:
    enqueue_ops = [queue.enqueue_many(tensor_list)] * threads
  else:
    enqueue_ops = [queue.enqueue(tensor_list)] * threads
  queue_runner.add_queue_runner(queue_runner.QueueRunner(queue, enqueue_ops))


# Batching functions ----------------------------------------------------------


def batch(tensor_list, batch_size, num_threads=1, capacity=32,
          enqueue_many=False, shapes=None,
          shared_name=None, name=None):
  """Creates batches of tensors in `tensor_list`.

  This function is implemented using a queue. A `QueueRunner` for the
  queue is added to the current `Graph`'s `QUEUE_RUNNER` collection.

  If `enqueue_many` is `False`, `tensor_list` is assumed to represent a
  single example.  An input tensor with shape `[x, y, z]` will be output
  as a tensor with shape `[batch_size, x, y, z]`.

  If `enqueue_many` is `True`, `tensor_list` is assumed to represent a
  batch of examples, where the first dimension is indexed by example,
  and all members of `tensor_list` should have the same size in the
  first dimension.  If an input tensor has shape `[*, x, y, z]`, the
  output will have shape `[batch_size, x, y, z]`.  The `capacity` argument
  controls the how long the prefetching is allowed to grow the queues.

  The returned operation is a dequeue operation and will throw
  `tf.errors.OutOfRangeError` if the input queue is exhausted. If this
  operation is feeding another input queue, its queue runner will catch
  this exception, however, if this operation is used in your main thread
  you are responsible for catching this yourself.

  *N.B.:* You must ensure that either (i) the `shapes` argument is
  passed, or (ii) all of the tensors in `tensor_list` must have
  fully-defined shapes. `ValueError` will be raised if neither of
  these conditions holds.

  Args:
    tensor_list: The list of tensors to enqueue.
    batch_size: The new batch size pulled from the queue.
    num_threads: The number of threads enqueuing `tensor_list`.
    capacity: An integer. The maximum number of elements in the queue.
    enqueue_many: Whether each tensor in `tensor_list` is a single example.
    shapes: (Optional) The shapes for each example.  Defaults to the
      inferred shapes for `tensor_list`.
    shared_name: (optional). If set, this queue will be shared under the given
      name across multiple sessions.
    name: (Optional) A name for the operations.

  Returns:
    A list of tensors with the same number and types as `tensor_list`.

  Raises:
    ValueError: If the `shapes` are not specified, and cannot be
      inferred from the elements of `tensor_list`.
  """
  with ops.op_scope(tensor_list, name, "batch") as name:
    tensor_list = _validate(tensor_list)
    (tensor_list, sparse_info) = _serialize_sparse_tensors(
        tensor_list, enqueue_many)
    types = _dtypes([tensor_list])
    shapes = _shapes([tensor_list], shapes, enqueue_many)
    # TODO(josh11b,mrry): Switch to BatchQueue once it is written.
    queue = data_flow_ops.FIFOQueue(
        capacity=capacity, dtypes=types, shapes=shapes, shared_name=shared_name)
    _enqueue(queue, tensor_list, num_threads, enqueue_many)
    logging_ops.scalar_summary(
        "queue/%s/fraction_of_%d_full" % (queue.name, capacity),
        math_ops.cast(queue.size(), dtypes.float32) * (1. / capacity))

    dequeued = queue.dequeue_many(batch_size, name=name)
    dequeued = _deserialize_sparse_tensors(dequeued, sparse_info)
    return dequeued


# TODO(josh11b): Add a thread_multiplier or num_threads (that has to be
# a multiple of len(tensor_list_list)?) parameter, to address the use
# case where you want more parallelism than you can support different
# readers (either because you don't have that many files or can't
# read that many files in parallel due to the number of seeks required).
# Once this is done, batch() can be written as a call to batch_join().
def batch_join(tensor_list_list, batch_size, capacity=32, enqueue_many=False,
               shapes=None, shared_name=None, name=None):
  """Runs a list of tensors to fill a queue to create batches of examples.

  Enqueues a different list of tensors in different threads.
  Implemented using a queue -- a `QueueRunner` for the queue
  is added to the current `Graph`'s `QUEUE_RUNNER` collection.

  `len(tensor_list_list)` threads will be started,
  with thread `i` enqueuing the tensors from
  `tensor_list_list[i]`. `tensor_list_list[i1][j]` must match
  `tensor_list_list[i2][j]` in type and shape, except in the first
  dimension if `enqueue_many` is true.

  If `enqueue_many` is `False`, each `tensor_list_list[i]` is assumed
  to represent a single example. An input tensor `x` will be output as a
  tensor with shape `[batch_size] + x.shape`.

  If `enqueue_many` is `True`, `tensor_list_list[i]` is assumed to
  represent a batch of examples, where the first dimension is indexed
  by example, and all members of `tensor_list_list[i]` should have the
  same size in the first dimension.  The slices of any input tensor
  `x` are treated as examples, and the output tensors will have shape
  `[batch_size] + x.shape[1:]`.

  The `capacity` argument controls the how long the prefetching is allowed to
  grow the queues.

  The returned operation is a dequeue operation and will throw
  `tf.errors.OutOfRangeError` if the input queue is exhausted. If this
  operation is feeding another input queue, its queue runner will catch
  this exception, however, if this operation is used in your main thread
  you are responsible for catching this yourself.

  *N.B.:* You must ensure that either (i) the `shapes` argument is
  passed, or (ii) all of the tensors in `tensor_list_list` must have
  fully-defined shapes. `ValueError` will be raised if neither of
  these conditions holds.

  Args:
    tensor_list_list: A list of tuples of tensors to enqueue.
    batch_size: An integer. The new batch size pulled from the queue.
    capacity: An integer. The maximum number of elements in the queue.
    enqueue_many: Whether each tensor in `tensor_list_list` is a single
      example.
    shapes: (Optional) The shapes for each example.  Defaults to the
      inferred shapes for `tensor_list_list[i]`.
    shared_name: (Optional) If set, this queue will be shared under the given
      name across multiple sessions.
    name: (Optional) A name for the operations.

  Returns:
    A list of tensors with the same number and types as
    `tensor_list_list[i]`.

  Raises:
    ValueError: If the `shapes` are not specified, and cannot be
      inferred from the elements of `tensor_list_list`.
  """
  with ops.op_scope(_flatten(tensor_list_list), name, "batch_join") as name:
    tensor_list_list = _validate_join(tensor_list_list)
    tensor_list_list, sparse_info = _serialize_sparse_tensors_join(
        tensor_list_list, enqueue_many)
    types = _dtypes(tensor_list_list)
    shapes = _shapes(tensor_list_list, shapes, enqueue_many)
    # TODO(josh11b,mrry): Switch to BatchQueue once it is written.
    queue = data_flow_ops.FIFOQueue(
        capacity=capacity, dtypes=types, shapes=shapes, shared_name=shared_name)
    _enqueue_join(queue, tensor_list_list, enqueue_many)
    logging_ops.scalar_summary(
        "queue/%s/fraction_of_%d_full" % (queue.name, capacity),
        math_ops.cast(queue.size(), dtypes.float32) * (1. / capacity))

    dequeued = queue.dequeue_many(batch_size, name=name)
    dequeued = _deserialize_sparse_tensors(dequeued, sparse_info)
    return dequeued


def shuffle_batch(tensor_list, batch_size, capacity, min_after_dequeue,
                  num_threads=1, seed=None, enqueue_many=False, shapes=None,
                  shared_name=None, name=None):
  """Creates batches by randomly shuffling tensors.

  This function adds the following to the current `Graph`:

  * A shuffling queue into which tensors from `tensor_list` are enqueued.
  * A `dequeue_many` operation to create batches from the queue.
  * A `QueueRunner` to `QUEUE_RUNNER` collection, to enqueue the tensors
    from `tensor_list`.

  If `enqueue_many` is `False`, `tensor_list` is assumed to represent a
  single example.  An input tensor with shape `[x, y, z]` will be output
  as a tensor with shape `[batch_size, x, y, z]`.

  If `enqueue_many` is `True`, `tensor_list` is assumed to represent a
  batch of examples, where the first dimension is indexed by example,
  and all members of `tensor_list` should have the same size in the
  first dimension.  If an input tensor has shape `[*, x, y, z]`, the
  output will have shape `[batch_size, x, y, z]`.

  The `capacity` argument controls the how long the prefetching is allowed to
  grow the queues.

  The returned operation is a dequeue operation and will throw
  `tf.errors.OutOfRangeError` if the input queue is exhausted. If this
  operation is feeding another input queue, its queue runner will catch
  this exception, however, if this operation is used in your main thread
  you are responsible for catching this yourself.

  For example:

  ```python
  # Creates batches of 32 images and 32 labels.
  image_batch, label_batch = tf.train.shuffle_batch(
        [single_image, single_label],
        batch_size=32,
        num_threads=4,
        capacity=50000,
        min_after_dequeue=10000)
  ```

  *N.B.:* You must ensure that either (i) the `shapes` argument is
  passed, or (ii) all of the tensors in `tensor_list` must have
  fully-defined shapes. `ValueError` will be raised if neither of
  these conditions holds.

  Args:
    tensor_list: The list of tensors to enqueue.
    batch_size: The new batch size pulled from the queue.
    capacity: An integer. The maximum number of elements in the queue.
    min_after_dequeue: Minimum number elements in the queue after a
      dequeue, used to ensure a level of mixing of elements.
    num_threads: The number of threads enqueuing `tensor_list`.
    seed: Seed for the random shuffling within the queue.
    enqueue_many: Whether each tensor in `tensor_list` is a single example.
    shapes: (Optional) The shapes for each example.  Defaults to the
      inferred shapes for `tensor_list`.
    shared_name: (Optional) If set, this queue will be shared under the given
      name across multiple sessions.
    name: (Optional) A name for the operations.

  Returns:
    A list of tensors with the same number and types as `tensor_list`.

  Raises:
    ValueError: If the `shapes` are not specified, and cannot be
      inferred from the elements of `tensor_list`.
  """
  with ops.op_scope(tensor_list, name, "shuffle_batch") as name:
    tensor_list = _validate(tensor_list)
    tensor_list, sparse_info = _serialize_sparse_tensors(
        tensor_list, enqueue_many)
    types = _dtypes([tensor_list])
    shapes = _shapes([tensor_list], shapes, enqueue_many)
    queue = data_flow_ops.RandomShuffleQueue(
        capacity=capacity, min_after_dequeue=min_after_dequeue, seed=seed,
        dtypes=types, shapes=shapes, shared_name=shared_name)
    _enqueue(queue, tensor_list, num_threads, enqueue_many)
    full = (math_ops.cast(math_ops.maximum(0, queue.size() - min_after_dequeue),
                          dtypes.float32) *
            (1. / (capacity - min_after_dequeue)))
    # Note that name contains a '/' at the end so we intentionally do not place
    # a '/' after %s below.
    summary_name = (
        "queue/%sfraction_over_%d_of_%d_full" %
        (name, min_after_dequeue, capacity - min_after_dequeue))
    logging_ops.scalar_summary(summary_name, full)

    dequeued = queue.dequeue_many(batch_size, name=name)
    dequeued = _deserialize_sparse_tensors(dequeued, sparse_info)
    return dequeued


def shuffle_batch_join(tensor_list_list, batch_size, capacity,
                       min_after_dequeue, seed=None, enqueue_many=False,
                       shapes=None, shared_name=None, name=None):
  """Create batches by randomly shuffling tensors.

  This version enqueues a different list of tensors in different threads.
  It adds the following to the current `Graph`:

  * A shuffling queue into which tensors from `tensor_list_list` are enqueued.
  * A `dequeue_many` operation to create batches from the queue.
  * A `QueueRunner` to `QUEUE_RUNNER` collection, to enqueue the tensors
    from `tensor_list_list`.

  `len(tensor_list_list)` threads will be started, with thread `i` enqueuing
  the tensors from `tensor_list_list[i]`. `tensor_list_list[i1][j]` must match
  `tensor_list_list[i2][j]` in type and shape, except in the first dimension if
  `enqueue_many` is true.

  If `enqueue_many` is `False`, each `tensor_list_list[i]` is assumed
  to represent a single example.  An input tensor with shape `[x, y,
  z]` will be output as a tensor with shape `[batch_size, x, y, z]`.

  If `enqueue_many` is `True`, `tensor_list_list[i]` is assumed to
  represent a batch of examples, where the first dimension is indexed
  by example, and all members of `tensor_list_list[i]` should have the
  same size in the first dimension.  If an input tensor has shape `[*, x,
  y, z]`, the output will have shape `[batch_size, x, y, z]`.

  The `capacity` argument controls the how long the prefetching is allowed to
  grow the queues.

  The returned operation is a dequeue operation and will throw
  `tf.errors.OutOfRangeError` if the input queue is exhausted. If this
  operation is feeding another input queue, its queue runner will catch
  this exception, however, if this operation is used in your main thread
  you are responsible for catching this yourself.

  Args:
    tensor_list_list: A list of tuples of tensors to enqueue.
    batch_size: An integer. The new batch size pulled from the queue.
    capacity: An integer. The maximum number of elements in the queue.
    min_after_dequeue: Minimum number elements in the queue after a
      dequeue, used to ensure a level of mixing of elements.
    seed: Seed for the random shuffling within the queue.
    enqueue_many: Whether each tensor in `tensor_list_list` is a single
      example.
    shapes: (Optional) The shapes for each example.  Defaults to the
      inferred shapes for `tensor_list_list[i]`.
    shared_name: (optional). If set, this queue will be shared under the given
      name across multiple sessions.
    name: (Optional) A name for the operations.

  Returns:
    A list of tensors with the same number and types as `tensor_list_list[i]`.

  Raises:
    ValueError: If the `shapes` are not specified, and cannot be
      inferred from the elements of `tensor_list_list`.
  """
  with ops.op_scope(
      _flatten(tensor_list_list), name, "shuffle_batch_join") as name:
    tensor_list_list = _validate_join(tensor_list_list)
    tensor_list_list, sparse_info = _serialize_sparse_tensors_join(
        tensor_list_list, enqueue_many)
    types = _dtypes(tensor_list_list)
    shapes = _shapes(tensor_list_list, shapes, enqueue_many)
    queue = data_flow_ops.RandomShuffleQueue(
        capacity=capacity, min_after_dequeue=min_after_dequeue, seed=seed,
        dtypes=types, shapes=shapes, shared_name=shared_name)
    _enqueue_join(queue, tensor_list_list, enqueue_many)
    full = (math_ops.cast(math_ops.maximum(0, queue.size() - min_after_dequeue),
                          dtypes.float32) *
            (1. / (capacity - min_after_dequeue)))
    # Note that name contains a '/' at the end so we intentionally do not place
    # a '/' after %s below.
    summary_name = (
        "queue/%sfraction_over_%d_of_%d_full" %
        (name, min_after_dequeue, capacity - min_after_dequeue))
    logging_ops.scalar_summary(summary_name, full)

    dequeued = queue.dequeue_many(batch_size, name=name)
    dequeued = _deserialize_sparse_tensors(dequeued, sparse_info)
    return dequeued
