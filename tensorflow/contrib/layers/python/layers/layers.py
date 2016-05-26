# Copyright 2016 Google Inc. All Rights Reserved.
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

# pylint: disable=g-short-docstring-punctuation
"""Higher level ops for building layers."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import functools

from tensorflow.contrib.framework.python.ops import add_arg_scope
from tensorflow.contrib.framework.python.ops import variables
from tensorflow.contrib.layers.python.layers import initializers
from tensorflow.contrib.layers.python.layers import utils


from tensorflow.python.framework import ops
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import init_ops
from tensorflow.python.ops import nn
from tensorflow.python.ops import standard_ops
from tensorflow.python.ops import variable_scope
from tensorflow.python.training import moving_averages

# TODO(b/28426988): Replace legacy_* fns migrated from slim.
# TODO(b/28426988): Remove legacy_* when all uses have migrated to new API.
__all__ = ['bias_add',
           'batch_norm',
           'conv2d',
           'convolution2d',
           'fully_connected',
           'legacy_convolution2d',
           'legacy_fully_connected',
           'legacy_linear',
           'legacy_relu',
           'legacy_relu6']


def _apply_activation(y, activation_fn, output_collections):
  if activation_fn:
    y = activation_fn(y)
  ops.add_to_collections(list(output_collections or []) +
                         [ops.GraphKeys.ACTIVATIONS], y)
  return y


@add_arg_scope
def bias_add(inputs,
             activation_fn=None,
             initializer=init_ops.zeros_initializer,
             regularizer=None,
             reuse=None,
             variables_collections=None,
             outputs_collections=None,
             scope=None):
  """Adds a bias to the inputs.

  Can be used as a normalizer function for conv2d and fully_connected.

  Args:
    inputs: a tensor of with at least rank 2 and value for the last dimension,
      e.g. `[batch_size, depth]`, `[None, None, None, depth]`.
    activation_fn: Optional activation function.
    initializer: An initializer for the bias, defaults to 0.
    regularizer: A regularizer like the result of
      `l1_regularizer` or `l2_regularizer`.
    reuse: whether or not the layer and its variables should be reused. To be
      able to reuse the layer scope must be given.
    variables_collections: optional collections for the variables.
    outputs_collections: collections to add the outputs.
    scope: Optional scope for variable_op_scope.

  Returns:
    a tensor representing the result of adding biases to the inputs.
  """
  with variable_scope.variable_op_scope([inputs],
                                        scope, 'BiasAdd', reuse=reuse) as sc:
    dtype = inputs.dtype.base_dtype
    num_features = utils.last_dimension(inputs.get_shape(), min_rank=2)
    biases_collections = utils.get_variable_collections(variables_collections,
                                                        'biases')
    biases = variables.model_variable('biases',
                                      shape=[num_features,],
                                      dtype=dtype,
                                      initializer=initializer,
                                      regularizer=regularizer,
                                      collections=biases_collections)
    outputs = nn.bias_add(inputs, biases)
    if activation_fn:
      outputs = activation_fn(outputs)
    return utils.collect_named_outputs(outputs_collections, sc.name, outputs)


@add_arg_scope
def batch_norm(inputs,
               decay=0.999,
               center=True,
               scale=False,
               epsilon=0.001,
               activation_fn=None,
               updates_collections=ops.GraphKeys.UPDATE_OPS,
               is_training=True,
               reuse=None,
               variables_collections=None,
               outputs_collections=None,
               scope=None):
  """Adds a Batch Normalization layer from http://arxiv.org/abs/1502.03167.

    "Batch Normalization: Accelerating Deep Network Training by Reducing
    Internal Covariate Shift"

    Sergey Ioffe, Christian Szegedy

  Can be used as a normalizer function for conv2d and fully_connected.

  Args:
    inputs: a tensor of size `[batch_size, height, width, channels]`
            or `[batch_size, channels]`.
    decay: decay for the moving average.
    center: If True, subtract `beta`. If False, `beta` is ignored.
    scale: If True, multiply by `gamma`. If False, `gamma` is
      not used. When the next layer is linear (also e.g. `nn.relu`), this can be
      disabled since the scaling can be done by the next layer.
    epsilon: small float added to variance to avoid dividing by zero.
    activation_fn: Optional activation function.
    updates_collections: collections to collect the update ops for computation.
      If None, a control dependency would be added to make sure the updates are
      computed.
    is_training: whether or not the layer is in training mode. In training mode
      it would accumulate the statistics of the moments into `moving_mean` and
      `moving_variance` using an exponential moving average with the given
      `decay`. When it is not in training mode then it would use the values of
      the `moving_mean` and the `moving_variance`.
    reuse: whether or not the layer and its variables should be reused. To be
      able to reuse the layer scope must be given.
    variables_collections: optional collections for the variables.
    outputs_collections: collections to add the outputs.
    scope: Optional scope for `variable_op_scope`.

  Returns:
    a tensor representing the output of the operation.

  """
  with variable_scope.variable_op_scope([inputs],
                                        scope, 'BatchNorm', reuse=reuse) as sc:
    inputs_shape = inputs.get_shape()
    dtype = inputs.dtype.base_dtype
    axis = list(range(len(inputs_shape) - 1))
    params_shape = inputs_shape[-1:]
    # Allocate parameters for the beta and gamma of the normalization.
    beta, gamma = None, None
    if center:
      beta_collections = utils.get_variable_collections(variables_collections,
                                                        'beta')
      beta = variables.model_variable('beta',
                                      shape=params_shape,
                                      dtype=dtype,
                                      initializer=init_ops.zeros_initializer,
                                      collections=beta_collections)
    if scale:
      gamma_collections = utils.get_variable_collections(variables_collections,
                                                         'gamma')
      gamma = variables.model_variable('gamma',
                                       shape=params_shape,
                                       dtype=dtype,
                                       initializer=init_ops.ones_initializer,
                                       collections=gamma_collections)
    # Create moving_mean and moving_variance variables and add them to the
    # appropiate collections.
    moving_mean_collections = utils.get_variable_collections(
        variables_collections, 'moving_mean')
    moving_mean = variables.model_variable(
        'moving_mean',
        shape=params_shape,
        dtype=dtype,
        initializer=init_ops.zeros_initializer,
        trainable=False,
        collections=moving_mean_collections)
    moving_variance_collections = utils.get_variable_collections(
        variables_collections, 'moving_variance')
    moving_variance = variables.model_variable(
        'moving_variance',
        shape=params_shape,
        dtype=dtype,
        initializer=init_ops.ones_initializer,
        trainable=False,
        collections=moving_variance_collections)
    if is_training:
      # Calculate the moments based on the individual batch.
      mean, variance = nn.moments(inputs, axis, shift=moving_mean)
      # Update the moving_mean and moving_variance moments.
      update_moving_mean = moving_averages.assign_moving_average(
          moving_mean, mean, decay)
      update_moving_variance = moving_averages.assign_moving_average(
          moving_variance, variance, decay)
      if updates_collections is None:
        # Make sure the updates are computed here.
        with ops.control_dependencies([update_moving_mean,
                                       update_moving_variance]):
          outputs = nn.batch_normalization(
              inputs, mean, variance, beta, gamma, epsilon)
      else:
        # Collect the updates to be computed later.
        ops.add_to_collections(updates_collections, update_moving_mean)
        ops.add_to_collections(updates_collections, update_moving_variance)
        outputs = nn.batch_normalization(
            inputs, mean, variance, beta, gamma, epsilon)
    else:
      outputs = nn.batch_normalization(
          inputs, moving_mean, moving_variance, beta, gamma, epsilon)
    outputs.set_shape(inputs.get_shape())
    if activation_fn:
      outputs = activation_fn(outputs)
    return utils.collect_named_outputs(outputs_collections, sc.name, outputs)


@add_arg_scope
def convolution2d(inputs,
                  num_outputs,
                  kernel_size,
                  stride=1,
                  padding='SAME',
                  activation_fn=nn.relu,
                  normalizer_fn=None,
                  normalizer_params=None,
                  weights_initializer=None,
                  weights_regularizer=None,
                  biases_initializer=init_ops.zeros_initializer,
                  biases_regularizer=None,
                  reuse=None,
                  variables_collections=None,
                  outputs_collections=None,
                  scope=None):
  """Adds a 2D convolution followed by an optional batch_norm layer.

  `convolution2d` creates a variable called `weights`, representing the
  convolutional kernel, that is convolved with the `inputs` to produce a
  `Tensor` of activations. If a `normalizer_fn` is provided (such as
  `batch_norm`), it is then applied. Otherwise, if `normalizer_fn` is
  None and a `biases_initializer` is provided then a `biases` variable would be
  created and added the activations. Finally, if `activation_fn` is not `None`,
  it is applied to the activations as well.

  Args:
    inputs: a 4-D tensor  `[batch_size, height, width, channels]`.
    num_outputs: integer, the number of output filters.
    kernel_size: a list of length 2 `[kernel_height, kernel_width]` of
      of the filters. Can be an int if both values are the same.
    stride: a list of length 2 `[stride_height, stride_width]`.
      Can be an int if both strides are the same. Note that presently
      both strides must have the same value.
    padding: one of `VALID` or `SAME`.
    activation_fn: activation function.
    normalizer_fn: normalization function to use instead of `biases`. If
      `normalize_fn` is provided then `biases_initializer` and
      `biases_regularizer` are ignored and `biases` are not created nor added.
    normalizer_params: normalization function parameters.
    weights_initializer: An initializer for the weights.
    weights_regularizer: Optional regularizer for the weights.
    biases_initializer: An initializer for the biases. If None skip biases.
    biases_regularizer: Optional regularizer for the biases.
    reuse: whether or not the layer and its variables should be reused. To be
      able to reuse the layer scope must be given.
    variables_collections: optional list of collections for all the variables or
      a dictionay containing a different list of collection per variable.
    outputs_collections: collection to add the outputs.
    scope: Optional scope for `variable_op_scope`.

  Returns:
    a tensor representing the output of the operation.

  """
  with variable_scope.variable_op_scope([inputs],
                                        scope, 'Conv', reuse=reuse) as sc:
    dtype = inputs.dtype.base_dtype
    kernel_h, kernel_w = utils.two_element_tuple(kernel_size)
    stride_h, stride_w = utils.two_element_tuple(stride)
    num_filters_in = utils.last_dimension(inputs.get_shape(), min_rank=4)
    weights_shape = [kernel_h, kernel_w,
                     num_filters_in, num_outputs]
    weights_collections = utils.get_variable_collections(
        variables_collections, 'weights')
    weights = variables.model_variable('weights',
                                       shape=weights_shape,
                                       dtype=dtype,
                                       initializer=weights_initializer,
                                       regularizer=weights_regularizer,
                                       collections=weights_collections)
    outputs = nn.conv2d(inputs, weights, [1, stride_h, stride_w, 1],
                        padding=padding)
    if normalizer_fn:
      normalizer_params = normalizer_params or {}
      normalizer_params['variables_collections'] = normalizer_params.get(
          'variables_collections', variables_collections)
      outputs = normalizer_fn(outputs, **normalizer_params)
    else:
      if biases_initializer is not None:
        biases_collections = utils.get_variable_collections(
            variables_collections, 'biases')
        biases = variables.model_variable('biases',
                                          shape=[num_outputs,],
                                          dtype=dtype,
                                          initializer=biases_initializer,
                                          regularizer=biases_regularizer,
                                          collections=biases_collections)
        outputs = nn.bias_add(outputs, biases)
    if activation_fn:
      outputs = activation_fn(outputs)
    return utils.collect_named_outputs(outputs_collections, sc.name, outputs)


@add_arg_scope
def fully_connected(inputs,
                    num_outputs,
                    activation_fn=nn.relu,
                    normalizer_fn=None,
                    normalizer_params=None,
                    weights_initializer=None,
                    weights_regularizer=None,
                    biases_initializer=init_ops.zeros_initializer,
                    biases_regularizer=None,
                    reuse=None,
                    variables_collections=None,
                    outputs_collections=None,
                    scope=None):
  """Adds a fully connected layer.

  `fully_connected` creates a variable called `weights`, representing a fully
  connected weight matrix, which is multiplied by the `inputs` to produce a
  `Tensor` of hidden units. If a `normalizer_fn` is provided (such as
  `batch_norm`), it is then applied. Otherwise, if `normalizer_fn` is
  None and a `biases_initializer` is provided then a `biases` variable would be
  created and added the hidden units. Finally, if `activation_fn` is not `None`,
  it is applied to the hidden units as well.

  Note: that if `inputs` have a rank greater than 2, then `inputs` is flattened
  prior to the initial matrix multiply by `weights`.

  Args:
    inputs: A tensor of with at least rank 2 and value for the last dimension,
      i.e. `[batch_size, depth]`, `[None, None, None, channels]`.
    num_outputs: Integer, the number of output units in the layer.
    activation_fn: activation function.
    normalizer_fn: normalization function to use instead of `biases`. If
      `normalize_fn` is provided then `biases_initializer` and
      `biases_regularizer` are ignored and `biases` are not created nor added.
    normalizer_params: normalization function parameters.
    weights_initializer: An initializer for the weights.
    weights_regularizer: Optional regularizer for the weights.
    biases_initializer: An initializer for the biases. If None skip biases.
    biases_regularizer: Optional regularizer for the biases.
    reuse: whether or not the layer and its variables should be reused. To be
      able to reuse the layer scope must be given.
    variables_collections: Optional list of collections for all the variables or
      a dictionay containing a different list of collection per variable.
    outputs_collections: collection to add the outputs.
    scope: Optional scope for variable_op_scope.

  Returns:
     the tensor variable representing the result of the series of operations.

  Raises:
    ValueError: if x has rank less than 2 or if its last dimension is not set.
  """
  with variable_scope.variable_op_scope([inputs],
                                        scope, 'FC', reuse=reuse) as sc:
    dtype = inputs.dtype.base_dtype
    num_input_units = utils.last_dimension(inputs.get_shape(), min_rank=2)

    static_shape = inputs.get_shape().as_list()
    static_shape[-1] = num_outputs

    out_shape = array_ops.unpack(array_ops.shape(inputs))
    out_shape[-1] = num_outputs

    weights_shape = [num_input_units, num_outputs]
    weights_collections = utils.get_variable_collections(
        variables_collections, 'weights')
    weights = variables.model_variable('weights',
                                       shape=weights_shape,
                                       dtype=dtype,
                                       initializer=weights_initializer,
                                       regularizer=weights_regularizer,
                                       collections=weights_collections)
    if len(static_shape) > 2:
      # Reshape inputs
      inputs = array_ops.reshape(inputs, [-1, num_input_units])
    outputs = standard_ops.matmul(inputs, weights)
    if normalizer_fn:
      normalizer_params = normalizer_params or {}
      normalizer_params['variables_collections'] = normalizer_params.get(
          'variables_collections', variables_collections)
      outputs = normalizer_fn(outputs, **normalizer_params)
    else:
      if biases_initializer is not None:
        biases_collections = utils.get_variable_collections(
            variables_collections, 'biases')
        biases = variables.model_variable('biases',
                                          shape=[num_outputs,],
                                          dtype=dtype,
                                          initializer=biases_initializer,
                                          regularizer=biases_regularizer,
                                          collections=biases_collections)
        outputs = nn.bias_add(outputs, biases)
    if len(static_shape) > 2:
      # Reshape back outputs
      outputs = array_ops.reshape(outputs, array_ops.pack(out_shape))
      outputs.set_shape(static_shape)
    if activation_fn:
      outputs = activation_fn(outputs)
    return utils.collect_named_outputs(outputs_collections, sc.name, outputs)


def legacy_fully_connected(x,
                           num_output_units,
                           activation_fn=None,
                           weight_init=initializers.xavier_initializer(),
                           bias_init=init_ops.zeros_initializer,
                           name=None,
                           weight_collections=(ops.GraphKeys.WEIGHTS,),
                           bias_collections=(ops.GraphKeys.BIASES,),
                           output_collections=(ops.GraphKeys.ACTIVATIONS,),
                           trainable=True,
                           weight_regularizer=None,
                           bias_regularizer=None):
  # pylint: disable=anomalous-backslash-in-string
  r"""Adds the parameters for a fully connected layer and returns the output.

  A fully connected layer is generally defined as a matrix multiply:
  `y = f(w * x + b)` where `f` is given by `activation_fn`. If
  `activation_fn` is `None`, the result of `y = w * x + b` is
  returned.

  If `x` has shape [\\\(\\text{dim}_0, \\text{dim}_1, ..., \\text{dim}_n\\\)]
  with more than 2 dimensions (\\\(n > 1\\\)), then we repeat the matrix
  multiply along the first dimensions. The result r is a tensor of shape
  [\\\(\\text{dim}_0, ..., \\text{dim}_{n-1},\\\) `num_output_units`],
  where \\\( r_{i_0, ..., i_{n-1}, k} =
  \\sum_{0 \\leq j < \\text{dim}_n} x_{i_0, ... i_{n-1}, j} \cdot w_{j, k}\\\).
  This is accomplished by reshaping `x` to 2-D
  [\\\(\\text{dim}_0 \\cdot ... \\cdot \\text{dim}_{n-1}, \\text{dim}_n\\\)]
  before the matrix multiply and afterwards reshaping it to
  [\\\(\\text{dim}_0, ..., \\text{dim}_{n-1},\\\) `num_output_units`].

  This op creates `w` and optionally `b`. Bias (`b`) can be disabled by setting
  `bias_init` to `None`.

  The variable creation is compatible with `tf.variable_scope` and so can be
  reused with `tf.variable_scope` or `tf.make_template`.

  Most of the details of variable creation can be controlled by specifying the
  initializers (`weight_init` and `bias_init`) and in which collections to place
  the created variables (`weight_collections` and `bias_collections`; note that
  the variables are always added to the `VARIABLES` collection). The output of
  the layer can be placed in custom collections using `output_collections`.
  The collections arguments default to `WEIGHTS`, `BIASES` and `ACTIVATIONS`,
  respectively.

  A per layer regularization can be specified by setting `weight_regularizer`
  and `bias_regularizer`, which are applied to the weights and biases
  respectively, and whose output is added to the `REGULARIZATION_LOSSES`
  collection.

  Args:
    x: The input `Tensor`.
    num_output_units: The size of the output.
    activation_fn: A function that requires a single Tensor that is applied as a
      non-linearity. If None is used, do not apply any activation.
    weight_init: An optional weight initialization, defaults to
      `xavier_initializer`.
    bias_init: An initializer for the bias, defaults to 0. Set to `None` in
      order to disable bias.
    name: The name for this operation is used to name operations and to find
      variables. If specified it must be unique for this scope, otherwise a
      unique name starting with "fully_connected" will be created.  See
      `tf.variable_op_scope` for details.
    weight_collections: List of graph collections to which weights are added.
    bias_collections: List of graph collections to which biases are added.
    output_collections: List of graph collections to which outputs are added.
    trainable: If `True` also add variables to the graph collection
      `GraphKeys.TRAINABLE_VARIABLES` (see tf.Variable).
    weight_regularizer: A regularizer like the result of
      `l1_regularizer` or `l2_regularizer`. Used for weights.
    bias_regularizer: A regularizer like the result of
      `l1_regularizer` or `l2_regularizer`. Used for biases.

  Returns:
    The output of the fully connected layer.

  Raises:
    ValueError: if x has rank less than 2 or if its last dimension is not set.
  """
  with variable_scope.variable_op_scope([x], name, 'fully_connected'):
    dims = x.get_shape().dims
    if dims is None:
      raise ValueError('dims of x must be known but is None')
    if len(dims) < 2:
      raise ValueError('rank of x must be at least 2 not: %d' % len(dims))
    num_input_units = dims[-1].value
    if num_input_units is None:
      raise ValueError('last dimension of x must be known but is None')
    dtype = x.dtype.base_dtype

    weight_collections = set(list(weight_collections or []) +
                             [ops.GraphKeys.VARIABLES])
    w = variable_scope.get_variable('weights',
                                    shape=[num_input_units, num_output_units],
                                    dtype=dtype,
                                    initializer=weight_init,
                                    collections=weight_collections,
                                    regularizer=weight_regularizer,
                                    trainable=trainable)
    x_2_dim = x if len(dims) <= 2 else array_ops.reshape(x,
                                                         [-1, num_input_units])
    y = standard_ops.matmul(x_2_dim, w)

    if bias_init is not None:
      bias_collections = set(list(bias_collections or []) +
                             [ops.GraphKeys.VARIABLES])
      b = variable_scope.get_variable('bias',
                                      shape=[num_output_units],
                                      dtype=dtype,
                                      initializer=bias_init,
                                      collections=bias_collections,
                                      regularizer=bias_regularizer,
                                      trainable=trainable)

      y = nn.bias_add(y, b)

    if len(dims) > 2:
      out_shape = array_ops.unpack(array_ops.shape(x))
      out_shape[-1] = num_output_units

      y = array_ops.reshape(y, array_ops.pack(out_shape))

      static_shape = x.get_shape().as_list()
      static_shape[-1] = num_output_units
      y.set_shape(static_shape)

    return _apply_activation(y, activation_fn, output_collections)


def legacy_convolution2d(x,
                         num_output_channels,
                         kernel_size,
                         activation_fn=None,
                         stride=(1, 1),
                         padding='SAME',
                         weight_init=initializers.xavier_initializer_conv2d(),
                         bias_init=standard_ops.zeros_initializer,
                         name=None,
                         weight_collections=(ops.GraphKeys.WEIGHTS,),
                         bias_collections=(ops.GraphKeys.BIASES,),
                         output_collections=(ops.GraphKeys.ACTIVATIONS,),
                         trainable=True,
                         weight_regularizer=None,
                         bias_regularizer=None):
  # pylint: disable=g-docstring-has-escape
  """Adds the parameters for a conv2d layer and returns the output.

  A neural network convolution layer is generally defined as:
  \\\\(y = f(conv2d(w, x) + b)\\\\) where **f** is given by `activation_fn`,
  **conv2d** is `tf.nn.conv2d` and `x` has shape
  `[batch, height, width, channels]`. The output of this op is of shape
  `[batch, out_height, out_width, num_output_channels]`, where `out_width` and
  `out_height` are determined by the `padding` argument. See `conv2D` for
  details.

  This op creates `w` and optionally `b` and adds various summaries that can be
  useful for visualizing learning or diagnosing training problems. Bias can be
  disabled by setting `bias_init` to `None`.

  The variable creation is compatible with `tf.variable_scope` and so can be
  reused with `tf.variable_scope` or `tf.make_template`.

  Most of the details of variable creation can be controlled by specifying the
  initializers (`weight_init` and `bias_init`) and which collections to place
  the created variables in (`weight_collections` and `bias_collections`).

  A per layer regularization can be specified by setting `weight_regularizer`.
  This is only applied to weights and not the bias.

  Args:
    x: A 4-D input `Tensor`.
    num_output_channels: The number of output channels (i.e. the size of the
      last dimension of the output).
    kernel_size: A length 2 `list` or `tuple` containing the kernel size.
    activation_fn: A function that requires a single Tensor that is applied as a
      non-linearity.
    stride: A length 2 `list` or `tuple` specifying the stride of the sliding
      window across the image.
    padding: A `string` from: "SAME", "VALID". The type of padding algorithm to
      use.
    weight_init: An optional initialization. If not specified, uses Xavier
      initialization (see `tf.learn.xavier_initializer`).
    bias_init: An initializer for the bias, defaults to 0. Set to`None` in order
      to disable bias.
    name: The name for this operation is used to name operations and to find
      variables. If specified it must be unique for this scope, otherwise a
      unique name starting with "convolution2d" will be created.  See
      `tf.variable_op_scope` for details.
    weight_collections: List of graph collections to which weights are added.
    bias_collections: List of graph collections to which biases are added.
    output_collections: List of graph collections to which outputs are added.
    trainable: If `True` also add variables to the graph collection
      `GraphKeys.TRAINABLE_VARIABLES` (see tf.Variable).
    weight_regularizer: A regularizer like the result of
      `l1_regularizer` or `l2_regularizer`. Used for weights.
    bias_regularizer: A regularizer like the result of
      `l1_regularizer` or `l2_regularizer`. Used for biases.

  Returns:
    The result of applying a 2-D convolutional layer.

  Raises:
    ValueError: If `kernel_size` or `stride` are not length 2.
  """
  with variable_scope.variable_op_scope([x], name, 'convolution2d'):
    num_input_channels = x.get_shape().dims[3].value

    if len(kernel_size) != 2:
      raise ValueError('kernel_size must be length 2: %d ' % kernel_size)
    if len(stride) != 2:
      raise ValueError('stride must be length 2: %d' % stride)

    stride = [1, stride[0], stride[1], 1]
    shape = [kernel_size[0], kernel_size[1], num_input_channels,
             num_output_channels]
    dtype = x.dtype.base_dtype

    weight_collections = set(list(weight_collections or []) +
                             [ops.GraphKeys.VARIABLES])
    w = variable_scope.get_variable('weights',
                                    shape=shape,
                                    dtype=dtype,
                                    initializer=weight_init,
                                    collections=weight_collections,
                                    regularizer=weight_regularizer,
                                    trainable=trainable)

    y = nn.conv2d(x, w, stride, padding)

    if bias_init is not None:
      bias_collections = set(list(bias_collections or []) +
                             [ops.GraphKeys.VARIABLES])
      b = variable_scope.get_variable('bias',
                                      shape=[num_output_channels],
                                      dtype=dtype,
                                      initializer=bias_init,
                                      collections=bias_collections,
                                      regularizer=bias_regularizer,
                                      trainable=trainable)

      y = nn.bias_add(y, b)

    return _apply_activation(y, activation_fn, output_collections)


# TODO(eiderm): Verify and fix autocomplete in colab (also relu6).
legacy_relu = functools.partial(legacy_fully_connected, activation_fn=nn.relu)
legacy_relu6 = functools.partial(legacy_fully_connected, activation_fn=nn.relu6)
# Simple alias for fully_connected which removes the activation_fn parameter.
legacy_linear = functools.partial(legacy_fully_connected, activation_fn=None)

# Simple alias for convolution2d.
conv2d = convolution2d
