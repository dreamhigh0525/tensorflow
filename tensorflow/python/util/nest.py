# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

"""## Functions for working with arbitrarily nested sequences of elements.

This module is used to perform any operations on nested structures, which can be
specified as sequences that contain non-sequence elements or other sequences.
The utilities here assume (and do not check) that the nested structures form a
'tree', i.e. no references in the structure of the input of these functions
should be recursive.

@@is_sequence
@@flatten
@@pack_sequence_as
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections

import six


def _sequence_like(instance, args):
  """Converts the sequence `args` to the same type as `instance`.

  Args:
    instance: an instance of `tuple`, `list`, or a `namedtuple` class.
    args: elements to be converted to a sequence.

  Returns:
    `args` with the type of `instance`.
  """
  if (isinstance(instance, tuple) and
      hasattr(instance, "_fields") and
      isinstance(instance._fields, collections.Sequence) and
      all(isinstance(f, six.string_types) for f in instance._fields)):
    # This is a namedtuple
    return type(instance)(*args)
  else:
    # Not a namedtuple
    return type(instance)(args)


def _yield_flat_nest(nest):
  for n in nest:
    if is_sequence(n):
      for ni in _yield_flat_nest(n):
        yield ni
    else:
      yield n


def is_sequence(seq):
  """Returns a true if its input is a collections.Sequence (except strings).

  Args:
    seq: an input sequence.

  Returns:
    True if the sequence is a not a string and is a collections.Sequence.
  """
  return (isinstance(seq, collections.Sequence)
          and not isinstance(seq, six.string_types))


def flatten(nest):
  """Returns a flat sequence from a given nested structure.

  Args:
    nest: an arbitrarily nested structure.

  Returns:
    The flattened version of the input.

  Raises:
    TypeError: If the input is not a sequence.
  """
  if not is_sequence(nest):
    raise TypeError("input must be a sequence, but received %s" % nest)
  return _sequence_like(nest, list(_yield_flat_nest(nest)))


def _packed_nest_with_indices(structure, flat, index):
  """Helper function for pack_nest_as.

  Args:
    structure: Substructure (tuple of elements and/or tuples) to mimic
    flat: Flattened values to output substructure for.
    index: Index at which to start reading from flat.

  Returns:
    The tuple (new_index, child), where:
      * new_index - the updated index into `flat` having processed `structure`.
      * packed - the subset of `flat` corresponding to `structure`,
                 having started at `index`, and packed into the same nested
                 format.

  Raises:
    ValueError: if `structure` contains more elements than `flat`
      (assuming indexing starts from `index`).
  """
  packed = []
  for s in structure:
    if is_sequence(s):
      new_index, child = _packed_nest_with_indices(s, flat, index)
      packed.append(_sequence_like(s, child))
      index = new_index
    else:
      packed.append(flat[index])
      index += 1
  return index, packed


def pack_sequence_as(structure, flat_sequence):
  """Returns a given flattened sequence packed into a nest.

  Args:
    structure: tuple or list constructed of scalars and/or other tuples/lists.
    flat_sequence: flat sequence to pack.

  Returns:
    packed: `flat_sequence` converted to have the same recursive structure as
      `structure`.

  Raises:
    TypeError: If structure or flat_sequence is not a tuple or list.
    ValueError: If nest and structure have different element counts.
  """
  if not is_sequence(structure):
    raise TypeError("structure must be a sequence")
  if not is_sequence(flat_sequence):
    raise TypeError("flat_sequence must be a sequence")

  flat_structure = flatten(structure)
  if len(flat_structure) != len(flat_sequence):
    raise ValueError(
        "Could not pack sequence. Structure had %d elements, but flat_sequence "
        "had %d elements.  Structure: %s, flat_sequence: %s."
        % (len(flat_structure), len(flat_sequence), structure, flat_sequence))

  _, packed = _packed_nest_with_indices(structure, flat_sequence, 0)
  return _sequence_like(structure, packed)
