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
"""Keras initializers for TF 2.
"""
# pylint: disable=g-classes-have-attributes
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.keras import backend
from tensorflow.python.ops import init_ops_v2
from tensorflow.python.util.tf_export import keras_export


@keras_export('keras.initializers.Initializer')
class Initializer(object):
  """Initializer base class: all Keras initializers inherit from this class.

  Initializers should implement a `__call__` method with the following
  signature:

  ```python
  def __call__(self, shape, dtype=None)`:
    # returns a tensor of shape `shape` and dtype `dtype`
    # containing values drawn from a distribution of your choice.
  ```

  Optionally, you an also implement the method `get_config` and the class
  method `from_config` in order to support serialization -- just like with
  any Keras object.

  Here's a simple example: a random normal initializer.

  ```python
  import tensorflow as tf

  class ExampleRandomNormal(tf.keras.initializers.Initializer):

    def __init__(self, mean, stddev):
      self.mean = mean
      self.stddev = stddev

    def __call__(self, shape, dtype=None)`:
      return tf.random.normal(
          shape, mean=self.mean, stddev=self.stddev, dtype=dtype)

    def get_config(self):  # To support serialization
      return {"mean": self.mean, "stddev": self.stddev}
  ```

  Note that we don't have to implement `from_config` in the example above since
  the constructor arguments of the class the keys in the config returned by
  `get_config` are the same. In this case, the default `from_config`
  works fine.
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized as specified by the initializer.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor.
    """
    raise NotImplementedError

  def get_config(self):
    """Returns the configuration of the initializer as a JSON-serializable dict.

    Returns:
      A JSON-serializable Python dict.
    """
    return {}

  @classmethod
  def from_config(cls, config):
    """Instantiates an initializer from a configuration dictionary.

    Example:

    ```python
    initializer = RandomUniform(-1, 1)
    config = initializer.get_config()
    initializer = RandomUniform.from_config(config)
    ```

    Args:
      config: A Python dictionary, the output of `get_config`.

    Returns:
      A `tf.keras.initializers.Initializer` instance.
    """
    config.pop('dtype', None)
    return cls(**config)


