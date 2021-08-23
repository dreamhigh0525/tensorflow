# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for operator dispatch."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import collections
import typing

from tensorflow.python.eager import context
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import extension_type
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gen_math_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.ops.linalg import linear_operator_diag
from tensorflow.python.ops.proto_ops import decode_proto
from tensorflow.python.platform import googletest
from tensorflow.python.platform import test
from tensorflow.python.platform import tf_logging
from tensorflow.python.util import deprecation
from tensorflow.python.util import dispatch
from tensorflow.python.util import nest
from tensorflow.python.util.tf_export import get_canonical_name_for_symbol
from tensorflow.python.util.tf_export import tf_export


class CustomTensor(object):
  """A fake composite tensor class, for testing type-based dispatching."""

  def __init__(self, tensor, score):
    self.tensor = ops.convert_to_tensor(tensor)
    self.score = score


@tf_export("test_op")
@dispatch.add_dispatch_support
def test_op(x, y, z):
  """A fake op for testing dispatch of Python ops."""
  return x + (2 * y) + (3 * z)


class TensorTracer(object):
  """An object used to trace TensorFlow graphs.

  This is an example class that is used to test global op dispatchers.  The
  global op dispatcher for TensorTracers is defined below.
  """

  def __init__(self, name, args=None, kwargs=None):
    self.name = name
    self.args = args
    self.kwargs = kwargs
    self.shape = array_ops.ones(shape=(4, 4)).shape
    self.dtype = dtypes.float32

  def __repr__(self):
    if self.args is None and self.kwargs is None:
      return self.name
    else:
      args = [str(x) for x in self.args]
      args += sorted(
          ["{}={}".format(name, x) for (name, x) in self.kwargs.items()])
      return "{}({})".format(self.name, ", ".join(args))

  @property
  def is_tensor_like(self):
    return True

  @classmethod
  def _overload_all_operators(cls):  # pylint: disable=invalid-name
    """Register overloads for all operators."""
    for operator in ops.Tensor.OVERLOADABLE_OPERATORS:
      cls._overload_operator(operator)

  @classmethod
  def _overload_operator(cls, operator):  # pylint: disable=invalid-name
    """Overload an operator with the same overloading as `ops.Tensor`."""
    tensor_oper = getattr(ops.Tensor, operator)

    # Compatibility with Python 2:
    # Python 2 unbound methods have type checks for the first arg,
    # so we need to extract the underlying function
    tensor_oper = getattr(tensor_oper, "__func__", tensor_oper)
    setattr(cls, operator, tensor_oper)


TensorTracer._overload_all_operators()  # pylint: disable=protected-access


class TensorTracerOpDispatcher(dispatch.GlobalOpDispatcher):
  """Global op dispatcher for TensorTracer."""

  def _flatten_with_slice_flattening(self, x):
    flat = []
    for val in nest.flatten(x):
      if isinstance(val, slice):
        flat.extend((val.start, val.stop, val.step))
      else:
        flat.append(val)
    return flat

  def handle(self, op, args, kwargs):
    # Dispatcher only applies if at least one arg is a TensorTracer.
    if not (any(self.is_tensor_tracer_arg(x) for x in args) or
            any(self.is_tensor_tracer_arg(x) for x in kwargs.values())):
      return self.NOT_SUPPORTED

    symbol_name = get_canonical_name_for_symbol(op)
    return TensorTracer(symbol_name, args, kwargs)

  def is_tensor_tracer_arg(self, value):
    return any(
        isinstance(x, TensorTracer)
        for x in self._flatten_with_slice_flattening(value))


