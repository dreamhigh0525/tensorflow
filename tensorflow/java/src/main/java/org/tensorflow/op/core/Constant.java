/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

package org.tensorflow.op.core;

import java.nio.ByteBuffer;
import java.nio.DoubleBuffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;
import java.nio.LongBuffer;
import java.nio.charset.Charset;

import org.tensorflow.DataType;
import org.tensorflow.Operand;
import org.tensorflow.Operation;
import org.tensorflow.Output;
import org.tensorflow.Shape;
import org.tensorflow.Tensor;
import org.tensorflow.Tensors;
import org.tensorflow.op.PrimitiveOp;
import org.tensorflow.op.Scope;
import org.tensorflow.op.annotation.Operator;

/** An operator producing a constant value. */
@Operator
public final class Constant<T> extends PrimitiveOp implements Operand<T> {

  /**
   * Create a {@link DataType#INT32} constant with data from the given buffer.
   *
   * <p>Creates a constant with the given shape by copying elements from the buffer (starting from
   * its current position) into the tensor. For example, if {@code shape = {2,3} } (which represents
   * a 2x3 matrix) then the buffer must have 6 elements remaining, which will be consumed by this
   * method.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param shape the tensor shape.
   * @param data a buffer containing the tensor data.
   * @return an integer constant
   * @throws IllegalArgumentException If the tensor shape is not compatible with the buffer
   */
  public static Constant<Integer> create(Scope scope, Shape shape, IntBuffer data) {
    try (Tensor<Integer> value = Tensor.create(shape.asArray(), data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Creates a constant containing a single {@code int} element.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param data The value to put into the new constant.
   * @return an integer constant
   */
  public static Constant<Integer> create(Scope scope, int data) {
    try (Tensor<Integer> value = Tensors.create(data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Create a {@link DataType#FLOAT} constant with data from the given buffer.
   *
   * <p>Creates a constant with the given shape by copying elements from the buffer (starting from
   * its current position) into the tensor. For example, if {@code shape = {2,3} } (which represents
   * a 2x3 matrix) then the buffer must have 6 elements remaining, which will be consumed by this
   * method.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param shape the tensor shape.
   * @param data a buffer containing the tensor data.
   * @return a float constant
   * @throws IllegalArgumentException If the tensor shape is not compatible with the buffer
   */
  public static Constant<Float> create(Scope scope, Shape shape, FloatBuffer data) {
    try (Tensor<Float> value = Tensor.create(shape.asArray(), data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Creates a constant containing a single {@code float} element.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param data The value to put into the new constant. 
   * @return a float constant
   */
  public static Constant<Float> create(Scope scope, float data) {
    try (Tensor<Float> value = Tensors.create(data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Create a {@link DataType#DOUBLE} constant with data from the given buffer.
   *
   * <p>Creates a constant with the given shape by copying elements from the buffer (starting from
   * its current position) into the tensor. For example, if {@code shape = {2,3} } (which represents
   * a 2x3 matrix) then the buffer must have 6 elements remaining, which will be consumed by this
   * method.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param shape the tensor shape.
   * @param data a buffer containing the tensor data.
   * @return a double constant
   * @throws IllegalArgumentException If the tensor shape is not compatible with the buffer
   */
  public static Constant<Double> create(Scope scope, Shape shape, DoubleBuffer data) {
    try (Tensor<Double> value = Tensor.create(shape.asArray(), data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Creates a constant containing a single {@code double} element.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param data The value to put into the new constant.
   * @return a double constant
   */
  public static Constant<Double> create(Scope scope, double data) {
    try (Tensor<Double> value = Tensors.create(data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Create a {@link DataType#INT64} constant with data from the given buffer.
   *
   * <p>Creates a constant with the given shape by copying elements from the buffer (starting from
   * its current position) into the tensor. For example, if {@code shape = {2,3} } (which represents
   * a 2x3 matrix) then the buffer must have 6 elements remaining, which will be consumed by this
   * method.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param shape the tensor shape.
   * @param data a buffer containing the tensor data.
   * @return a long constant
   * @throws IllegalArgumentException If the tensor shape is not compatible with the buffer
   */
  public static Constant<Long> create(Scope scope, Shape shape, LongBuffer data) {
    try (Tensor<Long> value = Tensor.create(shape.asArray(), data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Creates a constant containing a single {@code long} element.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param data The value to put into the new constant.
   * @return a long constant
   */
  public static Constant<Long> create(Scope scope, long data) {
    try (Tensor<Long> value = Tensors.create(data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Creates a constant containing a single {@code boolean} element.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param data The value to put into the new constant.
   * @return a boolean constant
   */
  public static Constant<Boolean> create(Scope scope, boolean data) {
    try (Tensor<Boolean> value = Tensors.create(data)) {
      return createWithTensor(scope, value); 
    }
  }

  /**
   * Creates a String constant using the default, UTF-8 encoding.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param data The string to put into the new constant.
   * @return a string constant
   */
  public static Constant<String> create(Scope scope, String data) {
    try (Tensor<String> value = Tensors.create(data)) {
      return createWithTensor(scope, value); 
    }
  }

  /**
   * Creates a String constant using a specified encoding.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param charset The encoding from String to bytes.
   * @param data The string to put into the new constant.
   * @return a string constant
   */
  public static Constant<String> create(Scope scope, String data, Charset charset) {
    try (Tensor<String> value = Tensor.create(data.getBytes(charset), String.class)) {
      return createWithTensor(scope, Tensor.create(data.getBytes(charset), String.class));
    }
  }

  /**
   * Create a constant with data from the given buffer.
   *
   * <p>Creates a Constant with the provided shape of any type where the constant data has been
   * encoded into {@code data} as per the specification of the TensorFlow <a
   * href="https://www.tensorflow.org/code/tensorflow/c/c_api.h">C
   * API</a>.
   *
   * @param scope is a scope used to add the underlying operation.
   * @param type the tensor datatype.
   * @param shape the tensor shape.
   * @param data a buffer containing the tensor data.
   * @return a constant of type `type`
   * @throws IllegalArgumentException If the tensor datatype or shape is not compatible with the
   *     buffer
   */
  public static <T> Constant<T> create(Scope scope, Class<T> type, Shape shape, ByteBuffer data) {
    try (Tensor<T> value = Tensor.create(type, shape.asArray(), data)) {
      return createWithTensor(scope, value);
    }
  }

  /**
   * Create a constant from a Java object.
   *
   * <p>The argument {@code object} is first converted into a Tensor using {@link
   * org.tensorflow.Tensor#create(Object)}, so only Objects supported by this method must be
   * provided. For example:
   *
   * <pre>{@code
   * Constant.create(scope, new int[]{{1, 2}, {3, 4}}, Integer.class); // returns a 2x2 integer matrix
   * }</pre>
   *
   * @param scope is a scope used to add the underlying operation.
   * @param object a Java object representing the constant.
   * @return a constant of type `type`
   * @see org.tensorflow.Tensor#create(Object) Tensor.create
   */
  public static <T> Constant<T> create(Scope scope, Object object, Class<T> type) {
    try (Tensor<T> value = Tensor.create(object, type)) {
      return createWithTensor(scope, value);
    }
  }

  private static <T> Constant<T> createWithTensor(Scope scope, Tensor<T> value) {
    return new Constant<T>(
        scope
            .graph()
            .opBuilder("Const", scope.makeOpName("Const"))
            .setAttr("value", value)
            .setAttr("dtype", value.dataType())
            .build());
  }

  @Override
  public Output<T> asOutput() {
    return output;
  }

  private Constant(Operation operation) {
    super(operation);
    output = operation.output(0);
  }

  private final Output<T> output;
}
