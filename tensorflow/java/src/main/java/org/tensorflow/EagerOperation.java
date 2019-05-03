/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

import java.util.Arrays;

/**
 * Implementation of an {@link Operation} executed eagerly.
 * 
 * <p>EagerOperation instances are valid only as long as the {@link EagerSession} they are a part of is
 * valid. Thus, if {@link EagerSession#close()} has been invoked, then methods on the EagerOperation
 * instance may fail with an {@code IllegalStateException}.
 *
 * <p>EagerOperation instances are thread-safe.
 */
class EagerOperation extends AbstractOperation {
  
  EagerOperation(EagerSession session, long opNativeHandle, long[] outputNativeHandles, String type, String name) {
    this.session = session;
    this.type = type;
    this.name = name;
    this.nativeRef = new NativeReference(session, this, opNativeHandle, outputNativeHandles);
  }

  @Override
  public String name() {
    return name;
  }

  @Override
  public String type() {
    return type;
  }

  @Override
  public int numOutputs() {
    return nativeRef.outputHandles.length;
  }

  @Override
  public int outputListLength(final String name) {
    return outputListLength(nativeRef.opHandle, name);
  }

  @Override
  public int inputListLength(final String name) {
    return inputListLength(nativeRef.opHandle, name);
  }

  @Override
  public long getUnsafeNativeHandle(int outputIndex) {
    return nativeRef.outputHandles[outputIndex];
  }

  @Override
  public long[] shape(int outputIndex) {
    long outputNativeHandle = getUnsafeNativeHandle(outputIndex);
    long[] shape = new long[numDims(outputNativeHandle)];
    for (int i = 0; i < shape.length; ++i) {
      shape[i] = dim(outputNativeHandle, i);
    }
    return shape;
  }

  @Override
  public DataType dtype(int outputIndex) {
    long outputNativeHandle = getUnsafeNativeHandle(outputIndex);
    return DataType.fromC(dataType(outputNativeHandle));
  }

  private static class NativeReference extends EagerSession.NativeReference {

    NativeReference(EagerSession session, EagerOperation operation, long opHandle, long[] outputHandles) {
      super(session, operation);
      this.opHandle = opHandle;
      this.outputHandles = outputHandles;
    }

    @Override
    void delete() {
      if (opHandle != 0L) {
        for (long tensorHandle : outputHandles) {
          if (tensorHandle != 0L) {
            EagerOperation.deleteTensorHandle(tensorHandle);
          }
        }
        EagerOperation.delete(opHandle);
        opHandle = 0L;
        Arrays.fill(outputHandles, 0L);
      }
    }
    
    private long opHandle;
    private final long[] outputHandles;
  }

  private final EagerSession session;
  private final NativeReference nativeRef;
  private final String type;
  private final String name;
  
  private static native void delete(long handle);

  private static native void deleteTensorHandle(long handle);
  
  private static native int outputListLength(long handle, String name);

  private static native int inputListLength(long handle, String name);

  private static native int dataType(long handle);

  private static native int numDims(long handle);

  private static native long dim(long handle, int index);
}
