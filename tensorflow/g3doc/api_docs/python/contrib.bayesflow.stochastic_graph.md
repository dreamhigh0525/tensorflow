<!-- This file is machine generated: DO NOT EDIT! -->

# BayesFlow Stochastic Graph (contrib)
[TOC]

Classes and helper functions for Stochastic Computation Graphs.

## Stochastic Computation Graph Classes

- - -

### `class tf.contrib.bayesflow.stochastic_graph.StochasticTensor` {#StochasticTensor}

Base Class for Tensor-like objects that emit stochastic values.
- - -

#### `tf.contrib.bayesflow.stochastic_graph.StochasticTensor.__init__(**kwargs)` {#StochasticTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.StochasticTensor.dtype` {#StochasticTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.StochasticTensor.graph` {#StochasticTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.StochasticTensor.input_dict` {#StochasticTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.StochasticTensor.name` {#StochasticTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.StochasticTensor.surrogate_loss(sample_losses)` {#StochasticTensor.surrogate_loss}

Returns the surrogate loss given the list of sample_losses.

This method is called by `surrogate_losses`.  The input `sample_losses`
presumably have already had `stop_gradient` applied to them.  This is
because the surrogate_loss usually provides a monte carlo sample term
of the form `differentiable_surrogate * sum(sample_losses)` where
`sample_losses` is considered constant with respect to the input
for purposes of the gradient.

##### Args:


*  <b>`sample_losses`</b>: a list of Tensors, the sample losses downstream of this
    `StochasticTensor`.

##### Returns:

  Either either `None` or a `Tensor` whose gradient is the
   score function.


- - -

#### `tf.contrib.bayesflow.stochastic_graph.StochasticTensor.value(name=None)` {#StochasticTensor.value}





- - -

### `class tf.contrib.bayesflow.stochastic_graph.DistributionTensor` {#DistributionTensor}

