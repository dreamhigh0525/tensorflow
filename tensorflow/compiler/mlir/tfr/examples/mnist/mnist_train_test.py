# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
"""Test for tfr mnist training example."""

from absl.testing import parameterized

from tensorflow.compiler.mlir.tfr.examples.mnist import mnist_train
from tensorflow.python.distribute import combinations
from tensorflow.python.distribute import strategy_combinations
from tensorflow.python.distribute import test_util
from tensorflow.python.platform import test


class MnistTrainTest(test.TestCase, parameterized.TestCase):

  # TODO(b/172367622) Switch to strategy_combinations.all_strategies after
  # issues with TPU strategies are resolved.
  @combinations.generate(
      combinations.combine(strategy=strategy_combinations.strategies_minus_tpu))
  def testMnistTrain(self, strategy):
    accuracy = mnist_train.main(strategy)
    self.assertGreater(accuracy, 0.75, 'accuracy sanity check')


if __name__ == '__main__':
  test_util.main()
