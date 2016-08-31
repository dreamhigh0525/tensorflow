<!-- This file is machine generated: DO NOT EDIT! -->

# BayesFlow Stochastic Tensors (contrib)
[TOC]

Classes and helper functions for creating Stochastic Tensors.

`StochasticTensor` objects wrap `Distribution` objects.  Their
values may be samples from the underlying distribution, or the distribution
mean (as governed by `value_type`).  These objects provide a `loss`
method for use when sampling from a non-reparameterized distribution.
The `loss`method is used in conjunction with `stochastic_graph.surrogate_loss`
to produce a single differentiable loss in stochastic graphs having
both continuous and discrete stochastic nodes.

## Stochastic Tensor Classes

- - -

### `class tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor` {#BaseStochasticTensor}

Base Class for Tensor-like objects that emit stochastic values.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor.__init__()` {#BaseStochasticTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor.dtype` {#BaseStochasticTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor.graph` {#BaseStochasticTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor.input_dict` {#BaseStochasticTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor.loss(sample_loss)` {#BaseStochasticTensor.loss}

Returns the term to add to the surrogate loss.

This method is called by `surrogate_loss`.  The input `sample_loss` should
have already had `stop_gradient` applied to it.  This is because the
surrogate_loss usually provides a Monte Carlo sample term of the form
`differentiable_surrogate * sample_loss` where `sample_loss` is considered
constant with respect to the input for purposes of the gradient.

##### Args:


*  <b>`sample_loss`</b>: `Tensor`, sample loss downstream of this `StochasticTensor`.

##### Returns:

  Either `None` or a `Tensor`.


- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor.name` {#BaseStochasticTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BaseStochasticTensor.value(name=None)` {#BaseStochasticTensor.value}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.StochasticTensor` {#StochasticTensor}

StochasticTensor is a BaseStochasticTensor backed by a distribution.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.__init__(dist_cls, name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#StochasticTensor.__init__}

Construct a `StochasticTensor`.

`StochasticTensor` will instantiate a distribution from `dist_cls` and
`dist_args` and its `value` method will return the same value each time
it is called. What `value` is returned is controlled by the
`dist_value_type` (defaults to `SampleAndReshapeValue`).

Some distributions' sample functions are not differentiable (e.g. a sample
from a discrete distribution like a Bernoulli) and so to differentiate
wrt parameters upstream of the sample requires a gradient estimator like
the score function estimator. This is accomplished by passing a
differentiable `loss_fn` to the `StochasticTensor`, which
defaults to a function whose derivative is the score function estimator.
Calling `stochastic_graph.surrogate_loss(final_losses)` will call
`loss()` on every `StochasticTensor` upstream of final losses.

`loss()` will return None for `StochasticTensor`s backed by
reparameterized distributions; it will also return None if the value type is
`MeanValueType` or if `loss_fn=None`.

##### Args:


*  <b>`dist_cls`</b>: a `Distribution` class.
*  <b>`name`</b>: a name for this `StochasticTensor` and its ops.
*  <b>`dist_value_type`</b>: a `_StochasticValueType`, which will determine what the
      `value` of this `StochasticTensor` will be. If not provided, the
      value type set with the `value_type` context manager will be used.
*  <b>`loss_fn`</b>: callable that takes `(dt, dt.value(), influenced_loss)`, where
      `dt` is this `StochasticTensor`, and returns a `Tensor` loss. By
      default, `loss_fn` is the `score_function`, or more precisely, the
      integral of the score function, such that when the gradient is taken,
      the score function results. See the `stochastic_gradient_estimators`
      module for additional loss functions and baselines.
*  <b>`**dist_args`</b>: keyword arguments to be passed through to `dist_cls` on
      construction.

##### Raises:


*  <b>`TypeError`</b>: if `dist_cls` is not a `Distribution`.
*  <b>`TypeError`</b>: if `loss_fn` is not `callable`.


- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.clone(name=None, **dist_args)` {#StochasticTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.distribution` {#StochasticTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.dtype` {#StochasticTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.entropy(name='entropy')` {#StochasticTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.graph` {#StochasticTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.input_dict` {#StochasticTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.loss(final_loss, name='Loss')` {#StochasticTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.mean(name='mean')` {#StochasticTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.name` {#StochasticTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.value(name='value')` {#StochasticTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StochasticTensor.value_type` {#StochasticTensor.value_type}






## Stochastic Tensor Value Types

- - -

### `class tf.contrib.bayesflow.stochastic_tensor.MeanValue` {#MeanValue}


- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MeanValue.__init__(stop_gradient=False)` {#MeanValue.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MeanValue.declare_inputs(unused_stochastic_tensor, unused_inputs_dict)` {#MeanValue.declare_inputs}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MeanValue.popped_above(unused_value_type)` {#MeanValue.popped_above}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MeanValue.pushed_above(unused_value_type)` {#MeanValue.pushed_above}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MeanValue.stop_gradient` {#MeanValue.stop_gradient}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.SampleValue` {#SampleValue}

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

#### `tf.contrib.bayesflow.stochastic_tensor.SampleValue.__init__(n=1, stop_gradient=False)` {#SampleValue.__init__}

Sample `n` times and concatenate along a new outer dimension.

##### Args:


*  <b>`n`</b>: A python integer or int32 tensor. The number of samples to take.
*  <b>`stop_gradient`</b>: If `True`, StochasticTensors' values are wrapped in
    `stop_gradient`, to avoid backpropagation through.


- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleValue.declare_inputs(unused_stochastic_tensor, unused_inputs_dict)` {#SampleValue.declare_inputs}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleValue.n` {#SampleValue.n}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleValue.popped_above(unused_value_type)` {#SampleValue.popped_above}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleValue.pushed_above(unused_value_type)` {#SampleValue.pushed_above}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleValue.stop_gradient` {#SampleValue.stop_gradient}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.SampleAndReshapeValue` {#SampleAndReshapeValue}

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

#### `tf.contrib.bayesflow.stochastic_tensor.SampleAndReshapeValue.__init__(n=1, stop_gradient=False)` {#SampleAndReshapeValue.__init__}

Sample `n` times and reshape the outer 2 axes so rank does not change.

##### Args:


*  <b>`n`</b>: A python integer or int32 tensor.  The number of samples to take.
*  <b>`stop_gradient`</b>: If `True`, StochasticTensors' values are wrapped in
    `stop_gradient`, to avoid backpropagation through.


- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleAndReshapeValue.declare_inputs(unused_stochastic_tensor, unused_inputs_dict)` {#SampleAndReshapeValue.declare_inputs}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleAndReshapeValue.n` {#SampleAndReshapeValue.n}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleAndReshapeValue.popped_above(unused_value_type)` {#SampleAndReshapeValue.popped_above}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleAndReshapeValue.pushed_above(unused_value_type)` {#SampleAndReshapeValue.pushed_above}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.SampleAndReshapeValue.stop_gradient` {#SampleAndReshapeValue.stop_gradient}






- - -

### `tf.contrib.bayesflow.stochastic_tensor.value_type(dist_value_type)` {#value_type}

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

### `tf.contrib.bayesflow.stochastic_tensor.get_current_value_type()` {#get_current_value_type}






## Automatically Generated StochasticTensors

- - -

### `class tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor` {#BernoulliTensor}

`BernoulliTensor` is a `StochasticTensor` backed by the distribution `Bernoulli`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#BernoulliTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.clone(name=None, **dist_args)` {#BernoulliTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.distribution` {#BernoulliTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.dtype` {#BernoulliTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.entropy(name='entropy')` {#BernoulliTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.graph` {#BernoulliTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.input_dict` {#BernoulliTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.loss(final_loss, name='Loss')` {#BernoulliTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.mean(name='mean')` {#BernoulliTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.name` {#BernoulliTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.value(name='value')` {#BernoulliTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BernoulliTensor.value_type` {#BernoulliTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.BetaTensor` {#BetaTensor}

`BetaTensor` is a `StochasticTensor` backed by the distribution `Beta`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#BetaTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.clone(name=None, **dist_args)` {#BetaTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.distribution` {#BetaTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.dtype` {#BetaTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.entropy(name='entropy')` {#BetaTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.graph` {#BetaTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.input_dict` {#BetaTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.loss(final_loss, name='Loss')` {#BetaTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.mean(name='mean')` {#BetaTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.name` {#BetaTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.value(name='value')` {#BetaTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BetaTensor.value_type` {#BetaTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.BinomialTensor` {#BinomialTensor}

`BinomialTensor` is a `StochasticTensor` backed by the distribution `Binomial`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#BinomialTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.clone(name=None, **dist_args)` {#BinomialTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.distribution` {#BinomialTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.dtype` {#BinomialTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.entropy(name='entropy')` {#BinomialTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.graph` {#BinomialTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.input_dict` {#BinomialTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.loss(final_loss, name='Loss')` {#BinomialTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.mean(name='mean')` {#BinomialTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.name` {#BinomialTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.value(name='value')` {#BinomialTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.BinomialTensor.value_type` {#BinomialTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor` {#CategoricalTensor}

`CategoricalTensor` is a `StochasticTensor` backed by the distribution `Categorical`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#CategoricalTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.clone(name=None, **dist_args)` {#CategoricalTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.distribution` {#CategoricalTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.dtype` {#CategoricalTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.entropy(name='entropy')` {#CategoricalTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.graph` {#CategoricalTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.input_dict` {#CategoricalTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.loss(final_loss, name='Loss')` {#CategoricalTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.mean(name='mean')` {#CategoricalTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.name` {#CategoricalTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.value(name='value')` {#CategoricalTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.CategoricalTensor.value_type` {#CategoricalTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor` {#Chi2Tensor}

`Chi2Tensor` is a `StochasticTensor` backed by the distribution `Chi2`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#Chi2Tensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.clone(name=None, **dist_args)` {#Chi2Tensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.distribution` {#Chi2Tensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.dtype` {#Chi2Tensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.entropy(name='entropy')` {#Chi2Tensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.graph` {#Chi2Tensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.input_dict` {#Chi2Tensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.loss(final_loss, name='Loss')` {#Chi2Tensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.mean(name='mean')` {#Chi2Tensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.name` {#Chi2Tensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.value(name='value')` {#Chi2Tensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.Chi2Tensor.value_type` {#Chi2Tensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.DirichletTensor` {#DirichletTensor}

`DirichletTensor` is a `StochasticTensor` backed by the distribution `Dirichlet`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#DirichletTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.clone(name=None, **dist_args)` {#DirichletTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.distribution` {#DirichletTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.dtype` {#DirichletTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.entropy(name='entropy')` {#DirichletTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.graph` {#DirichletTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.input_dict` {#DirichletTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.loss(final_loss, name='Loss')` {#DirichletTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.mean(name='mean')` {#DirichletTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.name` {#DirichletTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.value(name='value')` {#DirichletTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletTensor.value_type` {#DirichletTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor` {#DirichletMultinomialTensor}

`DirichletMultinomialTensor` is a `StochasticTensor` backed by the distribution `DirichletMultinomial`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#DirichletMultinomialTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.clone(name=None, **dist_args)` {#DirichletMultinomialTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.distribution` {#DirichletMultinomialTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.dtype` {#DirichletMultinomialTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.entropy(name='entropy')` {#DirichletMultinomialTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.graph` {#DirichletMultinomialTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.input_dict` {#DirichletMultinomialTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.loss(final_loss, name='Loss')` {#DirichletMultinomialTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.mean(name='mean')` {#DirichletMultinomialTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.name` {#DirichletMultinomialTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.value(name='value')` {#DirichletMultinomialTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.DirichletMultinomialTensor.value_type` {#DirichletMultinomialTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor` {#ExponentialTensor}

`ExponentialTensor` is a `StochasticTensor` backed by the distribution `Exponential`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#ExponentialTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.clone(name=None, **dist_args)` {#ExponentialTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.distribution` {#ExponentialTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.dtype` {#ExponentialTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.entropy(name='entropy')` {#ExponentialTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.graph` {#ExponentialTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.input_dict` {#ExponentialTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.loss(final_loss, name='Loss')` {#ExponentialTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.mean(name='mean')` {#ExponentialTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.name` {#ExponentialTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.value(name='value')` {#ExponentialTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.ExponentialTensor.value_type` {#ExponentialTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.GammaTensor` {#GammaTensor}

`GammaTensor` is a `StochasticTensor` backed by the distribution `Gamma`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#GammaTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.clone(name=None, **dist_args)` {#GammaTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.distribution` {#GammaTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.dtype` {#GammaTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.entropy(name='entropy')` {#GammaTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.graph` {#GammaTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.input_dict` {#GammaTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.loss(final_loss, name='Loss')` {#GammaTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.mean(name='mean')` {#GammaTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.name` {#GammaTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.value(name='value')` {#GammaTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.GammaTensor.value_type` {#GammaTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor` {#InverseGammaTensor}

`InverseGammaTensor` is a `StochasticTensor` backed by the distribution `InverseGamma`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#InverseGammaTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.clone(name=None, **dist_args)` {#InverseGammaTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.distribution` {#InverseGammaTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.dtype` {#InverseGammaTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.entropy(name='entropy')` {#InverseGammaTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.graph` {#InverseGammaTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.input_dict` {#InverseGammaTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.loss(final_loss, name='Loss')` {#InverseGammaTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.mean(name='mean')` {#InverseGammaTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.name` {#InverseGammaTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.value(name='value')` {#InverseGammaTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.InverseGammaTensor.value_type` {#InverseGammaTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor` {#LaplaceTensor}

`LaplaceTensor` is a `StochasticTensor` backed by the distribution `Laplace`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#LaplaceTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.clone(name=None, **dist_args)` {#LaplaceTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.distribution` {#LaplaceTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.dtype` {#LaplaceTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.entropy(name='entropy')` {#LaplaceTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.graph` {#LaplaceTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.input_dict` {#LaplaceTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.loss(final_loss, name='Loss')` {#LaplaceTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.mean(name='mean')` {#LaplaceTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.name` {#LaplaceTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.value(name='value')` {#LaplaceTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.LaplaceTensor.value_type` {#LaplaceTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.MixtureTensor` {#MixtureTensor}

`MixtureTensor` is a `StochasticTensor` backed by the distribution `Mixture`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#MixtureTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.clone(name=None, **dist_args)` {#MixtureTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.distribution` {#MixtureTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.dtype` {#MixtureTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.entropy(name='entropy')` {#MixtureTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.graph` {#MixtureTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.input_dict` {#MixtureTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.loss(final_loss, name='Loss')` {#MixtureTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.mean(name='mean')` {#MixtureTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.name` {#MixtureTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.value(name='value')` {#MixtureTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MixtureTensor.value_type` {#MixtureTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor` {#MultinomialTensor}

`MultinomialTensor` is a `StochasticTensor` backed by the distribution `Multinomial`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#MultinomialTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.clone(name=None, **dist_args)` {#MultinomialTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.distribution` {#MultinomialTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.dtype` {#MultinomialTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.entropy(name='entropy')` {#MultinomialTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.graph` {#MultinomialTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.input_dict` {#MultinomialTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.loss(final_loss, name='Loss')` {#MultinomialTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.mean(name='mean')` {#MultinomialTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.name` {#MultinomialTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.value(name='value')` {#MultinomialTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultinomialTensor.value_type` {#MultinomialTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor` {#MultivariateNormalCholeskyTensor}

`MultivariateNormalCholeskyTensor` is a `StochasticTensor` backed by the distribution `MultivariateNormalCholesky`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#MultivariateNormalCholeskyTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.clone(name=None, **dist_args)` {#MultivariateNormalCholeskyTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.distribution` {#MultivariateNormalCholeskyTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.dtype` {#MultivariateNormalCholeskyTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.entropy(name='entropy')` {#MultivariateNormalCholeskyTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.graph` {#MultivariateNormalCholeskyTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.input_dict` {#MultivariateNormalCholeskyTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.loss(final_loss, name='Loss')` {#MultivariateNormalCholeskyTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.mean(name='mean')` {#MultivariateNormalCholeskyTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.name` {#MultivariateNormalCholeskyTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.value(name='value')` {#MultivariateNormalCholeskyTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalCholeskyTensor.value_type` {#MultivariateNormalCholeskyTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor` {#MultivariateNormalDiagTensor}

`MultivariateNormalDiagTensor` is a `StochasticTensor` backed by the distribution `MultivariateNormalDiag`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#MultivariateNormalDiagTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.clone(name=None, **dist_args)` {#MultivariateNormalDiagTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.distribution` {#MultivariateNormalDiagTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.dtype` {#MultivariateNormalDiagTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.entropy(name='entropy')` {#MultivariateNormalDiagTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.graph` {#MultivariateNormalDiagTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.input_dict` {#MultivariateNormalDiagTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.loss(final_loss, name='Loss')` {#MultivariateNormalDiagTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.mean(name='mean')` {#MultivariateNormalDiagTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.name` {#MultivariateNormalDiagTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.value(name='value')` {#MultivariateNormalDiagTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagTensor.value_type` {#MultivariateNormalDiagTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor` {#MultivariateNormalDiagPlusVDVTTensor}

`MultivariateNormalDiagPlusVDVTTensor` is a `StochasticTensor` backed by the distribution `MultivariateNormalDiagPlusVDVT`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#MultivariateNormalDiagPlusVDVTTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.clone(name=None, **dist_args)` {#MultivariateNormalDiagPlusVDVTTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.distribution` {#MultivariateNormalDiagPlusVDVTTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.dtype` {#MultivariateNormalDiagPlusVDVTTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.entropy(name='entropy')` {#MultivariateNormalDiagPlusVDVTTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.graph` {#MultivariateNormalDiagPlusVDVTTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.input_dict` {#MultivariateNormalDiagPlusVDVTTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.loss(final_loss, name='Loss')` {#MultivariateNormalDiagPlusVDVTTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.mean(name='mean')` {#MultivariateNormalDiagPlusVDVTTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.name` {#MultivariateNormalDiagPlusVDVTTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.value(name='value')` {#MultivariateNormalDiagPlusVDVTTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalDiagPlusVDVTTensor.value_type` {#MultivariateNormalDiagPlusVDVTTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor` {#MultivariateNormalFullTensor}

`MultivariateNormalFullTensor` is a `StochasticTensor` backed by the distribution `MultivariateNormalFull`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#MultivariateNormalFullTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.clone(name=None, **dist_args)` {#MultivariateNormalFullTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.distribution` {#MultivariateNormalFullTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.dtype` {#MultivariateNormalFullTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.entropy(name='entropy')` {#MultivariateNormalFullTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.graph` {#MultivariateNormalFullTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.input_dict` {#MultivariateNormalFullTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.loss(final_loss, name='Loss')` {#MultivariateNormalFullTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.mean(name='mean')` {#MultivariateNormalFullTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.name` {#MultivariateNormalFullTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.value(name='value')` {#MultivariateNormalFullTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.MultivariateNormalFullTensor.value_type` {#MultivariateNormalFullTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.NormalTensor` {#NormalTensor}

`NormalTensor` is a `StochasticTensor` backed by the distribution `Normal`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#NormalTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.clone(name=None, **dist_args)` {#NormalTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.distribution` {#NormalTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.dtype` {#NormalTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.entropy(name='entropy')` {#NormalTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.graph` {#NormalTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.input_dict` {#NormalTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.loss(final_loss, name='Loss')` {#NormalTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.mean(name='mean')` {#NormalTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.name` {#NormalTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.value(name='value')` {#NormalTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.NormalTensor.value_type` {#NormalTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.PoissonTensor` {#PoissonTensor}

`PoissonTensor` is a `StochasticTensor` backed by the distribution `Poisson`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#PoissonTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.clone(name=None, **dist_args)` {#PoissonTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.distribution` {#PoissonTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.dtype` {#PoissonTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.entropy(name='entropy')` {#PoissonTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.graph` {#PoissonTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.input_dict` {#PoissonTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.loss(final_loss, name='Loss')` {#PoissonTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.mean(name='mean')` {#PoissonTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.name` {#PoissonTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.value(name='value')` {#PoissonTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.PoissonTensor.value_type` {#PoissonTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.StudentTTensor` {#StudentTTensor}

`StudentTTensor` is a `StochasticTensor` backed by the distribution `StudentT`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#StudentTTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.clone(name=None, **dist_args)` {#StudentTTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.distribution` {#StudentTTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.dtype` {#StudentTTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.entropy(name='entropy')` {#StudentTTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.graph` {#StudentTTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.input_dict` {#StudentTTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.loss(final_loss, name='Loss')` {#StudentTTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.mean(name='mean')` {#StudentTTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.name` {#StudentTTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.value(name='value')` {#StudentTTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.StudentTTensor.value_type` {#StudentTTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor` {#TransformedDistributionTensor}

`TransformedDistributionTensor` is a `StochasticTensor` backed by the distribution `TransformedDistribution`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#TransformedDistributionTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.clone(name=None, **dist_args)` {#TransformedDistributionTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.distribution` {#TransformedDistributionTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.dtype` {#TransformedDistributionTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.entropy(name='entropy')` {#TransformedDistributionTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.graph` {#TransformedDistributionTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.input_dict` {#TransformedDistributionTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.loss(final_loss, name='Loss')` {#TransformedDistributionTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.mean(name='mean')` {#TransformedDistributionTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.name` {#TransformedDistributionTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.value(name='value')` {#TransformedDistributionTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.TransformedDistributionTensor.value_type` {#TransformedDistributionTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.UniformTensor` {#UniformTensor}

`UniformTensor` is a `StochasticTensor` backed by the distribution `Uniform`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#UniformTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.clone(name=None, **dist_args)` {#UniformTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.distribution` {#UniformTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.dtype` {#UniformTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.entropy(name='entropy')` {#UniformTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.graph` {#UniformTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.input_dict` {#UniformTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.loss(final_loss, name='Loss')` {#UniformTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.mean(name='mean')` {#UniformTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.name` {#UniformTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.value(name='value')` {#UniformTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.UniformTensor.value_type` {#UniformTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor` {#WishartCholeskyTensor}

`WishartCholeskyTensor` is a `StochasticTensor` backed by the distribution `WishartCholesky`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#WishartCholeskyTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.clone(name=None, **dist_args)` {#WishartCholeskyTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.distribution` {#WishartCholeskyTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.dtype` {#WishartCholeskyTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.entropy(name='entropy')` {#WishartCholeskyTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.graph` {#WishartCholeskyTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.input_dict` {#WishartCholeskyTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.loss(final_loss, name='Loss')` {#WishartCholeskyTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.mean(name='mean')` {#WishartCholeskyTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.name` {#WishartCholeskyTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.value(name='value')` {#WishartCholeskyTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartCholeskyTensor.value_type` {#WishartCholeskyTensor.value_type}





- - -

### `class tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor` {#WishartFullTensor}

`WishartFullTensor` is a `StochasticTensor` backed by the distribution `WishartFull`.
- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.__init__(name=None, dist_value_type=None, loss_fn=score_function, **dist_args)` {#WishartFullTensor.__init__}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.clone(name=None, **dist_args)` {#WishartFullTensor.clone}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.distribution` {#WishartFullTensor.distribution}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.dtype` {#WishartFullTensor.dtype}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.entropy(name='entropy')` {#WishartFullTensor.entropy}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.graph` {#WishartFullTensor.graph}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.input_dict` {#WishartFullTensor.input_dict}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.loss(final_loss, name='Loss')` {#WishartFullTensor.loss}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.mean(name='mean')` {#WishartFullTensor.mean}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.name` {#WishartFullTensor.name}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.value(name='value')` {#WishartFullTensor.value}




- - -

#### `tf.contrib.bayesflow.stochastic_tensor.WishartFullTensor.value_type` {#WishartFullTensor.value_type}





