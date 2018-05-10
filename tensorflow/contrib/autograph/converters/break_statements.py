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
"""Canonicalizes break statements by de-sugaring into a control boolean."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.contrib.autograph.pyct import anno
from tensorflow.contrib.autograph.pyct import templates
from tensorflow.contrib.autograph.pyct import transformer
from tensorflow.contrib.autograph.pyct.static_analysis.annos import NodeAnno


# Tags for local state.
BREAK_USED = 'break_used'
CONTROL_VAR_NAME = 'control_var_name'


class BreakStatementTransformer(transformer.Base):
  """Canonicalizes break statements into additional conditionals."""

  def _track_body(self, nodes, break_var):
    self.enter_local_scope()
    self.set_local(CONTROL_VAR_NAME, break_var)
    nodes = self.visit_block(nodes)
    break_used = self.get_local(BREAK_USED, False)
    self.exit_local_scope()
    return nodes, break_used

  def visit_Break(self, node):
    self.set_local(BREAK_USED, True)
    var_name = self.get_local(CONTROL_VAR_NAME)
    # TODO(mdan): This will fail when expanded inside a top-level else block.
    template = """
      var_name = True
      continue
    """
    return templates.replace(template, var_name=var_name)

  def _guard_if_present(self, block, var_name):
    """Prevents the block from executing if var_name is set."""
    if not block:
      return block
    template = """
        if not var_name:
          block
      """
    node = templates.replace(
        template,
        var_name=var_name,
        block=block)
    return node

  def visit_While(self, node):
    scope = anno.getanno(node, NodeAnno.BODY_SCOPE)
    break_var = self.context.namer.new_symbol('break__', scope.referenced)

    node.test = self.visit(node.test)
    node.body, break_used = self._track_body(node.body, break_var)
    # A break in the else clause applies to the containing scope.
    node.orelse = self.visit_block(node.orelse)

    if break_used:
      template = """
        var_name = False
        while test and not var_name:
          body
        else:
          orelse
      """
      # Python's else clause only triggers if the loop exited cleanly (e.g.
      # break did not trigger).
      node = templates.replace(
          template,
          var_name=break_var,
          test=node.test,
          body=node.body,
          orelse=self._guard_if_present(node.orelse, break_var))

    return node

  def visit_For(self, node):
    scope = anno.getanno(node, NodeAnno.BODY_SCOPE)
    break_var = self.context.namer.new_symbol('break__', scope.referenced)

    node.target = self.visit(node.target)
    node.iter = self.visit(node.iter)
    node.body, break_used = self._track_body(node.body, break_var)
    # A break in the else clause applies to the containing scope.
    node.orelse = self.visit_block(node.orelse)

    if break_used:
      node.orelse = self._guard_if_present(node.orelse, break_var)
      template = """
        var_name = False
        for_stmt
      """
      # Python's else clause only triggers if the loop exited cleanly (e.g.
      # break did not trigger).
      node = templates.replace(
          template,
          var_name=break_var,
          for_stmt=node)
      extra_test = templates.replace_as_expression(
          'not var_name', var_name=break_var)
      anno.setanno(node[1], 'extra_test', extra_test)

    return node


def transform(node, context):
  return BreakStatementTransformer(context).visit(node)
