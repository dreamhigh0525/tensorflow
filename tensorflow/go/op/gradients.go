/*
Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package op

import tf "github.com/tensorflow/tensorflow/tensorflow/go"

// Gradients adds gradients computation ops to the graph according to scope.
//
// Arguments:
//  prefix: unique string prefix applied before the names of nodes added to the graph to
//    compute gradients. If null, will use "Gradients".
//  y: output of the function to derive
//  x: inputs of the function for which partial derivatives are computed
//  dx: if not null, the partial derivatives of some loss function L w.r.t. y
//
//  return the partial derivatives
func Gradients(scope *Scope, prefix string, y []tf.Output, x []tf.Output, dx ...tf.Output) (output []tf.Output) {
	var err error
	if prefix == "" {
		prefix = "Gradients"
	}
	if output, err = scope.graph.AddGradients(scope.opName(scope.uniqueName(prefix)), y, x, dx); err != nil {
		scope.UpdateErr("Gradients", err)
		return
	}
	return output
}
