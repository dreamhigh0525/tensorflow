# Scikit Flow

This is a simplified interface for TensorFlow, to get people started on predictive analytics and data mining.

## Installation

First, make sure you have TensorFlow and Scikit Learn installed, then just run:

    pip install git+git://github.com/google/skflow.git

## Tutorial

* [Introduction to Scikit Flow and why you want to start learning TensorFlow](https://medium.com/@ilblackdragon/tensorflow-tutorial-part-1-c559c63c0cb1)
* More coming soon.

## Usage

Example usage:

### Linear Classifier

Simple linear classification.

```Python
iris = datasets.load_iris()
classifier = skflow.TensorFlowLinearClassifier(n_classes=3)
classifier.fit(iris.data, iris.target)
score = accuracy_score(classifier.predict(iris.data), iris.target)
```

### Deep Neural Network

Example of 3 layer network with 10, 20 and 10 hidden units respectively:

```Python
classifier = skflow.TensorFlowDNNClassifier(hidden_units=[10, 20, 10], n_classes=3)
classifier.fit(iris.data, iris.target)
score = accuracy_score(classifier.predict(iris.data), iris.target)
```

### Custom model

This example how to pass custom model to the TensorFlowEstimator

```Python
def my_model(X, y):
    """This is DNN with 10, 20, 10 hidden layers, and dropout of 0.5 probability."""
    layers = skflow.ops.dnn(X, [10, 20, 10], keep_proba=0.5)
    return skflow.logistic_classifier(layers, y)

classifier = skflow.TensorFlowEstimator(model_fn=my_model, n_classes=3)
classifier.fit(iris.data, iris.target)
score = accuracy_score(classifier.predict(iris.data), iris.target)
```

## Coming soon

* Easy way to handle categorical variables
* Text categorization
* Images (CNNs)
* More & deeper

