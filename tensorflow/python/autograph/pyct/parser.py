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
"""Converting code to AST.

Adapted from Tangent.
"""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import re
import textwrap
import tokenize

import gast
import six

from tensorflow.python.autograph.pyct import inspect_utils
from tensorflow.python.util import tf_inspect


STANDARD_PREAMBLE = textwrap.dedent("""
    from __future__ import division
    from __future__ import print_function
""")
STANDARD_PREAMBLE_LEN = 2


_LEADING_WHITESPACE = re.compile(r'\s*')


def dedent_block(code_string):
  """Dedents a code so that its first line starts at row zero."""

  token_gen = tokenize.generate_tokens(six.StringIO(code_string).readline)

  block_indentation = None
  tokens = []
  try:
    for tok in token_gen:
      tokens.append(tok)
  except tokenize.TokenError:
    # Resolution of lambda functions may yield incomplete code, which can
    # in turn generate this error. We silently ignore this error because the
    # parser may still be able to deal with it.
    pass

  for tok in tokens:
    tok_type, tok_string, _, _, _ = tok
    if tok_type == tokenize.INDENT:
      block_indentation = tok_string
      block_level = len(block_indentation)
      break
    elif tok_type not in (
        tokenize.NL, tokenize.NEWLINE, tokenize.STRING, tokenize.COMMENT):
      block_indentation = ''
      break

  if not block_indentation:
    return code_string

  block_level = len(block_indentation)
  first_indent_uses_tabs = '\t' in block_indentation
  for i, tok in enumerate(tokens):
    tok_type, tok_string, _, _, _ = tok
    if tok_type == tokenize.INDENT:
      if ((' ' in tok_string and first_indent_uses_tabs)
          or ('\t' in tok_string and not first_indent_uses_tabs)):
        # TODO(mdan): We could attempt to convert tabs to spaces by unix rule.
        # See:
        # https://docs.python.org/3/reference/lexical_analysis.html#indentation
        raise ValueError(
            'code mixing tabs and spaces for intentation is not allowed')
      if len(tok_string) >= block_level:
        tok_string = tok_string[block_level:]
      tokens[i] = (tok_type, tok_string)

  new_code = tokenize.untokenize(tokens)

  # Note: untokenize respects the line structure, but not the whitespace within
  # lines. For example, `def foo()` may be untokenized as `def foo ()`
  # So instead of using the output of dedent, we match the leading whitespace
  # on each line.
  dedented_code = []
  for line, new_line in zip(code_string.split('\n'), new_code.split('\n')):
    original_indent = re.match(_LEADING_WHITESPACE, line).group()
    new_indent = re.match(_LEADING_WHITESPACE, new_line).group()
    if len(original_indent) > len(new_indent):
      dedented_line = line[len(original_indent) - len(new_indent):]
    else:
      dedented_line = line
    dedented_code.append(dedented_line)
  new_code = '\n'.join(dedented_code)

  return new_code


def parse_entity(entity, future_features):
  """Returns the AST and source code of given entity.

  Args:
    entity: Any, Python function/method/class
    future_features: Iterable[Text], future features to use (e.g.
      'print_statement'). See
      https://docs.python.org/2/reference/simple_stmts.html#future

  Returns:
    gast.AST, Text: the parsed AST node; the source code that was parsed to
    generate the AST (including any prefixes that this function may have added).
  """
  try:
    original_source = inspect_utils.getimmediatesource(entity)
  except (IOError, OSError) as e:
    raise ValueError(
        'Unable to locate the source code of {}. Note that functions defined'
        ' in certain environments, like the interactive Python shell do not'
        ' expose their source code. If that is the case, you should to define'
        ' them in a .py source file. If you are certain the code is'
        ' graph-compatible, wrap the call using'
        ' @tf.autograph.do_not_convert. Original error: {}'.format(entity, e))

  def raise_parse_failure(comment):
    raise ValueError(
        'Failed to parse source code of {}, which Python reported as:\n{}\n'
        '{}'.format(entity, original_source, comment))

  source = dedent_block(original_source)

  future_statements = tuple(
      'from __future__ import {}'.format(name) for name in future_features)
  source = '\n'.join(future_statements + (source,))

  try:
    return parse_str(source, preamble_len=len(future_features)), source

  except IndentationError:
    # The text below lists the causes of this error known to us. There may
    # be more.
    raise_parse_failure(
        'This may be caused by multiline strings or comments not indented at'
        ' the same level as the code.')

  except SyntaxError as e:
    if not tf_inspect.isfunction(entity) or entity.__name__ != '<lambda>':
      raise

    # Certain entities, like lambdas, only hold the raw code lines which defined
    # them, which may include surrounding tokens and may be syntactically
    # invalid out of context. For example:
    #
    #     l = (
    #         lambda x: x,)[0]
    #
    # will have the dedented source "lambda x: x,)[0]"
    # Here we make an attempt to stip away the garbage by looking at the
    # information in the syntax error.
    lines = source.split('\n')
    lineno, offset = e.lineno, e.offset  # 1-based

    # Give up if there's nothing we can chip away.
    if len(lines) == lineno and len(lines[-1]) == offset:
      raise_parse_failure(
          'If this is a lambda function, the error may be avoided by creating'
          ' the lambda in a standalone statement.')

    # Drop all lines following the error location
    # TODO(mdan): What's with the pylint errors?
    lines = lines[:lineno]  # pylint:disable=invalid-slice-index
    # Drop all characters following the error location
    lines[-1] = lines[-1][:offset - 1]  # pylint:disable=invalid-slice-index
    source = '\n'.join(lines)

    try:
      return parse_str(source, preamble_len=len(future_features)), source
    except SyntaxError as e:
      raise_parse_failure(
          'If this is a lambda function, the error may be avoided by creating'
          ' the lambda in a standalone statement.')


# TODO(mdan): This should take futures as input instead.
def parse_str(src, preamble_len=0, single_node=True):
  """Returns the AST of given piece of code.

  Args:
    src: Text
    preamble_len: Int, indicates leading nodes in the parsed AST which should be
      dropped.
    single_node: Bool, whether `src` is assumed to be represented by exactly one
      AST node.

  Returns:
    ast.AST
  """
  module_node = gast.parse(src)
  nodes = module_node.body
  if preamble_len:
    nodes = nodes[preamble_len:]
  if single_node:
    if len(nodes) != 1:
      raise ValueError('expected exactly one node node, found {}'.format(nodes))
    return nodes[0]
  return nodes


def parse_expression(src):
  """Returns the AST of given identifier.

  Args:
    src: A piece of code that represents a single Python expression
  Returns:
    A gast.AST object.
  Raises:
    ValueError: if src does not consist of a single Expression.
  """
  src = STANDARD_PREAMBLE + src.strip()
  node = parse_str(src, preamble_len=STANDARD_PREAMBLE_LEN, single_node=True)
  if __debug__:
    if not isinstance(node, gast.Expr):
      raise ValueError(
          'expected a single expression, found instead {}'.format(node))
  return node.value
