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

package org.tensorflow;

/**
 * A {@code Scope} represents a set of related properties when creating Tensorflow Operations, such
 * as a common name prefix.
 *
 * <p>A {@code Scope} is a container for common properties applied to TensorFlow Ops. Normal user
 * code initializes a {@code Scope} and provides it to Operation building classes. For example:
 *
 * <pre>{@code
 * Scope scope = Scope.create(graph);
 * Constant c = Constant.create(scope, 42);
 * }</pre>
 *
 * <p>An Operation building class acquires a Scope, and uses it to set properties on the underlying
 * Tensorflow ops. For example:
 *
 * <pre>{@code
 * // An operator class that adds a constant.
 * public class Constant {
 *   public static Constant create(Scope scope, ...) {
 *      scope.graph().opBuilder(
 *        "Constant", scope.makeOpName("Constant"))
 *        .setAttr(...)
 *        .build()
 *      ...
 *   }
 * }
 * }</pre>
 *
 * <p><b>Scope hierarchy:</b>
 *
 * <p>A {@code Scope} provides various {@code with()} methods that create a new scope. The new scope
 * typically has one property changed while other properties are inherited from the parent scope.
 *
 * <p>An example using {@code Constant} implemented as before:
 *
 * <pre>{@code
 * Scope root = Scope.create(graph);
 *
 * // The linear subscope will generate names like linear/...
 * Scope linear = Scope.withSubScope("linear");
 *
 * // This op name will be "linear/W"
 * Constant.create(linear.withOpName("W"), ...);
 *
 * // This op will be "linear/Constant", using the default
 * // name provided by Constant
 * Constant.create(linear, ...);
 *
 * // This op will be "linear/Constant_1", using the default
 * // name provided by Constant and making it unique within
 * // this scope
 * Constant.create(linear, ...);
 * }</pre>
 *
 * <p>Scope objects are thread-safe.
 */
public final class Scope {
  /**
   * Create a new top-level scope.
   *
   * @param graph The graph instance to be managed by the scope.
   * @return a top-level Scope.
   */
  public static Scope create(Graph graph) {
    return builder(graph).build();
  }

  /** @return the graph managed by this scope. */
  public Graph graph() {
    return graph;
  }

  /**
   * Return a new subscope with the provided name.
   *
   * <p>Ops created with this scope will have {@code name/childScopeName/} as the prefix. The actual
   * name will be unique in the returned scope. All other properties are inherited from the current
   * scope.
   *
   * <p>Valid child scope names must match one of the following regular expressions:
   *
   * <pre>{@code
   * [A-Za-z0-9.][A-Za-z0-9_.\\-]* (for scopes at the root)
   * [A-Za-z0-9_.\\-]+ (for other scopes)
   * }</pre>
   *
   * @param childScopeName name for the new child scope
   * @return a new subscope
   * @throws IllegalArgumentException if the name is invalid
   */
  public Scope withSubScope(String childScopeName) {
    return toBuilder().nameScope(nameScope.withSubScope(childScopeName)).build();
  }

  /**
   * Return a new scope that uses the provided name for an op.
   *
   * <p>Operations created within this scope will have a name of the form {@code
   * name/opName[_suffix]}. This lets you name a specific operator more meaningfully.
   *
   * <p>Valid operator names must match the regular expression {@code [A-Za-z0-9.][A-Za-z0-9_.\\-]*}
   *
   * @param opName name for an operator in the returned scope
   * @return a new Scope that uses opName for operations.
   * @throws IllegalArgumentException if the name is invalid
   */
  public Scope withOpName(String opName) {
    return toBuilder().nameScope(nameScope.withOpName(opName)).build();
  }

  /**
   * Create a unique name for an operator, using a provided default if necessary.
   *
   * <p>This is normally called only by operator building classes.
   *
   * <p>This method generates a unique name, appropriate for the name scope controlled by this
   * instance. Typical operator building code might look like
   *
   * <pre>{@code
   * scope.graph().opBuilder("Constant", scope.makeOpName("Constant"))...
   * }</pre>
   *
   * <p><b>Note:</b> if you provide a composite operator building class (i.e, a class that adds a
   * set of related operations to the graph by calling other operator building code) you should also
   * create a {@link #withSubScope(String)} scope for the underlying operators to group them under a
   * meaningful name.
   *
   * <pre>{@code
   * public static Stddev create(Scope scope, ...) {
   *   // group sub-operations under a common name
   *   Scope group = scope.withSubScope("stddev");
   *   ... Sqrt.create(group, Mean.create(group, ...))
   * }
   * }</pre>
   *
   * @param defaultName name for the underlying operator.
   * @return unique name for the operator.
   * @throws IllegalArgumentException if the default name is invalid.
   */
  public String makeOpName(String defaultName) {
    return nameScope.makeOpName(defaultName);
  }

  private Scope(Builder builder) {
    graph = builder.graph;
    if (builder.nameScope != null) {
      nameScope = builder.nameScope;
    } else {
      nameScope = NameScope.create();
    }
  }

  private final Graph graph;
  private final NameScope nameScope;

  private Builder toBuilder() {
    return builder(graph).nameScope(nameScope);
  }

  private static Builder builder(Graph graph) {
    return new Builder(graph);
  }

  private static final class Builder {
    private Builder(Graph g) {
      graph = g;
    }

    private Builder nameScope(NameScope ns) {
      nameScope = ns;
      return this;
    }

    private Scope build() {
      return new Scope(this);
    }

    private final Graph graph;
    private NameScope nameScope;
  }
}
