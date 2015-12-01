<!-- This file is machine generated: DO NOT EDIT! -->

# Sparse Tensors

Note: Functions taking `Tensor` arguments can also take anything accepted by
[`tf.convert_to_tensor`](../../api_docs/python/framework.md#convert_to_tensor).

[TOC]

## Sparse Tensor Representation

Tensorflow supports a `SparseTensor` representation for data that is sparse
in multiple dimensions. Contrast this representation with `IndexedSlices`,
which is efficient for representing tensors that are sparse in their first
dimension, and dense along all other dimensions.

- - -

### `class tf.SparseTensor` {#SparseTensor}

Represents a sparse tensor.

Tensorflow represents a sparse tensor as three separate dense tensors:
`indices`, `values`, and `dense_shape`.  In Python, the three tensors are
collected into a `SparseTensor` class for ease of use.  If you have separate
`indices`, `values`, and `dense_shape` tensors, wrap them in a `SparseTensor`
object before passing to the Ops below.

Concretely, the sparse tensor `SparseTensor(values, indices, dense_shape)` is

* `indices`: A 2-D int64 tensor of shape `[N, ndims]`.
* `values`: A 1-D tensor of any type and shape `[N]`.
* `dense_shape`: A 1-D int64 tensor of shape `[ndims]`.

where `N` and `ndims` are the number of values, and number of dimensions in
the `SparseTensor` respectively.

The corresponding dense tensor satisfies

```python
dense.shape = dense_shape
dense[tuple(indices[i])] = values[i]
```

By convention, `indices` should be sorted in row-major order (or equivalently
lexicographic order on the tuples `indices[i]`).  This is not enforced when
`SparseTensor` objects are constructed, but most ops assume correct ordering.
If the ordering is wrong, it can be fixed by calling `sparse_reorder` on the
misordered `SparseTensor`.

Example: The sparse tensor

```python
SparseTensor(values=[1, 2], indices=[[0, 0], [1, 2]], shape=[3, 4])
```

represents the dense tensor

```python
[[1, 0, 0, 0]
 [0, 0, 2, 0]
 [0, 0, 0, 0]]
```

- - -

#### `tf.SparseTensor.__init__(indices, values, shape)` {#SparseTensor.__init__}

Creates a `SparseTensor`.

##### Args:


*  <b>`indices`</b>: A 2-D int64 tensor of shape `[N, ndims]`.
*  <b>`values`</b>: A 1-D tensor of any type and shape `[N]`.
*  <b>`shape`</b>: A 1-D int64 tensor of shape `[ndims]`.

##### Returns:

  A `SparseTensor`


- - -

#### `tf.SparseTensor.indices` {#SparseTensor.indices}

The indices of non-zero values in the represented dense tensor.

##### Returns:

  A 2-D Tensor of int64 with shape `[N, ndims]`, where `N` is the
    number of non-zero values in the tensor, and `ndims` is the rank.


- - -

#### `tf.SparseTensor.values` {#SparseTensor.values}

The non-zero values in the represented dense tensor.

##### Returns:

  A 1-D Tensor of any data type.


- - -

#### `tf.SparseTensor.dtype` {#SparseTensor.dtype}

The `DType` of elements in this tensor.


- - -

#### `tf.SparseTensor.shape` {#SparseTensor.shape}

A 1-D Tensor of int64 representing the shape of the dense tensor.


- - -

#### `tf.SparseTensor.graph` {#SparseTensor.graph}

The `Graph` that contains the index, value, and shape tensors.



- - -

### `class tf.SparseTensorValue` {#SparseTensorValue}

SparseTensorValue(indices, values, shape)
- - -

#### `tf.SparseTensorValue.indices` {#SparseTensorValue.indices}

Alias for field number 0


- - -

#### `tf.SparseTensorValue.shape` {#SparseTensorValue.shape}

Alias for field number 2


- - -

#### `tf.SparseTensorValue.values` {#SparseTensorValue.values}

Alias for field number 1




## Sparse to Dense Conversion

- - -

### `tf.sparse_to_dense(sparse_indices, output_shape, sparse_values, default_value, name=None)` {#sparse_to_dense}

Converts a sparse representation into a dense tensor.

Builds an array `dense` with shape `output_shape` such that

```prettyprint
# If sparse_indices is scalar
dense[i] = (i == sparse_indices ? sparse_values : default_value)

# If sparse_indices is a vector, then for each i
dense[sparse_indices[i]] = sparse_values[i]

# If sparse_indices is an n by d matrix, then for each i in [0, n)
dense[sparse_indices[i][0], ..., sparse_indices[i][d-1]] = sparse_values[i]
```

All other values in `dense` are set to `default_value`.  If `sparse_values` is a
scalar, all sparse indices are set to this single value.

##### Args:


*  <b>`sparse_indices`</b>: A `Tensor`. Must be one of the following types: `int32`, `int64`.
    0-D, 1-D, or 2-D.  `sparse_indices[i]` contains the complete
    index where `sparse_values[i]` will be placed.
*  <b>`output_shape`</b>: A `Tensor`. Must have the same type as `sparse_indices`.
    1-D.  Shape of the dense output tensor.
*  <b>`sparse_values`</b>: A `Tensor`.
    1-D.  Values corresponding to each row of `sparse_indices`,
    or a scalar value to be used for all sparse indices.
*  <b>`default_value`</b>: A `Tensor`. Must have the same type as `sparse_values`.
    Scalar value to set for indices not specified in
    `sparse_indices`.
*  <b>`name`</b>: A name for the operation (optional).

##### Returns:

  A `Tensor`. Has the same type as `sparse_values`.
  Dense output tensor of shape `output_shape`.


- - -

### `tf.sparse_tensor_to_dense(sp_input, default_value, name=None)` {#sparse_tensor_to_dense}

Converts a `SparseTensor` into a dense tensor.

This op is a convenience wrapper around `sparse_to_dense` for `SparseTensor`s.

For example, if `sp_input` has shape `[3, 5]` and non-empty string values:

    [0, 1]: a
    [0, 3]: b
    [2, 0]: c

and `default_value` is `x`, then the output will be a dense `[3, 5]`
string tensor with values:

    [[x a x b x]
     [x x x x x]
     [c x x x x]]

##### Args:


*  <b>`sp_input`</b>: The input `SparseTensor`.
*  <b>`default_value`</b>: Scalar value to set for indices not specified in
    `sp_input`.
*  <b>`name`</b>: A name prefix for the returned tensors (optional).

##### Returns:

  A dense tensor with shape `sp_input.shape` and values specified by
  the non-empty values in `sp_input`. Indices not in `sp_input` are assigned
  `default_value`.

##### Raises:


*  <b>`TypeError`</b>: If `sp_input` is not a `SparseTensor`.


- - -

### `tf.sparse_to_indicator(sp_input, vocab_size, name=None)` {#sparse_to_indicator}

Converts a `SparseTensor` of ids into a dense bool indicator tensor.

The last dimension of `sp_input` is discarded and replaced with the values of
`sp_input`.  If `sp_input.shape = [D0, D1, ..., Dn, K]`, then
`output.shape = [D0, D1, ..., Dn, vocab_size]`, where

    output[d_0, d_1, ..., d_n, sp_input[d_0, d_1, ..., d_n, k]] = True

and False elsewhere in `output`.

For example, if `sp_input.shape = [2, 3, 4]` with non-empty values:

    [0, 0, 0]: 0
    [0, 1, 0]: 10
    [1, 0, 3]: 103
    [1, 1, 2]: 112
    [1, 1, 3]: 113
    [1, 2, 1]: 121

and `vocab_size = 200`, then the output will be a `[2, 3, 200]` dense bool
tensor with False everywhere except at positions

    (0, 0, 0), (0, 1, 10), (1, 0, 103), (1, 1, 112), (1, 1, 113), (1, 2, 121).

This op is useful for converting `SparseTensor`s into dense formats for
compatibility with ops that expect dense tensors.

The input `SparseTensor` must be in row-major order.

##### Args:


*  <b>`sp_input`</b>: A `SparseTensor` of type `int32` or `int64`.
*  <b>`vocab_size`</b>: The new size of the last dimension, with
    `all(0 <= sp_input.values < vocab_size)`.
*  <b>`name`</b>: A name prefix for the returned tensors (optional)

##### Returns:

  A dense bool indicator tensor representing the indices with specified value.

##### Raises:


*  <b>`TypeError`</b>: If `sp_input` is not a `SparseTensor`.



## Manipulation

- - -

### `tf.sparse_concat(concat_dim, sp_inputs, name=None)` {#sparse_concat}

Concatenates a list of `SparseTensor` along the specified dimension.

Concatenation is with respect to the dense versions of each sparse input.
It is assumed that each inputs is a `SparseTensor` whose elements are ordered
along increasing dimension number.

All inputs' shapes must match, except for the concat dimension.  The
`indices`, `values`, and `shapes` lists must have the same length.

The output shape is identical to the inputs', except along the concat
dimension, where it is the sum of the inputs' sizes along that dimension.

The output elements will be resorted to preserve the sort order along
increasing dimension number.

This op runs in `O(M log M)` time, where `M` is the total number of non-empty
values across all inputs. This is due to the need for an internal sort in
order to concatenate efficiently across an arbitrary dimension.

For example, if `concat_dim = 1` and the inputs are

    sp_inputs[0]: shape = [2, 3]
    [0, 2]: "a"
    [1, 0]: "b"
    [1, 1]: "c"

    sp_inputs[1]: shape = [2, 4]
    [0, 1]: "d"
    [0, 2]: "e"

then the output will be

    shape = [2, 7]
    [0, 2]: "a"
    [0, 4]: "d"
    [0, 5]: "e"
    [1, 0]: "b"
    [1, 1]: "c"

Graphically this is equivalent to doing

    [    a] concat [  d e  ] = [    a   d e  ]
    [b c  ]        [       ]   [b c          ]

##### Args:


*  <b>`concat_dim`</b>: Dimension to concatenate along.
*  <b>`sp_inputs`</b>: List of `SparseTensor` to concatenate.
*  <b>`name`</b>: A name prefix for the returned tensors (optional).

##### Returns:

  A `SparseTensor` with the concatenated output.

##### Raises:


*  <b>`TypeError`</b>: If `sp_inputs` is not a list of `SparseTensor`.


- - -

### `tf.sparse_reorder(sp_input, name=None)` {#sparse_reorder}

Reorders a `SparseTensor` into the canonical, row-major ordering.

Note that by convention, all sparse ops preserve the canonical ordering
along increasing dimension number. The only time ordering can be violated
is during manual manipulation of the indices and values to add entries.

Reordering does not affect the shape of the `SparseTensor`.

For example, if `sp_input` has shape `[4, 5]` and `indices` / `values`:

    [0, 3]: b
    [0, 1]: a
    [3, 1]: d
    [2, 0]: c

then the output will be a `SparseTensor` of shape `[4, 5]` and
`indices` / `values`:

    [0, 1]: a
    [0, 3]: b
    [2, 0]: c
    [3, 1]: d

##### Args:


*  <b>`sp_input`</b>: The input `SparseTensor`.
*  <b>`name`</b>: A name prefix for the returned tensors (optional)

##### Returns:

  A `SparseTensor` with the same shape and non-empty values, but in
  canonical ordering.

##### Raises:


*  <b>`TypeError`</b>: If `sp_input` is not a `SparseTensor`.


- - -

### `tf.sparse_retain(sp_input, to_retain)` {#sparse_retain}

Retains specified non-empty values within a `SparseTensor`.

For example, if `sp_input` has shape `[4, 5]` and 4 non-empty string values:

    [0, 1]: a
    [0, 3]: b
    [2, 0]: c
    [3, 1]: d

and `to_retain = [True, False, False, True]`, then the output will
be a `SparseTensor` of shape `[4, 5]` with 2 non-empty values:

    [0, 1]: a
    [3, 1]: d

##### Args:


*  <b>`sp_input`</b>: The input `SparseTensor` with `N` non-empty elements.
*  <b>`to_retain`</b>: A bool vector of length `N` with `M` true values.

##### Returns:

  A `SparseTensor` with the same shape as the input and `M` non-empty
  elements corresponding to the true positions in `to_retain`.

##### Raises:


*  <b>`TypeError`</b>: If `sp_input` is not a `SparseTensor`.


- - -

### `tf.sparse_fill_empty_rows(sp_input, default_value, name=None)` {#sparse_fill_empty_rows}

Fills empty rows in the input 2-D `SparseTensor` with a default value.

This op adds entries with the specified `default_value` at index
`[row, 0]` for any row in the input that does not already have a value.

For example, suppose `sp_input` has shape `[5, 6]` and non-empty values:

    [0, 1]: a
    [0, 3]: b
    [2, 0]: c
    [3, 1]: d

Rows 1 and 4 are empty, so the output will be of shape `[5, 6]` with values:

    [0, 1]: a
    [0, 3]: b
    [1, 0]: default_value
    [2, 0]: c
    [3, 1]: d
    [4, 0]: default_value

Note that the input may have empty columns at the end, with no effect on
this op.

The output `SparseTensor` will be in row-major order and will have the
same shape as the input.

This op also returns an indicator vector such that

    empty_row_indicator[i] = True iff row i was an empty row.

##### Args:


*  <b>`sp_input`</b>: A `SparseTensor` with shape `[N, M]`.
*  <b>`default_value`</b>: The value to fill for empty rows, with the same type as
    `sp_input.`
*  <b>`name`</b>: A name prefix for the returned tensors (optional)

##### Returns:


*  <b>`sp_ordered_output`</b>: A `SparseTensor` with shape `[N, M]`, and with all empty
    rows filled in with `default_value`.
*  <b>`empty_row_indicator`</b>: A bool vector of length `N` indicating whether each
    input row was empty.

##### Raises:


*  <b>`TypeError`</b>: If `sp_input` is not a `SparseTensor`.


