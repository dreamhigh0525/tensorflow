<!-- This file is machine generated: DO NOT EDIT! -->

# Random variable transformations (contrib)
[TOC]

Bijector Ops.

An API for reversible (bijective) transformations of random variables.

## Background

Differentiable, bijective transformations of continuous random variables alter
the calculations made in the cumulative/probability distribution functions and
sample function.  This module provides a standard interface for making these
manipulations.

For more details and examples, see the `Bijector` docstring.

To apply a `Bijector`, use `distributions.TransformedDistribution`.

## Bijectors

- - -

### `class tf.contrib.distributions.bijector.Bijector` {#Bijector}

Interface for transforming a `Distribution` via `TransformedDistribution`.

A `Bijector` implements a bijective, differentiable function by transforming
an input `Tensor`. The output `Tensor` shape is constrained by the input
`sample`, `batch`, and `event` shape.  A `Bijector` is characterized by three
operations:

1. Forward Evaluation

   Useful for turning one random outcome into another random outcome from a
   different distribution.

2. Inverse Evaluation

   Useful for "reversing" a transformation to compute one probability in
   terms of another.

3. (log o det o Jacobian o inverse)(x)

   "The log of the determinant of the matrix of all first-order partial
   derivatives of the inverse function."
   Useful for inverting a transformation to compute one probability in terms
   of another.  Geometrically, the det(Jacobian) is the volume of the
   transformation and is used to scale the probability.

By convention, transformations of random variables are named in terms of the
forward transformation. The forward transformation creates samples, the
inverse is useful for computing probabilities.

Example Use:

  - Basic properties:

  ```python
  x = ... # A tensor.
  # Evaluate forward transformation.
  fwd_x = my_bijector.forward(x)
  x == my_bijector.inverse(fwd_x)
  x != my_bijector.forward(fwd_x)  # Not equal because g(x) != g(g(x)).
  ```

  - Computing a log-likelihood:

  ```python
  def transformed_log_pdf(bijector, log_pdf, x):
    return (bijector.inverse_log_det_jacobian(x) +
            log_pdf(bijector.inverse(x)))
  ```

  - Transforming a random outcome:

  ```python
  def transformed_sample(bijector, x):
    return bijector.forward(x)
  ```

Example transformations:

  - "Exponential"

    ```
    Y = g(X) = exp(X)
    X ~ Normal(0, 1)  # Univariate.
    ```

    Implies:

    ```
      g^{-1}(Y) = log(Y)
      |Jacobian(g^{-1})(y)| = 1 / y
      Y ~ LogNormal(0, 1), i.e.,
      prob(Y=y) = |Jacobian(g^{-1})(y)| * prob(X=g^{-1}(y))
                = (1 / y) Normal(log(y); 0, 1)
    ```

  - "ScaleAndShift"

    ```
    Y = g(X) = sqrtSigma * X + mu
    X ~ MultivariateNormal(0, I_d)
    ```

    Implies:

    ```
      g^{-1}(Y) = inv(sqrtSigma) * (Y - mu)
      |Jacobian(g^{-1})(y)| = det(inv(sqrtSigma))
      Y ~ MultivariateNormal(mu, sqrtSigma) , i.e.,
      prob(Y=y) = |Jacobian(g^{-1})(y)| * prob(X=g^{-1}(y))
                = det(sqrtSigma)^(-d) *
                  MultivariateNormal(inv(sqrtSigma) * (y - mu); 0, I_d)
    ```

Example of why a `Bijector` needs to understand sample, batch, event
partitioning:

- Consider the `Exp` `Bijector` applied to a `Tensor` which has sample, batch,
  and event (S, B, E) shape semantics.  Suppose
  the `Tensor`'s partitioned-shape is `(S=[4], B=[2], E=[3, 3])`.

  For `Exp`, the shape of the `Tensor` returned by `forward` and `inverse` is
  unchanged, i.e., `[4, 2, 3, 3]`. However the shape returned by
  `inverse_log_det_jacobian` is `[4, 2]` because the Jacobian is a reduction
  over the event dimensions.

