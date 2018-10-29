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

"""Version 2 of class Optimizer."""
# pylint: disable=g-bad-name

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import abc

from tensorflow.python.eager import backprop
from tensorflow.python.eager import context
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import ops
from tensorflow.python.keras import initializers
from tensorflow.python.keras.engine import base_layer
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import gradients
from tensorflow.python.ops import variable_scope
from tensorflow.python.ops import variables
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.training import distribution_strategy_context
from tensorflow.python.training import optimizer as optimizer_v1
from tensorflow.python.util import nest


class OptimizerV2(optimizer_v1.Optimizer):
  """Updated base class for optimizers.

  This class defines the API to add Ops to train a model.  You never use this
  class directly, but instead instantiate one of its subclasses such as
  `GradientDescentOptimizer`, `AdagradOptimizer`, or `MomentumOptimizer`.

  ### Usage

  ```python
  # Create an optimizer with the desired parameters.
  opt = GradientDescentOptimizer(learning_rate=0.1)
  # Add Ops to the graph to minimize a cost by updating a list of variables.
  # "cost" is a Tensor, and the list of variables contains tf.Variable
  # objects.
  opt_op = opt.minimize(cost, var_list=<list of variables>)
  ```

  In the training program you will just have to run the returned Op.

  ```python
  # Execute opt_op to do one step of training:
  opt_op.run()
  ```

  ### Processing gradients before applying them.

  Calling `minimize()` takes care of both computing the gradients and
  applying them to the variables.  If you want to process the gradients
  before applying them you can instead use the optimizer in three steps:

  1.  Compute the gradients with `compute_gradients()`.
  2.  Process the gradients as you wish.
  3.  Apply the processed gradients with `apply_gradients()`.

  Example:

  ```python
  # Create an optimizer.
  opt = GradientDescentOptimizer(learning_rate=0.1)

  # Compute the gradients for a list of variables.
  grads_and_vars = opt.compute_gradients(loss, <list of variables>)

  # grads_and_vars is a list of tuples (gradient, variable).  Do whatever you
  # need to the 'gradient' part, for example cap them, etc.
  capped_grads_and_vars = [(MyCapper(gv[0]), gv[1]) for gv in grads_and_vars]

  # Ask the optimizer to apply the capped gradients.
  opt.apply_gradients(capped_grads_and_vars)
  ```

  ### Slots

  Some optimizer subclasses, such as `MomentumOptimizer` and `AdagradOptimizer`
  allocate and manage additional variables associated with the variables to
  train.  These are called <i>Slots</i>.  Slots have names and you can ask the
  optimizer for the names of the slots that it uses.  Once you have a slot name
  you can ask the optimizer for the variable it created to hold the slot value.

  This can be useful if you want to log debug a training algorithm, report stats
  about the slots, etc.

  ### Hyper parameters

  These are arguments passed to the optimizer subclass constructor
  (the `__init__` method), and then passed to `self._set_hyper()`.
  They can be either regular Python values (like 1.0), tensors, or
  callables. If they are callable, the callable will be called during
  `apply_gradients()` to get the value for the hyper parameter.

  """

  def __init__(self, name):
    """Create a new Optimizer.

    This must be called by the constructors of subclasses.
    Note that Optimizer instances should not bind to a single graph,
    and so shouldn't keep Tensors as member variables. Generally
    you should be able to use the _set_hyper()/state.get_hyper()
    facility instead.

    This class in stateful and thread-compatible.

    Args:
      name: A non-empty string.  The name to use for accumulators created
        for the optimizer.

    Raises:
      ValueError: If name is malformed.
      RuntimeError: If _create_slots has been overridden instead of
          _create_vars.
    """
    self._use_locking = True
    super(OptimizerV2, self).__init__(self._use_locking, name)
    self._hyper = {}
    self._slots = {}
    self._prepared = False

  def minimize(self,
               loss,
               var_list,
               aggregation_method=None,
               colocate_gradients_with_ops=False,
               name=None,
               grad_loss=None):
    """Add operations to minimize `loss` by updating `var_list`.

    This method simply combines calls `compute_gradients()` and
    `apply_gradients()`. If you want to process the gradient before applying
    them call `compute_gradients()` and `apply_gradients()` explicitly instead
    of using this function.

    Args:
      loss: A `Tensor` containing the value to minimize.
      var_list: list or tuple of `Variable` objects to update to minimize
        `loss`.
      aggregation_method: Specifies the method used to combine gradient terms.
        Valid values are defined in the class `AggregationMethod`.
      colocate_gradients_with_ops: If True, try colocating gradients with the
        corresponding op.
      name: Optional name for the returned operation.
      grad_loss: Optional. A `Tensor` holding the gradient computed for `loss`.

    Returns:
      An Operation that updates the variables in `var_list`.  If `global_step`
      was not `None`, that operation also increments `global_step`.

    Raises:
      ValueError: If some of the variables are not `Variable` objects.

    @compatibility(eager)
    When eager execution is enabled, `loss` should be a Python function that
    takes no arguments and computes the value to be minimized. Minimization (and
    gradient computation) is done with respect to the elements of `var_list` if
    not None, else with respect to any trainable variables created during the
    execution of the `loss` function. `gate_gradients`, `aggregation_method`,
    `colocate_gradients_with_ops` and `grad_loss` are ignored when eager
    execution is enabled.
    @end_compatibility
    """
    grads_and_vars = self.compute_gradients(
        loss,
        var_list=var_list,
        aggregation_method=aggregation_method,
        colocate_gradients_with_ops=colocate_gradients_with_ops,
        grad_loss=grad_loss)

    return self.apply_gradients(grads_and_vars, name=name)

  def compute_gradients(self,
                        loss,
                        var_list,
                        aggregation_method=None,
                        colocate_gradients_with_ops=False,
                        grad_loss=None,
                        stop_gradients=None):
    """Compute gradients of `loss` for the variables in `var_list`.

    This is the first part of `minimize()`.  It returns a list
    of (gradient, variable) pairs where "gradient" is the gradient
    for "variable".  Note that "gradient" can be a `Tensor`, an
    `IndexedSlices`, or `None` if there is no gradient for the
    given variable.

    Args:
      loss: A Tensor containing the value to minimize or a callable taking no
        arguments which returns the value to minimize. When eager execution is
        enabled it must be a callable.
      var_list: Optional list or tuple of `tf.Variable` to update to minimize
        `loss`.  Defaults to the list of variables collected in the graph under
        the key `GraphKeys.TRAINABLE_VARIABLES`.
      aggregation_method: Specifies the method used to combine gradient terms.
        Valid values are defined in the class `AggregationMethod`.
      colocate_gradients_with_ops: If True, try colocating gradients with the
        corresponding op.
      grad_loss: Optional. A `Tensor` holding the gradient computed for `loss`.
      stop_gradients: Optional. A Tensor or list of tensors not to differentiate
        through.

    Returns:
      A list of (gradient, variable) pairs. Variable is always present, but
      gradient can be `None`.

    Raises:
      TypeError: If `var_list` contains anything else than `Variable` objects.
      ValueError: If some arguments are invalid, or var_list is None.
      RuntimeError: If called with eager execution enabled and `loss` is
        not callable.

    @compatibility(eager)
    When eager execution is enabled, `aggregation_method`, and
    `colocate_gradients_with_ops` are ignored.
    @end_compatibility
    """
    var_list = nest.flatten(var_list)
    # TODO(josh11b): Test that we handle weight decay in a reasonable way.
    if callable(loss):
      with backprop.GradientTape() as tape:
        tape.watch(var_list)
        loss_value = loss()
      grads = tape.gradient(loss_value, var_list, grad_loss)
    else:
      if context.executing_eagerly():
        raise RuntimeError("`loss` passed to Optimizer.compute_gradients "
                           "should be a function when eager execution is "
                           "enabled.")
      self._assert_valid_dtypes([loss])
      if grad_loss is not None:
        self._assert_valid_dtypes([grad_loss])
      grads = gradients.gradients(
          loss,
          var_list,
          grad_ys=grad_loss,
          aggregation_method=aggregation_method,
          colocate_gradients_with_ops=colocate_gradients_with_ops,
          stop_gradients=stop_gradients)

    grads_and_vars = list(zip(grads, var_list))
    self._assert_valid_dtypes([
        v for g, v in grads_and_vars
        if g is not None and v.dtype != dtypes.resource
    ])

    return grads_and_vars

  def apply_gradients(self, grads_and_vars, name=None):
    """Apply gradients to variables.

    This is the second part of `minimize()`. It returns an `Operation` that
    applies gradients.

    Args:
      grads_and_vars: List of (gradient, variable) pairs as returned by
        `compute_gradients()`.
      name: Optional name for the returned operation.  Default to the name
        passed to the `Optimizer` constructor.

    Returns:
      An `Operation` that applies the specified gradients. If `global_step`
      was not None, that operation also increments `global_step`.

    Raises:
      TypeError: If `grads_and_vars` is malformed.
      ValueError: If none of the variables have gradients.
    """
    grads_and_vars = _filter_grads(grads_and_vars)
    var_list = [v for (_, v) in grads_and_vars]
    if distribution_strategy_context.has_distribution_strategy():
      reduced_grads = merge_grads(grads_and_vars)
      grads_and_vars = zip(reduced_grads, var_list)

    with ops.init_scope():
      self._create_slots(var_list)
    update_ops = []

    def update_grad_to_var(grad, var):
      """Apply gradient to variable."""
      if isinstance(var, ops.Tensor):
        raise NotImplementedError("Trying to update a Tensor ", var)
      if isinstance(grad, ops.IndexedSlices):
        if var.constraint is not None:
          raise RuntimeError(
              "Cannot use a constraint function on a sparse variable.")
        return self._resource_apply_sparse_duplicate_indices(
            grad.values, var, grad.indices)
      update_op = self._resource_apply_dense(grad, var)
      if var.constraint is not None:
        with ops.control_dependencies([update_op]):
          return var.assign(var.constraint(var))
      else:
        return update_op

    with ops.name_scope(name, self._name) as name:
      self._prepare()
      for grad, var in grads_and_vars:
        scope_name = "" if in_eager_execution() else "_" + var.op.name
        with ops.name_scope("update" + scope_name), ops.colocate_with(var):
          update_ops.append(update_grad_to_var(grad, var))
      with ops.colocate_with(self._iterations):
        update_ops.append(self._iterations.assign_add(1))
      return control_flow_ops.group(*update_ops)

  def _set_hyper(self, name, value):
    self._hyper[name] = value

  def _get_hyper(self, name):
    # TODO(tanzheny): if hyper variable exists then return it.
    value = self._hyper[name]
    return self._call_if_callable(value)

  def add_slot(self, var, slot_name):
    slot_key = _get_slot_key_from_var(var, slot_name)
    if slot_key not in self._slots:
      self._slots[slot_key] = self.add_weight(
          name=slot_key, shape=var.shape, dtype=var.dtype)

  def get_slot(self, var, slot_name):
    slot_key = _get_slot_key_from_var(var, slot_name)
    return self._slots[slot_key]

  def _prepare(self):
    if self._prepared:
      return
    # This is where all hyper variables will be created.
    with ops.device("cpu:0"):
      self._iterations = self.add_weight(
          self._name + "/iter",
          shape=[],
          trainable=False,
          aggregation=variables.VariableAggregation.ONLY_FIRST_REPLICA)
    self._prepared = True

  @property
  def iteration(self):
    if not self._prepared:
      self._prepare()
    return self._iterations

  @abc.abstractmethod
  def get_config(self):
    """Returns the config of the optimimizer.

    An optimizer config is a Python dictionary (serializable)
    containing the configuration of an optimizer.
    The same optimizer can be reinstantiated later
    (without any saved state) from this configuration.

    Returns:
        Python dictionary.
    """
    return {"name": self._name}

  @classmethod
  def from_config(cls, config, custom_objects=None):
    """Creates an optimizer from its config.

    This method is the reverse of `get_config`,
    capable of instantiating the same optimizer from the config
    dictionary.

    Arguments:
        config: A Python dictionary, typically the output of get_config.
        custom_objects: A Python dictionary mapping names to additional Python
          objects used to create this optimizer, such as a function used for a
          hyperparameter.

    Returns:
        An optimizer instance.
    """
    return cls(**config)

  def _serialize_hyperparameter(self, hyperparameter_name):
    """Serialize a hyperparameter that can be a float, callable, or Tensor."""
    return self._hyper[hyperparameter_name]

  def add_weight(self,
                 name,
                 shape,
                 dtype=None,
                 initializer="zeros",
                 trainable=None,
                 synchronization=variables.VariableSynchronization.AUTO,
                 aggregation=variables.VariableAggregation.NONE):

    if dtype is None:
      dtype = dtypes.float32
    initializer = initializers.get(initializer)

    if synchronization == variables.VariableSynchronization.ON_READ:
      if trainable:
        raise ValueError(
            "Synchronization value can be set to "
            "VariableSynchronization.ON_READ only for non-trainable variables. "
            "You have specified trainable=True and "
            "synchronization=VariableSynchronization.ON_READ.")
      else:
        # Set trainable to be false when variable is to be synced on read.
        trainable = False
    elif trainable is None:
      trainable = True

    variable = self._add_variable_with_custom_getter(
        name=name,
        shape=shape,
        getter=base_layer.make_variable,
        overwrite=True,
        initializer=initializers.get(initializer),
        dtype=dtype,
        trainable=trainable,
        use_resource=True,
        synchronization=synchronization,
        aggregation=aggregation)

    return variable


