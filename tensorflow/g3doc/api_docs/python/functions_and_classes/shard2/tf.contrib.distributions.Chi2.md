The Chi2 distribution with degrees of freedom df.

The PDF of this distribution is:

```pdf(x) = (x^(df/2 - 1)e^(-x/2))/(2^(df/2)Gamma(df/2)), x > 0```

Note that the Chi2 distribution is a special case of the Gamma distribution,
with Chi2(df) = Gamma(df/2, 1/2).
- - -

#### `tf.contrib.distributions.Chi2.__init__(df, strict=True, name='Chi2')` {#Chi2.__init__}

Construct Chi2 distributions with parameter `df`.

##### Args:


*  <b>`df`</b>: `float` or `double` tensor, the degrees of freedom of the
    distribution(s).  `df` must contain only positive values.
*  <b>`strict`</b>: Whether to assert that `df > 0`, and that `x > 0` in the
    methods `pdf(x)` and `log_pdf(x)`. If `strict` is False
    and the inputs are invalid, correct behavior is not guaranteed.
*  <b>`name`</b>: The name to prepend to all ops created by this distribution.


- - -

#### `tf.contrib.distributions.Chi2.alpha` {#Chi2.alpha}

Shape parameter.


- - -

#### `tf.contrib.distributions.Chi2.batch_shape(name='batch_shape')` {#Chi2.batch_shape}

Batch dimensions of this instance as a 1-D int32 `Tensor`.

The product of the dimensions of the `batch_shape` is the number of
independent distributions of this kind the instance represents.

##### Args:


*  <b>`name`</b>: name to give to the op

##### Returns:

  `Tensor` `batch_shape`


- - -

#### `tf.contrib.distributions.Chi2.beta` {#Chi2.beta}

Inverse scale parameter.


- - -

#### `tf.contrib.distributions.Chi2.cdf(x, name='cdf')` {#Chi2.cdf}

CDF of observations `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`cdf`</b>: tensor of dtype `dtype`, the CDFs of `x`.


- - -

#### `tf.contrib.distributions.Chi2.df` {#Chi2.df}




- - -

#### `tf.contrib.distributions.Chi2.dtype` {#Chi2.dtype}

dtype of samples from this distribution.


- - -

#### `tf.contrib.distributions.Chi2.entropy(name='entropy')` {#Chi2.entropy}

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

#### `tf.contrib.distributions.Chi2.event_shape(name='event_shape')` {#Chi2.event_shape}

Shape of a sample from a single distribution as a 1-D int32 `Tensor`.

##### Args:


*  <b>`name`</b>: name to give to the op

##### Returns:

  `Tensor` `event_shape`


- - -

#### `tf.contrib.distributions.Chi2.get_batch_shape()` {#Chi2.get_batch_shape}

`TensorShape` available at graph construction time.

Same meaning as `batch_shape`. May be only partially defined.

##### Returns:

  `TensorShape` object.


- - -

#### `tf.contrib.distributions.Chi2.get_event_shape()` {#Chi2.get_event_shape}

`TensorShape` available at graph construction time.

Same meaning as `event_shape`. May be only partially defined.

##### Returns:

  `TensorShape` object.


- - -

#### `tf.contrib.distributions.Chi2.is_reparameterized` {#Chi2.is_reparameterized}




- - -

#### `tf.contrib.distributions.Chi2.log_cdf(x, name='log_cdf')` {#Chi2.log_cdf}

Log CDF of observations `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`log_cdf`</b>: tensor of dtype `dtype`, the log-CDFs of `x`.


- - -

#### `tf.contrib.distributions.Chi2.log_likelihood(value, name='log_likelihood')` {#Chi2.log_likelihood}

Log likelihood of this distribution (same as log_pdf).


- - -

#### `tf.contrib.distributions.Chi2.log_pdf(x, name='log_pdf')` {#Chi2.log_pdf}

Log pdf of observations in `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`log_pdf`</b>: tensor of dtype `dtype`, the log-PDFs of `x`.

##### Raises:


*  <b>`TypeError`</b>: if `x` and `alpha` are different dtypes.


- - -

#### `tf.contrib.distributions.Chi2.mean(name='mean')` {#Chi2.mean}

Mean of each batch member.


- - -

#### `tf.contrib.distributions.Chi2.mode(name='mode')` {#Chi2.mode}

Mode of each batch member.  Defined only if alpha >= 1.


- - -

#### `tf.contrib.distributions.Chi2.name` {#Chi2.name}

Name to prepend to all ops.


- - -

#### `tf.contrib.distributions.Chi2.pdf(x, name='pdf')` {#Chi2.pdf}

Pdf of observations in `x` under these Gamma distribution(s).

##### Args:


*  <b>`x`</b>: tensor of dtype `dtype`, must be broadcastable with `alpha` and `beta`.
*  <b>`name`</b>: The name to give this op.

##### Returns:


*  <b>`pdf`</b>: tensor of dtype `dtype`, the PDFs of `x`

##### Raises:


*  <b>`TypeError`</b>: if `x` and `alpha` are different dtypes.


- - -

#### `tf.contrib.distributions.Chi2.sample(n, seed=None, name='sample')` {#Chi2.sample}

Draws `n` samples from the Gamma distribution(s).

See the doc for tf.random_gamma for further detail.

##### Args:


*  <b>`n`</b>: Python integer, the number of observations to sample from each
    distribution.
*  <b>`seed`</b>: Python integer, the random seed for this operation.
*  <b>`name`</b>: Optional name for the operation.

##### Returns:


*  <b>`samples`</b>: a `Tensor` of shape `(n,) + self.batch_shape + self.event_shape`
      with values of type `self.dtype`.


- - -

#### `tf.contrib.distributions.Chi2.std(name='std')` {#Chi2.std}

Standard deviation of this distribution.


- - -

#### `tf.contrib.distributions.Chi2.strict` {#Chi2.strict}

Boolean describing behavior on invalid input.


- - -

#### `tf.contrib.distributions.Chi2.variance(name='variance')` {#Chi2.variance}

Variance of each batch member.


