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

from sklearn import datasets, cross_validation, metrics
from sklearn import preprocessing

import skflow

random.seed(42)

# Load dataset
boston = datasets.load_boston()
X, y = boston.data, boston.target

# Scale data to 0 mean and unit std dev.
scaler = preprocessing.StandardScaler()
X = scaler.fit_transform(X)

# Split dataset into train / test
X_train, X_test, y_train, y_test = cross_validation.train_test_split(X, y,
    test_size=0.2, random_state=42)

# Build 2 layer fully connected DNN with 10, 10 units respecitvely.
regressor = skflow.TensorFlowDNNRegressor(hidden_units=[10, 10],
    steps=5000, learning_rate=0.1, batch_size=1)

# Fit and predict.
regressor.fit(X_train, y_train)
score = metrics.mean_squared_error(regressor.predict(X_test), y_test)
print('MSE: {0:f}'.format(score))