The DistributionTensor is a StochasticTensor backed by a distribution.
- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.__init__(dist_cls, name=None, dist_value_type=None, **dist_args)` {#DistributionTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.clone(name=None, **dist_args)` {#DistributionTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.distribution` {#DistributionTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.dtype` {#DistributionTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.entropy(name='entropy')` {#DistributionTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.graph` {#DistributionTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.input_dict` {#DistributionTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.mean(name='mean')` {#DistributionTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.name` {#DistributionTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.surrogate_loss(losses, name=None)` {#DistributionTensor.surrogate_loss}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.DistributionTensor.value(name='value')` {#DistributionTensor.value}






## Stochastic Computation Value Types

- - -

### `class tf.contrib.bayesflow.stochastic_graph.MeanValue` {#MeanValue}


- - -

#### `tf.contrib.bayesflow.stochastic_graph.MeanValue.__init__(stop_gradient=False)` {#MeanValue.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.MeanValue.declare_inputs(unused_stochastic_tensor, unused_inputs_dict)` {#MeanValue.declare_inputs}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.MeanValue.popped_above(unused_value_type)` {#MeanValue.popped_above}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.MeanValue.pushed_above(unused_value_type)` {#MeanValue.pushed_above}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.MeanValue.stop_gradient` {#MeanValue.stop_gradient}





- - -

### `class tf.contrib.bayesflow.stochastic_graph.SampleValue` {#SampleValue}

Draw n samples along a new outer dimension.

This ValueType draws `n` samples from StochasticTensors run within its
context, increasing the rank by one along a new outer dimension.

Example:

```python
mu = tf.zeros((2,3))
sigma = tf.ones((2, 3))
with sg.value_type(sg.SampleValue(n=4)):
  dt = sg.DistributionTensor(
    distributions.Normal, mu=mu, sigma=sigma)
# draws 4 samples each with shape (2, 3) and concatenates
assertEqual(dt.value().get_shape(), (4, 2, 3))
```
- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleValue.__init__(n=1, stop_gradient=False)` {#SampleValue.__init__}

Sample `n` times and concatenate along a new outer dimension.

##### Args:


*  <b>`n`</b>: A python integer or int32 tensor. The number of samples to take.
*  <b>`stop_gradient`</b>: If `True`, StochasticTensors' values are wrapped in
    `stop_gradient`, to avoid backpropagation through.


- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleValue.declare_inputs(unused_stochastic_tensor, unused_inputs_dict)` {#SampleValue.declare_inputs}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleValue.n` {#SampleValue.n}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleValue.popped_above(unused_value_type)` {#SampleValue.popped_above}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleValue.pushed_above(unused_value_type)` {#SampleValue.pushed_above}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleValue.stop_gradient` {#SampleValue.stop_gradient}





- - -

### `class tf.contrib.bayesflow.stochastic_graph.SampleAndReshapeValue` {#SampleAndReshapeValue}

Ask the StochasticTensor for n samples and reshape the result.

Sampling from a StochasticTensor increases the rank of the value by 1
(because each sample represents a new outer dimension).

This ValueType requests `n` samples from StochasticTensors run within its
context that the outer two dimensions are reshaped to intermix the samples
with the outermost (usually batch) dimension.

Example:

```python
# mu and sigma are both shaped (2, 3)
mu = [[0.0, -1.0, 1.0], [0.0, -1.0, 1.0]]
sigma = tf.constant([[1.1, 1.2, 1.3], [1.1, 1.2, 1.3]])

with sg.value_type(sg.SampleAndReshapeValue(n=2)):
  dt = sg.DistributionTensor(
      distributions.Normal, mu=mu, sigma=sigma)

# sample(2) creates a (2, 2, 3) tensor, and the two outermost dimensions
# are reshaped into one: the final value is a (4, 3) tensor.
dt_value = dt.value()
assertEqual(dt_value.get_shape(), (4, 3))

dt_value_val = sess.run([dt_value])[0]  # or e.g. run([tf.identity(dt)])[0]
assertEqual(dt_value_val.shape, (4, 3))
```
- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleAndReshapeValue.__init__(n=1, stop_gradient=False)` {#SampleAndReshapeValue.__init__}

Sample `n` times and reshape the outer 2 axes so rank does not change.

##### Args:


*  <b>`n`</b>: A python integer or int32 tensor.  The number of samples to take.
*  <b>`stop_gradient`</b>: If `True`, StochasticTensors' values are wrapped in
    `stop_gradient`, to avoid backpropagation through.


- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleAndReshapeValue.declare_inputs(unused_stochastic_tensor, unused_inputs_dict)` {#SampleAndReshapeValue.declare_inputs}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleAndReshapeValue.n` {#SampleAndReshapeValue.n}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleAndReshapeValue.popped_above(unused_value_type)` {#SampleAndReshapeValue.popped_above}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleAndReshapeValue.pushed_above(unused_value_type)` {#SampleAndReshapeValue.pushed_above}




- - -

#### `tf.contrib.bayesflow.stochastic_graph.SampleAndReshapeValue.stop_gradient` {#SampleAndReshapeValue.stop_gradient}





- - -

### `tf.contrib.bayesflow.stochastic_graph.value_type(dist_value_type)` {#value_type}

Creates a value type context for any StochasticTensor created within.

Typical usage:

```
with sg.value_type(sg.MeanValue(stop_gradients=True)):
  dt = sg.DistributionTensor(distributions.Normal, mu=mu, sigma=sigma)
```

In the example above, `dt.value()` (or equivalently, `tf.identity(dt)`) will
be the mean value of the Normal distribution, i.e., `mu` (possibly
broadcasted to the shape of `sigma`).  Furthermore, because the `MeanValue`
was marked with `stop_gradients=True`, this value will have been wrapped
in a `stop_gradients` call to disable any possible backpropagation.

##### Args:


*  <b>`dist_value_type`</b>: An instance of `MeanValue`, `SampleAndReshapeValue`, or
    any other stochastic value type.

##### Yields:

  A context for `StochasticTensor` objects that controls the
  value created when they are initialized.

##### Raises:


*  <b>`TypeError`</b>: if `dist_value_type` is not an instance of a stochastic value
    type.


- - -

### `tf.contrib.bayesflow.stochastic_graph.get_current_value_type()` {#get_current_value_type}





## Stochastic Computation Graph Helper Functions

- - -

### `tf.contrib.bayesflow.stochastic_graph.surrogate_losses(sample_losses, name=None)` {#surrogate_losses}




