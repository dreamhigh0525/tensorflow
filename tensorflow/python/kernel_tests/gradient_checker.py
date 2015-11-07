"""Gradient checker for any ops, graphs.

The gradient checker verifies numerically that an op/graph properly
computes the gradients
"""
import tensorflow.python.platform

import numpy as np

from tensorflow.python.framework import ops
from tensorflow.python.framework import types
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import constant_op
from tensorflow.python.ops import gradients
from tensorflow.python.platform import logging


def _Product(t):
  if isinstance(t, int):
    return t
  else:
    y = 1
    for x in t:
      y *= x
    return y


def _ComputeTheoricalJacobian(x, x_shape, x_data, dy, dy_shape, dx):
  """Computes the theoretical Jacobian for dy/dx.

  Computes the theoretical Jacobian using the ops generated by
  ComputeGradient().

  Args:
    x: the tensor "x".
    x_shape: the dimensions of x as a tuple or an array of ints.
    x_data: a numpy parray as the input data for x
    dy: the tensor "dy".
    dy_shape: the dimensions of dy as a tuple or an array of ints.
    dx: Tensor or IndexedSlices representing dx

  Returns:
    A 2-d numpy array representing the Jacobian for dy/dx. It has "x_size" rows
    and "dy_size" columns where "x_size" is the number of elements in x and
    "dy_size" is the number of elements in dy.
  """
  # To compute the jacobian, we treat x and y are one-dimensional vectors
  x_size = _Product(x_shape)
  x_val_size = _Product(x_shape[1:])  # This is used for sparse gradients
  dy_size = _Product(dy_shape)

  jacobian = np.zeros((x_size, dy_size), dtype=x_data.dtype)
  # For each of the entry of dy, we set this to be 1 and
  # everything else to be 0 and compute the backprop -- this will give us one
  # one column of the Jacobian matrix.
  for col in range(0, dy_size):
    dy_data = np.zeros(dy_shape, dtype=x_data.dtype)
    dy_data.flat[col] = 1
    sess = ops.get_default_session()
    if isinstance(dx, ops.IndexedSlices):
      backprop_indices, backprop_values = sess.run(
          [dx.indices, dx.values], feed_dict={x: x_data, dy: dy_data})
      for i, v in zip(backprop_indices, backprop_values):
        r_begin = i * x_val_size
        r_end = r_begin + x_val_size
        jacobian[r_begin:r_end, col] += v.flat
    else:
      assert isinstance(dx, ops.Tensor), "dx = " + str(dx)
      backprop = sess.run(dx, feed_dict={x: x_data, dy: dy_data})
      jacobian[:, col] = backprop.reshape(x_size)

  logging.vlog(1, "Theoretical Jacobian =\n%s", jacobian)
  return jacobian


def _ComputeNumericJacobian(x, x_shape, x_data, y, y_shape, delta):
  """Computes the numeric Jacobian for dy/dx.

  Computes the numeric Japcobian by slightly perturbing the inputs and
  measuring the differences on the output.

  Args:
    x: the tensor "x".
    x_shape: the dimensions of x as a tuple or an array of ints.
    x_data: a numpy array as the input data for x
    y: the tensor "y".
    y_shape: the dimensions of y as a tuple or an array of ints.
    delta: the amount of perturbation we give to the input

  Returns:
    A 2-d numpy array representing the Jacobian for dy/dx. It has "x_size" rows
    and "y_size" columns where "x_size" is the number of elements in x and
    "y_size" is the number of elements in y.
  """

  # To compute the jacobian, we treat x and y are one-dimensional vectors
  x_size = _Product(x_shape)
  y_size = _Product(y_shape)

  jacobian = np.zeros((x_size, y_size), dtype=x_data.dtype)
  # For each of the entry of x, we slightly perturbs this by adding and
  # subtracting a delta and then compute difference between the outputs. This
  # will give us one row of the Jacobian matrix.
  for row in range(0, x_size):
    x_pos = x_data.copy()
    x_pos.flat[row] += delta
    y_pos = y.eval(feed_dict={x: x_pos})
    x_neg = x_data.copy()
    x_neg.flat[row] -= delta
    y_neg = y.eval(feed_dict={x: x_neg})
    diff = (y_pos - y_neg) / (2 * delta)
    jacobian[row, :] = diff.reshape(y_size)

  logging.vlog(1, "Numeric Jacobian =\n%s", jacobian)
  return jacobian


def _ComputeDxAndDy(x, y, y_shape):
  """Returns a node to compute gradient of x wrt y."""
  # We make up a dy so that we can compute the gradients. We don't really use
  # the value of dy -- we will always feed it. We need to add an identity node
  # so that we can always feed it properly. Otherwise, for the Add operation,
  # dx is the same as dy and we cannot fetch the tensor that we are feeding.
  with x.graph.as_default():
    dy_orig = constant_op.constant(1.0, shape=y_shape, dtype=y.dtype)
    dy = array_ops.identity(dy_orig)
  # We compute the gradients for x wrt. y
  grads = gradients.gradients(y, x, dy)
  assert len(grads) == 1
  return grads[0], dy_orig


