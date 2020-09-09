// RUN: tf-opt -tf-legalize-hlo %s | FileCheck %s


func @biasAdd_NHWC(%arg0: tensor<1x32x10x32xi32>, %arg1: tensor<32xi32>) -> tensor<1x32x10x32xi32> {
  %0 = "chlo.broadcast_add"(%arg0, %arg1) {broadcast_dimensions = dense<3> : tensor<1xi64>} : (tensor<1x32x10x32xi32>, tensor<32xi32>) -> tensor<1x32x10x32xi32>
  return %0 : tensor<1x32x10x32xi32>
}

func @biasAdd_NCHW(%arg0: tensor<1x32x10x32xi32>, %arg1: tensor<32xi32>) -> tensor<1x32x10x32xi32> {
  %0 = "chlo.broadcast_add"(%arg0, %arg1) {broadcast_dimensions = dense<3> : tensor<1xi64>} : (tensor<1x32x10x32xi32>, tensor<32xi32>) -> tensor<1x32x10x32xi32>
  return %0 : tensor<1x32x10x32xi32>
}

func @biasAdd_dynamic(%arg0: tensor<?x?x?x?xi32>, %arg1: tensor<?xi32>) -> tensor<?x?x?x?xi32> {
  %0 = "chlo.broadcast_add"(%arg0, %arg1) {broadcast_dimensions = dense<3> : tensor<1xi64>} : (tensor<?x?x?x?xi32>, tensor<?xi32>) -> tensor<?x?x?x?xi32>
  return %0 : tensor<?x?x?x?xi32>
}

func @add(%arg0: tensor<2xi32>) -> tensor<2xi32> {
  %0 = mhlo.add %arg0, %arg0 : tensor<2xi32>
  %1 = mhlo.add %0, %arg0 : tensor<2xi32>
  return %1 : tensor<2xi32>
}

