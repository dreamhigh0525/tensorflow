Bernoulli distribution.

The Bernoulli distribution is parameterized by p, the probability of a
positive event.

Note, the following methods of the base class aren't implemented:
  * cdf
  * log_cdf
- - -

#### `tf.contrib.distributions.Bernoulli.__init__(p, dtype=tf.int32, strict=True, name='Bernoulli')` {#Bernoulli.__init__}

Construct Bernoulli distributions.

##### Args:


*  <b>`p`</b>: An N-D `Tensor` representing the probability of a positive
      event. Each entry in the `Tensor` parameterizes an independent
      Bernoulli distribution.
*  <b>`dtype`</b>: dtype for samples. Note that other values will take the dtype of p.
*  <b>`strict`</b>: Whether to assert that `0 <= p <= 1`. If not strict, `log_pmf` may
    return nans.
*  <b>`name`</b>: A name for this distribution.


- - -

#### `tf.contrib.distributions.Bernoulli.batch_shape(name='batch_shape')` {#Bernoulli.batch_shape}




- - -

#### `tf.contrib.distributions.Bernoulli.cdf(value, name='cdf')` {#Bernoulli.cdf}

Cumulative distribution function.


- - -

#### `tf.contrib.distributions.Bernoulli.dtype` {#Bernoulli.dtype}




- - -

#### `tf.contrib.distributions.Bernoulli.entropy(name='entropy')` {#Bernoulli.entropy}

Entropy of the distribution.

##### Args:


*  <b>`name`</b>: Name for the op.

##### Returns:


*  <b>`entropy`</b>: `Tensor` of the same type and shape as `p`.


- - -

#### `tf.contrib.distributions.Bernoulli.event_shape(name='event_shape')` {#Bernoulli.event_shape}




- - -

#### `tf.contrib.distributions.Bernoulli.get_batch_shape()` {#Bernoulli.get_batch_shape}




- - -

#### `tf.contrib.distributions.Bernoulli.get_event_shape()` {#Bernoulli.get_event_shape}




- - -

#### `tf.contrib.distributions.Bernoulli.is_reparameterized` {#Bernoulli.is_reparameterized}




- - -

#### `tf.contrib.distributions.Bernoulli.log_cdf(value, name='log_cdf')` {#Bernoulli.log_cdf}

Log CDF.


- - -

#### `tf.contrib.distributions.Bernoulli.log_likelihood(value, name='log_likelihood')` {#Bernoulli.log_likelihood}

Log likelihood of this distribution (same as log_pmf).


- - -

#### `tf.contrib.distributions.Bernoulli.log_pmf(event, name='log_pmf')` {#Bernoulli.log_pmf}

Log of the probability mass function.

##### Args:


*  <b>`event`</b>: `int32` or `int64` binary Tensor.
*  <b>`name`</b>: A name for this operation (optional).

##### Returns:

  The log-probabilities of the events.


- - -

#### `tf.contrib.distributions.Bernoulli.mean(name='mean')` {#Bernoulli.mean}

Mean of the distribution.

##### Args:


*  <b>`name`</b>: Name for the op.

##### Returns:


*  <b>`mean`</b>: `Tensor` of the same type and shape as `p`.


- - -

#### `tf.contrib.distributions.Bernoulli.mode(name='mode')` {#Bernoulli.mode}

Mode of the distribution.

1 if p > 1-p. 0 otherwise.

##### Args:


*  <b>`name`</b>: Name for the op.

##### Returns:


*  <b>`mode`</b>: binary `Tensor` of type self.dtype.


- - -

#### `tf.contrib.distributions.Bernoulli.name` {#Bernoulli.name}




- - -

#### `tf.contrib.distributions.Bernoulli.p` {#Bernoulli.p}




- - -

#### `tf.contrib.distributions.Bernoulli.pmf(event, name='pmf')` {#Bernoulli.pmf}

Probability mass function.

##### Args:


*  <b>`event`</b>: `int32` or `int64` binary Tensor; must be broadcastable with `p`.
*  <b>`name`</b>: A name for this operation.

##### Returns:

  The probabilities of the events.


- - -

#### `tf.contrib.distributions.Bernoulli.q` {#Bernoulli.q}

1-p.


- - -

#### `tf.contrib.distributions.Bernoulli.sample(n, seed=None, name='sample')` {#Bernoulli.sample}

Generate `n` samples.

##### Args:


*  <b>`n`</b>: scalar.  Number of samples to draw from each distribution.
*  <b>`seed`</b>: Python integer seed for RNG.
*  <b>`name`</b>: name to give to the op.

##### Returns:


*  <b>`samples`</b>: a `Tensor` of shape `(n,) + self.batch_shape` with values of type
      `self.dtype`.


- - -

#### `tf.contrib.distributions.Bernoulli.std(name='std')` {#Bernoulli.std}

Standard deviation of the distribution.

##### Args:


*  <b>`name`</b>: Name for the op.

##### Returns:


*  <b>`std`</b>: `Tensor` of the same type and shape as `p`.


- - -

#### `tf.contrib.distributions.Bernoulli.variance(name='variance')` {#Bernoulli.variance}

Variance of the distribution.

##### Args:


*  <b>`name`</b>: Name for the op.

##### Returns:


*  <b>`variance`</b>: `Tensor` of the same type and shape as `p`.