@test_util.run_all_in_graph_and_eager_modes
class DispatchTest(test_util.TensorFlowTestCase):

  def testAddDispatchForTypes_With_CppOp(self):
    original_handlers = gen_math_ops.atan2._tf_fallback_dispatchers[:]

    # Override the behavior of gen_math_ops.atan2 and make it look like add.
    @dispatch.dispatch_for_types(gen_math_ops.atan2, CustomTensor)
    def custom_atan2(y, x, name=None):  # pylint: disable=unused-variable
      return CustomTensor(
          gen_math_ops.add(y.tensor, x.tensor, name), (x.score + y.score) / 2.0)

    self.assertEqual(
        len(math_ops.atan2._tf_fallback_dispatchers),
        len(original_handlers) + 1)

    # Test that we see the overridden behavior when using CustomTensors.
    x = CustomTensor([1., 2., 3.], 2.0)
    y = CustomTensor([7., 8., 2.], 0.0)
    x_plus_y = gen_math_ops.atan2(y, x)
    self.assertAllEqual(self.evaluate(x_plus_y.tensor), [8, 10, 5])
    self.assertNear(x_plus_y.score, 1.0, 0.001)

    # Test that we still get the right behavior when using normal Tensors.
    a = [1., 2., 3.]
    b = [7., 8., 2.]
    a_plus_b = gen_math_ops.atan2(a, b)
    self.assertAllClose(a_plus_b, [0.14189707, 0.24497867, 0.98279375])

    # Test that we still get a TypeError or ValueError if we pass some
    # type that's not supported by any dispatcher.
    with self.assertRaises((TypeError, ValueError)):
      gen_math_ops.atan2(a, None)

    # Clean up
    gen_math_ops.atan2._tf_fallback_dispatchers = original_handlers

  def testAddDispatchForTypes_With_PythonOp(self):
    original_handlers = test_op._tf_fallback_dispatchers[:]

    @dispatch.dispatch_for_types(test_op, CustomTensor)
    def override_for_test_op(x, y, z):  # pylint: disable=unused-variable
      return CustomTensor(
          test_op(x.tensor, y.tensor, z.tensor),
          (x.score + y.score + z.score) / 3.0)

    x = CustomTensor([1, 2, 3], 0.2)
    y = CustomTensor([7, 8, 2], 0.4)
    z = CustomTensor([0, 1, 2], 0.6)

    result = test_op(x, y, z)
    self.assertAllEqual(self.evaluate(result.tensor), [15, 21, 13])
    self.assertNear(result.score, 0.4, 0.001)

    # Clean up
    test_op._tf_fallback_dispatchers = original_handlers

  def testDispatchForTypes_SignatureMismatch(self):
    with self.assertRaisesRegex(
        AssertionError, "The decorated function's "
        "signature must exactly match.*"):

      @dispatch.dispatch_for_types(test_op, CustomTensor)
      def override_for_test_op(a, b, c):  # pylint: disable=unused-variable
        return CustomTensor(
            test_op(a.tensor, b.tensor, c.tensor),
            (a.score + b.score + c.score) / 3.0)

  def testDispatchForTypes_OpDoesNotSupportDispatch(self):

    def some_op(x, y):
      return x + y

    with self.assertRaisesRegex(AssertionError, "Dispatching not enabled for"):

      @dispatch.dispatch_for_types(some_op, CustomTensor)
      def override_for_some_op(x, y):  # pylint: disable=unused-variable
        return x if x.score > 0 else y

  @test.mock.patch.object(tf_logging, "warning", autospec=True)
  def testInteractionWithDeprecationWarning(self, mock_warning):

    @deprecation.deprecated(date=None, instructions="Instructions")
    @dispatch.add_dispatch_support
    def some_op(x):
      return x

    some_op(5)

    message = mock_warning.call_args[0][0] % mock_warning.call_args[0][1:]
    self.assertRegex(
        message, r".*some_op \(from __main__\) is deprecated and will be "
        "removed in a future version.*")

  def testGlobalDispatcher(self):
    original_global_dispatchers = dispatch._GLOBAL_DISPATCHERS
    try:
      TensorTracerOpDispatcher().register()

      x = TensorTracer("x")
      y = TensorTracer("y")
      trace = math_ops.reduce_sum(math_ops.add(math_ops.abs(x), y), axis=3)
      self.assertEqual(
          str(trace), "math.reduce_sum(math.add(math.abs(x), y), axis=3)")

      proto_val = TensorTracer("proto")
      trace = decode_proto(proto_val, "message_type", ["field"], ["float32"])
      self.assertIn("io.decode_proto(bytes=proto,", str(trace))

    finally:
      # Clean up.
      dispatch._GLOBAL_DISPATCHERS = original_global_dispatchers

  def testGlobalDispatcherConvertToTensor(self):
    original_global_dispatchers = dispatch._GLOBAL_DISPATCHERS
    try:
      TensorTracerOpDispatcher().register()

      x = TensorTracer("x")
      y = TensorTracer("y")
      trace = math_ops.add(
          math_ops.abs(ops.convert_to_tensor_v2_with_dispatch(x)), y)
      self.assertEqual(
          str(trace), "math.add(math.abs(convert_to_tensor(x)), y)")

    finally:
      # Clean up.
      dispatch._GLOBAL_DISPATCHERS = original_global_dispatchers

  def testGlobalDispatcherGetItem(self):
    original_global_dispatchers = dispatch._GLOBAL_DISPATCHERS
    try:
      TensorTracerOpDispatcher().register()

      x = TensorTracer("x")
      trace = x[0]
      self.assertEqual(str(trace), "__operators__.getitem(x, 0)")

      x = TensorTracer("x")
      y = TensorTracer("y")
      trace = x[y]
      self.assertEqual(str(trace), "__operators__.getitem(x, y)")

      x = TensorTracer("x")
      y = TensorTracer("y")
      trace = x[:y]  # pylint: disable=invalid-slice-index
      self.assertEqual(
          str(trace), "__operators__.getitem(x, slice(None, y, None))")

      x = array_ops.ones(shape=(3, 3))
      y = TensorTracer("y")
      trace = x[y]
      self.assertEqual(str(trace), "__operators__.getitem(%s, y)" % x)

      trace = x[:y]  # pylint: disable=invalid-slice-index
      self.assertEqual(
          str(trace), "__operators__.getitem(%s, slice(None, y, None))" % x)

    finally:
      # Clean up.
      dispatch._GLOBAL_DISPATCHERS = original_global_dispatchers

  def testGlobalDispatcherLinearOperators(self):
    original_global_dispatchers = dispatch._GLOBAL_DISPATCHERS
    try:
      TensorTracerOpDispatcher().register()

      x = TensorTracer("x")

      # To grab the eigenvalues the diag operator just calls convert_to_tensor
      # (twice) in this case.
      trace = linear_operator_diag.LinearOperatorDiag(x).eigvals()
      self.assertEqual(
          str(trace),
          "convert_to_tensor(convert_to_tensor(x, dtype=None, dtype_hint=None, "
          "name=diag))")

      # The diagonal tensor addition gets traced even though the linear_operator
      # API only uses dispatchable ops instead of directly exposing dispatching.
      trace = linear_operator_diag.LinearOperatorDiag(x).add_to_tensor(x)
      self.assertIn(
          "linalg.set_diag(convert_to_tensor(x, name=x), __operators__.add("
          "convert_to_tensor(x, dtype=None, dtype_hint=None, name=diag), "
          "linalg.diag_part(convert_to_tensor(x, name=x)), "
          "name=", str(trace))

      # The dispatch-supporting ops the non-singular check calls out to
      # get traced.
      trace = linear_operator_diag.LinearOperatorDiag(x).assert_non_singular()
      self.assertIn("debugging.assert_less", str(trace))
      self.assertIn(
          "message=Singular operator:  Diagonal contained zero values.",
          str(trace))

    finally:
      # Clean up.
      dispatch._GLOBAL_DISPATCHERS = original_global_dispatchers