@keras_export('keras.initializers.Zeros', 'keras.initializers.zeros', v1=[])
class Zeros(init_ops_v2.Zeros, Initializer):
  """Initializer that generates tensors initialized to 0.

  Also available via the shortcut function `tf.keras.initializers.zeros`.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.Zeros()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.Zeros()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized as specified by the initializer.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. Only numeric or boolean dtypes are
       supported. If not specified, `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`).
    """
    return super(Zeros, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.Ones', 'keras.initializers.ones', v1=[])
class Ones(init_ops_v2.Ones, Initializer):
  """Initializer that generates tensors initialized to 1.

  Also available via the shortcut function `tf.keras.initializers.ones`.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.Ones()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.Ones()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized as specified by the initializer.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. Only numeric or boolean dtypes are
       supported. If not specified, `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`).
    """
    return super(Ones, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.Constant',
              'keras.initializers.constant',
              v1=[])
class Constant(Initializer):
  """Initializer that generates tensors with constant values.

  Also available via the shortcut function `tf.keras.initializers.constant`.

  Only scalar values are allowed.
  The constant value provided must be convertible to the dtype requested
  when calling the initializer.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.Constant(3.)
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.Constant(3.)
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    value: A Python scalar.
  """

  def __init__(self, value=0):
    self.value = value

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized to `self.value`.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. If not specified,
       `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`).
    """
    return constant_op.constant(
        self.value, dtype=_get_dtype(dtype), shape=shape)

  def get_config(self):
    return {'value': self.value}


@keras_export('keras.initializers.RandomUniform',
              'keras.initializers.random_uniform',
              v1=[])
class RandomUniform(init_ops_v2.RandomUniform, Initializer):
  """Initializer that generates tensors with a uniform distribution.

  Also available via the shortcut function
  `tf.keras.initializers.random_uniform`.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.RandomUniform(minval=0., maxval=1.)
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.RandomUniform(minval=0., maxval=1.)
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    minval: A python scalar or a scalar tensor. Lower bound of the range of
      random values to generate (inclusive).
    maxval: A python scalar or a scalar tensor. Upper bound of the range of
      random values to generate (exclusive).
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized as specified by the initializer.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. Only floating point and integer
      types are supported. If not specified,
        `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`).
    """
    return super(RandomUniform, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.RandomNormal',
              'keras.initializers.random_normal',
              v1=[])
class RandomNormal(init_ops_v2.RandomNormal, Initializer):
  """Initializer that generates tensors with a normal distribution.

  Also available via the shortcut function
  `tf.keras.initializers.random_normal`.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.RandomNormal(mean=0., stddev=1.)
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.RandomNormal(mean=0., stddev=1.)
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    mean: a python scalar or a scalar tensor. Mean of the random values to
      generate.
    stddev: a python scalar or a scalar tensor. Standard deviation of the random
      values to generate.
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized to random normal values.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. Only floating point types are
       supported. If not specified, `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`)
    """
    return super(RandomNormal, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.TruncatedNormal',
              'keras.initializers.truncated_normal',
              v1=[])
class TruncatedNormal(init_ops_v2.TruncatedNormal, Initializer):
  """Initializer that generates a truncated normal distribution.

  Also available via the shortcut function
  `tf.keras.initializers.truncated_normal`.

  The values generated are similar to values from a
  `tf.keras.initializers.RandomNormal` initializer except that values more
  than two standard deviations from the mean are
  discarded and re-drawn.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.TruncatedNormal(mean=0., stddev=1.)
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.TruncatedNormal(mean=0., stddev=1.)
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    mean: a python scalar or a scalar tensor. Mean of the random values
      to generate.
    stddev: a python scalar or a scalar tensor. Standard deviation of the
      random values to generate.
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized to random normal values (truncated).

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. Only floating point types are
       supported. If not specified, `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`)
    """
    return super(TruncatedNormal, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.VarianceScaling',
              'keras.initializers.variance_scaling',
              v1=[])
class VarianceScaling(init_ops_v2.VarianceScaling, Initializer):
  """Initializer capable of adapting its scale to the shape of weights tensors.

  Also available via the shortcut function
  `tf.keras.initializers.variance_scaling`.

  With `distribution="truncated_normal" or "untruncated_normal"`, samples are
  drawn from a truncated/untruncated normal distribution with a mean of zero and
  a standard deviation (after truncation, if used) `stddev = sqrt(scale / n)`
  where n is:

  - number of input units in the weight tensor, if mode = "fan_in"
  - number of output units, if mode = "fan_out"
  - average of the numbers of input and output units, if mode = "fan_avg"

  With `distribution="uniform"`, samples are drawn from a uniform distribution
  within [-limit, limit], with `limit = sqrt(3 * scale / n)`.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.VarianceScaling(
  ... scale=0.1, mode='fan_in', distribution='uniform')
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.VarianceScaling(
  ... scale=0.1, mode='fan_in', distribution='uniform')
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    scale: Scaling factor (positive float).
    mode: One of "fan_in", "fan_out", "fan_avg".
    distribution: Random distribution to use. One of "truncated_normal",
      "untruncated_normal" and  "uniform".
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized as specified by the initializer.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. Only floating point types are
       supported. If not specified, `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`)
    """
    return super(VarianceScaling, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.Orthogonal',
              'keras.initializers.orthogonal',
              v1=[])
class Orthogonal(init_ops_v2.Orthogonal, Initializer):
  """Initializer that generates an orthogonal matrix.

  Also available via the shortcut function `tf.keras.initializers.orthogonal`.

  If the shape of the tensor to initialize is two-dimensional, it is initialized
  with an orthogonal matrix obtained from the QR decomposition of a matrix of
  random numbers drawn from a normal distribution.
  If the matrix has fewer rows than columns then the output will have orthogonal
  rows. Otherwise, the output will have orthogonal columns.

  If the shape of the tensor to initialize is more than two-dimensional,
  a matrix of shape `(shape[0] * ... * shape[n - 2], shape[n - 1])`
  is initialized, where `n` is the length of the shape vector.
  The matrix is subsequently reshaped to give a tensor of the desired shape.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.Orthogonal()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.Orthogonal()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    gain: multiplicative factor to apply to the orthogonal matrix
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.

  References:
      [Saxe et al., 2014](https://openreview.net/forum?id=_wzZwKpTDF_9C)
      ([pdf](https://arxiv.org/pdf/1312.6120.pdf))
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized to an orthogonal matrix.

    Args:
      shape: Shape of the tensor.
      dtype: Optional dtype of the tensor. Only floating point types are
        supported. If not specified, `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`)
    """
    return super(Orthogonal, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.Identity',
              'keras.initializers.identity',
              v1=[])
class Identity(init_ops_v2.Identity, Initializer):
  """Initializer that generates the identity matrix.

  Also available via the shortcut function `tf.keras.initializers.identity`.

  Only usable for generating 2D matrices.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.Identity()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.Identity()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    gain: Multiplicative factor to apply to the identity matrix.
  """

  def __call__(self, shape, dtype=None):
    """Returns a tensor object initialized to a 2D identity matrix.

    Args:
      shape: Shape of the tensor. It should have exactly rank 2.
      dtype: Optional dtype of the tensor. Only floating point types are
       supported. If not specified, `tf.keras.backend.floatx()` is used,
       which default to `float32` unless you configured it otherwise
       (via `tf.keras.backend.set_floatx(float_dtype)`)
    """
    return super(Identity, self).__call__(shape, dtype=_get_dtype(dtype))


@keras_export('keras.initializers.GlorotUniform',
              'keras.initializers.glorot_uniform',
              v1=[])
class GlorotUniform(VarianceScaling):
  """The Glorot uniform initializer, also called Xavier uniform initializer.

  Also available via the shortcut function
  `tf.keras.initializers.glorot_uniform`.

  Draws samples from a uniform distribution within [-limit, limit] where `limit`
  is `sqrt(6 / (fan_in + fan_out))` where `fan_in` is the number of input units
  in the weight tensor and `fan_out` is the number of output units in the weight
  tensor.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.GlorotUniform()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.GlorotUniform()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.

  References:
      [Glorot et al., 2010](http://proceedings.mlr.press/v9/glorot10a.html)
      ([pdf](http://jmlr.org/proceedings/papers/v9/glorot10a/glorot10a.pdf))
  """

  def __init__(self, seed=None):
    super(GlorotUniform, self).__init__(
        scale=1.0,
        mode='fan_avg',
        distribution='uniform',
        seed=seed)

  def get_config(self):
    return {'seed': self.seed}


@keras_export('keras.initializers.GlorotNormal',
              'keras.initializers.glorot_normal',
              v1=[])
class GlorotNormal(VarianceScaling):
  """The Glorot normal initializer, also called Xavier normal initializer.

  Also available via the shortcut function
  `tf.keras.initializers.glorot_normal`.

  Draws samples from a truncated normal distribution centered on 0 with `stddev
  = sqrt(2 / (fan_in + fan_out))` where `fan_in` is the number of input units in
  the weight tensor and `fan_out` is the number of output units in the weight
  tensor.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.GlorotNormal()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.GlorotNormal()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Args:
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.

  References:
      [Glorot et al., 2010](http://proceedings.mlr.press/v9/glorot10a.html)
      ([pdf](http://jmlr.org/proceedings/papers/v9/glorot10a/glorot10a.pdf))
  """

  def __init__(self, seed=None):
    super(GlorotNormal, self).__init__(
        scale=1.0,
        mode='fan_avg',
        distribution='truncated_normal',
        seed=seed)

  def get_config(self):
    return {'seed': self.seed}


@keras_export('keras.initializers.LecunNormal',
              'keras.initializers.lecun_normal',
              v1=[])
class LecunNormal(VarianceScaling):
  """Lecun normal initializer.

   Also available via the shortcut function
  `tf.keras.initializers.lecun_normal`.

  Initializers allow you to pre-specify an initialization strategy, encoded in
  the Initializer object, without knowing the shape and dtype of the variable
  being initialized.

  Draws samples from a truncated normal distribution centered on 0 with `stddev
  = sqrt(1 / fan_in)` where `fan_in` is the number of input units in the weight
  tensor.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.LecunNormal()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.LecunNormal()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Arguments:
    seed: A Python integer. Used to seed the random generator.

  References:
      - Self-Normalizing Neural Networks,
      [Klambauer et al., 2017]
      (https://papers.nips.cc/paper/6698-self-normalizing-neural-networks)
      ([pdf]
      (https://papers.nips.cc/paper/6698-self-normalizing-neural-networks.pdf))
      - Efficient Backprop,
      [Lecun et al., 1998](http://yann.lecun.com/exdb/publis/pdf/lecun-98b.pdf)
  """

  def __init__(self, seed=None):
    super(LecunNormal, self).__init__(
        scale=1., mode='fan_in', distribution='truncated_normal', seed=seed)

  def get_config(self):
    return {'seed': self.seed}


@keras_export('keras.initializers.LecunUniform',
              'keras.initializers.lecun_uniform',
              v1=[])
class LecunUniform(VarianceScaling):
  """Lecun uniform initializer.

   Also available via the shortcut function
  `tf.keras.initializers.lecun_uniform`.

  Draws samples from a uniform distribution within [-limit, limit] where `limit`
  is `sqrt(3 / fan_in)` where `fan_in` is the number of input units in the
  weight tensor.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.LecunUniform()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.LecunUniform()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Arguments:
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.

  References:
      - Self-Normalizing Neural Networks,
      [Klambauer et al., 2017](https://papers.nips.cc/paper/6698-self-normalizing-neural-networks) # pylint: disable=line-too-long
      ([pdf](https://papers.nips.cc/paper/6698-self-normalizing-neural-networks.pdf))
      - Efficient Backprop,
      [Lecun et al., 1998](http://yann.lecun.com/exdb/publis/pdf/lecun-98b.pdf)
  """

  def __init__(self, seed=None):
    super(LecunUniform, self).__init__(
        scale=1., mode='fan_in', distribution='uniform', seed=seed)

  def get_config(self):
    return {'seed': self.seed}


@keras_export('keras.initializers.HeNormal',
              'keras.initializers.he_normal',
              v1=[])
class HeNormal(VarianceScaling):
  """He normal initializer.

   Also available via the shortcut function
  `tf.keras.initializers.he_normal`.

  It draws samples from a truncated normal distribution centered on 0 with
  `stddev = sqrt(2 / fan_in)` where `fan_in` is the number of input units in the
  weight tensor.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.HeNormal()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.HeNormal()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Arguments:
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.

  References:
      [He et al., 2015](https://www.cv-foundation.org/openaccess/content_iccv_2015/html/He_Delving_Deep_into_ICCV_2015_paper.html) # pylint: disable=line-too-long
      ([pdf](https://www.cv-foundation.org/openaccess/content_iccv_2015/papers/He_Delving_Deep_into_ICCV_2015_paper.pdf))
  """

  def __init__(self, seed=None):
    super(HeNormal, self).__init__(
        scale=2., mode='fan_in', distribution='truncated_normal', seed=seed)

  def get_config(self):
    return {'seed': self.seed}


@keras_export('keras.initializers.HeUniform',
              'keras.initializers.he_uniform',
              v1=[])
class HeUniform(VarianceScaling):
  """He uniform variance scaling initializer.

   Also available via the shortcut function
  `tf.keras.initializers.he_uniform`.

  Draws samples from a uniform distribution within [-limit, limit] where `limit`
  is `sqrt(6 / fan_in)` where `fan_in` is the number of input units in the
  weight tensor.

  Examples:

  >>> # Standalone usage:
  >>> initializer = tf.keras.initializers.HeUniform()
  >>> values = initializer(shape=(2, 2))

  >>> # Usage in a Keras layer:
  >>> initializer = tf.keras.initializers.HeUniform()
  >>> layer = tf.keras.layers.Dense(3, kernel_initializer=initializer)

  Arguments:
    seed: A Python integer. An initializer created with a given seed will
      always produce the same random tensor for a given shape and dtype.

  References:
      [He et al., 2015](https://www.cv-foundation.org/openaccess/content_iccv_2015/html/He_Delving_Deep_into_ICCV_2015_paper.html) # pylint: disable=line-too-long
      ([pdf](https://www.cv-foundation.org/openaccess/content_iccv_2015/papers/He_Delving_Deep_into_ICCV_2015_paper.pdf))
  """

  def __init__(self, seed=None):
    super(HeUniform, self).__init__(
        scale=2., mode='fan_in', distribution='uniform', seed=seed)

  def get_config(self):
    return {'seed': self.seed}


def _get_dtype(dtype):
  if dtype is None:
    dtype = backend.floatx()
  return dtypes.as_dtype(dtype)
