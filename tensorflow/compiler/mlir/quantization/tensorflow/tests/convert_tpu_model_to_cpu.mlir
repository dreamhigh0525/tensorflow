// RUN: tf-quant-opt %s -quant-convert-tpu-model-to-cpu | FileCheck %s

// Remove TPU related ops.
func.func @tpu_conv(%arg0: tensor<1x3x4x3xf32>) -> tensor<1x3x2x2xf32> {
  %0 = "tf.TPUOrdinalSelector"() {device = ""} : () -> tensor<?xi32>
  %1 = "tf.TPUPartitionedCall"(%arg0, %0) {autotuner_thresh = 0 : i64, device = "", f = @tpu_func_0_optim0} : (tensor<1x3x4x3xf32>, tensor<?xi32>) -> tensor<1x3x2x2xf32>
  %2 = "tf.Identity"(%1) {device = ""} : (tensor<1x3x2x2xf32>) -> tensor<1x3x2x2xf32>
  %3 = "tf.IdentityN"(%2) {device = ""} : (tensor<1x3x2x2xf32>) -> tensor<1x3x2x2xf32>
  func.return %3 : tensor<1x3x2x2xf32>
}
func.func private @tpu_func_0_optim0(%arg0: tensor<1x3x4x3xf32>) -> tensor<1x3x2x2xf32> attributes {tf._original_func_name = "tpu_func_0_optim"} {
  %cst = "tf.Const"() {device = "", value = dense_resource<__elided__> : tensor<2x3x3x2xbf16>} : () -> tensor<2x3x3x2xbf16>
  %cst_0 = "tf.Const"() {device = "", value = dense<[0, 3, 1, 2]> : tensor<4xi32>} : () -> tensor<4xi32>
  %cst_1 = "tf.Const"() {_tpu_replicate = "cluster", device = "", value = dense<[0, 2, 3, 1]> : tensor<4xi32>} : () -> tensor<4xi32>
  %0 = "tf.Cast"(%arg0) {Truncate = false, device = ""} : (tensor<1x3x4x3xf32>) -> tensor<1x3x4x3xbf16>
  "tf.TPUReplicateMetadata"() {_tpu_replicate = "cluster", allow_soft_placement = false, computation_shape = [], device = "", device_assignment = [], host_compute_core = [], num_cores_per_replica = 1 : i64, num_replicas = 1 : i64, padding_map = [], step_marker_location = "STEP_MARK_AT_ENTRY", topology = "", tpu_compile_options_proto = "", use_spmd_for_xla_partitioning = false, use_tpu = true} : () -> ()
  %1 = "tf.TPUCompilationResult"() {_tpu_compilation_status = "cluster", device = ""} : () -> tensor<!tf_type.string>
  %2 = "tf.Transpose"(%0, %cst_0) {device = ""} : (tensor<1x3x4x3xbf16>, tensor<4xi32>) -> tensor<1x3x3x4xbf16>
  %3 = "tf.TPUReplicatedInput"(%2) {device = "", index = -1 : i64, is_mirrored_variable = false, is_packed = false} : (tensor<1x3x3x4xbf16>) -> tensor<1x3x3x4xbf16>
  %4 = "tf.Transpose"(%3, %cst_1) {_tpu_replicate = "cluster", device = ""} : (tensor<1x3x3x4xbf16>, tensor<4xi32>) -> tensor<1x3x4x3xbf16>
  %5 = "tf.Conv2D"(%4, %cst) {data_format = "NHWC", device = "", dilations = [1, 1, 1, 1], explicit_paddings = [], padding = "SAME", strides = [1, 1, 2, 1], use_cudnn_on_gpu = true} : (tensor<1x3x4x3xbf16>, tensor<2x3x3x2xbf16>) -> tensor<1x3x2x2xbf16>
  %6 = "tf.Identity"(%5) {device = ""} : (tensor<1x3x2x2xbf16>) -> tensor<1x3x2x2xbf16>
  %7 = "tf.Identity"(%6) {device = ""} : (tensor<1x3x2x2xbf16>) -> tensor<1x3x2x2xbf16>
  %8 = "tf.TPUReplicatedOutput"(%7) {device = ""} : (tensor<1x3x2x2xbf16>) -> tensor<1x3x2x2xbf16>
  %9 = "tf.Cast"(%8) {Truncate = false} : (tensor<1x3x2x2xbf16>) -> tensor<1x3x2x2xf32>
  func.return %9 : tensor<1x3x2x2xf32>
}

// CHECK-LABEL: func @tpu_conv
// CHECK-DAG: %[[const:.*]] = "tf.Const"() {device = "", value = dense_resource<__elided__> : tensor<2x3x3x2xbf16>} : () -> tensor<2x3x3x2xbf16>
// CHECK: %0 = "tf.Cast"(%arg0) {Truncate = false, device = ""} : (tensor<1x3x4x3xf32>) -> tensor<1x3x4x3xbf16>
// CHECK: %1 = "tf.Conv2D"(%0, %[[const]])
// CHECK: %2 = "tf.Identity"(%1) {device = ""} : (tensor<1x3x2x2xbf16>) -> tensor<1x3x2x2xbf16>
// CHECK: %3 = "tf.Identity"(%2) {device = ""} : (tensor<1x3x2x2xbf16>) -> tensor<1x3x2x2xbf16>
// CHECK: %4 = "tf.Cast"(%3) {Truncate = false} : (tensor<1x3x2x2xbf16>) -> tensor<1x3x2x2xf32>
// CHECK: %5 = "tf.Identity"(%4) {device = ""} : (tensor<1x3x2x2xf32>) -> tensor<1x3x2x2xf32>
// CHECK: %6 = "tf.IdentityN"(%5) {device = ""} : (tensor<1x3x2x2xf32>) -> tensor<1x3x2x2xf32>
// CHECK: return %6 : tensor<1x3x2x2xf32>

