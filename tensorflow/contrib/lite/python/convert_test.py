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
"""TensorFlow Lite Python Interface: Sanity check."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.contrib.lite.python import convert
from tensorflow.contrib.lite.python import lite_constants
from tensorflow.contrib.lite.python import op_hint
from tensorflow.python.client import session
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import test_util
from tensorflow.python.framework.graph_util_impl import _bfs_for_reachable_nodes
from tensorflow.python.framework.graph_util_impl import _extract_graph_summary
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops
from tensorflow.python.platform import test


class ConvertTest(test_util.TensorFlowTestCase):

  def testBasic(self):
    in_tensor = array_ops.placeholder(shape=[1, 16, 16, 3],
                                      dtype=dtypes.float32)
    out_tensor = in_tensor + in_tensor
    sess = session.Session()
    # Try running on valid graph
    result = convert.toco_convert(sess.graph_def, [in_tensor], [out_tensor])
    self.assertTrue(result)
    # TODO(aselle): remove tests that fail (we must get TOCO to not fatal
    # all the time).
    # Try running on identity graph (known fail)
    # with self.assertRaisesRegexp(RuntimeError, "!model->operators.empty()"):
    #   result = convert.toco_convert(sess.graph_def, [in_tensor], [in_tensor])

  def testQuantization(self):
    in_tensor = array_ops.placeholder(shape=[1, 16, 16, 3],
                                      dtype=dtypes.float32)
    out_tensor = array_ops.fake_quant_with_min_max_args(in_tensor + in_tensor,
                                                        min=0., max=1.)
    sess = session.Session()
    result = convert.toco_convert(
        sess.graph_def, [in_tensor], [out_tensor],
        inference_type=lite_constants.QUANTIZED_UINT8,
        quantized_input_stats=[(0., 1.)])
    self.assertTrue(result)


class ConvertTestOpHint(test_util.TensorFlowTestCase):
  """Test the hint to stub functionality."""

  def _getGraphOpTypes(self, graphdef, output_nodes):
    """Returns used op types in `graphdef` reachable from `output_nodes`.

    This is used to check that after the stub transformation the expected
    nodes are there. Typically use this with self.assertCountEqual(...).

    NOTE: this is not a exact test that the graph is the correct output, but
      it balances compact expressibility of test with sanity checking.

    Args:
      graphdef: TensorFlow proto graphdef.
      output_nodes: A list of output node names that we need to reach.

    Returns:
      A set of node types reachable from `output_nodes`.
    """
    name_to_input_name, name_to_node, _ = (
        _extract_graph_summary(graphdef))
    # Find all nodes that are needed by the outputs
    used_node_names = _bfs_for_reachable_nodes(output_nodes, name_to_input_name)
    return set([name_to_node[node_name].op for node_name in used_node_names])

  def _countIdentities(self, nodes):
    """Count the number of "Identity" op types in the list of proto nodes.

    Args:
      nodes: NodeDefs of the graph.

    Returns:
      The number of nodes with op type "Identity" found.
    """
    return len([x for x in nodes if x.op == "Identity"])

  def testSwishLiteHint(self):
    """Makes a custom op swish and makes sure it gets converted as a unit."""
    image = array_ops.constant([1., 2., 3., 4.])
    swish_scale = array_ops.constant(1.0)

    def _swish(input_tensor, scale):
      custom = op_hint.OpHint("cool_activation")
      input_tensor, scale = custom.add_inputs(input_tensor, scale)
      output = math_ops.sigmoid(input_tensor) * input_tensor * scale
      output, = custom.add_outputs(output)
      return output
    output = array_ops.identity(_swish(image, swish_scale), name="ModelOutput")

    with self.test_session() as sess:
      # check if identities have been put into the graph (2 input, 1 output,
      # and 1 final output).
      self.assertEqual(self._countIdentities(sess.graph_def.node), 4)

      stubbed_graphdef = op_hint.convert_op_hints_to_stubs(
          graph_def=sess.graph_def)

      self.assertCountEqual(
          self._getGraphOpTypes(
              stubbed_graphdef,
              output_nodes=[op_hint._tensor_name_base(output.name)]),
          ["cool_activation", "Const", "Identity"])

  def testScaleAndBiasAndIdentity(self):
    """This tests a scaled add which has 3 inputs and 2 outputs."""
    a = array_ops.constant(1.)
    x = array_ops.constant([2., 3.])
    b = array_ops.constant([4., 5.])

    def _scaled_and_bias_and_identity(a, x, b):
      custom = op_hint.OpHint("scale_and_bias_and_identity")
      a, x, b = custom.add_inputs(a, x, b)
      return custom.add_outputs(a * x + b, x)
    output = array_ops.identity(_scaled_and_bias_and_identity(a, x, b),
                                name="ModelOutput")

    with self.test_session() as sess:
      # make sure one identity for each input (3) and output (2) => 3 + 2 = 5
      # +1 for the final output
      self.assertEqual(self._countIdentities(sess.graph_def.node), 6)

      stubbed_graphdef = op_hint.convert_op_hints_to_stubs(
          graph_def=sess.graph_def)

      self.assertCountEqual(
          self._getGraphOpTypes(
              stubbed_graphdef,
              output_nodes=[op_hint._tensor_name_base(output.name)]),
          ["scale_and_bias_and_identity", "Const", "Identity", "Pack"])

  def testTwoFunctions(self):
    """Tests if two functions are converted correctly."""
    a = array_ops.constant([1.])
    b = array_ops.constant([1.])
    def _double_values(x):
      custom = op_hint.OpHint("add_test")
      x, = custom.add_inputs(x)
      output = math_ops.multiply(x, x)
      output, = custom.add_outputs(output)
      return output
    output = array_ops.identity(
        math_ops.add(_double_values(a), _double_values(b)), name="ModelOutput")

    with self.test_session() as sess:
      # make sure one identity for each input (2) and output (2) => 2 + 2
      # +1 for the final output
      self.assertEqual(self._countIdentities(sess.graph_def.node), 5)
      stubbed_graphdef = op_hint.convert_op_hints_to_stubs(
          graph_def=sess.graph_def)
      self.assertCountEqual(
          self._getGraphOpTypes(
              stubbed_graphdef,
              output_nodes=[op_hint._tensor_name_base(output.name)]),
          ["add_test", "Const", "Identity", "Add"])

  def _get_input_index(self, x):
    return x.op.node_def.attr[op_hint.OpHint.FUNCTION_INPUT_INDEX_ATTR].i

  def _get_output_index(self, x):
    return x.op.node_def.attr[op_hint.OpHint.FUNCTION_OUTPUT_INDEX_ATTR].i

  def _get_sort_index(self, x):
    return x.op.node_def.attr[op_hint.OpHint.FUNCTION_SORT_INDEX_ATTR].i

  def testTags(self):
    """Test if multiple args with the same tag are grouped."""
    a = array_ops.constant([1.])
    b = array_ops.constant([2.])
    c = array_ops.constant([3.])
    d = array_ops.constant([4.])
    custom = op_hint.OpHint("test_tag")
    a = custom.add_input(a, tag="mytag",
                         aggregate=op_hint.OpHint.AGGREGATE_STACK)
    b, = custom.add_inputs(b)
    c = custom.add_input(c, tag="mytag",
                         aggregate=op_hint.OpHint.AGGREGATE_STACK)
    d = custom.add_input(d, tag="mytag2",
                         aggregate=op_hint.OpHint.AGGREGATE_STACK)
    res = math_ops.add(math_ops.mul(a, b), math_ops.mul(c, b))
    custom.add_outputs([res])
    with self.test_session():
      self.assertEqual(self._get_input_index(a), 0L)
      self.assertEqual(self._get_sort_index(a), 0L)
      self.assertEqual(self._get_input_index(b), 1L)
      self.assertEqual(self._get_input_index(c), 0L)
      self.assertEqual(self._get_sort_index(c), 1L)

  def testOverrideIndex(self):
    a = array_ops.constant([1.])
    b = array_ops.constant([2.])
    c = array_ops.constant([3.])
    custom = op_hint.OpHint("test_override")
    b = custom.add_input(b)  # should auto assign 0
    a = custom.add_input(a, index_override=1)
    c = custom.add_input(c)  # should auto assign 2
    with self.test_session():
      self.assertEqual(self._get_input_index(a), 1L)
      self.assertEqual(self._get_input_index(b), 0L)
      self.assertEqual(self._get_input_index(c), 2L)

  def testAggregate(self):
    a = array_ops.constant([3., 4.])
    b = array_ops.constant([5., 6.])
    hint = op_hint.OpHint("agg")
    a0, a1 = array_ops.unstack(a)
    b0, b1 = array_ops.unstack(b)

    a0 = hint.add_input(a0, tag="c", aggregate=op_hint.OpHint.AGGREGATE_STACK)
    b0 = hint.add_input(b0, tag="n", aggregate=op_hint.OpHint.AGGREGATE_STACK)
    a1 = hint.add_input(a1, tag="c", aggregate=op_hint.OpHint.AGGREGATE_STACK)
    b1 = hint.add_input(b1, tag="n", aggregate=op_hint.OpHint.AGGREGATE_STACK)

    c0 = math_ops.add(a0, b0, name="addleft")
    c1 = math_ops.add(a1, b1, name="addright")
    c0 = hint.add_output(
        c0, tag="out", aggregate=op_hint.OpHint.AGGREGATE_STACK)
    c1 = hint.add_output(
        c1, tag="out", aggregate=op_hint.OpHint.AGGREGATE_STACK)

    curr = array_ops.stack([c0, c1])
    output = array_ops.identity(curr, name="FINAL_OUTPUT")
    with self.test_session() as sess:
      stubbed_graphdef = op_hint.convert_op_hints_to_stubs(
          graph_def=sess.graph_def)
      print(stubbed_graphdef)
      self.assertCountEqual(
          self._getGraphOpTypes(
              stubbed_graphdef,
              output_nodes=[op_hint._tensor_name_base(output.name)]),
          ["agg", "Const", "Identity"])


if __name__ == "__main__":
  test.main()
