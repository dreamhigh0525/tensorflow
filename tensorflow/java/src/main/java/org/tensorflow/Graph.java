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

/**
 * A data flow graph representing a TensorFlow computation.
 *
 * <p>Instances of a Graph are thread-safe.
 *
 * <p><b>WARNING:</b> Resources consumed by the Graph object msut be explicitly freed by invoking
 * the {@link #close()} method then the Graph object is no longer needed.
 */
public final class Graph implements AutoCloseable {

  /** Create an empty Graph. */
  public Graph() {
    nativeHandle = allocate();
  }

  /**
   * Release resources associated with the Graph.
   *
   * <p>A Graph is no longer usable after close returns.
   */
  @Override
  public void close() {
    synchronized (nativeHandleLock) {
      if (nativeHandle != 0) {
        delete(nativeHandle);
        nativeHandle = 0;
      }
    }
  }

  /**
   * Import a serialized representation of a TensorFlow graph.
   *
   * <p>The serialized representation of the graph, often referred to as a <i>GraphDef</i>, can be
   * generated by {@link #toGraphDef()} and equivalents in other language APIs.
   *
   * @throws IllegalArgumentException if graphDef is not a recognized serialization of a graph.
   * @see #importGraphDef(byte[], String)
   */
  public void importGraphDef(byte[] graphDef) throws IllegalArgumentException {
    importGraphDef(graphDef, "");
  }

  /**
   * Import a serialized representation of a TensorFlow graph.
   *
   * @param graphDef the serialized representation of a TensorFlow graph.
   * @param prefix a prefix that will be prepended to names in graphDef
   * @throws IllegalArgumentException if graphDef is not a recognized serialization of a graph.
   * @see #importGraphDef(byte[])
   */
  public void importGraphDef(byte[] graphDef, String prefix) throws IllegalArgumentException {
    if (graphDef == null || prefix == null) {
      throw new IllegalArgumentException("graphDef and prefix cannot be null");
    }
    synchronized (nativeHandleLock) {
      importGraphDef(nativeHandle, graphDef, prefix);
    }
  }

  /**
   * Generate a serialized representation of the Graph.
   *
   * @see #importGraphDef(byte[])
   * @see #importGraphDef(byte[], String)
   */
  public byte[] toGraphDef() {
    synchronized (nativeHandleLock) {
      return toGraphDef(nativeHandle);
    }
  }

  private final Object nativeHandleLock = new Object();
  private long nativeHandle;

  private static native long allocate();

  private static native void delete(long handle);

  private static native void importGraphDef(long handle, byte[] graphDef, String prefix)
      throws IllegalArgumentException;

  private static native byte[] toGraphDef(long handle);

  static {
    TensorFlow.init();
  }
}
