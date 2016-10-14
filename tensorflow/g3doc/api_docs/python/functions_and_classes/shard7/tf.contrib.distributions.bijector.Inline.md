Bijector constructed from callables implementing forward, inverse, and inverse_log_det_jacobian.

Example Use:

```python
exp = Inline(
  forward_fn=tf.exp,
  inverse_fn=tf.log,
  inverse_log_det_jacobian_fn=(
    lambda y: -tf.reduce_sum(tf.log(y), reduction_indices=-1)),
  name="Exp")
```

The above example is equivalent to the `Bijector` `Exp(event_ndims=1)`.
- - -

#### `tf.contrib.distributions.bijector.Inline.__init__(forward_fn, inverse_fn, inverse_log_det_jacobian_fn, is_constant_jacobian=False, name='Inline')` {#Inline.__init__}

Creates a `Bijector` from callables.

##### Args:


*  <b>`forward_fn`</b>: Python callable implementing the forward transformation.
*  <b>`inverse_fn`</b>: Python callable implementing the inverse transformation.
*  <b>`inverse_log_det_jacobian_fn`</b>: Python callable implementing the
    inverse_log_det_jacobian transformation.
*  <b>`is_constant_jacobian`</b>: `Boolean` indicating that the Jacobian is constant
    for all input arguments.
*  <b>`name`</b>: `String`, name given to ops managed by this object.


- - -

#### `tf.contrib.distributions.bijector.Inline.dtype` {#Inline.dtype}

dtype of `Tensor`s transformable by this distribution.


- - -

#### `tf.contrib.distributions.bijector.Inline.forward(x, name='forward')` {#Inline.forward}

Returns the forward `Bijector` evaluation, i.e., X = g(Y).

##### Args:


*  <b>`x`</b>: `Tensor`. The input to the "forward" evaluation.
*  <b>`name`</b>: The name to give this op.

##### Returns:

  `Tensor`.

##### Raises:


*  <b>`TypeError`</b>: if `self.dtype` is specified and `x.dtype` is not
    `self.dtype`.
*  <b>`NotImplementedError`</b>: if `_forward` is not implemented.


- - -

#### `tf.contrib.distributions.bijector.Inline.inverse(x, name='inverse')` {#Inline.inverse}

Returns the inverse `Bijector` evaluation, i.e., X = g^{-1}(Y).

##### Args:


*  <b>`x`</b>: `Tensor`. The input to the "inverse" evaluation.
*  <b>`name`</b>: The name to give this op.

##### Returns:

  `Tensor`.

##### Raises:


*  <b>`TypeError`</b>: if `self.dtype` is specified and `x.dtype` is not
    `self.dtype`.
*  <b>`NotImplementedError`</b>: if neither `_inverse` nor
    `_inverse_and_inverse_log_det_jacobian` are implemented.


- - -

#### `tf.contrib.distributions.bijector.Inline.inverse_and_inverse_log_det_jacobian(x, name='inverse_and_inverse_log_det_jacobian')` {#Inline.inverse_and_inverse_log_det_jacobian}

Returns both the inverse evaluation and inverse_log_det_jacobian.

Enables possibly more efficient calculation when both inverse and
corresponding Jacobian are needed.

See `inverse()`, `inverse_log_det_jacobian()` for more details.

##### Args:


*  <b>`x`</b>: `Tensor`. The input to the "inverse" Jacobian evaluation.
*  <b>`name`</b>: The name to give this op.

##### Returns:

  `Tensor`.

##### Raises:


*  <b>`TypeError`</b>: if `self.dtype` is specified and `x.dtype` is not
    `self.dtype`.
*  <b>`NotImplementedError`</b>: if neither `_inverse_and_inverse_log_det_jacobian`
    nor {`_inverse`, `_inverse_log_det_jacobian`} are implemented.


- - -

#### `tf.contrib.distributions.bijector.Inline.inverse_log_det_jacobian(x, name='inverse_log_det_jacobian')` {#Inline.inverse_log_det_jacobian}

Returns the (log o det o Jacobian o inverse)(x).

Mathematically, returns: log(det(dY/dX g^{-1}))(Y).

Note that forward_log_det_jacobian is the negative of this function. (See
is_constant_jacobian for related proof.)

##### Args:


*  <b>`x`</b>: `Tensor`. The input to the "inverse" Jacobian evaluation.
*  <b>`name`</b>: The name to give this op.

##### Returns:

  `Tensor`.

##### Raises:


*  <b>`TypeError`</b>: if `self.dtype` is specified and `x.dtype` is not
    `self.dtype`.
*  <b>`NotImplementedError`</b>: if neither `_inverse_log_det_jacobian` nor
    `_inverse_and_inverse_log_det_jacobian` are implemented.


- - -

#### `tf.contrib.distributions.bijector.Inline.is_constant_jacobian` {#Inline.is_constant_jacobian}

Returns true iff the Jacobian is not a function of x.

Note: Jacobian is either constant for both forward and inverse or neither.

##### Returns:

  `Boolean`.


- - -

#### `tf.contrib.distributions.bijector.Inline.name` {#Inline.name}

Returns the string name of this `Bijector`.


- - -

#### `tf.contrib.distributions.bijector.Inline.parameters` {#Inline.parameters}

Returns this `Bijector`'s parameters as a name/value dictionary.


- - -

#### `tf.contrib.distributions.bijector.Inline.shaper` {#Inline.shaper}

Returns shape object used to manage shape constraints.


- - -

#### `tf.contrib.distributions.bijector.Inline.validate_args` {#Inline.validate_args}

Returns True if Tensor arguments will be validated.