Subclass Requirements:

- Subclasses are expected to implement `_forward` and one or both of:
    - `_inverse`, `_inverse_log_det_jacobian`,
    - `_inverse_and_inverse_log_det_jacobian`.

- If computation can be shared among `_inverse` and
  `_inverse_log_det_jacobian` it is preferable to implement
  `_inverse_and_inverse_log_det_jacobian`. This usually reduces
  graph-construction overhead because a `Distribution`'s implementation of
  `log_prob` will need to evaluate both the inverse Jacobian as well as the
  inverse function.

- If an additional use case needs just `inverse` or just
  `inverse_log_det_jacobian` then he or she may also wish to implement these
  functions to avoid computing the `inverse_log_det_jacobian` or the
  `inverse`, respectively.
- - -

#### `tf.contrib.distributions.bijector.Bijector.__init__(batch_ndims=None, event_ndims=None, parameters=None, is_constant_jacobian=False, validate_args=False, dtype=None, name=None)` {#Bijector.__init__}

Constructs Bijector.

A `Bijector` transforms random variables into new random variables.

Examples:

```python
# Create the Y = g(X) = X transform which operates on 4-Tensors of vectors.
identity = Identity(batch_ndims=4, event_ndims=1)

# Create the Y = g(X) = exp(X) transform which operates on matrices.
exp = Exp(batch_ndims=0, event_ndims=2)
```

See `Bijector` subclass docstring for more details and specific examples.

##### Args:


*  <b>`batch_ndims`</b>: number of dimensions associated with batch coordinates.
*  <b>`event_ndims`</b>: number of dimensions associated with event coordinates.
*  <b>`parameters`</b>: Dictionary of parameters used by this `Bijector`
*  <b>`is_constant_jacobian`</b>: `Boolean` indicating that the Jacobian is not a
    function of the input.
*  <b>`validate_args`</b>: `Boolean`, default `False`.  Whether to validate input with
    asserts. If `validate_args` is `False`, and the inputs are invalid,
    correct behavior is not guaranteed.
*  <b>`dtype`</b>: `tf.dtype` supported by this `Bijector`. `None` means dtype is not
    enforced.
*  <b>`name`</b>: The name to give Ops created by the initializer.


- - -

#### `tf.contrib.distributions.bijector.Bijector.dtype` {#Bijector.dtype}

dtype of `Tensor`s transformable by this distribution.


- - -

#### `tf.contrib.distributions.bijector.Bijector.forward(x, name='forward')` {#Bijector.forward}

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

#### `tf.contrib.distributions.bijector.Bijector.inverse(x, name='inverse')` {#Bijector.inverse}

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

#### `tf.contrib.distributions.bijector.Bijector.inverse_and_inverse_log_det_jacobian(x, name='inverse_and_inverse_log_det_jacobian')` {#Bijector.inverse_and_inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Bijector.inverse_log_det_jacobian(x, name='inverse_log_det_jacobian')` {#Bijector.inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Bijector.is_constant_jacobian` {#Bijector.is_constant_jacobian}

Returns true iff the Jacobian is not a function of x.

Note: Jacobian is either constant for both forward and inverse or neither.

##### Returns:

  `Boolean`.


- - -

#### `tf.contrib.distributions.bijector.Bijector.name` {#Bijector.name}

Returns the string name of this `Bijector`.


- - -

#### `tf.contrib.distributions.bijector.Bijector.parameters` {#Bijector.parameters}

Returns this `Bijector`'s parameters as a name/value dictionary.


- - -

#### `tf.contrib.distributions.bijector.Bijector.shaper` {#Bijector.shaper}

Returns shape object used to manage shape constraints.


- - -

#### `tf.contrib.distributions.bijector.Bijector.validate_args` {#Bijector.validate_args}

Returns True if Tensor arguments will be validated.



- - -

### `class tf.contrib.distributions.bijector.Identity` {#Identity}

