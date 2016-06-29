The Exponential distribution with rate parameter lam.

The PDF of this distribution is:

```pdf(x) = (lam * e^(-lam * x)), x > 0```

Note that the Exponential distribution is a special case of the Gamma
distribution, with Exponential(lam) = Gamma(1, lam).
- - -

#### `tf.contrib.distributions.Exponential.__init__(lam, strict=True, strict_statistics=True, name='Exponential')` {#Exponential.__init__}

Construct Exponential distribution with parameter `lam`.

##### Args:


*  <b>`lam`</b>: `float` or `double` tensor, the rate of the distribution(s).
    `lam` must contain only positive values.
*  <b>`strict`</b>: Whether to assert that `lam > 0`, and that `x > 0` in the
    methods `pdf(x)` and `log_pdf(x)`.  If `strict` is False
    and the inputs are invalid, correct behavior is not guaranteed.
*  <b>`strict_statistics`</b>: Boolean, default True.  If True, raise an exception if
    a statistic (e.g. mean/mode/etc...) is undefined for any batch member.
    If False, batch members with valid parameters leading to undefined
    statistics will return NaN for this statistic.
*  <b>`name`</b>: The name to prepend to all ops created by this distribution.


- - -

#### `tf.contrib.distributions.Exponential.alpha` {#Exponential.alpha}

Shape parameter.


- - -

#### `tf.contrib.distributions.Exponential.batch_shape(name='batch_shape')` {#Exponential.batch_shape}

Batch dimensions of this instance as a 1-D int32 `Tensor`.

The product of the dimensions of the `batch_shape` is the number of
independent distributions of this kind the instance represents.

##### Args:


*  <b>`name`</b>: name to give to the op

##### Returns:

  `Tensor` `batch_shape`


- - -

#### `tf.contrib.distributions.Exponential.beta` {#Exponential.beta}

Inverse scale parameter.


- - -

#### `tf.contrib.distributions.Exponential.cdf(x, name='cdf')` {#Exponential.cdf}

CDF of observations `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`cdf`</b>: tensor of dtype `dtype`, the CDFs of `x`.


- - -

#### `tf.contrib.distributions.Exponential.dtype` {#Exponential.dtype}

dtype of samples from this distribution.


- - -

#### `tf.contrib.distributions.Exponential.entropy(name='entropy')` {#Exponential.entropy}

The entropy of Gamma distribution(s).

This is defined to be

```
entropy = alpha - log(beta) + log(Gamma(alpha))
             + (1-alpha)digamma(alpha)
```

where digamma(alpha) is the digamma function.

##### Args:


*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`entropy`</b>: tensor of dtype `dtype`, the entropy.


- - -

#### `tf.contrib.distributions.Exponential.event_shape(name='event_shape')` {#Exponential.event_shape}

Shape of a sample from a single distribution as a 1-D int32 `Tensor`.

##### Args:


*  <b>`name`</b>: name to give to the op

##### Returns:

  `Tensor` `event_shape`


- - -

#### `tf.contrib.distributions.Exponential.get_batch_shape()` {#Exponential.get_batch_shape}

`TensorShape` available at graph construction time.

Same meaning as `batch_shape`. May be only partially defined.

##### Returns:

  `TensorShape` object.


- - -

#### `tf.contrib.distributions.Exponential.get_event_shape()` {#Exponential.get_event_shape}

`TensorShape` available at graph construction time.

Same meaning as `event_shape`. May be only partially defined.

##### Returns:

  `TensorShape` object.


- - -

#### `tf.contrib.distributions.Exponential.is_reparameterized` {#Exponential.is_reparameterized}




- - -

#### `tf.contrib.distributions.Exponential.lam` {#Exponential.lam}




- - -

#### `tf.contrib.distributions.Exponential.log_cdf(x, name='log_cdf')` {#Exponential.log_cdf}

Log CDF of observations `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`log_cdf`</b>: tensor of dtype `dtype`, the log-CDFs of `x`.


- - -

#### `tf.contrib.distributions.Exponential.log_likelihood(value, name='log_likelihood')` {#Exponential.log_likelihood}

Log likelihood of this distribution (same as log_pdf).


- - -

#### `tf.contrib.distributions.Exponential.log_pdf(x, name='log_pdf')` {#Exponential.log_pdf}

Log pdf of observations in `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`log_pdf`</b>: tensor of dtype `dtype`, the log-PDFs of `x`.

##### Raises:


*  <b>`TypeError`</b>: if `x` and `alpha` are different dtypes.


- - -

#### `tf.contrib.distributions.Exponential.mean(name='mean')` {#Exponential.mean}

Mean of each batch member.


- - -

#### `tf.contrib.distributions.Exponential.mode(name='mode')` {#Exponential.mode}

Mode of each batch member.

The mode of a gamma distribution is `(alpha - 1) / beta` when `alpha > 1`,
and `NaN` otherwise.  If `self.strict_statistics` is `True`, an exception
will be raised rather than returning `NaN`.

##### Args:


*  <b>`name`</b>: A name to give this op.

##### Returns:

  The mode for every batch member, a `Tensor` with same `dtype` as self.


- - -

#### `tf.contrib.distributions.Exponential.name` {#Exponential.name}

Name to prepend to all ops.


- - -

#### `tf.contrib.distributions.Exponential.pdf(x, name='pdf')` {#Exponential.pdf}

Pdf of observations in `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`pdf`</b>: tensor of dtype `dtype`, the PDFs of `x`

##### Raises:


*  <b>`TypeError`</b>: if `x` and `alpha` are different dtypes.


- - -

#### `tf.contrib.distributions.Exponential.sample(n, seed=None, name=None)` {#Exponential.sample}

Sample `n` observations from the Exponential Distributions.

##### Args:


*  <b>`n`</b>: `Scalar`, type int32, the number of observations to sample.
*  <b>`seed`</b>: Python integer, the random seed.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`samples`</b>: `[n, ...]`, a `Tensor` of `n` samples for each
    of the distributions determined by the hyperparameters.


- - -

#### `tf.contrib.distributions.Exponential.std(name='std')` {#Exponential.std}

Standard deviation of this distribution.


- - -

#### `tf.contrib.distributions.Exponential.strict` {#Exponential.strict}

Boolean describing behavior on invalid input.


- - -

#### `tf.contrib.distributions.Exponential.strict_statistics` {#Exponential.strict_statistics}

Boolean describing behavior when a stat is undefined for batch member.


- - -

#### `tf.contrib.distributions.Exponential.variance(name='variance')` {#Exponential.variance}

Variance of each batch member.


