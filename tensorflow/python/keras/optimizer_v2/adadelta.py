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

"""Adadelta for TensorFlow."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.keras.optimizer_v2 import optimizer_v2
from tensorflow.python.ops import math_ops
from tensorflow.python.training import training_ops


class Adadelta(optimizer_v2.OptimizerV2):
  r"""Optimizer that implements the Adadelta algorithm.

  Adadelta optimization is a stochastic gradient descent method that is based on
  adaptive learning rate per dimension to address two drawbacks:
    1) the continual decay of learning rates throughout training
    2) the need for a manually selected global learning rate

  Two accumulation steps are required:
    1) the accumulation of gradients squared,
    2) the accumulation of updates squared.

  Initialization:

  $$accum_g_0 := 0 \text{(Initialize gradient 2nd order moment vector)}$$
  $$accum_x_0 := 0 \text{(Initialize variable update 2nd order moment vector)}$$

  $$t := t + 1$$
  $$accum_g_t := rho * accum_g_{t-1} + (1 - rho) * g * g$$
  $$delta = -\sqrt{accum_x_{t-1}} / (\sqrt{accum_g_{t-1}} + \epsilon)$$
  $$accum_x_t := rho * accum_x_{t-1} + (1 - rho) * delta * delta$$

  References
    See [M. D. Zeiler](http://arxiv.org/abs/1212.5701)
      ([pdf](http://arxiv.org/pdf/1212.5701v1.pdf))

  """

  def __init__(self,
               learning_rate=0.001,
               rho=0.95,
               epsilon=1e-8,
               name='Adadelta'):
    """Construct a new Adadelta optimizer.

    Adadelta is a more robust extension of Adagrad that adapts learning rates
    based on a moving window of gradient updates, instead of accumulating all
    past gradients. This way, Adadelta continues learning even when many updates
    have been done. Compared to Adagrad, in the original version of Adadelta you
    don't have to set an initial learning rate. In this version, initial
    learning rate can be set, as in most other Keras optimizers.

    Args:
      learning_rate: A `Tensor` or a floating point value. The learning rate.
        To match the exact form in the original paper use 1.0.
      rho: A `Tensor` or a floating point value. The decay rate.
      epsilon: A `Tensor` or a floating point value.  A constant epsilon used
               to better conditioning the grad update.
      name: Optional name prefix for the operations created when applying
        gradients.  Defaults to "Adadelta".

    @compatibility(eager)
    When eager execution is enabled, `learning_rate`, `rho`, and `epsilon` can
    each be a callable that takes no arguments and returns the actual value to
    use. This can be useful for changing these values across different
    invocations of optimizer functions.
    @end_compatibility
    """
    super(Adadelta, self).__init__(name)
    self._set_hyper('learning_rate', learning_rate)
    self._set_hyper('rho', rho)
    self._set_hyper('epsilon', epsilon)

  def _create_slots(self, var_list):
    for v in var_list:
      self.add_slot(v, 'accum_grad')
      self.add_slot(v, 'accum_var')

  def _resource_apply_dense(self, grad, var):
    accum_grad = self.get_slot(var, 'accum_grad')
    accum_var = self.get_slot(var, 'accum_var')
    return training_ops.resource_apply_adadelta(
        var.handle,
        accum_grad.handle,
        accum_var.handle,
        math_ops.cast(self._get_hyper('learning_rate'), grad.dtype.base_dtype),
        math_ops.cast(self._get_hyper('rho'), grad.dtype.base_dtype),
        math_ops.cast(self._get_hyper('epsilon'), grad.dtype.base_dtype),
        grad,
        use_locking=self._use_locking)

  def _resource_apply_sparse(self, grad, var, indices):
    accum_grad = self.get_slot(var, 'accum_grad')
    accum_var = self.get_slot(var, 'accum_var')
    return training_ops.resource_sparse_apply_adadelta(
        var.handle,
        accum_grad.handle,
        accum_var.handle,
        math_ops.cast(self._get_hyper('learning_rate'), grad.dtype.base_dtype),
        math_ops.cast(self._get_hyper('rho'), grad.dtype.base_dtype),
        math_ops.cast(self._get_hyper('epsilon'), grad.dtype.base_dtype),
        grad,
        indices,
        use_locking=self._use_locking)

  def get_config(self):
    config = super(Adadelta, self).get_config()
    config.update({
        'learning_rate': self._serialize_hyperparameter('learning_rate'),
        'rho': self._serialize_hyperparameter('rho'),
        'epsilon': self._serialize_hyperparameter('epsilon'),
    })
    return config