Bijector which computes Y = g(X) = X.

Example Use:

```python
# Create the Y=g(X)=X transform which is intended for Tensors with 1 batch
# ndim and 1 event ndim (i.e., vector of vectors).
identity = Identity(batch_ndims=1, event_ndims=1)
x = [[1., 2],
     [3, 4]]
x == identity.forward(x) == identity.inverse(x)
```
- - -

#### `tf.contrib.distributions.bijector.Identity.__init__(validate_args=False, name='Identity')` {#Identity.__init__}




- - -

#### `tf.contrib.distributions.bijector.Identity.dtype` {#Identity.dtype}

dtype of `Tensor`s transformable by this distribution.


- - -

#### `tf.contrib.distributions.bijector.Identity.forward(x, name='forward')` {#Identity.forward}

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

#### `tf.contrib.distributions.bijector.Identity.inverse(x, name='inverse')` {#Identity.inverse}

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

#### `tf.contrib.distributions.bijector.Identity.inverse_and_inverse_log_det_jacobian(x, name='inverse_and_inverse_log_det_jacobian')` {#Identity.inverse_and_inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Identity.inverse_log_det_jacobian(x, name='inverse_log_det_jacobian')` {#Identity.inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Identity.is_constant_jacobian` {#Identity.is_constant_jacobian}

Returns true iff the Jacobian is not a function of x.

Note: Jacobian is either constant for both forward and inverse or neither.

##### Returns:

  `Boolean`.


- - -

#### `tf.contrib.distributions.bijector.Identity.name` {#Identity.name}

Returns the string name of this `Bijector`.


- - -

#### `tf.contrib.distributions.bijector.Identity.parameters` {#Identity.parameters}

Returns this `Bijector`'s parameters as a name/value dictionary.


- - -

#### `tf.contrib.distributions.bijector.Identity.shaper` {#Identity.shaper}

Returns shape object used to manage shape constraints.


- - -

#### `tf.contrib.distributions.bijector.Identity.validate_args` {#Identity.validate_args}

Returns True if Tensor arguments will be validated.



- - -

### `class tf.contrib.distributions.bijector.Inline` {#Inline}

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



- - -

### `class tf.contrib.distributions.bijector.Exp` {#Exp}

Bijector which computes Y = g(X) = exp(X).

Example Use:

```python
# Create the Y=g(X)=exp(X) transform which works only on Tensors with 1
# batch ndim and 2 event ndims (i.e., vector of matrices).
exp = Exp(batch_ndims=1, event_ndims=2)
x = [[[1., 2],
       [3, 4]],
      [[5, 6],
       [7, 8]]]
exp(x) == exp.forward(x)
log(x) == exp.inverse(x)
```

Note: the exp(.) is applied element-wise but the Jacobian is a reduction
over the event space.
- - -

#### `tf.contrib.distributions.bijector.Exp.__init__(event_ndims=0, validate_args=False, name='Exp')` {#Exp.__init__}




- - -

#### `tf.contrib.distributions.bijector.Exp.dtype` {#Exp.dtype}

dtype of `Tensor`s transformable by this distribution.


- - -

#### `tf.contrib.distributions.bijector.Exp.forward(x, name='forward')` {#Exp.forward}

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

#### `tf.contrib.distributions.bijector.Exp.inverse(x, name='inverse')` {#Exp.inverse}

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

#### `tf.contrib.distributions.bijector.Exp.inverse_and_inverse_log_det_jacobian(x, name='inverse_and_inverse_log_det_jacobian')` {#Exp.inverse_and_inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Exp.inverse_log_det_jacobian(x, name='inverse_log_det_jacobian')` {#Exp.inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.Exp.is_constant_jacobian` {#Exp.is_constant_jacobian}

Returns true iff the Jacobian is not a function of x.

Note: Jacobian is either constant for both forward and inverse or neither.

##### Returns:

  `Boolean`.


- - -

#### `tf.contrib.distributions.bijector.Exp.name` {#Exp.name}