def _ComputeGradient(x, x_shape, dx, y, y_shape, dy,
                     x_init_value=None, delta=1e-3):
  """Computes the theoretical and numerical jacobian."""
  t = types.as_dtype(x.dtype)
  allowed_types = [types.float32, types.float64]
  assert t.base_dtype in allowed_types, "Don't support type %s for x" % t.name
  t2 = types.as_dtype(y.dtype)
  assert t2.base_dtype in allowed_types, "Don't support type %s for y" % t2.name

  if x_init_value is not None:
    i_shape = list(x_init_value.shape)
    assert(list(x_shape) == i_shape), "x_shape = %s, init_data shape = %s" % (
        x_shape, i_shape)
    x_data = x_init_value
  else:
    if t == types.float32:
      dtype = np.float32
    else:
      dtype = np.float64
    x_data = np.asfarray(np.random.random_sample(x_shape), dtype=dtype)

  jacob_t = _ComputeTheoricalJacobian(x, x_shape, x_data, dy, y_shape, dx)
  jacob_n = _ComputeNumericJacobian(x, x_shape, x_data, y, y_shape, delta)
  return jacob_t, jacob_n


def _ComputeGradientList(
    x, x_shape, y, y_shape, x_init_value=None, delta=1e-3, init_targets=None):
  """Compute gradients for a list of x values."""
  assert isinstance(x, list)
  dx, dy = zip(*[_ComputeDxAndDy(xi, y, y_shape) for xi in x])

  if init_targets is not None:
    assert isinstance(init_targets, (list, tuple))
    for init in init_targets:
      init.run()
  if x_init_value is None:
    x_init_value = [None] * len(x)
  ret = [_ComputeGradient(xi, x_shapei, dxi, y, y_shape, dyi,
                          x_init_valuei, delta)
         for xi, x_shapei, dxi, dyi, x_init_valuei in
         zip(x, x_shape, dx, dy, x_init_value)]
  return ret


def ComputeGradient(
    x, x_shape, y, y_shape, x_init_value=None, delta=1e-3, init_targets=None):
  """Computes and returns the theoretical and numerical Jacobian.

  Args:
    x: a tensor or list of tensors
    x_shape: the dimensions of x as a tuple or an array of ints. If x is a list,
    then this is the list of shapes.
    y: a tensor
    y_shape: the dimensions of y as a tuple or an array of ints.
    x_init_value: (optional) a numpy array of the same shape as "x"
      representing the initial value of x. If x is a list, this should be a list
      of numpy arrays.  If this is none, the function will pick a random tensor
      as the initial value.
    delta: (optional) the amount of perturbation.
    init_targets: list of targets to run to initialize model params.
      TODO(mrry): remove this argument.

  Returns:
    Two 2-d numpy arrays representing the theoretical and numerical
    Jacobian for dy/dx. Each has "x_size" rows and "y_size" columns
    where "x_size" is the number of elements in x and "y_size" is the
    number of elements in y. If x is a list, returns a list of two numpy arrays.
  """
  if isinstance(x, list):
    return _ComputeGradientList(x, x_shape, y, y_shape, x_init_value,
                                delta, init_targets)
  else:
    if init_targets is not None:
      assert isinstance(init_targets, (list, tuple))
      for init in init_targets:
        init.run()
    dx, dy = _ComputeDxAndDy(x, y, y_shape)
    ret = _ComputeGradient(x, x_shape, dx, y, y_shape, dy, x_init_value, delta)
    return ret


def ComputeGradientError(
    x, x_shape, y, y_shape, x_init_value=None, delta=1e-3, init_targets=None):
  """Computes the gradient error.

  Computes the maximum error for dy/dx between the computed Jacobian and the
  numerically estimated Jacobian.

  This function will modify the tensors passed in as it adds more operations
  and hence changing the consumers of the operations of the input tensors.

  This function adds operations to the current session. To compute the error
  using a particular device, such as a GPU, use the standard methods for
  setting a device (e.g. using with sess.graph.device() or setting a device
  function in the session constructor).

  Args:
    x: a tensor or list of tensors
    x_shape: the dimensions of x as a tuple or an array of ints. If x is a list,
    then this is the list of shapes.
    y: a tensor
    y_shape: the dimensions of y as a tuple or an array of ints.
    x_init_value: (optional) a numpy array of the same shape as "x"
      representing the initial value of x. If x is a list, this should be a list
      of numpy arrays.  If this is none, the function will pick a random tensor
      as the initial value.
    delta: (optional) the amount of perturbation.
    init_targets: list of targets to run to initialize model params.
      TODO(mrry): Remove this argument.

  Returns:
    The maximum error in between the two Jacobians.
  """
  grad = ComputeGradient(x, x_shape, y, y_shape, x_init_value,
                         delta, init_targets)
  if isinstance(grad, tuple):
    grad = [grad]
  return max(np.fabs(j_t - j_n).max() for j_t, j_n in grad)
