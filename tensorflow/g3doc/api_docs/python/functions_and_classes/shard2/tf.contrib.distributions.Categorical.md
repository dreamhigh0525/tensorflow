Categorical distribution.

The categorical distribution is parameterized by the log-probabilities
of a set of classes.

Note, the following methods of the base class aren't implemented:
  * mean
  * cdf
  * log_cdf
- - -

#### `tf.contrib.distributions.Categorical.__init__(logits, dtype=tf.int32, strict=True, strict_statistics=True, name='Categorical')` {#Categorical.__init__}

Initialize Categorical distributions using class log-probabilities.

##### Args:


*  <b>`logits`</b>: An N-D `Tensor`, `N >= 1`, representing the log probabilities
      of a set of Categorical distributions. The first `N - 1` dimensions
      index into a batch of independent distributions and the last dimension
      indexes into the classes.
*  <b>`dtype`</b>: The type of the event samples (default: int32).
*  <b>`strict`</b>: Unused in this distribution.
*  <b>`strict_statistics`</b>: Boolean, default True.  If True, raise an exception if
    a statistic (e.g. mean/mode/etc...) is undefined for any batch member.
    If False, batch members with valid parameters leading to undefined
    statistics will return NaN for this statistic.
*  <b>`name`</b>: A name for this distribution (optional).


- - -

#### `tf.contrib.distributions.Categorical.batch_shape(name='batch_shape')` {#Categorical.batch_shape}




- - -

#### `tf.contrib.distributions.Categorical.cdf(value, name='cdf')` {#Categorical.cdf}

Cumulative distribution function.


- - -

#### `tf.contrib.distributions.Categorical.dtype` {#Categorical.dtype}




- - -

#### `tf.contrib.distributions.Categorical.entropy(name='sample')` {#Categorical.entropy}




- - -

#### `tf.contrib.distributions.Categorical.event_shape(name='event_shape')` {#Categorical.event_shape}




- - -

#### `tf.contrib.distributions.Categorical.get_batch_shape()` {#Categorical.get_batch_shape}




- - -

#### `tf.contrib.distributions.Categorical.get_event_shape()` {#Categorical.get_event_shape}




- - -

#### `tf.contrib.distributions.Categorical.is_reparameterized` {#Categorical.is_reparameterized}




- - -

#### `tf.contrib.distributions.Categorical.log_cdf(value, name='log_cdf')` {#Categorical.log_cdf}

Log CDF.


- - -

#### `tf.contrib.distributions.Categorical.log_likelihood(value, name='log_likelihood')` {#Categorical.log_likelihood}

Log likelihood of this distribution (same as log_pmf).


- - -

#### `tf.contrib.distributions.Categorical.log_pmf(k, name='log_pmf')` {#Categorical.log_pmf}

Log-probability of class `k`.

##### Args:


*  <b>`k`</b>: `int32` or `int64` Tensor with shape = `self.batch_shape()`.
*  <b>`name`</b>: A name for this operation (optional).

##### Returns:

  The log-probabilities of the classes indexed by `k`


- - -

#### `tf.contrib.distributions.Categorical.logits` {#Categorical.logits}




- - -

#### `tf.contrib.distributions.Categorical.mean(name='mean')` {#Categorical.mean}

Mean of the distribution.


- - -

#### `tf.contrib.distributions.Categorical.mode(name='mode')` {#Categorical.mode}




- - -

#### `tf.contrib.distributions.Categorical.name` {#Categorical.name}




- - -

#### `tf.contrib.distributions.Categorical.num_classes` {#Categorical.num_classes}




- - -

#### `tf.contrib.distributions.Categorical.pmf(k, name='pmf')` {#Categorical.pmf}

Probability of class `k`.

##### Args:


*  <b>`k`</b>: `int32` or `int64` Tensor with shape = `self.batch_shape()`.
*  <b>`name`</b>: A name for this operation (optional).

##### Returns:

  The probabilities of the classes indexed by `k`


- - -

#### `tf.contrib.distributions.Categorical.sample(n, seed=None, name='sample')` {#Categorical.sample}

Sample `n` observations from the Categorical distribution.

##### Args:


*  <b>`n`</b>: 0-D.  Number of independent samples to draw for each distribution.
*  <b>`seed`</b>: Random seed (optional).
*  <b>`name`</b>: A name for this operation (optional).

##### Returns:

  An `int64` `Tensor` with shape `[n, batch_shape, event_shape]`


- - -

#### `tf.contrib.distributions.Categorical.std(name='std')` {#Categorical.std}

Standard deviation of the distribution.


- - -

#### `tf.contrib.distributions.Categorical.strict` {#Categorical.strict}

Boolean describing behavior on invalid input.


- - -

#### `tf.contrib.distributions.Categorical.strict_statistics` {#Categorical.strict_statistics}

Boolean describing behavior when a stat is undefined for batch member.


- - -

#### `tf.contrib.distributions.Categorical.variance(name='variance')` {#Categorical.variance}

Variance of the distribution.


