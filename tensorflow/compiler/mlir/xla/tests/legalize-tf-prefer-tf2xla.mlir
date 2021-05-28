// RUN: tf-opt "-xla-legalize-tf=allow-partial-conversion device-type=XLA_CPU_JIT legalize-chlo=false use-tf2xla-fallback=true prefer-tf2xla=true" %s | FileCheck %s
// RUN: tf-opt "-xla-legalize-tf=allow-partial-conversion device-type=XLA_CPU_JIT legalize-chlo=false prefer-tf2xla=true" %s | FileCheck --check-prefix NOFALLBACK %s

module attributes {tf.versions = {bad_consumers = [], min_consumer = 0 : i32, producer = 268 : i32}} {

// CHECK-LABEL: @abs
func @abs(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  // CHECK-NOT: tf.Abs
  %0 = "tf.Abs"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

// -----

// CHECK-LABEL: func @softplus
func @softplus(%arg0: tensor<8x16xf16>) -> tensor<8x16xf16> {
  // CHECK:     tf.Softplus
  %0 = "tf.Softplus"(%arg0) : (tensor<8x16xf16>) -> tensor<8x16xf16>
  return %0 : tensor<8x16xf16>
}

// -----

// CHECK-LABEL: @acos
func @acos(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  // CHECK-NOT:  tf.Acos
  %0 = "tf.Acos"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

// -----

// NOFALLBACK-LABEL: @xla_svd
func @xla_svd(%arg0: tensor<1x1xf32>) -> (tensor<1xf32>, tensor<1x1xf32>, tensor<1x1xf32>) {
  // NOFALLBACK: XlaSvd
  %s, %u, %v = "tf.XlaSvd"(%arg0) {max_iter = 1, epsilon = 1.0E-09 : f32, precision_config = ""} : (tensor<1x1xf32>) -> (tensor<1xf32>, tensor<1x1xf32>, tensor<1x1xf32>)
  return %s, %u, %v : tensor<1xf32>, tensor<1x1xf32>, tensor<1x1xf32>
}

}