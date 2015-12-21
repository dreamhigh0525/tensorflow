#  Copyright 2015 Google Inc. All Rights Reserved.
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.

import random

from sklearn import datasets
from sklearn.grid_search import GridSearchCV
from sklearn.metrics import accuracy_score, mean_squared_error

import skflow

import tensorflow as tf
from tensorflow.python.platform import googletest


class GridSearchTest(googletest.TestCase):

    def testIrisDNN(self):
        random.seed(42)
        iris = datasets.load_iris()
        classifier = skflow.TensorFlowDNNClassifier(
            hidden_units=[10, 20, 10], n_classes=3, steps=50)
        grid_search = GridSearchCV(classifier,
            {'hidden_units': [[5, 5], [10, 10]],
             'learning_rate': [0.1, 0.01]})
        grid_search.fit(iris.data, iris.target)
        score = accuracy_score(grid_search.predict(iris.data), iris.target)
        self.assertGreater(score, 0.5, "Failed with score = {0}".format(score))


if __name__ == "__main__":
    googletest.main()