class MaskedTensor(extension_type.ExtensionType):
  """Simple ExtensionType for testing v2 dispatch."""
  values: ops.Tensor
  mask: ops.Tensor


@test_util.run_all_in_graph_and_eager_modes
class DispatchV2Test(test_util.TensorFlowTestCase):

  def testDispatchForOneSignature(self):

    @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor, "y": MaskedTensor})
    def masked_add(x, y, name=None):
      with ops.name_scope(name):
        return MaskedTensor(x.values + y.values, x.mask & y.mask)

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = MaskedTensor([1, 1, 1, 1, 1], [1, 1, 0, 1, 0])
      z = math_ops.add(x, y)
      self.assertAllEqual(z.values, x.values + y.values)
      self.assertAllEqual(z.mask, x.mask & y.mask)

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchSignatureWithUnspecifiedParameter(self):
    @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor})
    def masked_add(x, y):
      if y is None:
        return x
      y_values = y.values if isinstance(y, MaskedTensor) else y
      y_mask = y.mask if isinstance(y, MaskedTensor) else True
      return MaskedTensor(x.values + y_values, x.mask & y_mask)

    try:
      a = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      b = constant_op.constant([10, 20, 30, 40, 50])
      c = [10, 20, 30, 40, 50]
      d = 50
      e = None
      # As long as `x` is a MaskedTensor, the dispatcher will be called
      # (regardless of the type for `y`):
      self.assertAllEqual(math_ops.add(a, b).values, [11, 22, 33, 44, 55])
      self.assertAllEqual(math_ops.add(a, c).values, [11, 22, 33, 44, 55])
      self.assertAllEqual(math_ops.add(a, d).values, [51, 52, 53, 54, 55])
      self.assertAllEqual(math_ops.add(a, e).values, [1, 2, 3, 4, 5])

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchForMultipleSignatures(self):

    @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor},
                           {"y": MaskedTensor})
    def masked_add(x, y, name=None):
      with ops.name_scope(name):
        x_values = x.values if isinstance(x, MaskedTensor) else x
        x_mask = x.mask if isinstance(x, MaskedTensor) else True
        y_values = y.values if isinstance(y, MaskedTensor) else y
        y_mask = y.mask if isinstance(y, MaskedTensor) else True
        return MaskedTensor(x_values + y_values, x_mask & y_mask)

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = constant_op.constant([10, 20, 30, 40, 50])
      z = math_ops.add(x, y)
      self.assertAllEqual(z.values, x.values + y)
      self.assertAllEqual(z.mask, x.mask)

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchForList(self):

    @dispatch.dispatch_for(array_ops.concat,
                           {"values": typing.List[MaskedTensor]})
    def masked_concat(values, axis, name=None):
      with ops.name_scope(name):
        return MaskedTensor(
            array_ops.concat([v.values for v in values], axis),
            array_ops.concat([v.mask for v in values], axis))

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = MaskedTensor([1, 1, 1], [1, 1, 0])
      z = array_ops.concat([x, y], axis=0)
      self.assertAllEqual(z.values, array_ops.concat([x.values, y.values], 0))
      self.assertAllEqual(z.mask, array_ops.concat([x.mask, y.mask], 0))

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(array_ops.concat, masked_concat)

  def testDispatchForUnion(self):
    MaybeMasked = typing.Union[MaskedTensor, ops.Tensor]

    @dispatch.dispatch_for(math_ops.add, {"x": MaybeMasked, "y": MaybeMasked})
    def masked_add(x, y, name=None):
      with ops.name_scope(name):
        x_values = x.values if isinstance(x, MaskedTensor) else x
        x_mask = x.mask if isinstance(x, MaskedTensor) else True
        y_values = y.values if isinstance(y, MaskedTensor) else y
        y_mask = y.mask if isinstance(y, MaskedTensor) else True
        return MaskedTensor(x_values + y_values, x_mask & y_mask)

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = constant_op.constant([10, 20, 30, 40, 50])
      z = math_ops.add(x, y)
      self.assertAllEqual(z.values, x.values + y)
      self.assertAllEqual(z.mask, x.mask)

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchForOptional(self):
    # Note: typing.Optional[X] == typing.Union[X, NoneType].

    @dispatch.dispatch_for(
        array_ops.where_v2, {
            "condition": MaskedTensor,
            "x": typing.Optional[MaskedTensor],
            "y": typing.Optional[MaskedTensor]
        })
    def masked_where(condition, x=None, y=None, name=None):
      del condition, x, y, name
      return "stub"

    try:
      x = MaskedTensor([True, False, True, True, True], [1, 0, 1, 1, 1])
      self.assertEqual(array_ops.where_v2(x), "stub")
      self.assertEqual(array_ops.where_v2(x, x, x), "stub")

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(array_ops.where_v2, masked_where)

  def testDispatchForSignatureFromAnnotations(self):

    @dispatch.dispatch_for(math_ops.add)
    def masked_add(x: MaskedTensor, y: MaskedTensor, name=None):
      with ops.name_scope(name):
        return MaskedTensor(x.values + y.values, x.mask & y.mask)

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = MaskedTensor([1, 1, 1, 1, 1], [1, 1, 0, 1, 0])
      z = math_ops.add(x, y)
      self.assertAllEqual(z.values, x.values + y.values)
      self.assertAllEqual(z.mask, x.mask & y.mask)

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchForPositionalSignature(self):

    @dispatch.dispatch_for(math_ops.add, {0: MaskedTensor, 1: MaskedTensor})
    def masked_add(x, y, name=None):
      with ops.name_scope(name):
        return MaskedTensor(x.values + y.values, x.mask & y.mask)

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = MaskedTensor([1, 1, 1, 1, 1], [1, 1, 0, 1, 0])
      z = math_ops.add(x, y)
      self.assertAllEqual(z.values, x.values + y.values)
      self.assertAllEqual(z.mask, x.mask & y.mask)

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchWithVarargs(self):

    @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor, "y": MaskedTensor})
    def masked_add(*args, **kwargs):
      self.assertAllEqual(args[0].values, x.values)
      self.assertAllEqual(args[1].values, y.values)
      self.assertEmpty(kwargs)
      return "stub"

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = MaskedTensor([1, 1, 1, 1, 1], [1, 1, 0, 1, 0])
      self.assertEqual(math_ops.add(x, y), "stub")

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchWithKwargs(self):

    @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor, "y": MaskedTensor})
    def masked_add(*args, **kwargs):
      self.assertAllEqual(kwargs["x"].values, x.values)
      self.assertAllEqual(kwargs["y"].values, y.values)
      self.assertEmpty(args)
      return "stub"

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = MaskedTensor([1, 1, 1, 1, 1], [1, 1, 0, 1, 0])
      self.assertEqual(math_ops.add(x=x, y=y), "stub")

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchErrorForBadAPI(self):

    def api_without_dispatch_support(x):
      return x + 1

    with self.assertRaisesRegex(ValueError, ".* does not support dispatch."):

      @dispatch.dispatch_for(api_without_dispatch_support, {"x": MaskedTensor})
      def my_version(x):  # pylint: disable=unused-variable
        del x

  def testDispatchErrorForNoSignature(self):
    with self.assertRaisesRegex(ValueError,
                                "must be called with at least one signature"):

      @dispatch.dispatch_for(math_ops.add)
      def my_add(x, y, name=None):  # pylint: disable=unused-variable
        del x, y, name

  def testDispatchErrorSignatureMismatchParamName(self):
    with self.assertRaisesRegex(
        ValueError, r"Dispatch function's signature \(x, why, name=None\) does "
        r"not match API's signature \(x, y, name=None\)."):

      @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor})
      def my_add(x, why, name=None):  # pylint: disable=unused-variable
        del x, why, name

  def testDispatchErrorSignatureMismatchExtraParam(self):
    with self.assertRaisesRegex(
        ValueError, r"Dispatch function's signature \(x, y, name=None, extra_"
        r"arg=None\) does not match API's signature \(x, y, name=None\)."):

      @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor})
      def my_add(x, y, name=None, extra_arg=None):  # pylint: disable=unused-variable
        del x, y, name, extra_arg

  def testDispatchErrorForUnsupportedTypeAnnotation(self):
    with self.assertRaisesRegex(
        ValueError,
        "Type annotation .* is not currently supported by dispatch."):

      @dispatch.dispatch_for(math_ops.add, {"x": typing.Tuple[MaskedTensor]})
      def my_add(x, y, name=None):  # pylint: disable=unused-variable
        del x, y, name

  def testDispatchErrorForUnknownParameter(self):
    with self.assertRaisesRegex(
        ValueError, "signature includes annotation for unknown parameter 'z'."):

      @dispatch.dispatch_for(math_ops.add, {"z": MaskedTensor})
      def my_add(x, y, name=None):  # pylint: disable=unused-variable
        del x, y, name

  def testDispatchErrorUnsupportedKeywordOnlyAnnotation(self):

    @dispatch.add_dispatch_support
    def foo(x, *, y):
      return x + y

    with self.assertRaisesRegex(
        ValueError, "Dispatch currently only supports type "
        "annotations for positional parameters"):

      @dispatch.dispatch_for(foo, {"y": MaskedTensor})
      def masked_foo(x, *, y):  # pylint: disable=unused-variable
        del x, y

  def testDispatchErrorBadSignatureType(self):
    with self.assertRaisesRegex(
        TypeError, "signatures must be dictionaries mapping parameter "
        "names to type annotations"):

      @dispatch.dispatch_for(math_ops.add, [MaskedTensor])
      def my_add(x, y, name=None):  # pylint: disable=unused-variable
        del x, y, name

    with self.assertRaisesRegex(
        TypeError, "signatures must be dictionaries mapping parameter "
        "names to type annotations"):

      @dispatch.dispatch_for(math_ops.multiply, {None: MaskedTensor})
      def my_multiply(x, y, name=None):  # pylint: disable=unused-variable
        del x, y, name

  def testDispatchErrorNotCallable(self):
    with self.assertRaisesRegex(TypeError,
                                "Expected dispatch_target to be callable"):
      dispatch.dispatch_for(math_ops.abs, {0: MaskedTensor})("not_callable")

  def testRegisterDispatchableType(self):
    Car = collections.namedtuple("Car", ["size", "speed"])
    dispatch.register_dispatchable_type(Car)

    @dispatch.dispatch_for(math_ops.add, {"x": Car, "y": Car})
    def add_car(x, y, name=None):
      with ops.name_scope(name):
        return Car(x.size + y.size, x.speed + y.speed)

    try:
      x = Car(constant_op.constant(1), constant_op.constant(3))
      y = Car(constant_op.constant(10), constant_op.constant(20))
      z = math_ops.add(x, y)
      self.assertAllEqual(z.size, 11)
      self.assertAllEqual(z.speed, 23)

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, add_car)

  def testTypeCheckersAreCached(self):
    checker1 = dispatch.make_type_checker(int)
    checker2 = dispatch.make_type_checker(int)
    self.assertIs(checker1, checker2)

  def testDispatchTargetWithNoNameArgument(self):

    @dispatch.dispatch_for(math_ops.add, {"x": MaskedTensor, "y": MaskedTensor})
    def masked_add(x, y):
      return MaskedTensor(x.values + y.values, x.mask & y.mask)

    try:
      x = MaskedTensor([1, 2, 3, 4, 5], [1, 0, 1, 1, 1])
      y = MaskedTensor([1, 1, 1, 1, 1], [1, 1, 0, 1, 0])

      # pass name w/ keyword arg
      a = math_ops.add(x, y, name="MyAdd")
      if not context.executing_eagerly():  # names not defined in eager mode.
        self.assertRegex(a.values.name, r"^MyAdd/add.*")
        self.assertRegex(a.mask.name, r"^MyAdd/and.*")

      # pass name w/ positional arg
      b = math_ops.add(x, y, "B")
      if not context.executing_eagerly():  # names not defined in eager mode.
        self.assertRegex(b.values.name, r"^B/add.*")
        self.assertRegex(b.mask.name, r"^B/and.*")

      # default name value
      c = math_ops.add(x, y)
      if not context.executing_eagerly():  # names not defined in eager mode.
        self.assertRegex(c.values.name, r"^add.*")
        self.assertRegex(c.mask.name, r"^and.*")

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)

  def testDispatchApiWithNoNameArg(self):
    # Note: The "tensor_equals" API has no "name" argument.
    signature = {"self": MaskedTensor, "other": MaskedTensor}

    @dispatch.dispatch_for(math_ops.tensor_equals, signature)
    def masked_tensor_equals(self, other):
      del self, other

    dispatch.unregister_dispatch_target(math_ops.tensor_equals,
                                        masked_tensor_equals)  # clean up.

    with self.assertRaisesRegexp(
        ValueError, r"Dispatch function's signature \(self, other, name=None\) "
        r"does not match API's signature \(self, other\)\."):

      @dispatch.dispatch_for(math_ops.tensor_equals, signature)
      def masked_tensor_equals_2(self, other, name=None):
        del self, other, name

      del masked_tensor_equals_2  # avoid pylint unused variable warning.

  def testDispatchWithIterableParams(self):
    # The add_n API supports having `inputs` be an iterable (and not just
    # a sequence).
    @dispatch.dispatch_for(math_ops.add_n,
                           {"inputs": typing.List[MaskedTensor]})
    def masked_add_n(inputs):
      masks = array_ops.stack([x.mask for x in inputs])
      return MaskedTensor(
          math_ops.add_n([x.values for x in inputs]),
          math_ops.reduce_all(masks, axis=0))

    try:
      generator = (MaskedTensor([i], [True]) for i in range(5))
      y = math_ops.add_n(generator)
      self.assertAllEqual(y.values, [0 + 1 + 2 + 3 + 4])
      self.assertAllEqual(y.mask, [True])

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add_n, masked_add_n)

  def testBadIterableParametersError(self):
    fn = lambda x: [t + 1 for t in x]
    with self.assertRaisesRegex(
        TypeError, "iterable_parameters should be a list or tuple of string"):
      dispatch.add_dispatch_support(iterable_parameters="x")(fn)

  def testUnregisterDispatchTargetBadTargetError(self):
    fn = lambda x: x + 1
    with self.assertRaisesRegex(ValueError, ".* does not support dispatch"):
      dispatch.unregister_dispatch_target(fn, fn)

  def testUnregisterDispatchTargetBadDispatchTargetError(self):
    fn = lambda x: x + 1
    with self.assertRaisesRegex(ValueError, ".* was not registered for .*"):
      dispatch.unregister_dispatch_target(math_ops.add, fn)

  def testAddDuplicateApiDisptacherError(self):
    some_op = lambda x: x
    some_op = dispatch.add_type_based_api_dispatcher(some_op)
    with self.assertRaisesRegex(
        ValueError, ".* already has a type-based API dispatcher."):
      some_op = dispatch.add_type_based_api_dispatcher(some_op)

  def testGetApisWithTypeBasedDispatch(self):
    dispatch_apis = dispatch.apis_with_type_based_dispatch()
    self.assertIn(math_ops.add, dispatch_apis)
    self.assertIn(array_ops.concat, dispatch_apis)

  def testTypeBasedDispatchTargetsFor(self):
    MaskedTensorList = typing.List[typing.Union[MaskedTensor, ops.Tensor]]
    try:
      @dispatch.dispatch_for(math_ops.add)
      def masked_add(x: MaskedTensor, y: MaskedTensor):
        del x, y

      @dispatch.dispatch_for(array_ops.concat)
      def masked_concat(values: MaskedTensorList, axis):
        del values, axis

      targets = dispatch.type_based_dispatch_signatures_for(MaskedTensor)
      expected = {math_ops.add: [{"x": MaskedTensor, "y": MaskedTensor}],
                  array_ops.concat: [{"values": MaskedTensorList}]}
      self.assertEqual(targets, expected)

    finally:
      # Clean up dispatch table.
      dispatch.unregister_dispatch_target(math_ops.add, masked_add)
      dispatch.unregister_dispatch_target(array_ops.concat, masked_concat)


if __name__ == "__main__":
  googletest.main()
