# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
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
"""Live entity inspection utilities.

This module contains whatever inspect doesn't offer out of the box.
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import itertools
import types

import six

from tensorflow.python.util import tf_inspect


def isbuiltin(f):
  """Returns True if the argument is a built-in function."""
  # Note these return false for isinstance(f, types.BuiltinFunctionType) so we
  # need to specifically check for them.
  if f in (range, int, float):
    return True
  if six.PY2:
    if f in (xrange,):
      return True
  if isinstance(f, types.BuiltinFunctionType):
    return True
  if tf_inspect.isbuiltin(f):
    return True
  return False


def getnamespace(f):
  """Returns the complete namespace of a function.

  Namespace is defined here as the mapping of all non-local variables to values.
  This includes the globals and the closure variables. Note that this captures
  the entire globals collection of the function, and may contain extra symbols
  that it does not actually use.

  Args:
    f: User defined function.
  Returns:
    A dict mapping symbol names to values.
  """
  namespace = dict(six.get_function_globals(f))
  closure = six.get_function_closure(f)
  freevars = six.get_function_code(f).co_freevars
  if freevars and closure:
    for name, cell in zip(freevars, closure):
      namespace[name] = cell.cell_contents
  return namespace


def getqualifiedname(namespace, object_, max_depth=2):
  """Returns the name by which a value can be referred to in a given namespace.

  This function will recurse inside modules, but it will not search objects for
  attributes. The recursion depth is controlled by max_depth.

  Args:
    namespace: Dict[str, Any], the namespace to search into.
    object_: Any, the value to search.
    max_depth: Optional[int], a limit to the recursion depth when searching
        inside modules.
  Returns: Union[str, None], the fully-qualified name that resolves to the value
      o, or None if it couldn't be found.
  """
  for name, value in namespace.items():
    # The value may be referenced by more than one symbol, case in which
    # any symbol will be fine. If the program contains symbol aliases that
    # change over time, this may capture a symbol that will later point to
    # something else.
    # TODO(mdan): Prefer the symbol that matches the value type name.
    if object_ is value:
      return name

  # TODO(mdan): Use breadth-first search and avoid visiting modules twice.
  if max_depth:
    # Iterating over a copy prevents "changed size due to iteration" errors.
    # It's unclear why those occur - suspecting new modules may load during
    # iteration.
    for name, value in namespace.copy().items():
      if tf_inspect.ismodule(value):
        name_in_module = getqualifiedname(value.__dict__, object_,
                                          max_depth - 1)
        if name_in_module is not None:
          return '{}.{}'.format(name, name_in_module)
  return None


def _get_unbound_function(m):
  # TODO(mdan): Figure out why six.get_unbound_function fails in some cases.
  # The failure case is for tf.keras.Model.
  if hasattr(m, 'im_func'):
    return m.im_func
  return m


def getdefiningclass(m, owner_class):
  """Resolves the class (e.g. one of the superclasses) that defined a method."""
  # Normalize bound functions to their respective unbound versions.
  m = _get_unbound_function(m)
  for superclass in owner_class.__bases__:
    if hasattr(superclass, m.__name__):
      superclass_m = getattr(superclass, m.__name__)
      if _get_unbound_function(superclass_m) is m:
        return superclass
      elif hasattr(m, '__self__') and m.__self__ == owner_class:
        # Python 3 class methods only work this way it seems :S
        return superclass
  return owner_class


def getmethodclass(m):
  """Resolves a function's owner, e.g. a method's class.

  Note that this returns the object that the function was retrieved from, not
  necessarily the class where it was defined.

  This function relies on Python stack frame support in the interpreter, and
  has the same limitations that inspect.currentframe.

  Limitations. This function will only work correctly if the owned class is
  visible in the caller's global or local variables.

  Args:
    m: A user defined function

  Returns:
    The class that this function was retrieved from, or None if the function
    is not an object or class method, or the class that owns the object or
    method is not visible to m.

  Raises:
    ValueError: if the class could not be resolved for any unexpected reason.
  """

  # Callable objects: return their own class.
  if (not hasattr(m, '__name__') and hasattr(m, '__class__') and
      hasattr(m, '__call__')):
    if isinstance(m.__class__, six.class_types):
      return m.__class__

  # Instance method and class methods: should be bound to a non-null "self".
  # If self is a class, then it's a class method.
  if hasattr(m, '__self__'):
    if m.__self__:
      if tf_inspect.isclass(m.__self__):
        return m.__self__
      return type(m.__self__)

  # Class, static and unbound methods: search all defined classes in any
  # namespace. This is inefficient but more robust method.
  owners = []
  caller_frame = tf_inspect.currentframe().f_back
  try:
    # TODO(mdan): This doesn't consider cell variables.
    # TODO(mdan): This won't work if the owner is hidden inside a container.
    # Cell variables may be pulled using co_freevars and the closure.
    for v in itertools.chain(caller_frame.f_locals.values(),
                             caller_frame.f_globals.values()):
      if hasattr(v, m.__name__):
        candidate = getattr(v, m.__name__)
        # Py2 methods may be bound or unbound, extract im_func to get the
        # underlying function.
        if hasattr(candidate, 'im_func'):
          candidate = candidate.im_func
        if hasattr(m, 'im_func'):
          m = m.im_func
        if candidate is m:
          owners.append(v)
  finally:
    del caller_frame

  if owners:
    if len(owners) == 1:
      return owners[0]

    # If multiple owners are found, and are not subclasses, raise an error.
    owner_types = tuple(o if tf_inspect.isclass(o) else type(o) for o in owners)
    for o in owner_types:
      if tf_inspect.isclass(o) and issubclass(o, tuple(owner_types)):
        return o
    raise ValueError('Found too many owners of %s: %s' % (m, owners))

  return None
