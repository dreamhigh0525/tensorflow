#  Copyright 2015-present The Scikit Flow Authors. All Rights Reserved.
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
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from sklearn import datasets, metrics, cross_validation
import tensorflow as tf
from tensorflow.contrib import layers
from tensorflow.contrib import learn


def my_model(features, target):
  """DNN with 10, 20, 10 hidden layers, and dropout of 0.1 probability."""
  target = tf.one_hot(target, 3, 1, 0)
  features = layers.stack(features, layers.fully_connected, [10, 20, 10])
  prediction, loss = (
      tf.contrib.learn.models.logistic_regression_zero_init(features, target)
  )
  train_op = tf.contrib.layers.optimize_loss(
      loss, tf.contrib.framework.get_global_step(), optimizer='Adagrad',
      learning_rate=0.1)
  return {'class': tf.argmax(prediction, 1), 'prob': prediction}, loss, train_op


def main(unused_argv):
  iris = datasets.load_iris()
  x_train, x_test, y_train, y_test = cross_validation.train_test_split(
      iris.data, iris.target, test_size=0.2, random_state=42)

  classifier = learn.Estimator(model_fn=my_model)
  classifier.fit(x_train, y_train, steps=1000)

  y_predicted = classifier.predict(x_test)
  score = metrics.accuracy_score(y_test, y_predicted['class'])
  print('Accuracy: {0:f}'.format(score))


if __name__ == '__main__':
  tf.app.run()