def _filter_grads(grads_and_vars):
  """Filter out iterable with grad equal to None."""
  grads_and_vars = tuple(grads_and_vars)
  if not grads_and_vars:
    raise ValueError("No variables provided.")
  filtered = []
  vars_with_empty_grads = []
  for grad, var in grads_and_vars:
    if grad is None:
      vars_with_empty_grads.append(var)
    else:
      filtered.append((grad, var))
  filtered = tuple(filtered)
  if not filtered:
    raise ValueError("No gradients provided for any variable: %s." %
                     ([v.name for _, v in filtered],))
  if vars_with_empty_grads:
    logging.warning(
        ("Gradients does not exist for variables %s when minimizing the loss."),
        ([v.name for v in vars_with_empty_grads]))
  return filtered


def merge_grads(grads_and_vars):
  """Merge gradients from different replicas."""

  def merge_grad_fn(strategy, grads_and_vars):
    reduced_grads = strategy.batch_reduce(
        variable_scope.VariableAggregation.MEAN, grads_and_vars)
    return reduced_grads

  return distribution_strategy_context.get_tower_context().merge_call(
      merge_grad_fn, grads_and_vars)


def in_eager_execution():
  with ops.init_scope():
    return context.executing_eagerly()


def _get_slot_key_from_var(var, slot_name):
  """Get the slot key for the variable.

  Scope the slot name in the namespace of the primary variable.
  Set "primary.op.name + '/' + slot_name" as default name.

  In graph mode the name is derived from the op.
  In eager mode the name is derived from the var.
  If distribution strategy exists, then the name is derived from the primary
  variable instead of replica variable, i.e., /dense/kernel instead of
  /dense/kernel/replica_1. If the slot name is 'm', then the slot variables
  being created are /dense/kernel/m and /dense/kernel/m/replica_1, instead of
  /dense/kernel/replica_1/m/replica_1.

  Args:
    var: the variable.
    slot_name: the name of the slot.

  Returns:
    the name of the variable.
  """

  # pylint: disable=protected-access
  if distribution_strategy_context.has_distribution_strategy() and hasattr(
      var, "_primary_var"):
    var = var._primary_var
  if context.executing_eagerly():
    name = var._shared_name
  else:
    name = var.op.name
  return name + "/" + slot_name
