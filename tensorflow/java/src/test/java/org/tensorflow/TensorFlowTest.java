/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

package org.tensorflow;

import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

/** Unit tests for {@link org.tensorflow.TensorFlow}. */
@RunWith(JUnit4.class)
public class TensorFlowTest {
  @Test
  public void version() {
    assertTrue(TensorFlow.version().length() > 0);
  }

  @Test
  public void registeredOpList() {
    // Would be nice to actually parse the output as a tensorflow.OpList protocol buffer message,
    // but as of May 2017, bazel support for generating Java code from protocol buffer definitions
    // was not sorted out. Revisit? Till then, at least excercise the code.
    assertTrue(TensorFlow.registeredOpList().length > 0);
  }
}
