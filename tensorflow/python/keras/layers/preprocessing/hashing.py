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
"""Keras hashing preprocessing layer."""
# pylint: disable=g-classes-have-attributes
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import functools
import numpy as np

from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.framework import sparse_tensor
from tensorflow.python.framework import tensor_shape
from tensorflow.python.framework import tensor_spec
from tensorflow.python.framework import tensor_util
from tensorflow.python.keras.engine import base_preprocessing_layer
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gen_sparse_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops import sparse_ops
from tensorflow.python.ops import string_ops
from tensorflow.python.ops.ragged import ragged_tensor
from tensorflow.python.util.tf_export import keras_export

# Default key from tf.sparse.cross_hashed
_DEFAULT_SALT_KEY = [0xDECAFCAFFE, 0xDECAFCAFFE]


@keras_export('keras.layers.experimental.preprocessing.Hashing')
class Hashing(base_preprocessing_layer.PreprocessingLayer):
  """Implements categorical feature hashing, also known as "hashing trick".

  This layer transforms single or multiple categorical inputs to hashed output.
  It converts a sequence of int or string to a sequence of int. The stable hash
  function uses tensorflow::ops::Fingerprint to produce universal output that
  is consistent across platforms.

  This layer uses [FarmHash64](https://github.com/google/farmhash) by default,
  which provides a consistent hashed output across different platforms and is
  stable across invocations, regardless of device and context, by mixing the
  input bits thoroughly.

  If you want to obfuscate the hashed output, you can also pass a random `salt`
  argument in the constructor. In that case, the layer will use the
  [SipHash64](https://github.com/google/highwayhash) hash function, with
  the `salt` value serving as additional input to the hash function.

  Example (FarmHash64):

  >>> layer = tf.keras.layers.experimental.preprocessing.Hashing(num_bins=3)
  >>> inp = [['A'], ['B'], ['C'], ['D'], ['E']]
  >>> layer(inp)
  <tf.Tensor: shape=(5, 1), dtype=int64, numpy=
    array([[1],
           [0],
           [1],
           [1],
           [2]])>

  Example (FarmHash64) with a mask value:

  >>> layer = tf.keras.layers.experimental.preprocessing.Hashing(num_bins=3,
  ...    mask_value='')
  >>> inp = [['A'], ['B'], [''], ['C'], ['D']]
  >>> layer(inp)
  <tf.Tensor: shape=(5, 1), dtype=int64, numpy=
    array([[1],
           [1],
           [0],
           [2],
           [2]])>


  Example (FarmHash64) with list of inputs:
  >>> layer = tf.keras.layers.experimental.preprocessing.Hashing(num_bins=3)
  >>> inp_1 = [['A'], ['B'], ['C'], ['D'], ['E']]
  >>> inp_2 = np.asarray([[5], [4], [3], [2], [1]])
  >>> layer([inp_1, inp_2])
  <tf.Tensor: shape=(5, 1), dtype=int64, numpy=
    array([[1],
           [1],
           [0],
           [2],
           [0]])>


  Example (SipHash64):

  >>> layer = tf.keras.layers.experimental.preprocessing.Hashing(num_bins=3,
  ...    salt=[133, 137])
  >>> inp = [['A'], ['B'], ['C'], ['D'], ['E']]
  >>> layer(inp)
  <tf.Tensor: shape=(5, 1), dtype=int64, numpy=
    array([[1],
           [2],
           [1],
           [0],
           [2]])>

  Example (Siphash64 with a single integer, same as `salt=[133, 133]`

  >>> layer = tf.keras.layers.experimental.preprocessing.Hashing(num_bins=3,
  ...    salt=133)
  >>> inp = [['A'], ['B'], ['C'], ['D'], ['E']]
  >>> layer(inp)
  <tf.Tensor: shape=(5, 1), dtype=int64, numpy=
    array([[0],
           [0],
           [2],
           [1],
           [0]])>

  Reference: [SipHash with salt](https://www.131002.net/siphash/siphash.pdf)

  Args:
    num_bins: Number of hash bins. Note that this includes the `mask_value` bin,
      so the effective number of bins is `(num_bins - 1)` if `mask_value` is
      set.
    mask_value: A value that represents masked inputs, which are mapped to
      index 0. Defaults to None, meaning no mask term will be added and the
      hashing will start at index 0.
    salt: A single unsigned integer or None.
      If passed, the hash function used will be SipHash64, with these values
      used as an additional input (known as a "salt" in cryptography).
      These should be non-zero. Defaults to `None` (in that
      case, the FarmHash64 hash function is used). It also supports
      tuple/list of 2 unsigned integer numbers, see reference paper for details.
    name: Name to give to the layer.
    **kwargs: Keyword arguments to construct a layer.

  Input shape: A single or list of string, int32 or int64 `Tensor`,
    `SparseTensor` or `RaggedTensor` of shape `[batch_size, ...,]`

  Output shape: An int64 `Tensor`, `SparseTensor` or `RaggedTensor` of shape
    `[batch_size, ...]`. If any input is `RaggedTensor` then output is
    `RaggedTensor`, otherwise if any input is `SparseTensor` then output is
    `SparseTensor`, otherwise the output is `Tensor`.

  """

  def __init__(self, num_bins, mask_value=None, salt=None, name=None, **kwargs):
    if num_bins is None or num_bins <= 0:
      raise ValueError('`num_bins` cannot be `None` or non-positive values.')
    super(Hashing, self).__init__(name=name, **kwargs)
    base_preprocessing_layer.keras_kpl_gauge.get_cell('Hashing').set(True)
    self.num_bins = num_bins
    self.mask_value = mask_value
    self.strong_hash = True if salt is not None else False
    if salt is not None:
      if isinstance(salt, (tuple, list)) and len(salt) == 2:
        self.salt = salt
      elif isinstance(salt, int):
        self.salt = [salt, salt]
      else:
        raise ValueError('`salt can only be a tuple of size 2 integers, or a '
                         'single integer, given {}'.format(salt))
    else:
      self.salt = _DEFAULT_SALT_KEY

  def _preprocess_single_input(self, inp):
    if isinstance(inp, (list, tuple, np.ndarray)):
      inp = ops.convert_to_tensor_v2_with_dispatch(inp)
    return inp

  def _preprocess_inputs(self, inputs):
    if isinstance(inputs, (tuple, list)):
      # If any of them is tensor or ndarray, then treat as list
      if any(
          tensor_util.is_tf_type(inp) or isinstance(inp, np.ndarray)
          for inp in inputs):
        return [self._preprocess_single_input(inp) for inp in inputs]
    return self._preprocess_single_input(inputs)

  def call(self, inputs):
    inputs = self._preprocess_inputs(inputs)
    if isinstance(inputs, (tuple, list)):
      return self._process_input_list(inputs)
    elif isinstance(inputs, sparse_tensor.SparseTensor):
      return sparse_tensor.SparseTensor(
          indices=inputs.indices,
          values=self._hash_values_to_bins(inputs.values),
          dense_shape=inputs.dense_shape)
    return self._hash_values_to_bins(inputs)

  def _process_input_list(self, inputs):
    # TODO(momernick): support ragged_cross_hashed with corrected fingerprint
    # and siphash.
    if any(isinstance(inp, ragged_tensor.RaggedTensor) for inp in inputs):
      raise ValueError('Hashing with ragged input is not supported yet.')
    if self.mask_value is not None:
      raise ValueError(
          'Cross hashing with a mask_value is not supported yet, mask_value is '
          '{}.'.format(self.mask_value))
    sparse_inputs = [
        inp for inp in inputs if isinstance(inp, sparse_tensor.SparseTensor)
    ]
    dense_inputs = [
        inp for inp in inputs if not isinstance(inp, sparse_tensor.SparseTensor)
    ]
    all_dense = True if not sparse_inputs else False
    indices = [sp_inp.indices for sp_inp in sparse_inputs]
    values = [sp_inp.values for sp_inp in sparse_inputs]
    shapes = [sp_inp.dense_shape for sp_inp in sparse_inputs]
    indices_out, values_out, shapes_out = gen_sparse_ops.SparseCrossHashed(
        indices=indices,
        values=values,
        shapes=shapes,
        dense_inputs=dense_inputs,
        num_buckets=self.num_bins,
        strong_hash=self.strong_hash,
        salt=self.salt)
    sparse_out = sparse_tensor.SparseTensor(indices_out, values_out, shapes_out)
    if all_dense:
      return sparse_ops.sparse_tensor_to_dense(sparse_out)
    return sparse_out

  def _hash_values_to_bins(self, values):
    """Converts a non-sparse tensor of values to bin indices."""
    str_to_hash_bucket = self._get_string_to_hash_bucket_fn()
    num_available_bins = self.num_bins
    mask = None
    # If mask_value is set, the zeroth bin is reserved for it.
    if self.mask_value is not None and num_available_bins > 1:
      num_available_bins -= 1
      mask = math_ops.equal(values, self.mask_value)
    # Convert all values to strings before hashing.
    if values.dtype.is_integer:
      values = string_ops.as_string(values)
    values = str_to_hash_bucket(values, num_available_bins, name='hash')
    if mask is not None:
      values = math_ops.add(values, array_ops.ones_like(values))
      values = array_ops.where(mask, array_ops.zeros_like(values), values)
    return values

  def _get_string_to_hash_bucket_fn(self):
    """Returns the string_to_hash_bucket op to use based on `hasher_key`."""
    # string_to_hash_bucket_fast uses FarmHash64 as hash function.
    if not self.strong_hash:
      return string_ops.string_to_hash_bucket_fast
    # string_to_hash_bucket_strong uses SipHash64 as hash function.
    else:
      return functools.partial(
          string_ops.string_to_hash_bucket_strong, key=self.salt)

  def compute_output_shape(self, input_shape):
    if not isinstance(input_shape, (tuple, list)):
      return input_shape
    input_shapes = input_shape
    batch_size = None
    for inp_shape in input_shapes:
      inp_tensor_shape = tensor_shape.TensorShape(inp_shape).as_list()
      if len(inp_tensor_shape) != 2:
        raise ValueError('Inputs must be rank 2, get {}'.format(input_shapes))
      if batch_size is None:
        batch_size = inp_tensor_shape[0]
    # The second dimension is dynamic based on inputs.
    output_shape = [batch_size, None]
    return tensor_shape.TensorShape(output_shape)

  def compute_output_signature(self, input_spec):
    if not isinstance(input_spec, (tuple, list)):
      output_shape = self.compute_output_shape(input_spec.shape)
      output_dtype = dtypes.int64
      if isinstance(input_spec, sparse_tensor.SparseTensorSpec):
        return sparse_tensor.SparseTensorSpec(
            shape=output_shape, dtype=output_dtype)
      else:
        return tensor_spec.TensorSpec(shape=output_shape, dtype=output_dtype)
    input_shapes = [x.shape for x in input_spec]
    output_shape = self.compute_output_shape(input_shapes)
    if any(
        isinstance(inp_spec, ragged_tensor.RaggedTensorSpec)
        for inp_spec in input_spec):
      return tensor_spec.TensorSpec(shape=output_shape, dtype=dtypes.int64)
    elif any(
        isinstance(inp_spec, sparse_tensor.SparseTensorSpec)
        for inp_spec in input_spec):
      return sparse_tensor.SparseTensorSpec(
          shape=output_shape, dtype=dtypes.int64)
    return tensor_spec.TensorSpec(shape=output_shape, dtype=dtypes.int64)

  def get_config(self):
    config = {
        'num_bins': self.num_bins,
        'salt': self.salt,
        'mask_value': self.mask_value,
    }
    base_config = super(Hashing, self).get_config()
    return dict(list(base_config.items()) + list(config.items()))
