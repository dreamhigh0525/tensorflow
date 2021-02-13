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
"""Tests for utilities working with arbitrarily nested structures."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from absl.testing import parameterized

from tensorflow.python.data.util import random_seed as data_random_seed
from tensorflow.python.eager import context
from tensorflow.python.data.kernel_tests import test_base
from tensorflow.python.framework import combinations
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import random_seed
from tensorflow.python.platform import test


# NOTE(vikoth18): Arguments of parameterized tests are lifted into lambdas to make
# sure they are not executed before the (eager- or graph-mode) test environment
# has been set up.
#
class RandomSeedTest(test_base.DatasetTestBase, parameterized.TestCase):

  @combinations.generate(
      combinations.times(
          test_base.default_test_combinations(),
          combinations.combine(test_case_fn=[
              # Each test case is a tuple with input to get_seed:
              # (input_graph_seed, input_op_seed)
              # and output from get_seed:
              # (output_graph_seed, output_op_seed)
              combinations.NamedObject(
                  "Case_0",
                  lambda: ((None, None), (0, 0))
              ),
              combinations.NamedObject(
                  "Case_1",
                  lambda: ((None, 1), (random_seed.DEFAULT_GRAPH_SEED, 1)),
              ),
              combinations.NamedObject(
                  "Case_2",
                  lambda: ((1, 1), (1, 1)),
              ),
              combinations.NamedObject(
                  "Case_3",
                  # Avoid nondeterministic (0, 0) output
                  lambda: ((0, 0), (0, 2**31 - 1)),
              ),
              combinations.NamedObject(
                  "Case_4",
                  # Don't wrap to (0, 0) either
                  lambda: ((2**31 - 1, 0), (0, 2**31 - 1)),
              ),
              combinations.NamedObject(
                  "Case_5",
                  # Wrapping for the other argument
                  lambda: ((0, 2**31 - 1), (0, 2**31 - 1)),
              ),
              combinations.NamedObject(
                  "Case_6",
                  # Once more, with tensor-valued arguments
                  lambda: ((None, constant_op.constant(
                      1, dtype=dtypes.int64, name='one')),
                           (random_seed.DEFAULT_GRAPH_SEED, 1)),
              ),
              combinations.NamedObject(
                  "Case_7",
                  lambda: ((1, constant_op.constant(1, dtype=dtypes.int64, name='one')),
                           (1, 1)),
              ),
              combinations.NamedObject(
                  "Case_8",
                  lambda: ((0, constant_op.constant(
                      0, dtype=dtypes.int64, name='zero')),
                           (0, 2**31 - 1)),  # Avoid nondeterministic (0, 0) output
              ),
              combinations.NamedObject(
                  "Case_9",
                  lambda: ((2**31 - 1, constant_op.constant(
                      0, dtype=dtypes.int64, name='zero')),
                           (0, 2**31 - 1)),  # Don't wrap to (0, 0) either
              ),
              combinations.NamedObject(
                  "Case_10",
                  lambda: ((0, constant_op.constant(
                      2**31 - 1, dtype=dtypes.int64, name='intmax')),
                           (0, 2**31 - 1)),  # Wrapping for the other argument
              )
          ])
      )
  )
  def testRandomSeed(self, test_case_fn):
    test_case_fn = test_case_fn._obj  # pylint: disable=protected-access
    test_case = test_case_fn()
    tinput, toutput = test_case[0], test_case[1]

    def check(tinput, toutput):
      random_seed.set_random_seed(tinput[0])
      g_seed, op_seed = data_random_seed.get_seed(tinput[1])
      g_seed = self.evaluate(g_seed)
      op_seed = self.evaluate(op_seed)
      msg = 'test_case = {0}, got {1}, want {2}'.format(
          tinput, (g_seed, op_seed), toutput)
      self.assertEqual((g_seed, op_seed), toutput, msg=msg)
      random_seed.set_random_seed(None)

    check(tinput=tinput, toutput=toutput)

    if not context.executing_eagerly():
      random_seed.set_random_seed(1)
      for i in range(10):
        tinput = (1, None)
        toutput = (1, i)
        check(tinput=tinput, toutput=toutput)

if __name__ == '__main__':
  test.main()