Returns the string name of this `Bijector`.


- - -

#### `tf.contrib.distributions.bijector.Exp.parameters` {#Exp.parameters}

Returns this `Bijector`'s parameters as a name/value dictionary.


- - -

#### `tf.contrib.distributions.bijector.Exp.shaper` {#Exp.shaper}

Returns shape object used to manage shape constraints.


- - -

#### `tf.contrib.distributions.bijector.Exp.validate_args` {#Exp.validate_args}

Returns True if Tensor arguments will be validated.



- - -

### `class tf.contrib.distributions.bijector.ScaleAndShift` {#ScaleAndShift}

Bijector which computes Y = g(X; loc, scale) = scale * X + loc.

Example Use:

```python
# No batch, scalar.
mu = 0     # shape=[]
sigma = 1  # shape=[]
b = ScaleAndShift(loc=mu, scale=sigma)
# b.shaper.batch_ndims == 0
# b.shaper.event_ndims == 0

# One batch, scalar.
mu = ...    # shape=[b], b>0
sigma = ... # shape=[b], b>0
b = ScaleAndShift(loc=mu, scale=sigma)
# b.shaper.batch_ndims == 1
# b.shaper.event_ndims == 0

# No batch, multivariate.
mu = ...    # shape=[d],    d>0
sigma = ... # shape=[d, d], d>0
b = ScaleAndShift(loc=mu, scale=sigma, event_ndims=1)
# b.shaper.batch_ndims == 0
# b.shaper.event_ndims == 1

# (B1*B2*...*Bb)-batch, multivariate.
mu = ...    # shape=[B1,...,Bb, d],    b>0, d>0
sigma = ... # shape=[B1,...,Bb, d, d], b>0, d>0
b = ScaleAndShift(loc=mu, scale=sigma, event_ndims=1)
# b.shaper.batch_ndims == b
# b.shaper.event_ndims == 1

# Mu is broadcast:
mu = 1
sigma = [I, I]  # I is a 3x3 identity matrix.
b = ScaleAndShift(loc=mu, scale=sigma, event_ndims=1)
x = numpy.ones(S + sigma.shape)
b.forward(x) # == x + 1
```
- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.__init__(loc, scale, event_ndims=0, validate_args=False, name='ScaleAndShift')` {#ScaleAndShift.__init__}




- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.dtype` {#ScaleAndShift.dtype}

dtype of `Tensor`s transformable by this distribution.


- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.forward(x, name='forward')` {#ScaleAndShift.forward}

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

#### `tf.contrib.distributions.bijector.ScaleAndShift.inverse(x, name='inverse')` {#ScaleAndShift.inverse}

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

#### `tf.contrib.distributions.bijector.ScaleAndShift.inverse_and_inverse_log_det_jacobian(x, name='inverse_and_inverse_log_det_jacobian')` {#ScaleAndShift.inverse_and_inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.ScaleAndShift.inverse_log_det_jacobian(x, name='inverse_log_det_jacobian')` {#ScaleAndShift.inverse_log_det_jacobian}

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

#### `tf.contrib.distributions.bijector.ScaleAndShift.is_constant_jacobian` {#ScaleAndShift.is_constant_jacobian}

Returns true iff the Jacobian is not a function of x.

Note: Jacobian is either constant for both forward and inverse or neither.

##### Returns:

  `Boolean`.


- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.loc` {#ScaleAndShift.loc}




- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.name` {#ScaleAndShift.name}

Returns the string name of this `Bijector`.


- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.parameters` {#ScaleAndShift.parameters}

Returns this `Bijector`'s parameters as a name/value dictionary.


- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.scale` {#ScaleAndShift.scale}




- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.shaper` {#ScaleAndShift.shaper}

Returns shape object used to manage shape constraints.


- - -

#### `tf.contrib.distributions.bijector.ScaleAndShift.validate_args` {#ScaleAndShift.validate_args}

Returns True if Tensor arguments will be validated.