func @broadcast_add(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi32> {
  %0 = "chlo.broadcast_add"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
  return %0 : tensor<1x2xi32>
}

func @broadcast_multi_dim_add(%arg0: tensor<4x1x1xi32>, %arg1: tensor<4x4x4x4xi32>) -> tensor<4x4x4x4xi32> {
  %0 = "chlo.broadcast_add"(%arg0, %arg1) {broadcast_dimensions = dense<[1, 2, 3]> : tensor<3xi64>} : (tensor<4x1x1xi32>, tensor<4x4x4x4xi32>) -> tensor<4x4x4x4xi32>
  return %0 : tensor<4x4x4x4xi32>
}

func @div(%arg0: tensor<2xi32>) -> tensor<2xi32> {
  %0 = mhlo.divide %arg0, %arg0 : tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @broadcast_div(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi32> {
  %0 = "chlo.broadcast_divide"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
  return %0 : tensor<1x2xi32>
}

func @shift_left(%arg0: tensor<4xi32>, %arg1: tensor<4xi32>) -> tensor<4xi32> {
  %0 = mhlo.shift_left %arg0, %arg1 : tensor<4xi32>
  return %0 : tensor<4xi32>
}

func @div_dynamic(%arg0: tensor<?xi32>, %arg1: tensor<?x?xi32>) -> tensor<?x?xi32> {
  %0 = "chlo.broadcast_divide"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<?xi32>, tensor<?x?xi32>) -> tensor<?x?xi32>
  return %0 : tensor<?x?xi32>
}

func @maximum(%arg0: tensor<4xf32>, %arg1: tensor<4xf32>) -> tensor<4xf32> {
  %0 = mhlo.maximum %arg0, %arg1 : tensor<4xf32>
  return %0 : tensor<4xf32>
}

func @minimum(%arg0: tensor<4xf32>, %arg1: tensor<4xf32>) -> tensor<4xf32> {
  %0 = mhlo.minimum %arg0, %arg1 : tensor<4xf32>
  return %0 : tensor<4xf32>
}

func @mul(%arg0: tensor<2xi32>) -> tensor<2xi32> {
  %0 = mhlo.multiply %arg0, %arg0 : tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @broadcast_mul(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi32> {
  %0 = "chlo.broadcast_multiply"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
  return %0 : tensor<1x2xi32>
}

func @real_div(%arg0: tensor<2xi32>) -> tensor<2xi32> {
  %0 = mhlo.divide %arg0, %arg0 : tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @broadcast_real_div(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi32> {
  %0 = "chlo.broadcast_divide"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
  return %0 : tensor<1x2xi32>
}

func @sub(%arg0: tensor<2xi32>) -> tensor<2xi32> {
  %0 = mhlo.subtract %arg0, %arg0 : tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @broadcast_sub(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi32> {
  %0 = "chlo.broadcast_subtract"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
  return %0 : tensor<1x2xi32>
}

func @shift_right(%arg0: tensor<4xi32>, %arg1: tensor<4xi32>) -> tensor<4xi32> {
  %0 = mhlo.shift_right_arithmetic %arg0, %arg1 : tensor<4xi32>
  return %0 : tensor<4xi32>
}

func @broadcast_shift_right(%arg0: tensor<4xi32>, %arg1: tensor<2x4xi32>) -> tensor<2x4xi32> {
  %0 = "chlo.broadcast_shift_right_arithmetic"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<4xi32>, tensor<2x4xi32>) -> tensor<2x4xi32>
  return %0 : tensor<2x4xi32>
}

func @and(%arg0: tensor<2xi1>, %arg1: tensor<2xi1>) -> tensor<2xi1> {
  %0 = mhlo.and %arg0, %arg1 : tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @and_broadcast(%arg0: tensor<1xi1>, %arg1: tensor<1x2xi1>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_and"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi1>, tensor<1x2xi1>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @and_dynamic(%arg0: tensor<?xi1>, %arg1: tensor<1xi1>) -> tensor<?xi1> {
  %0 = "chlo.broadcast_and"(%arg0, %arg1) : (tensor<?xi1>, tensor<1xi1>) -> tensor<?xi1>
  return %0 : tensor<?xi1>
}

func @or(%arg0: tensor<2xi1>, %arg1: tensor<2xi1>) -> tensor<2xi1> {
  %0 = mhlo.or %arg0, %arg1 : tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @or_broadcast(%arg0: tensor<1xi1>, %arg1: tensor<1x2xi1>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_or"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi1>, tensor<1x2xi1>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @or_dynamic(%arg0: tensor<?xi1>, %arg1: tensor<1xi1>) -> tensor<?xi1> {
  %0 = "chlo.broadcast_or"(%arg0, %arg1) : (tensor<?xi1>, tensor<1xi1>) -> tensor<?xi1>
  return %0 : tensor<?xi1>
}

func @bitwise_or(%arg0: tensor<4xi32>, %arg1: tensor<4xi32>) -> tensor<4xi32> {
  %0 = mhlo.or %arg0, %arg1 : tensor<4xi32>
  return %0 : tensor<4xi32>
}

func @bitwise_or_broadcast(%arg0: tensor<1xi8>, %arg1: tensor<1x4xi8>) -> tensor<1x4xi8> {
  %0 = "chlo.broadcast_or"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi8>, tensor<1x4xi8>) -> tensor<1x4xi8>
  return %0 : tensor<1x4xi8>
}

func @bitwise_or_dynamic(%arg0: tensor<?xi32>, %arg1: tensor<1xi32>) -> tensor<?xi32> {
  %0 = "chlo.broadcast_or"(%arg0, %arg1) : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi32>
  return %0 : tensor<?xi32>
}

func @bitwise_and(%arg0: tensor<4xi32>, %arg1: tensor<4xi32>) -> tensor<4xi32> {
  %0 = mhlo.and %arg0, %arg1 : tensor<4xi32>
  return %0 : tensor<4xi32>
}

func @bitwise_and_broadcast(%arg0: tensor<1xi8>, %arg1: tensor<1x4xi8>) -> tensor<1x4xi8> {
  %0 = "chlo.broadcast_and"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<1xi8>, tensor<1x4xi8>) -> tensor<1x4xi8>
  return %0 : tensor<1x4xi8>
}

func @bitwise_and_dynamic(%arg0: tensor<?xi32>, %arg1: tensor<1xi32>) -> tensor<?xi32> {
  %0 = "chlo.broadcast_and"(%arg0, %arg1) : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi32>
  return %0 : tensor<?xi32>
}

func @pow(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = mhlo.power %arg0, %arg0 : tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @pow_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = mhlo.power %arg0, %arg0 : tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @floordiv_broadcast_i32(%arg0: tensor<2x3xi32>, %arg1: tensor<3xi32>) -> tensor<2x3xi32> {
  %0 = mhlo.constant dense<0> : tensor<2x3xi32>
  %1 = "chlo.broadcast_compare"(%arg0, %0) {comparison_direction = "LT"} : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi1>
  %2 = mhlo.constant dense<0> : tensor<3xi32>
  %3 = "chlo.broadcast_compare"(%arg1, %2) {comparison_direction = "LT"} : (tensor<3xi32>, tensor<3xi32>) -> tensor<3xi1>
  %4 = "chlo.broadcast_compare"(%1, %3) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "EQ"} : (tensor<2x3xi1>, tensor<3xi1>) -> tensor<2x3xi1>
  %5 = "chlo.broadcast_divide"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<2x3xi32>, tensor<3xi32>) -> tensor<2x3xi32>
  %6 = "mhlo.abs"(%arg0) : (tensor<2x3xi32>) -> tensor<2x3xi32>
  %7 = "mhlo.abs"(%arg1) : (tensor<3xi32>) -> tensor<3xi32>
  %8 = mhlo.constant dense<1> : tensor<3xi32>
  %9 = mhlo.subtract %7, %8 : tensor<3xi32>
  %10 = "chlo.broadcast_add"(%6, %9) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<2x3xi32>, tensor<3xi32>) -> tensor<2x3xi32>
  %11 = "mhlo.negate"(%10) : (tensor<2x3xi32>) -> tensor<2x3xi32>
  %12 = "mhlo.abs"(%arg1) : (tensor<3xi32>) -> tensor<3xi32>
  %13 = "chlo.broadcast_divide"(%11, %12) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<2x3xi32>, tensor<3xi32>) -> tensor<2x3xi32>
  %14 = "mhlo.select"(%4, %5, %13) : (tensor<2x3xi1>, tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
  return %14 : tensor<2x3xi32>
}

func @floordiv_reverse_broadcast_i32(%arg0: tensor<3xi32>, %arg1: tensor<2x3xi32>) -> tensor<2x3xi32> {
  %0 = mhlo.constant dense<0> : tensor<3xi32>
  %1 = "mhlo.compare"(%arg0, %0) {comparison_direction = "LT"} : (tensor<3xi32>, tensor<3xi32>) -> tensor<3xi1>
  %2 = mhlo.constant dense<0> : tensor<2x3xi32>
  %3 = "chlo.broadcast_compare"(%arg1, %2) {comparison_direction = "LT"} : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi1>
  %4 = "chlo.broadcast_compare"(%1, %3) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "EQ"} : (tensor<3xi1>, tensor<2x3xi1>) -> tensor<2x3xi1>
  %5 = "chlo.broadcast_divide"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
  %6 = "mhlo.abs"(%arg0) : (tensor<3xi32>) -> tensor<3xi32>
  %7 = "mhlo.abs"(%arg1) : (tensor<2x3xi32>) -> tensor<2x3xi32>
  %8 = mhlo.constant dense<1> : tensor<2x3xi32>
  %9 = mhlo.subtract %7, %8 : tensor<2x3xi32>
  %10 = "chlo.broadcast_add"(%6, %9) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
  %11 = "mhlo.negate"(%10) : (tensor<2x3xi32>) -> tensor<2x3xi32>
  %12 = "mhlo.abs"(%arg1) : (tensor<2x3xi32>) -> tensor<2x3xi32>
  %13 = mhlo.divide %11, %12 : tensor<2x3xi32>
  %14 = "mhlo.select"(%4, %5, %13) : (tensor<2x3xi1>, tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
  return %14 : tensor<2x3xi32>
}

func @floordiv_f32(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = mhlo.divide %arg0, %arg0 : tensor<2xf32>
  %1 = mhlo.divide %arg0, %arg0 : tensor<2xf32>
  %2 = "mhlo.floor"(%1) : (tensor<2xf32>) -> tensor<2xf32>
  return %2 : tensor<2xf32>
}

func @floordiv_f16_broadcast(%arg0: tensor<2x3xf16>, %arg1: tensor<3xf16>) -> tensor<2x3xf16> {
  %0 = "chlo.broadcast_divide"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<2x3xf16>, tensor<3xf16>) -> tensor<2x3xf16>
  %1 = "chlo.broadcast_divide"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>} : (tensor<2x3xf16>, tensor<3xf16>) -> tensor<2x3xf16>
  %2 = "mhlo.floor"(%1) : (tensor<2x3xf16>) -> tensor<2x3xf16>
  return %2 : tensor<2x3xf16>
}

func @equal(%arg0: tensor<2xi32>) -> tensor<2xi1> {
  %0 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "EQ"} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @equal_dynamic(%arg0: tensor<?xi32>, %arg1: tensor<1xi32>) -> tensor<?xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {comparison_direction = "EQ"} : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi1>
  return %0 : tensor<?xi1>
}

func @equal_broadcast(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "EQ"} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @equal_broadcast_no_incompatible_shapes_error(%arg0: tensor<2xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "EQ"} : (tensor<2xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @equal_incompatible_shape_broadcastable(%arg0: tensor<?xi32>, %arg1: tensor<1xi32>) -> tensor<?xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {comparison_direction = "EQ"} : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi1>
  return %0 : tensor<?xi1>
}

func @notequal(%arg0: tensor<2xi32>) -> tensor<2xi1> {
  %0 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "NE"} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @notequal_broadcast(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "NE"} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @notequal_broadcast_no_incompatible_shapes_error(%arg0: tensor<2xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "NE"} : (tensor<2xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @notequal_incompatible_shape_broadcastable(%arg0: tensor<?xi32>, %arg1: tensor<1xi32>) -> tensor<?xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {comparison_direction = "NE"} : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi1>
  return %0 : tensor<?xi1>
}

func @greater(%arg0: tensor<2xi32>) -> tensor<2xi1> {
  %0 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "GT"} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @broadcast_greater(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "GT"} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @greater_equal(%arg0: tensor<2xi32>) -> tensor<2xi1> {
  %0 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "GE"} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @broadcast_greater_equal(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "GE"} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @less(%arg0: tensor<2xi32>) -> tensor<2xi1> {
  %0 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "LT"} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @broadcast_less(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "LT"} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @less_equal(%arg0: tensor<2xi32>) -> tensor<2xi1> {
  %0 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "LE"} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @broadcast_less_equal(%arg0: tensor<1xi32>, %arg1: tensor<1x2xi32>) -> tensor<1x2xi1> {
  %0 = "chlo.broadcast_compare"(%arg0, %arg1) {broadcast_dimensions = dense<1> : tensor<1xi64>, comparison_direction = "LE"} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
  return %0 : tensor<1x2xi1>
}

func @concat_v2(%arg0: tensor<3x3xf32>, %arg1: tensor<3x3xf32>) -> tensor<6x3xf32> {
  %2 = "mhlo.concatenate"(%arg0, %arg1) {dimension = 0 : i64} : (tensor<3x3xf32>, tensor<3x3xf32>) -> tensor<6x3xf32>
  return %2 : tensor<6x3xf32>
}

func @concat_v2_1d_axis(%arg0: tensor<3x3xf32>, %arg1: tensor<3x3xf32>) -> tensor<3x6xf32> {
  %2 = "mhlo.concatenate"(%arg0, %arg1) {dimension = 1 : i64} : (tensor<3x3xf32>, tensor<3x3xf32>) -> tensor<3x6xf32>
  return %2 : tensor<3x6xf32>
}

func @const() -> tensor<2xi32> {
  %0 = mhlo.constant dense<0> : tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @relu(%arg0: tensor<1xi32>) -> tensor<1xi32> {
  %0 = mhlo.constant dense<0> : tensor<i32>
  %1 = "chlo.broadcast_maximum"(%0, %arg0) {broadcast_dimensions = dense<[]> : tensor<0xi64>} : (tensor<i32>, tensor<1xi32>) -> tensor<1xi32>
  return %1 : tensor<1xi32>
}

func @relu_unranked(%arg0: tensor<?xi32>) -> tensor<?xi32> {
  %0 = mhlo.constant dense<0> : tensor<i32>
  %1 = "chlo.broadcast_maximum"(%0, %arg0) {broadcast_dimensions = dense<[]> : tensor<0xi64>} : (tensor<i32>, tensor<?xi32>) -> tensor<?xi32>
  return %1 : tensor<?xi32>
}

func @relu6(%arg0: tensor<1xi32>) -> tensor<1xi32> {
  %0 = mhlo.constant dense<0> : tensor<i32>
  %1 = mhlo.constant dense<6> : tensor<i32>
  %2 = "chlo.broadcast_minimum"(%arg0, %1) {broadcast_dimensions = dense<[]> : tensor<0xi64>} : (tensor<1xi32>, tensor<i32>) -> tensor<1xi32>
  %3 = "chlo.broadcast_maximum"(%2, %0) {broadcast_dimensions = dense<[]> : tensor<0xi64>} : (tensor<1xi32>, tensor<i32>) -> tensor<1xi32>
  return %3 : tensor<1xi32>
}

func @relu6_unranked(%arg0: tensor<?xi32>) -> tensor<?xi32> {
  %0 = mhlo.constant dense<0> : tensor<i32>
  %1 = mhlo.constant dense<6> : tensor<i32>
  %2 = "chlo.broadcast_minimum"(%arg0, %1) {broadcast_dimensions = dense<[]> : tensor<0xi64>} : (tensor<?xi32>, tensor<i32>) -> tensor<?xi32>
  %3 = "chlo.broadcast_maximum"(%2, %0) {broadcast_dimensions = dense<[]> : tensor<0xi64>} : (tensor<?xi32>, tensor<i32>) -> tensor<?xi32>
  return %3 : tensor<?xi32>
}

func @relu_grad(%arg0: tensor<4x8xf32>, %arg1: tensor<?x?xf32>) -> tensor<4x8xf32> {
  %0 = mhlo.constant dense<0.000000e+00> : tensor<f32>
  %1 = "chlo.broadcast_compare"(%arg1, %0) {broadcast_dimensions = dense<[]> : tensor<0xi64>, comparison_direction = "GT"} : (tensor<?x?xf32>, tensor<f32>) -> tensor<?x?xi1>
  %2 = mhlo.constant dense<0.000000e+00> : tensor<4x8xf32>
  %3 = "mhlo.select"(%1, %arg0, %2) : (tensor<?x?xi1>, tensor<4x8xf32>, tensor<4x8xf32>) -> tensor<4x8xf32>
  return %3 : tensor<4x8xf32>
}

func @select(%arg0: tensor<2xi1>, %arg1: tensor<2xi32>, %arg2: tensor<2xi32>) -> tensor<2xi32> {
  %0 = "mhlo.select"(%arg0, %arg1, %arg2) : (tensor<2xi1>, tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @select_float(%arg0: tensor<2xi1>, %arg1: tensor<2xf32>, %arg2: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.select"(%arg0, %arg1, %arg2) : (tensor<2xi1>, tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @select_multidimensional(%arg0: tensor<3x2xi1>, %arg1: tensor<3x2xi32>, %arg2: tensor<3x2xi32>) -> tensor<3x2xi32> {
  %0 = "mhlo.select"(%arg0, %arg1, %arg2) : (tensor<3x2xi1>, tensor<3x2xi32>, tensor<3x2xi32>) -> tensor<3x2xi32>
  return %0 : tensor<3x2xi32>
}

func @selectv2(%arg0: tensor<2xi1>, %arg1: tensor<2xi32>, %arg2: tensor<2xi32>) -> tensor<2xi32> {
  %0 = "mhlo.select"(%arg0, %arg1, %arg2) : (tensor<2xi1>, tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @selectv2_pred_scalar(%arg0: tensor<i1>, %arg1: tensor<2xi32>, %arg2: tensor<2xi32>) -> tensor<2xi32> {
  %0 = "mhlo.select"(%arg0, %arg1, %arg2) : (tensor<i1>, tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @transpose_2d(%arg0: tensor<2x3xf32>) -> tensor<3x2xf32> {
  %0 = mhlo.constant dense<[1, 0]> : tensor<2xi64>
  %1 = mhlo.constant dense<[1, 0]> : tensor<2xi64>
  %2 = "mhlo.transpose"(%arg0) {permutation = dense<[1, 0]> : tensor<2xi64>} : (tensor<2x3xf32>) -> tensor<3x2xf32>
  return %2 : tensor<3x2xf32>
}

func @transpose_3d_int32(%arg0: tensor<1x2x3xf32>) -> tensor<3x2x1xf32> {
  %0 = mhlo.constant dense<[2, 1, 0]> : tensor<3xi32>
  %1 = mhlo.constant dense<[2, 1, 0]> : tensor<3xi64>
  %2 = "mhlo.transpose"(%arg0) {permutation = dense<[2, 1, 0]> : tensor<3xi64>} : (tensor<1x2x3xf32>) -> tensor<3x2x1xf32>
  return %2 : tensor<3x2x1xf32>
}

func @transpose_3d(%arg0: tensor<1x2x3xf32>) -> tensor<3x2x1xf32> {
  %0 = mhlo.constant dense<[2, 1, 0]> : tensor<3xi64>
  %1 = mhlo.constant dense<[2, 1, 0]> : tensor<3xi64>
  %2 = "mhlo.transpose"(%arg0) {permutation = dense<[2, 1, 0]> : tensor<3xi64>} : (tensor<1x2x3xf32>) -> tensor<3x2x1xf32>
  return %2 : tensor<3x2x1xf32>
}

func @transpose_dynamic_2d(%arg0: tensor<?x4xf32>) -> tensor<4x?xf32> {
  %0 = mhlo.constant dense<[1, 0]> : tensor<2xi64>
  %1 = mhlo.constant dense<[1, 0]> : tensor<2xi64>
  %2 = "mhlo.transpose"(%arg0) {permutation = dense<[1, 0]> : tensor<2xi64>} : (tensor<?x4xf32>) -> tensor<4x?xf32>
  return %2 : tensor<4x?xf32>
}

func @transpose_unranked_2d(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = mhlo.constant dense<[1, 0]> : tensor<2xi64>
  %1 = mhlo.constant dense<[1, 0]> : tensor<2xi64>
  %2 = "mhlo.transpose"(%arg0) {permutation = dense<[1, 0]> : tensor<2xi64>} : (tensor<*xf32>) -> tensor<*xf32>
  return %2 : tensor<*xf32>
}

func @abs(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.abs"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @abs_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.abs"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @abs_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.abs"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @ceil(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.ceil"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @ceil_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.ceil"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @ceil_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.ceil"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @complex_abs(%arg0: tensor<2xcomplex<f32>>) -> tensor<2xf32> {
  %0 = "mhlo.abs"(%arg0) : (tensor<2xcomplex<f32>>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @cos(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.cosine"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @cos_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.cosine"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @cos_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.cosine"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @exp(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.exponential"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @exp_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.exponential"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @exp_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.exponential"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @floor(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.floor"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @floor_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.floor"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @floor_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.floor"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @is_finite(%arg0: tensor<2xf32>) -> tensor<2xi1> {
  %0 = "mhlo.is_finite"(%arg0) : (tensor<2xf32>) -> tensor<2xi1>
  return %0 : tensor<2xi1>
}

func @is_finite_dynamic(%arg0: tensor<?xf32>) -> tensor<?xi1> {
  %0 = "mhlo.is_finite"(%arg0) : (tensor<?xf32>) -> tensor<?xi1>
  return %0 : tensor<?xi1>
}

func @is_finite_unranked(%arg0: tensor<*xf32>) -> tensor<*xi1> {
  %0 = "mhlo.is_finite"(%arg0) : (tensor<*xf32>) -> tensor<*xi1>
  return %0 : tensor<*xi1>
}

func @log(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.log"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @log_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.log"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @log_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.log"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @log1p(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.log_plus_one"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @log1p_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.log_plus_one"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @log1p_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.log_plus_one"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @neg(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.negate"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @neg_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.negate"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @neg_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.negate"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @sigmoid(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = mhlo.constant dense<5.000000e-01> : tensor<f32>
  %1 = mhlo.constant dense<2> : tensor<1xi64>
  %2 = mhlo.constant dense<5.000000e-01> : tensor<2xf32>
  %3 = mhlo.multiply %arg0, %2 : tensor<2xf32>
  %4 = "mhlo.tanh"(%3) : (tensor<2xf32>) -> tensor<2xf32>
  %5 = mhlo.multiply %4, %2 : tensor<2xf32>
  %6 = mhlo.add %5, %2 : tensor<2xf32>
  return %6 : tensor<2xf32>
}

func @sin(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.sine"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @sin_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.sine"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @sin_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.sine"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @rsqrt(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.rsqrt"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @rsqrt_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.rsqrt"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @rsqrt_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.rsqrt"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @sqrt(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.sqrt"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @sqrt_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.sqrt"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @sqrt_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.sqrt"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @tanh(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.tanh"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @tanh_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.tanh"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @tanh_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.tanh"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @bitcast(%arg0: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "mhlo.bitcast_convert"(%arg0) : (tensor<2xf32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @bitcast_dynamic(%arg0: tensor<?xf32>) -> tensor<?xf32> {
  %0 = "mhlo.bitcast_convert"(%arg0) : (tensor<?xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}

func @bitcast_unranked(%arg0: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "mhlo.bitcast_convert"(%arg0) : (tensor<*xf32>) -> tensor<*xf32>
  return %0 : tensor<*xf32>
}

func @bitcast_same_widths(%arg0: tensor<2xf32>) -> tensor<2xi32> {
  %0 = "mhlo.bitcast_convert"(%arg0) : (tensor<2xf32>) -> tensor<2xi32>
  return %0 : tensor<2xi32>
}

func @sign(%arg0: tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32> {
  %0 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "NE"} : (tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xi1>
  %1 = mhlo.constant dense<0.000000e+00> : tensor<1x2x3x4xf32>
  %2 = "mhlo.compare"(%arg0, %arg0) {comparison_direction = "NE"} : (tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xi1>
  %3 = mhlo.constant dense<0.000000e+00> : tensor<1x2x3x4xf32>
  %4 = "mhlo.sign"(%arg0) : (tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32>
  %5 = "mhlo.select"(%2, %3, %4) : (tensor<1x2x3x4xi1>, tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32>
  %6 = "mhlo.select"(%0, %1, %5) : (tensor<1x2x3x4xi1>, tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32>
  return %6 : tensor<1x2x3x4xf32>
}

func @size_rank_one_i32(%arg0: tensor<f32>) -> tensor<i32> {
  %0 = mhlo.constant dense<1> : tensor<i32>
  return %0 : tensor<i32>
}

func @size_rank_one_i64(%arg0: tensor<f32>) -> tensor<i64> {
  %0 = mhlo.constant dense<1> : tensor<i64>
  return %0 : tensor<i64>
}

func @complex(%arg0: tensor<3xf32>, %arg1: tensor<3xf32>) -> tensor<3xcomplex<f32>> {
  %0 = "mhlo.complex"(%arg0, %arg1) : (tensor<3xf32>, tensor<3xf32>) -> tensor<3xcomplex<f32>>
  return %0 : tensor<3xcomplex<f32>>
}

func @convert_i32_f32(%arg0: tensor<2xi32>) -> tensor<2xf32> {
  %0 = "mhlo.convert"(%arg0) : (tensor<2xi32>) -> tensor<2xf32>
  return %0 : tensor<2xf32>
}

func @convert_slice(%arg0: tensor<1x4672xf32>) -> tensor<1x519xf32> {
  %0 = "mhlo.slice"(%arg0) {limit_indices = dense<[1, 4672]> : tensor<2xi64>, start_indices = dense<[0, 4153]> : tensor<2xi64>, strides = dense<1> : tensor<2xi64>} : (tensor<1x4672xf32>) -> tensor<1x519xf32>
  return %0 : tensor<1x519xf32>
}

func @reshape(%arg0: tensor<4x6xf32>) -> tensor<2x2x6xf32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<4x6xf32>) -> tensor<2x2x6xf32>
  return %0 : tensor<2x2x6xf32>

}

func @convert_dot_1d_2d(%arg0: tensor<256xf32>, %arg1: tensor<256x1xf32>) -> tensor<1xf32> {
  %0 = "mhlo.dot"(%arg0, %arg1) {precision_config = ["DEFAULT", "DEFAULT"]} : (tensor<256xf32>, tensor<256x1xf32>) -> tensor<1xf32>
  return %0 : tensor<1xf32>
}

func @convert_dot_2d_1d(%arg0: tensor<1x256xf32>, %arg1: tensor<256xf32>) -> tensor<1xf32> {
  %0 = "mhlo.dot"(%arg0, %arg1) {precision_config = ["DEFAULT", "DEFAULT"]} : (tensor<1x256xf32>, tensor<256xf32>) -> tensor<1xf32>
  return %0 : tensor<1xf32>
}

func @convert_dot_1d_1d(%arg0: tensor<256xf32>, %arg1: tensor<256xf32>) -> tensor<f32> {
  %0 = "mhlo.dot"(%arg0, %arg1) {precision_config = ["DEFAULT", "DEFAULT"]} : (tensor<256xf32>, tensor<256xf32>) -> tensor<f32>
  return %0 : tensor<f32>
}

func @convert_dot_2d_2d(%arg0: tensor<1x256xf32>, %arg1: tensor<256x1xf32>) -> tensor<1x1xf32> {
  %0 = "mhlo.dot"(%arg0, %arg1) {precision_config = ["DEFAULT", "DEFAULT"]} : (tensor<1x256xf32>, tensor<256x1xf32>) -> tensor<1x1xf32>
  return %0 : tensor<1x1xf32>
}

func @broadcast_in_dim_tf_style(%arg0: tensor<8x1x16xf32>) -> tensor<3x8x8x16xf32> {
  %0 = "mhlo.broadcast_in_dim"(%arg0) {broadcast_dimensions = dense<[1, 2, 3]> : tensor<3xi64>, name = "broadcast.0"} : (tensor<8x1x16xf32>) -> tensor<3x8x8x16xf32>
  return %0 : tensor<3x8x8x16xf32>
}

func @broadcast_in_dim_general_case(%arg0: tensor<3x1x16xf32>) -> tensor<3x8x8x16xf32> {
  %0 = "mhlo.broadcast_in_dim"(%arg0) {broadcast_dimensions = dense<[0, 2, 3]> : tensor<3xi64>, name = "broadcast.0"} : (tensor<3x1x16xf32>) -> tensor<3x8x8x16xf32>
  return %0 : tensor<3x8x8x16xf32>
}

func @convert_dot_general(%arg0: tensor<3x2x6x5x1xf32>, %arg1: tensor<3x2x4x6xf32>) -> tensor<3x5x1x4xf32> {
  %0 = "mhlo.dot_general"(%arg0, %arg1) {dot_dimension_numbers = {lhs_batching_dimensions = dense<0> : tensor<1xi64>, lhs_contracting_dimensions = dense<[1, 2]> : tensor<2xi64>, rhs_batching_dimensions = dense<0> : tensor<1xi64>, rhs_contracting_dimensions = dense<[1, 3]> : tensor<2xi64>}, precision_config = ["DEFAULT", "DEFAULT"]} : (tensor<3x2x6x5x1xf32>, tensor<3x2x4x6xf32>) -> tensor<3x5x1x4xf32>
  return %0 : tensor<3x5x1x4xf32>
}

func @convert_conv2d(%arg0: tensor<1x8x8x207xf32>, %arg1: tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32> {
  %0 = "mhlo.convolution"(%arg0, %arg1) {batch_group_count = 1 : i64, dimension_numbers =
       {input_batch_dimension = 0 : i64, input_feature_dimension = 3 : i64, input_spatial_dimensions = dense<[1, 2]> : tensor<2xi64>, kernel_input_feature_dimension = 2 : i64, kernel_output_feature_dimension = 3 : i64, kernel_spatial_dimensions = dense<[0, 1]> : tensor<2xi64>, output_batch_dimension = 0 : i64, output_feature_dimension = 3 : i64, output_spatial_dimensions = dense<[1, 2]> : tensor<2xi64>},
       feature_group_count = 1 : i64, lhs_dilation = dense<1> : tensor<2xi64>, padding = dense<1> : tensor<2x2xi64>, precision_config = ["DEFAULT", "DEFAULT"], rhs_dilation = dense<1> : tensor<2xi64>, window_strides = dense<1> : tensor<2xi64>} :
       (tensor<1x8x8x207xf32>, tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32>
  return %0 : tensor<1x8x8x16xf32>
}

func @convert_depthwise_conv2d(%arg0: tensor<1x8x8x207xf32>, %arg1: tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32> {
  %0 = "mhlo.convolution"(%arg0, %arg1) {batch_group_count = 1 : i64, dimension_numbers =
       {input_batch_dimension = 0 : i64, input_feature_dimension = 3 : i64, input_spatial_dimensions = dense<[1, 2]> : tensor<2xi64>, kernel_input_feature_dimension = 2 : i64, kernel_output_feature_dimension = 3 : i64, kernel_spatial_dimensions = dense<[0, 1]> : tensor<2xi64>, output_batch_dimension = 0 : i64, output_feature_dimension = 3 : i64, output_spatial_dimensions = dense<[1, 2]> : tensor<2xi64>},
       feature_group_count = 207 : i64, lhs_dilation = dense<1> : tensor<2xi64>, padding = dense<1> : tensor<2x2xi64>, precision_config = ["DEFAULT", "DEFAULT"], rhs_dilation = dense<1> : tensor<2xi64>, window_strides = dense<1> : tensor<2xi64>} :
       (tensor<1x8x8x207xf32>, tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32>
  return %0 : tensor<1x8x8x16xf32>
}

func @convert_conv2d_valid_padding(%arg0: tensor<1x8x8x207xf32>, %arg1: tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32> {
  %0 = "mhlo.convolution"(%arg0, %arg1) {batch_group_count = 1 : i64, dimension_numbers =
       {input_batch_dimension = 0 : i64, input_feature_dimension = 3 : i64, input_spatial_dimensions = dense<[1, 2]> : tensor<2xi64>, kernel_input_feature_dimension = 2 : i64, kernel_output_feature_dimension = 3 : i64, kernel_spatial_dimensions = dense<[0, 1]> : tensor<2xi64>, output_batch_dimension = 0 : i64, output_feature_dimension = 3 : i64, output_spatial_dimensions = dense<[1, 2]> : tensor<2xi64>},
       feature_group_count = 1 : i64, lhs_dilation = dense<1> : tensor<2xi64>, padding = dense<0> : tensor<2x2xi64>, precision_config = ["DEFAULT", "DEFAULT"], rhs_dilation = dense<1> : tensor<2xi64>, window_strides = dense<1> : tensor<2xi64>} :
       (tensor<1x8x8x207xf32>, tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32>
  return %0 : tensor<1x8x8x16xf32>
}

func @convert_reduce_to_sum(%arg0: tensor<1x256xf32>) -> tensor<1xf32> {
  %0 = mhlo.constant dense<0.000000e+00> : tensor<f32>
  %1 = "mhlo.reduce"(%arg0, %0) ( {
  ^bb0(%arg1: tensor<f32>, %arg2: tensor<f32>):
    %2 = mhlo.add %arg1, %arg2 : tensor<f32>
    "mhlo.return"(%2) : (tensor<f32>) -> ()
  }) {dimensions = dense<1> : tensor<1xi64>} : (tensor<1x256xf32>, tensor<f32>) -> tensor<1xf32>
  return %1 : tensor<1xf32>
}

func @convert_reduce_to_max(%arg0: tensor<1x256xf32>) -> tensor<1xf32> {
  // "0xFF800000" represents -INF for f32.
  %0 = mhlo.constant dense<0xFF800000> : tensor<f32>
  %1 = "mhlo.reduce"(%arg0, %0) ( {
  ^bb0(%arg1: tensor<f32>, %arg2: tensor<f32>):
    %2 = mhlo.maximum %arg1, %arg2 : tensor<f32>
    "mhlo.return"(%2) : (tensor<f32>) -> ()
  }) {dimensions = dense<1> : tensor<1xi64>} : (tensor<1x256xf32>, tensor<f32>) -> tensor<1xf32>
  return %1 : tensor<1xf32>
}


func @convert_reduce_to_min(%arg0: tensor<1x256xf32>) -> tensor<1xf32> {
  // "0x7F800000" represents INF for f32.
  %0 = mhlo.constant dense<0x7F800000> : tensor<f32>
  %1 = "mhlo.reduce"(%arg0, %0) ( {
  ^bb0(%arg1: tensor<f32>, %arg2: tensor<f32>):
    %2 = mhlo.minimum %arg1, %arg2 : tensor<f32>
    "mhlo.return"(%2) : (tensor<f32>) -> ()
  }) {dimensions = dense<1> : tensor<1xi64>} : (tensor<1x256xf32>, tensor<f32>) -> tensor<1xf32>
  return %1 : tensor<1xf32>
}



// NOTE: Assertions have been autogenerated by utils/generate-test-checks.py


// CHECK-LABEL:   func @biasAdd_NHWC(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<1x32x10x32xi32>,
// CHECK-SAME:                       %[[VAL_1:.*]]: tensor<32xi32>) -> tensor<1x32x10x32xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.AddV2"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1x32x10x32xi32>, tensor<32xi32>) -> tensor<1x32x10x32xi32>
// CHECK:           return %[[VAL_2]] : tensor<1x32x10x32xi32>
// CHECK:         }

// CHECK-LABEL:   func @biasAdd_NCHW(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<1x32x10x32xi32>,
// CHECK-SAME:                       %[[VAL_1:.*]]: tensor<32xi32>) -> tensor<1x32x10x32xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.AddV2"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1x32x10x32xi32>, tensor<32xi32>) -> tensor<1x32x10x32xi32>
// CHECK:           return %[[VAL_2]] : tensor<1x32x10x32xi32>
// CHECK:         }

// CHECK-LABEL:   func @biasAdd_dynamic(
// CHECK-SAME:                          %[[VAL_0:.*]]: tensor<?x?x?x?xi32>,
// CHECK-SAME:                          %[[VAL_1:.*]]: tensor<?xi32>) -> tensor<?x?x?x?xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.AddV2"(%[[VAL_0]], %[[VAL_1]]) : (tensor<?x?x?x?xi32>, tensor<?xi32>) -> tensor<?x?x?x?xi32>
// CHECK:           return %[[VAL_2]] : tensor<?x?x?x?xi32>
// CHECK:         }

// CHECK-LABEL:   func @add(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.AddV2"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           %[[VAL_2:.*]] = "tf.AddV2"(%[[VAL_1]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_2]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_add(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                        %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.AddV2"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_multi_dim_add(
// CHECK-SAME:                                  %[[VAL_0:.*]]: tensor<4x1x1xi32>,
// CHECK-SAME:                                  %[[VAL_1:.*]]: tensor<4x4x4x4xi32>) -> tensor<4x4x4x4xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.AddV2"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4x1x1xi32>, tensor<4x4x4x4xi32>) -> tensor<4x4x4x4xi32>
// CHECK:           return %[[VAL_2]] : tensor<4x4x4x4xi32>
// CHECK:         }

// CHECK-LABEL:   func @div(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_1]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_div(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                        %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi32>
// CHECK:         }

// CHECK-LABEL:   func @shift_left(
// CHECK-SAME:                     %[[VAL_0:.*]]: tensor<4xi32>,
// CHECK-SAME:                     %[[VAL_1:.*]]: tensor<4xi32>) -> tensor<4xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.LeftShift"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4xi32>, tensor<4xi32>) -> tensor<4xi32>
// CHECK:           return %[[VAL_2]] : tensor<4xi32>
// CHECK:         }

// CHECK-LABEL:   func @div_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xi32>,
// CHECK-SAME:                      %[[VAL_1:.*]]: tensor<?x?xi32>) -> tensor<?x?xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_1]]) : (tensor<?xi32>, tensor<?x?xi32>) -> tensor<?x?xi32>
// CHECK:           return %[[VAL_2]] : tensor<?x?xi32>
// CHECK:         }

// CHECK-LABEL:   func @maximum(
// CHECK-SAME:                  %[[VAL_0:.*]]: tensor<4xf32>,
// CHECK-SAME:                  %[[VAL_1:.*]]: tensor<4xf32>) -> tensor<4xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Maximum"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4xf32>, tensor<4xf32>) -> tensor<4xf32>
// CHECK:           return %[[VAL_2]] : tensor<4xf32>
// CHECK:         }

// CHECK-LABEL:   func @minimum(
// CHECK-SAME:                  %[[VAL_0:.*]]: tensor<4xf32>,
// CHECK-SAME:                  %[[VAL_1:.*]]: tensor<4xf32>) -> tensor<4xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Minimum"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4xf32>, tensor<4xf32>) -> tensor<4xf32>
// CHECK:           return %[[VAL_2]] : tensor<4xf32>
// CHECK:         }

// CHECK-LABEL:   func @mul(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Mul"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_1]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_mul(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                        %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Mul"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi32>
// CHECK:         }

// CHECK-LABEL:   func @real_div(
// CHECK-SAME:                   %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_1]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_real_div(
// CHECK-SAME:                             %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                             %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi32>
// CHECK:         }

// CHECK-LABEL:   func @sub(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Sub"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_1]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_sub(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                        %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Sub"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi32>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi32>
// CHECK:         }

// CHECK-LABEL:   func @shift_right(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<4xi32>,
// CHECK-SAME:                      %[[VAL_1:.*]]: tensor<4xi32>) -> tensor<4xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.RightShift"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4xi32>, tensor<4xi32>) -> tensor<4xi32>
// CHECK:           return %[[VAL_2]] : tensor<4xi32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_shift_right(
// CHECK-SAME:                                %[[VAL_0:.*]]: tensor<4xi32>,
// CHECK-SAME:                                %[[VAL_1:.*]]: tensor<2x4xi32>) -> tensor<2x4xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.RightShift"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4xi32>, tensor<2x4xi32>) -> tensor<2x4xi32>
// CHECK:           return %[[VAL_2]] : tensor<2x4xi32>
// CHECK:         }

// CHECK-LABEL:   func @and(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xi1>,
// CHECK-SAME:              %[[VAL_1:.*]]: tensor<2xi1>) -> tensor<2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.LogicalAnd"(%[[VAL_0]], %[[VAL_1]]) : (tensor<2xi1>, tensor<2xi1>) -> tensor<2xi1>
// CHECK:           return %[[VAL_2]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @and_broadcast(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<1xi1>,
// CHECK-SAME:                        %[[VAL_1:.*]]: tensor<1x2xi1>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.LogicalAnd"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi1>, tensor<1x2xi1>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @and_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xi1>,
// CHECK-SAME:                      %[[VAL_1:.*]]: tensor<1xi1>) -> tensor<?xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.LogicalAnd"(%[[VAL_0]], %[[VAL_1]]) : (tensor<?xi1>, tensor<1xi1>) -> tensor<?xi1>
// CHECK:           return %[[VAL_2]] : tensor<?xi1>
// CHECK:         }

// CHECK-LABEL:   func @or(
// CHECK-SAME:             %[[VAL_0:.*]]: tensor<2xi1>,
// CHECK-SAME:             %[[VAL_1:.*]]: tensor<2xi1>) -> tensor<2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.LogicalOr"(%[[VAL_0]], %[[VAL_1]]) : (tensor<2xi1>, tensor<2xi1>) -> tensor<2xi1>
// CHECK:           return %[[VAL_2]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @or_broadcast(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<1xi1>,
// CHECK-SAME:                       %[[VAL_1:.*]]: tensor<1x2xi1>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.LogicalOr"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi1>, tensor<1x2xi1>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @or_dynamic(
// CHECK-SAME:                     %[[VAL_0:.*]]: tensor<?xi1>,
// CHECK-SAME:                     %[[VAL_1:.*]]: tensor<1xi1>) -> tensor<?xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.LogicalOr"(%[[VAL_0]], %[[VAL_1]]) : (tensor<?xi1>, tensor<1xi1>) -> tensor<?xi1>
// CHECK:           return %[[VAL_2]] : tensor<?xi1>
// CHECK:         }

// CHECK-LABEL:   func @bitwise_or(
// CHECK-SAME:                     %[[VAL_0:.*]]: tensor<4xi32>,
// CHECK-SAME:                     %[[VAL_1:.*]]: tensor<4xi32>) -> tensor<4xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.BitwiseOr"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4xi32>, tensor<4xi32>) -> tensor<4xi32>
// CHECK:           return %[[VAL_2]] : tensor<4xi32>
// CHECK:         }

// CHECK-LABEL:   func @bitwise_or_broadcast(
// CHECK-SAME:                               %[[VAL_0:.*]]: tensor<1xi8>,
// CHECK-SAME:                               %[[VAL_1:.*]]: tensor<1x4xi8>) -> tensor<1x4xi8> {
// CHECK:           %[[VAL_2:.*]] = "tf.BitwiseOr"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi8>, tensor<1x4xi8>) -> tensor<1x4xi8>
// CHECK:           return %[[VAL_2]] : tensor<1x4xi8>
// CHECK:         }

// CHECK-LABEL:   func @bitwise_or_dynamic(
// CHECK-SAME:                             %[[VAL_0:.*]]: tensor<?xi32>,
// CHECK-SAME:                             %[[VAL_1:.*]]: tensor<1xi32>) -> tensor<?xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.BitwiseOr"(%[[VAL_0]], %[[VAL_1]]) : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi32>
// CHECK:           return %[[VAL_2]] : tensor<?xi32>
// CHECK:         }

// CHECK-LABEL:   func @bitwise_and(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<4xi32>,
// CHECK-SAME:                      %[[VAL_1:.*]]: tensor<4xi32>) -> tensor<4xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.BitwiseAnd"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4xi32>, tensor<4xi32>) -> tensor<4xi32>
// CHECK:           return %[[VAL_2]] : tensor<4xi32>
// CHECK:         }

// CHECK-LABEL:   func @bitwise_and_broadcast(
// CHECK-SAME:                                %[[VAL_0:.*]]: tensor<1xi8>,
// CHECK-SAME:                                %[[VAL_1:.*]]: tensor<1x4xi8>) -> tensor<1x4xi8> {
// CHECK:           %[[VAL_2:.*]] = "tf.BitwiseAnd"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi8>, tensor<1x4xi8>) -> tensor<1x4xi8>
// CHECK:           return %[[VAL_2]] : tensor<1x4xi8>
// CHECK:         }

// CHECK-LABEL:   func @bitwise_and_dynamic(
// CHECK-SAME:                              %[[VAL_0:.*]]: tensor<?xi32>,
// CHECK-SAME:                              %[[VAL_1:.*]]: tensor<1xi32>) -> tensor<?xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.BitwiseAnd"(%[[VAL_0]], %[[VAL_1]]) : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi32>
// CHECK:           return %[[VAL_2]] : tensor<?xi32>
// CHECK:         }

// CHECK-LABEL:   func @pow(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Pow"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @pow_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Pow"(%[[VAL_0]], %[[VAL_0]]) : (tensor<?xf32>, tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @floordiv_broadcast_i32(
// CHECK-SAME:                                 %[[VAL_0:.*]]: tensor<2x3xi32>,
// CHECK-SAME:                                 %[[VAL_1:.*]]: tensor<3xi32>) -> tensor<2x3xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<0> : tensor<2x3xi32>} : () -> tensor<2x3xi32>
// CHECK:           %[[VAL_3:.*]] = "tf.Less"(%[[VAL_0]], %[[VAL_2]]) : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi1>
// CHECK:           %[[VAL_4:.*]] = "tf.Const"() {value = dense<0> : tensor<3xi32>} : () -> tensor<3xi32>
// CHECK:           %[[VAL_5:.*]] = "tf.Less"(%[[VAL_1]], %[[VAL_4]]) : (tensor<3xi32>, tensor<3xi32>) -> tensor<3xi1>
// CHECK:           %[[VAL_6:.*]] = "tf.Equal"(%[[VAL_3]], %[[VAL_5]]) {incompatible_shape_error = true} : (tensor<2x3xi1>, tensor<3xi1>) -> tensor<2x3xi1>
// CHECK:           %[[VAL_7:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_1]]) : (tensor<2x3xi32>, tensor<3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_8:.*]] = "tf.Abs"(%[[VAL_0]]) : (tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_9:.*]] = "tf.Abs"(%[[VAL_1]]) : (tensor<3xi32>) -> tensor<3xi32>
// CHECK:           %[[VAL_10:.*]] = "tf.Const"() {value = dense<1> : tensor<3xi32>} : () -> tensor<3xi32>
// CHECK:           %[[VAL_11:.*]] = "tf.Sub"(%[[VAL_9]], %[[VAL_10]]) : (tensor<3xi32>, tensor<3xi32>) -> tensor<3xi32>
// CHECK:           %[[VAL_12:.*]] = "tf.AddV2"(%[[VAL_8]], %[[VAL_11]]) : (tensor<2x3xi32>, tensor<3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_13:.*]] = "tf.Neg"(%[[VAL_12]]) : (tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_14:.*]] = "tf.Abs"(%[[VAL_1]]) : (tensor<3xi32>) -> tensor<3xi32>
// CHECK:           %[[VAL_15:.*]] = "tf.Div"(%[[VAL_13]], %[[VAL_14]]) : (tensor<2x3xi32>, tensor<3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_16:.*]] = "tf.Select"(%[[VAL_6]], %[[VAL_7]], %[[VAL_15]]) : (tensor<2x3xi1>, tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           return %[[VAL_16]] : tensor<2x3xi32>
// CHECK:         }

// CHECK-LABEL:   func @floordiv_reverse_broadcast_i32(
// CHECK-SAME:                                         %[[VAL_0:.*]]: tensor<3xi32>,
// CHECK-SAME:                                         %[[VAL_1:.*]]: tensor<2x3xi32>) -> tensor<2x3xi32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<0> : tensor<3xi32>} : () -> tensor<3xi32>
// CHECK:           %[[VAL_3:.*]] = "tf.Less"(%[[VAL_0]], %[[VAL_2]]) : (tensor<3xi32>, tensor<3xi32>) -> tensor<3xi1>
// CHECK:           %[[VAL_4:.*]] = "tf.Const"() {value = dense<0> : tensor<2x3xi32>} : () -> tensor<2x3xi32>
// CHECK:           %[[VAL_5:.*]] = "tf.Less"(%[[VAL_1]], %[[VAL_4]]) : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi1>
// CHECK:           %[[VAL_6:.*]] = "tf.Equal"(%[[VAL_3]], %[[VAL_5]]) {incompatible_shape_error = true} : (tensor<3xi1>, tensor<2x3xi1>) -> tensor<2x3xi1>
// CHECK:           %[[VAL_7:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_1]]) : (tensor<3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_8:.*]] = "tf.Abs"(%[[VAL_0]]) : (tensor<3xi32>) -> tensor<3xi32>
// CHECK:           %[[VAL_9:.*]] = "tf.Abs"(%[[VAL_1]]) : (tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_10:.*]] = "tf.Const"() {value = dense<1> : tensor<2x3xi32>} : () -> tensor<2x3xi32>
// CHECK:           %[[VAL_11:.*]] = "tf.Sub"(%[[VAL_9]], %[[VAL_10]]) : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_12:.*]] = "tf.AddV2"(%[[VAL_8]], %[[VAL_11]]) : (tensor<3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_13:.*]] = "tf.Neg"(%[[VAL_12]]) : (tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_14:.*]] = "tf.Abs"(%[[VAL_1]]) : (tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_15:.*]] = "tf.Div"(%[[VAL_13]], %[[VAL_14]]) : (tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           %[[VAL_16:.*]] = "tf.Select"(%[[VAL_6]], %[[VAL_7]], %[[VAL_15]]) : (tensor<2x3xi1>, tensor<2x3xi32>, tensor<2x3xi32>) -> tensor<2x3xi32>
// CHECK:           return %[[VAL_16]] : tensor<2x3xi32>
// CHECK:         }

// CHECK-LABEL:   func @floordiv_f32(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           %[[VAL_2:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           %[[VAL_3:.*]] = "tf.FloorDiv"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_3]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @floordiv_f16_broadcast(
// CHECK-SAME:                                 %[[VAL_0:.*]]: tensor<2x3xf16>,
// CHECK-SAME:                                 %[[VAL_1:.*]]: tensor<3xf16>) -> tensor<2x3xf16> {
// CHECK:           %[[VAL_2:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_1]]) : (tensor<2x3xf16>, tensor<3xf16>) -> tensor<2x3xf16>
// CHECK:           %[[VAL_3:.*]] = "tf.Div"(%[[VAL_0]], %[[VAL_1]]) : (tensor<2x3xf16>, tensor<3xf16>) -> tensor<2x3xf16>
// CHECK:           %[[VAL_4:.*]] = "tf.FloorDiv"(%[[VAL_0]], %[[VAL_1]]) : (tensor<2x3xf16>, tensor<3xf16>) -> tensor<2x3xf16>
// CHECK:           return %[[VAL_4]] : tensor<2x3xf16>
// CHECK:         }

// CHECK-LABEL:   func @equal(
// CHECK-SAME:                %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.Equal"(%[[VAL_0]], %[[VAL_0]]) {incompatible_shape_error = true} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
// CHECK:           return %[[VAL_1]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @equal_dynamic(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<?xi32>,
// CHECK-SAME:                        %[[VAL_1:.*]]: tensor<1xi32>) -> tensor<?xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.Equal"(%[[VAL_0]], %[[VAL_1]]) {incompatible_shape_error = true} : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi1>
// CHECK:           return %[[VAL_2]] : tensor<?xi1>
// CHECK:         }

// CHECK-LABEL:   func @equal_broadcast(
// CHECK-SAME:                          %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                          %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.Equal"(%[[VAL_0]], %[[VAL_1]]) {incompatible_shape_error = true} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @equal_broadcast_no_incompatible_shapes_error(
// CHECK-SAME:                                                       %[[VAL_0:.*]]: tensor<2xi32>,
// CHECK-SAME:                                                       %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.Equal"(%[[VAL_0]], %[[VAL_1]]) {incompatible_shape_error = true} : (tensor<2xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @equal_incompatible_shape_broadcastable(
// CHECK-SAME:                                                 %[[VAL_0:.*]]: tensor<?xi32>,
// CHECK-SAME:                                                 %[[VAL_1:.*]]: tensor<1xi32>) -> tensor<?xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.Equal"(%[[VAL_0]], %[[VAL_1]]) {incompatible_shape_error = true} : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi1>
// CHECK:           return %[[VAL_2]] : tensor<?xi1>
// CHECK:         }

// CHECK-LABEL:   func @notequal(
// CHECK-SAME:                   %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.NotEqual"(%[[VAL_0]], %[[VAL_0]]) {incompatible_shape_error = true} : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
// CHECK:           return %[[VAL_1]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @notequal_broadcast(
// CHECK-SAME:                             %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                             %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.NotEqual"(%[[VAL_0]], %[[VAL_1]]) {incompatible_shape_error = true} : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @notequal_broadcast_no_incompatible_shapes_error(
// CHECK-SAME:                                                          %[[VAL_0:.*]]: tensor<2xi32>,
// CHECK-SAME:                                                          %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.NotEqual"(%[[VAL_0]], %[[VAL_1]]) {incompatible_shape_error = true} : (tensor<2xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @notequal_incompatible_shape_broadcastable(
// CHECK-SAME:                                                    %[[VAL_0:.*]]: tensor<?xi32>,
// CHECK-SAME:                                                    %[[VAL_1:.*]]: tensor<1xi32>) -> tensor<?xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.NotEqual"(%[[VAL_0]], %[[VAL_1]]) {incompatible_shape_error = true} : (tensor<?xi32>, tensor<1xi32>) -> tensor<?xi1>
// CHECK:           return %[[VAL_2]] : tensor<?xi1>
// CHECK:         }

// CHECK-LABEL:   func @greater(
// CHECK-SAME:                  %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.Greater"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
// CHECK:           return %[[VAL_1]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_greater(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                            %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.Greater"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @greater_equal(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.GreaterEqual"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
// CHECK:           return %[[VAL_1]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_greater_equal(
// CHECK-SAME:                                  %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                                  %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.GreaterEqual"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @less(
// CHECK-SAME:               %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.Less"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
// CHECK:           return %[[VAL_1]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_less(
// CHECK-SAME:                         %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                         %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.Less"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @less_equal(
// CHECK-SAME:                     %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.LessEqual"(%[[VAL_0]], %[[VAL_0]]) : (tensor<2xi32>, tensor<2xi32>) -> tensor<2xi1>
// CHECK:           return %[[VAL_1]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_less_equal(
// CHECK-SAME:                               %[[VAL_0:.*]]: tensor<1xi32>,
// CHECK-SAME:                               %[[VAL_1:.*]]: tensor<1x2xi32>) -> tensor<1x2xi1> {
// CHECK:           %[[VAL_2:.*]] = "tf.LessEqual"(%[[VAL_0]], %[[VAL_1]]) : (tensor<1xi32>, tensor<1x2xi32>) -> tensor<1x2xi1>
// CHECK:           return %[[VAL_2]] : tensor<1x2xi1>
// CHECK:         }

// CHECK-LABEL:   func @concat_v2(
// CHECK-SAME:                    %[[VAL_0:.*]]: tensor<3x3xf32>,
// CHECK-SAME:                    %[[VAL_1:.*]]: tensor<3x3xf32>) -> tensor<6x3xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<0> : tensor<i64>} : () -> tensor<i64>
// CHECK:           %[[VAL_3:.*]] = "tf.ConcatV2"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<3x3xf32>, tensor<3x3xf32>, tensor<i64>) -> tensor<6x3xf32>
// CHECK:           return %[[VAL_3]] : tensor<6x3xf32>
// CHECK:         }

// CHECK-LABEL:   func @concat_v2_1d_axis(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<3x3xf32>,
// CHECK-SAME:                            %[[VAL_1:.*]]: tensor<3x3xf32>) -> tensor<3x6xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<1> : tensor<i64>} : () -> tensor<i64>
// CHECK:           %[[VAL_3:.*]] = "tf.ConcatV2"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<3x3xf32>, tensor<3x3xf32>, tensor<i64>) -> tensor<3x6xf32>
// CHECK:           return %[[VAL_3]] : tensor<3x6xf32>
// CHECK:         }

// CHECK-LABEL:   func @const() -> tensor<2xi32> {
// CHECK:           %[[VAL_0:.*]] = "tf.Const"() {value = dense<0> : tensor<2xi32>} : () -> tensor<2xi32>
// CHECK:           return %[[VAL_0]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @relu(
// CHECK-SAME:               %[[VAL_0:.*]]: tensor<1xi32>) -> tensor<1xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<0> : tensor<i32>} : () -> tensor<i32>
// CHECK:           %[[VAL_2:.*]] = "tf.Maximum"(%[[VAL_1]], %[[VAL_0]]) : (tensor<i32>, tensor<1xi32>) -> tensor<1xi32>
// CHECK:           return %[[VAL_2]] : tensor<1xi32>
// CHECK:         }

// CHECK-LABEL:   func @relu_unranked(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<?xi32>) -> tensor<?xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<0> : tensor<i32>} : () -> tensor<i32>
// CHECK:           %[[VAL_2:.*]] = "tf.Maximum"(%[[VAL_1]], %[[VAL_0]]) : (tensor<i32>, tensor<?xi32>) -> tensor<?xi32>
// CHECK:           return %[[VAL_2]] : tensor<?xi32>
// CHECK:         }

// CHECK-LABEL:   func @relu6(
// CHECK-SAME:                %[[VAL_0:.*]]: tensor<1xi32>) -> tensor<1xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<0> : tensor<i32>} : () -> tensor<i32>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<6> : tensor<i32>} : () -> tensor<i32>
// CHECK:           %[[VAL_3:.*]] = "tf.Minimum"(%[[VAL_0]], %[[VAL_2]]) : (tensor<1xi32>, tensor<i32>) -> tensor<1xi32>
// CHECK:           %[[VAL_4:.*]] = "tf.Maximum"(%[[VAL_3]], %[[VAL_1]]) : (tensor<1xi32>, tensor<i32>) -> tensor<1xi32>
// CHECK:           return %[[VAL_4]] : tensor<1xi32>
// CHECK:         }

// CHECK-LABEL:   func @relu6_unranked(
// CHECK-SAME:                         %[[VAL_0:.*]]: tensor<?xi32>) -> tensor<?xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<0> : tensor<i32>} : () -> tensor<i32>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<6> : tensor<i32>} : () -> tensor<i32>
// CHECK:           %[[VAL_3:.*]] = "tf.Minimum"(%[[VAL_0]], %[[VAL_2]]) : (tensor<?xi32>, tensor<i32>) -> tensor<?xi32>
// CHECK:           %[[VAL_4:.*]] = "tf.Maximum"(%[[VAL_3]], %[[VAL_1]]) : (tensor<?xi32>, tensor<i32>) -> tensor<?xi32>
// CHECK:           return %[[VAL_4]] : tensor<?xi32>
// CHECK:         }

// CHECK-LABEL:   func @relu_grad(
// CHECK-SAME:                    %[[VAL_0:.*]]: tensor<4x8xf32>,
// CHECK-SAME:                    %[[VAL_1:.*]]: tensor<?x?xf32>) -> tensor<4x8xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<0.000000e+00> : tensor<f32>} : () -> tensor<f32>
// CHECK:           %[[VAL_3:.*]] = "tf.Greater"(%[[VAL_1]], %[[VAL_2]]) : (tensor<?x?xf32>, tensor<f32>) -> tensor<?x?xi1>
// CHECK:           %[[VAL_4:.*]] = "tf.Const"() {value = dense<0.000000e+00> : tensor<4x8xf32>} : () -> tensor<4x8xf32>
// CHECK:           %[[VAL_5:.*]] = "tf.Select"(%[[VAL_3]], %[[VAL_0]], %[[VAL_4]]) : (tensor<?x?xi1>, tensor<4x8xf32>, tensor<4x8xf32>) -> tensor<4x8xf32>
// CHECK:           return %[[VAL_5]] : tensor<4x8xf32>
// CHECK:         }

// CHECK-LABEL:   func @select(
// CHECK-SAME:                 %[[VAL_0:.*]]: tensor<2xi1>,
// CHECK-SAME:                 %[[VAL_1:.*]]: tensor<2xi32>,
// CHECK-SAME:                 %[[VAL_2:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_3:.*]] = "tf.Select"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<2xi1>, tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_3]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @select_float(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<2xi1>,
// CHECK-SAME:                       %[[VAL_1:.*]]: tensor<2xf32>,
// CHECK-SAME:                       %[[VAL_2:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_3:.*]] = "tf.Select"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<2xi1>, tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_3]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @select_multidimensional(
// CHECK-SAME:                                  %[[VAL_0:.*]]: tensor<3x2xi1>,
// CHECK-SAME:                                  %[[VAL_1:.*]]: tensor<3x2xi32>,
// CHECK-SAME:                                  %[[VAL_2:.*]]: tensor<3x2xi32>) -> tensor<3x2xi32> {
// CHECK:           %[[VAL_3:.*]] = "tf.Select"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<3x2xi1>, tensor<3x2xi32>, tensor<3x2xi32>) -> tensor<3x2xi32>
// CHECK:           return %[[VAL_3]] : tensor<3x2xi32>
// CHECK:         }

// CHECK-LABEL:   func @selectv2(
// CHECK-SAME:                   %[[VAL_0:.*]]: tensor<2xi1>,
// CHECK-SAME:                   %[[VAL_1:.*]]: tensor<2xi32>,
// CHECK-SAME:                   %[[VAL_2:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_3:.*]] = "tf.Select"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<2xi1>, tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_3]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @selectv2_pred_scalar(
// CHECK-SAME:                               %[[VAL_0:.*]]: tensor<i1>,
// CHECK-SAME:                               %[[VAL_1:.*]]: tensor<2xi32>,
// CHECK-SAME:                               %[[VAL_2:.*]]: tensor<2xi32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_3:.*]] = "tf.Select"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<i1>, tensor<2xi32>, tensor<2xi32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_3]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @transpose_2d(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<2x3xf32>) -> tensor<3x2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_4:.*]] = "tf.Transpose"(%[[VAL_0]], %[[VAL_3]]) : (tensor<2x3xf32>, tensor<2xi64>) -> tensor<3x2xf32>
// CHECK:           return %[[VAL_4]] : tensor<3x2xf32>
// CHECK:         }

// CHECK-LABEL:   func @transpose_3d_int32(
// CHECK-SAME:                             %[[VAL_0:.*]]: tensor<1x2x3xf32>) -> tensor<3x2x1xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<[2, 1, 0]> : tensor<3xi32>} : () -> tensor<3xi32>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<[2, 1, 0]> : tensor<3xi64>} : () -> tensor<3xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Const"() {value = dense<[2, 1, 0]> : tensor<3xi64>} : () -> tensor<3xi64>
// CHECK:           %[[VAL_4:.*]] = "tf.Transpose"(%[[VAL_0]], %[[VAL_3]]) : (tensor<1x2x3xf32>, tensor<3xi64>) -> tensor<3x2x1xf32>
// CHECK:           return %[[VAL_4]] : tensor<3x2x1xf32>
// CHECK:         }

// CHECK-LABEL:   func @transpose_3d(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<1x2x3xf32>) -> tensor<3x2x1xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<[2, 1, 0]> : tensor<3xi64>} : () -> tensor<3xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<[2, 1, 0]> : tensor<3xi64>} : () -> tensor<3xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Const"() {value = dense<[2, 1, 0]> : tensor<3xi64>} : () -> tensor<3xi64>
// CHECK:           %[[VAL_4:.*]] = "tf.Transpose"(%[[VAL_0]], %[[VAL_3]]) : (tensor<1x2x3xf32>, tensor<3xi64>) -> tensor<3x2x1xf32>
// CHECK:           return %[[VAL_4]] : tensor<3x2x1xf32>
// CHECK:         }

// CHECK-LABEL:   func @transpose_dynamic_2d(
// CHECK-SAME:                               %[[VAL_0:.*]]: tensor<?x4xf32>) -> tensor<4x?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_4:.*]] = "tf.Transpose"(%[[VAL_0]], %[[VAL_3]]) : (tensor<?x4xf32>, tensor<2xi64>) -> tensor<4x?xf32>
// CHECK:           return %[[VAL_4]] : tensor<4x?xf32>
// CHECK:         }

// CHECK-LABEL:   func @transpose_unranked_2d(
// CHECK-SAME:                                %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Const"() {value = dense<[1, 0]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_4:.*]] = "tf.Transpose"(%[[VAL_0]], %[[VAL_3]]) : (tensor<*xf32>, tensor<2xi64>) -> tensor<*xf32>
// CHECK:           return %[[VAL_4]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @abs(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Abs"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @abs_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Abs"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @abs_unranked(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Abs"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @ceil(
// CHECK-SAME:               %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Ceil"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @ceil_dynamic(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Ceil"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @ceil_unranked(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Ceil"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @complex_abs(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<2xcomplex<f32>>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.ComplexAbs"(%[[VAL_0]]) : (tensor<2xcomplex<f32>>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @cos(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Cos"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @cos_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Cos"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @cos_unranked(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Cos"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @exp(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Exp"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @exp_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Exp"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @exp_unranked(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Exp"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @floor(
// CHECK-SAME:                %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Floor"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @floor_dynamic(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Floor"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @floor_unranked(
// CHECK-SAME:                         %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Floor"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @is_finite(
// CHECK-SAME:                    %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.IsFinite"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xi1>
// CHECK:           return %[[VAL_1]] : tensor<2xi1>
// CHECK:         }

// CHECK-LABEL:   func @is_finite_dynamic(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.IsFinite"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xi1>
// CHECK:           return %[[VAL_1]] : tensor<?xi1>
// CHECK:         }

// CHECK-LABEL:   func @is_finite_unranked(
// CHECK-SAME:                             %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xi1> {
// CHECK:           %[[VAL_1:.*]] = "tf.IsFinite"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xi1>
// CHECK:           return %[[VAL_1]] : tensor<*xi1>
// CHECK:         }

// CHECK-LABEL:   func @log(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Log"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @log_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Log"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @log_unranked(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Log"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @log1p(
// CHECK-SAME:                %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Log1p"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @log1p_dynamic(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Log1p"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @log1p_unranked(
// CHECK-SAME:                         %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Log1p"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @neg(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Neg"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @neg_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Neg"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @neg_unranked(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Neg"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @sigmoid(
// CHECK-SAME:                  %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<5.000000e-01> : tensor<f32>} : () -> tensor<f32>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<2> : tensor<1xi64>} : () -> tensor<1xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Const"() {value = dense<5.000000e-01> : tensor<2xf32>} : () -> tensor<2xf32>
// CHECK:           %[[VAL_4:.*]] = "tf.Mul"(%[[VAL_0]], %[[VAL_3]]) : (tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           %[[VAL_5:.*]] = "tf.Tanh"(%[[VAL_4]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           %[[VAL_6:.*]] = "tf.Mul"(%[[VAL_5]], %[[VAL_3]]) : (tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           %[[VAL_7:.*]] = "tf.AddV2"(%[[VAL_6]], %[[VAL_3]]) : (tensor<2xf32>, tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_7]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @sin(
// CHECK-SAME:              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Sin"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @sin_dynamic(
// CHECK-SAME:                      %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Sin"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @sin_unranked(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Sin"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @rsqrt(
// CHECK-SAME:                %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Rsqrt"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @rsqrt_dynamic(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Rsqrt"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @rsqrt_unranked(
// CHECK-SAME:                         %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Rsqrt"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @sqrt(
// CHECK-SAME:               %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Sqrt"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @sqrt_dynamic(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Sqrt"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @sqrt_unranked(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Sqrt"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @tanh(
// CHECK-SAME:               %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Tanh"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @tanh_dynamic(
// CHECK-SAME:                       %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Tanh"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @tanh_unranked(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Tanh"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @bitcast(
// CHECK-SAME:                  %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Bitcast"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @bitcast_dynamic(
// CHECK-SAME:                          %[[VAL_0:.*]]: tensor<?xf32>) -> tensor<?xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Bitcast"(%[[VAL_0]]) : (tensor<?xf32>) -> tensor<?xf32>
// CHECK:           return %[[VAL_1]] : tensor<?xf32>
// CHECK:         }

// CHECK-LABEL:   func @bitcast_unranked(
// CHECK-SAME:                           %[[VAL_0:.*]]: tensor<*xf32>) -> tensor<*xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Bitcast"(%[[VAL_0]]) : (tensor<*xf32>) -> tensor<*xf32>
// CHECK:           return %[[VAL_1]] : tensor<*xf32>
// CHECK:         }

// CHECK-LABEL:   func @bitcast_same_widths(
// CHECK-SAME:                              %[[VAL_0:.*]]: tensor<2xf32>) -> tensor<2xi32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Bitcast"(%[[VAL_0]]) : (tensor<2xf32>) -> tensor<2xi32>
// CHECK:           return %[[VAL_1]] : tensor<2xi32>
// CHECK:         }

// CHECK-LABEL:   func @sign(
// CHECK-SAME:               %[[VAL_0:.*]]: tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.NotEqual"(%[[VAL_0]], %[[VAL_0]]) {incompatible_shape_error = true} : (tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xi1>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<0.000000e+00> : tensor<1x2x3x4xf32>} : () -> tensor<1x2x3x4xf32>
// CHECK:           %[[VAL_3:.*]] = "tf.NotEqual"(%[[VAL_0]], %[[VAL_0]]) {incompatible_shape_error = true} : (tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xi1>
// CHECK:           %[[VAL_4:.*]] = "tf.Const"() {value = dense<0.000000e+00> : tensor<1x2x3x4xf32>} : () -> tensor<1x2x3x4xf32>
// CHECK:           %[[VAL_5:.*]] = "tf.Sign"(%[[VAL_0]]) : (tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32>
// CHECK:           %[[VAL_6:.*]] = "tf.Select"(%[[VAL_3]], %[[VAL_4]], %[[VAL_5]]) : (tensor<1x2x3x4xi1>, tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32>
// CHECK:           %[[VAL_7:.*]] = "tf.Select"(%[[VAL_1]], %[[VAL_2]], %[[VAL_6]]) : (tensor<1x2x3x4xi1>, tensor<1x2x3x4xf32>, tensor<1x2x3x4xf32>) -> tensor<1x2x3x4xf32>
// CHECK:           return %[[VAL_7]] : tensor<1x2x3x4xf32>
// CHECK:         }

// CHECK-LABEL:   func @size_rank_one_i32(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<f32>) -> tensor<i32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<1> : tensor<i32>} : () -> tensor<i32>
// CHECK:           return %[[VAL_1]] : tensor<i32>
// CHECK:         }

// CHECK-LABEL:   func @size_rank_one_i64(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<f32>) -> tensor<i64> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<1> : tensor<i64>} : () -> tensor<i64>
// CHECK:           return %[[VAL_1]] : tensor<i64>
// CHECK:         }

// CHECK-LABEL:   func @complex(
// CHECK-SAME:                  %[[VAL_0:.*]]: tensor<3xf32>,
// CHECK-SAME:                  %[[VAL_1:.*]]: tensor<3xf32>) -> tensor<3xcomplex<f32>> {
// CHECK:           %[[VAL_2:.*]] = "tf.Complex"(%[[VAL_0]], %[[VAL_1]]) : (tensor<3xf32>, tensor<3xf32>) -> tensor<3xcomplex<f32>>
// CHECK:           return %[[VAL_2]] : tensor<3xcomplex<f32>>
// CHECK:         }

// CHECK-LABEL:   func @convert_i32_f32(
// CHECK-SAME:                          %[[VAL_0:.*]]: tensor<2xi32>) -> tensor<2xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Cast"(%[[VAL_0]]) {Truncate = false} : (tensor<2xi32>) -> tensor<2xf32>
// CHECK:           return %[[VAL_1]] : tensor<2xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_slice(
// CHECK-SAME:                        %[[VAL_0:.*]]: tensor<1x4672xf32>) -> tensor<1x519xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<[0, 4153]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<[1, 519]> : tensor<2xi64>} : () -> tensor<2xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Slice"(%[[VAL_0]], %[[VAL_1]], %[[VAL_2]]) : (tensor<1x4672xf32>, tensor<2xi64>, tensor<2xi64>) -> tensor<1x519xf32>
// CHECK:           return %[[VAL_3]] : tensor<1x519xf32>
// CHECK:         }

// CHECK-LABEL:   func @reshape(
// CHECK-SAME:                  %[[VAL_0:.*]]: tensor<4x6xf32>) -> tensor<2x2x6xf32> {
// CHECK:           %[[VAL_1:.*]] = constant dense<[2, 2, 6]> : tensor<3xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.Reshape"(%[[VAL_0]], %[[VAL_1]]) : (tensor<4x6xf32>, tensor<3xi64>) -> tensor<2x2x6xf32>
// CHECK:           return %[[VAL_2]] : tensor<2x2x6xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_dot_1d_2d(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<256xf32>,
// CHECK-SAME:                            %[[VAL_1:.*]]: tensor<256x1xf32>) -> tensor<1xf32> {
// CHECK:           %[[VAL_2:.*]] = constant dense<[1, 256]> : tensor<2xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Reshape"(%[[VAL_0]], %[[VAL_2]]) : (tensor<256xf32>, tensor<2xi64>) -> tensor<1x256xf32>
// CHECK:           %[[VAL_4:.*]] = "tf.MatMul"(%[[VAL_3]], %[[VAL_1]]) {transpose_a = false, transpose_b = false} : (tensor<1x256xf32>, tensor<256x1xf32>) -> tensor<1x1xf32>
// CHECK:           %[[VAL_5:.*]] = constant dense<1> : tensor<1xi64>
// CHECK:           %[[VAL_6:.*]] = "tf.Reshape"(%[[VAL_4]], %[[VAL_5]]) : (tensor<1x1xf32>, tensor<1xi64>) -> tensor<1xf32>
// CHECK:           return %[[VAL_6]] : tensor<1xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_dot_2d_1d(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<1x256xf32>,
// CHECK-SAME:                            %[[VAL_1:.*]]: tensor<256xf32>) -> tensor<1xf32> {
// CHECK:           %[[VAL_2:.*]] = constant dense<[1, 256]> : tensor<2xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Reshape"(%[[VAL_1]], %[[VAL_2]]) : (tensor<256xf32>, tensor<2xi64>) -> tensor<1x256xf32>
// CHECK:           %[[VAL_4:.*]] = "tf.MatMul"(%[[VAL_0]], %[[VAL_3]]) {transpose_a = false, transpose_b = true} : (tensor<1x256xf32>, tensor<1x256xf32>) -> tensor<1x1xf32>
// CHECK:           %[[VAL_5:.*]] = constant dense<1> : tensor<1xi64>
// CHECK:           %[[VAL_6:.*]] = "tf.Reshape"(%[[VAL_4]], %[[VAL_5]]) : (tensor<1x1xf32>, tensor<1xi64>) -> tensor<1xf32>
// CHECK:           return %[[VAL_6]] : tensor<1xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_dot_1d_1d(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<256xf32>,
// CHECK-SAME:                            %[[VAL_1:.*]]: tensor<256xf32>) -> tensor<f32> {
// CHECK:           %[[VAL_2:.*]] = constant dense<[1, 256]> : tensor<2xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Reshape"(%[[VAL_0]], %[[VAL_2]]) : (tensor<256xf32>, tensor<2xi64>) -> tensor<1x256xf32>
// CHECK:           %[[VAL_4:.*]] = constant dense<[1, 256]> : tensor<2xi64>
// CHECK:           %[[VAL_5:.*]] = "tf.Reshape"(%[[VAL_1]], %[[VAL_4]]) : (tensor<256xf32>, tensor<2xi64>) -> tensor<1x256xf32>
// CHECK:           %[[VAL_6:.*]] = "tf.MatMul"(%[[VAL_3]], %[[VAL_5]]) {transpose_a = false, transpose_b = true} : (tensor<1x256xf32>, tensor<1x256xf32>) -> tensor<1x1xf32>
// CHECK:           %[[VAL_7:.*]] = constant dense<> : tensor<0xi64>
// CHECK:           %[[VAL_8:.*]] = "tf.Reshape"(%[[VAL_6]], %[[VAL_7]]) : (tensor<1x1xf32>, tensor<0xi64>) -> tensor<f32>
// CHECK:           return %[[VAL_8]] : tensor<f32>
// CHECK:         }

// CHECK-LABEL:   func @convert_dot_2d_2d(
// CHECK-SAME:                            %[[VAL_0:.*]]: tensor<1x256xf32>,
// CHECK-SAME:                            %[[VAL_1:.*]]: tensor<256x1xf32>) -> tensor<1x1xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.MatMul"(%[[VAL_0]], %[[VAL_1]]) {transpose_a = false, transpose_b = false} : (tensor<1x256xf32>, tensor<256x1xf32>) -> tensor<1x1xf32>
// CHECK:           return %[[VAL_2]] : tensor<1x1xf32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_in_dim_tf_style(
// CHECK-SAME:                                    %[[VAL_0:.*]]: tensor<8x1x16xf32>) -> tensor<3x8x8x16xf32> {
// CHECK:           %[[VAL_1:.*]] = constant dense<[3, 8, 8, 16]> : tensor<4xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.BroadcastTo"(%[[VAL_0]], %[[VAL_1]]) : (tensor<8x1x16xf32>, tensor<4xi64>) -> tensor<3x8x8x16xf32>
// CHECK:           return %[[VAL_2]] : tensor<3x8x8x16xf32>
// CHECK:         }

// CHECK-LABEL:   func @broadcast_in_dim_general_case(
// CHECK-SAME:                                        %[[VAL_0:.*]]: tensor<3x1x16xf32>) -> tensor<3x8x8x16xf32> {
// CHECK:           %[[VAL_1:.*]] = constant dense<[3, 1, 1, 16]> : tensor<4xi64>
// CHECK:           %[[VAL_2:.*]] = "tf.Reshape"(%[[VAL_0]], %[[VAL_1]]) : (tensor<3x1x16xf32>, tensor<4xi64>) -> tensor<3x1x1x16xf32>
// CHECK:           %[[VAL_3:.*]] = constant dense<[3, 8, 8, 16]> : tensor<4xi64>
// CHECK:           %[[VAL_4:.*]] = "tf.BroadcastTo"(%[[VAL_2]], %[[VAL_3]]) : (tensor<3x1x1x16xf32>, tensor<4xi64>) -> tensor<3x8x8x16xf32>
// CHECK:           return %[[VAL_4]] : tensor<3x8x8x16xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_dot_general(
// CHECK-SAME:                              %[[VAL_0:.*]]: tensor<3x2x6x5x1xf32>,
// CHECK-SAME:                              %[[VAL_1:.*]]: tensor<3x2x4x6xf32>) -> tensor<3x5x1x4xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<[0, 3, 4, 1, 2]> : tensor<5xi64>} : () -> tensor<5xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Transpose"(%[[VAL_0]], %[[VAL_2]]) : (tensor<3x2x6x5x1xf32>, tensor<5xi64>) -> tensor<3x5x1x2x6xf32>
// CHECK:           %[[VAL_4:.*]] = "tf.Const"() {value = dense<[0, 1, 3, 2]> : tensor<4xi64>} : () -> tensor<4xi64>
// CHECK:           %[[VAL_5:.*]] = "tf.Transpose"(%[[VAL_1]], %[[VAL_4]]) : (tensor<3x2x4x6xf32>, tensor<4xi64>) -> tensor<3x2x6x4xf32>
// CHECK:           %[[VAL_6:.*]] = constant dense<[3, 5, 12]> : tensor<3xi64>
// CHECK:           %[[VAL_7:.*]] = "tf.Reshape"(%[[VAL_3]], %[[VAL_6]]) : (tensor<3x5x1x2x6xf32>, tensor<3xi64>) -> tensor<3x5x12xf32>
// CHECK:           %[[VAL_8:.*]] = constant dense<[3, 12, 4]> : tensor<3xi64>
// CHECK:           %[[VAL_9:.*]] = "tf.Reshape"(%[[VAL_5]], %[[VAL_8]]) : (tensor<3x2x6x4xf32>, tensor<3xi64>) -> tensor<3x12x4xf32>
// CHECK:           %[[VAL_10:.*]] = "tf.BatchMatMulV2"(%[[VAL_7]], %[[VAL_9]]) {adj_x = false, adj_y = false} : (tensor<3x5x12xf32>, tensor<3x12x4xf32>) -> tensor<3x5x4xf32>
// CHECK:           %[[VAL_11:.*]] = constant dense<[3, 5, 1, 4]> : tensor<4xi64>
// CHECK:           %[[VAL_12:.*]] = "tf.Reshape"(%[[VAL_10]], %[[VAL_11]]) : (tensor<3x5x4xf32>, tensor<4xi64>) -> tensor<3x5x1x4xf32>
// CHECK:           return %[[VAL_12]] : tensor<3x5x1x4xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_conv2d(
// CHECK-SAME:                         %[[VAL_0:.*]]: tensor<1x8x8x207xf32>,
// CHECK-SAME:                         %[[VAL_1:.*]]: tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Conv2D"(%[[VAL_0]], %[[VAL_1]]) {data_format = "NHWC", dilations = [1, 1, 1, 1], explicit_paddings = [], padding = "SAME", strides = [1, 1, 1, 1], use_cudnn_on_gpu = true} : (tensor<1x8x8x207xf32>, tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32>
// CHECK:           return %[[VAL_2]] : tensor<1x8x8x16xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_depthwise_conv2d(
// CHECK-SAME:                                   %[[VAL_0:.*]]: tensor<1x8x8x207xf32>,
// CHECK-SAME:                                   %[[VAL_1:.*]]: tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.DepthwiseConv2dNative"(%[[VAL_0]], %[[VAL_1]]) {data_format = "NHWC", dilations = [1, 1, 1, 1], explicit_paddings = [], padding = "SAME", strides = [1, 1, 1, 1]} : (tensor<1x8x8x207xf32>, tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32>
// CHECK:           return %[[VAL_2]] : tensor<1x8x8x16xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_conv2d_valid_padding(
// CHECK-SAME:                                       %[[VAL_0:.*]]: tensor<1x8x8x207xf32>,
// CHECK-SAME:                                       %[[VAL_1:.*]]: tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32> {
// CHECK:           %[[VAL_2:.*]] = "tf.Conv2D"(%[[VAL_0]], %[[VAL_1]]) {data_format = "NHWC", dilations = [1, 1, 1, 1], explicit_paddings = [], padding = "VALID", strides = [1, 1, 1, 1], use_cudnn_on_gpu = true} : (tensor<1x8x8x207xf32>, tensor<3x3x207x16xf32>) -> tensor<1x8x8x16xf32>
// CHECK:           return %[[VAL_2]] : tensor<1x8x8x16xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_reduce_to_sum(
// CHECK-SAME:                                %[[VAL_0:.*]]: tensor<1x256xf32>) -> tensor<1xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<0.000000e+00> : tensor<f32>} : () -> tensor<f32>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<1> : tensor<1xi64>} : () -> tensor<1xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Sum"(%[[VAL_0]], %[[VAL_2]]) {keep_dims = false} : (tensor<1x256xf32>, tensor<1xi64>) -> tensor<1xf32>
// CHECK:           return %[[VAL_3]] : tensor<1xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_reduce_to_max(
// CHECK-SAME:                                %[[VAL_0:.*]]: tensor<1x256xf32>) -> tensor<1xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<0xFF800000> : tensor<f32>} : () -> tensor<f32>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<1> : tensor<1xi64>} : () -> tensor<1xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Max"(%[[VAL_0]], %[[VAL_2]]) {keep_dims = false} : (tensor<1x256xf32>, tensor<1xi64>) -> tensor<1xf32>
// CHECK:           return %[[VAL_3]] : tensor<1xf32>
// CHECK:         }

// CHECK-LABEL:   func @convert_reduce_to_min(
// CHECK-SAME:                                %[[VAL_0:.*]]: tensor<1x256xf32>) -> tensor<1xf32> {
// CHECK:           %[[VAL_1:.*]] = "tf.Const"() {value = dense<0x7F800000> : tensor<f32>} : () -> tensor<f32>
// CHECK:           %[[VAL_2:.*]] = "tf.Const"() {value = dense<1> : tensor<1xi64>} : () -> tensor<1xi64>
// CHECK:           %[[VAL_3:.*]] = "tf.Min"(%[[VAL_0]], %[[VAL_2]]) {keep_dims = false} : (tensor<1x256xf32>, tensor<1xi64>) -> tensor<1xf32>
// CHECK:           return %[[VAL_3]] : tensor<1xf32>
// CHECK:         }
