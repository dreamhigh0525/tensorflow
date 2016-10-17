Bijector which computes `Y = g(X) = Log[1 + exp(X)]`.

The softplus `Bijector` has the following two useful properties:

* The domain is the positive real numbers
* `softplus(x) approx x`, for large `x`, so it does not overflow as easily as
  the `Exp` `Bijector`.

  Example Use:

  ```python
  # Create the Y=g(X)=softplus(X) transform which works only on Tensors with 1
  # batch ndim and 2 event ndims (i.e., vector of matrices).
  softplus = Softplus(batch_ndims=1, event_ndims=2)
  x = [[[1., 2],
         [3, 4]],
        [[5, 6],
         [7, 8]]]
  log(1 + exp(x)) == softplus.forward(x)
  log(exp(x) - 1) == softplus.inverse(x)
  ```

  Note: log(.) and exp(.) are applied element-wise but the Jacobian is a
  reduction over the event space.
- - -

#### `tf.contrib.distributions.bijector.Softplus.__init__(event_ndims=0, validate_args=False, name='Softplus')` {#Softplus.__init__}




- - -

#### `tf.contrib.distributions.bijector.Softplus.dtype` {#Softplus.dtype}

dtype of `Tensor`s transformable by this distribution.


- - -

#### `tf.contrib.distributions.bijector.Softplus.forward(x, name='forward')` {#Softplus.forward}

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

#### `tf.contrib.distributions.bijector.Softplus.inverse(x, name='inverse')` {#Softplus.inverse}

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

#### `tf.contrib.distributions.bijector.Softplus.inverse_and_inverse_log_det_jacobian(x, name='inverse_and_inverse_log_det_jacobian')` {#Softplus.inverse_and_inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Softplus.inverse_log_det_jacobian(x, name='inverse_log_det_jacobian')` {#Softplus.inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Softplus.is_constant_jacobian` {#Softplus.is_constant_jacobian}

Returns true iff the Jacobian is not a function of x.

Note: Jacobian is either constant for both forward and inverse or neither.

##### Returns:

  `Boolean`.


- - -

#### `tf.contrib.distributions.bijector.Softplus.name` {#Softplus.name}

Returns the string name of this `Bijector`.


- - -

#### `tf.contrib.distributions.bijector.Softplus.parameters` {#Softplus.parameters}

Returns this `Bijector`'s parameters as a name/value dictionary.


- - -

#### `tf.contrib.distributions.bijector.Softplus.shaper` {#Softplus.shaper}

Returns shape object used to manage shape constraints.


- - -

#### `tf.contrib.distributions.bijector.Softplus.validate_args` {#Softplus.validate_args}

Returns True if Tensor arguments will be validated.


