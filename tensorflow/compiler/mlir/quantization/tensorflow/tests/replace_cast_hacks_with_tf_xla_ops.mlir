// Copyright 2022 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// RUN: tf-quant-opt %s -split-input-file -inline -quant-replace-cast-hacks-with-tf-xla-ops | FileCheck %s

// -----

module attributes {tf.versions = {bad_consumers = [], min_consumer = 12 : i32, producer = 1177 : i32}} {
  func.func @conv_with_bias_and_relu(%arg0: tensor<1x3x4x3xf32>) -> tensor<1x3x2x2xf32> {
    %cst = "tf.Const"() {value = dense<[162, 160]> : tensor<2xi32>} : () -> tensor<2xi32>
    %cst_0 = "tf.Const"() {value = dense<[[[[-85, 72], [23, -103], [-29, -96]], [[-128, -83], [81, -57], [67, 119]], [[44, 10], [-90, -107], [77, 122]]], [[[18, 61], [127, -20], [-107, 119]], [[12, -66], [-98, 15], [124, 9]], [[68, 119], [20, -52], [48, 123]]]]> : tensor<2x3x3x2xi8>} : () -> tensor<2x3x3x2xi8>
    %cst_1 = "tf.Const"() {value = dense<0.587548196> : tensor<f32>} : () -> tensor<f32>
    %cst_2 = "tf.Const"() {value = dense<-128> : tensor<i32>} : () -> tensor<i32>
    %cst_3 = "tf.Const"() {value = dense<18.1044273> : tensor<f32>} : () -> tensor<f32>
    %cst_4 = "tf.Const"() {value = dense<0.0748551115> : tensor<f32>} : () -> tensor<f32>
    %cst_5 = "tf.Const"() {value = dense<-1> : tensor<i32>} : () -> tensor<i32>
    %cst_6 = "tf.Const"() {value = dense<0.0439809859> : tensor<f32>} : () -> tensor<f32>
    %cst_7 = "tf.Const"() {value = dense<0> : tensor<i32>} : () -> tensor<i32>
    %0 = "tf.PartitionedCall"(%arg0, %cst_1, %cst_2) {config = "", config_proto = "", executor_type = "", f = @quantize_i8} : (tensor<1x3x4x3xf32>, tensor<f32>, tensor<i32>) -> tensor<1x3x4x3xi8>
    %1 = "tf.PartitionedCall"(%0, %cst_0, %cst, %cst_1, %cst_2, %cst_4, %cst_5, %cst_6, %cst_7, %cst_3, %cst_2) {config = "", config_proto = "", executor_type = "", f = @quantized_conv2d_with_bias_and_relu_fn_0} : (tensor<1x3x4x3xi8>, tensor<2x3x3x2xi8>, tensor<2xi32>, tensor<f32>, tensor<i32>, tensor<f32>, tensor<i32>, tensor<f32>, tensor<i32>, tensor<f32>, tensor<i32>) -> tensor<1x3x2x2xi8>
    %2 = "tf.PartitionedCall"(%1, %cst_3, %cst_2) {config = "", config_proto = "", executor_type = "", f = @dequantize_i8} : (tensor<1x3x2x2xi8>, tensor<f32>, tensor<i32>) -> tensor<1x3x2x2xf32>
    return %2 : tensor<1x3x2x2xf32>
  }
  func.func private @quantize_i8(%arg0: tensor<1x3x4x3xf32>, %arg1: tensor<f32>, %arg2: tensor<i32>) -> tensor<1x3x4x3xi8> {
    %0 = "tf.Div"(%arg0, %arg1) : (tensor<1x3x4x3xf32>, tensor<f32>) -> tensor<1x3x4x3xf32>
    %1 = "tf.Round"(%0) : (tensor<1x3x4x3xf32>) -> tensor<1x3x4x3xf32>
    %2 = "tf.Cast"(%1) : (tensor<1x3x4x3xf32>) -> tensor<1x3x4x3xi32>
    %3 = "tf.AddV2"(%2, %arg2) : (tensor<1x3x4x3xi32>, tensor<i32>) -> tensor<1x3x4x3xi32>
    %4 = "tf.Cast"(%3) {Truncate = false} : (tensor<1x3x4x3xi32>) -> tensor<1x3x4x3xi8>
    return %4 : tensor<1x3x4x3xi8>
  }
  func.func private @dequantize_i8(%arg0: tensor<1x3x2x2xi8>, %arg1: tensor<f32>, %arg2: tensor<i32>) -> tensor<1x3x2x2xf32> {
    %0 = "tf.Cast"(%arg0) : (tensor<1x3x2x2xi8>) -> tensor<1x3x2x2xi32>
    %1 = "tf.Sub"(%0, %arg2) : (tensor<1x3x2x2xi32>, tensor<i32>) -> tensor<1x3x2x2xi32>
    %2 = "tf.Cast"(%1) : (tensor<1x3x2x2xi32>) -> tensor<1x3x2x2xf32>
    %3 = "tf.Mul"(%2, %arg1) : (tensor<1x3x2x2xf32>, tensor<f32>) -> tensor<1x3x2x2xf32>
    return %3 : tensor<1x3x2x2xf32>
  }
  func.func private @quantized_conv2d_with_bias_and_relu_fn_0(%arg0: tensor<1x3x4x3xi8>, %arg1: tensor<2x3x3x2xi8>, %arg2: tensor<2xi32>, %arg3: tensor<f32>, %arg4: tensor<i32>, %arg5: tensor<f32>, %arg6: tensor<i32>, %arg7: tensor<f32>, %arg8: tensor<i32>, %arg9: tensor<f32>, %arg10: tensor<i32>) -> tensor<1x3x2x2xi8> {
    %cst = "tf.Const"() {value = dense<127> : tensor<i32>} : () -> tensor<i32>
    %cst_0 = "tf.Const"() {value = dense<-128> : tensor<i32>} : () -> tensor<i32>
    %0 = "tf.Cast"(%arg0) {Truncate = false} : (tensor<1x3x4x3xi8>) -> tensor<1x3x4x3xi32>
    %1 = "tf.Sub"(%0, %arg4) : (tensor<1x3x4x3xi32>, tensor<i32>) -> tensor<1x3x4x3xi32>
    %2 = "tf.Cast"(%arg1) {Truncate = false} : (tensor<2x3x3x2xi8>) -> tensor<2x3x3x2xi32>
    %3 = "tf.Sub"(%2, %arg6) : (tensor<2x3x3x2xi32>, tensor<i32>) -> tensor<2x3x3x2xi32>
    %4 = "tf.Conv2D"(%1, %3) {dilations = [1, 1, 1, 1], explicit_paddings = [], padding = "SAME", strides = [1, 1, 2, 1], use_cudnn_on_gpu = true} : (tensor<1x3x4x3xi32>, tensor<2x3x3x2xi32>) -> tensor<1x3x2x2xi32>
    %5 = "tf.AddV2"(%4, %arg2) : (tensor<1x3x2x2xi32>, tensor<2xi32>) -> tensor<1x3x2x2xi32>
    %6 = "tf.Mul"(%arg3, %arg5) : (tensor<f32>, tensor<f32>) -> tensor<f32>
    %7 = "tf.Div"(%6, %arg9) : (tensor<f32>, tensor<f32>) -> tensor<f32>
    %8 = "tf.Cast"(%5) {Truncate = false} : (tensor<1x3x2x2xi32>) -> tensor<1x3x2x2xf32>
    %9 = "tf.Mul"(%7, %8) : (tensor<f32>, tensor<1x3x2x2xf32>) -> tensor<1x3x2x2xf32>
    %10 = "tf.Round"(%9) : (tensor<1x3x2x2xf32>) -> tensor<1x3x2x2xf32>
    %11 = "tf.Cast"(%10) {Truncate = false} : (tensor<1x3x2x2xf32>) -> tensor<1x3x2x2xi32>
    %12 = "tf.AddV2"(%11, %arg10) : (tensor<1x3x2x2xi32>, tensor<i32>) -> tensor<1x3x2x2xi32>
    %13 = "tf.Maximum"(%cst_0, %arg10) : (tensor<i32>, tensor<i32>) -> tensor<i32>
    %14 = "tf.ClipByValue"(%12, %13, %cst) : (tensor<1x3x2x2xi32>, tensor<i32>, tensor<i32>) -> tensor<1x3x2x2xi32>
    %15 = "tf.Cast"(%14) {Truncate = false} : (tensor<1x3x2x2xi32>) -> tensor<1x3x2x2xi8>
    return %15 : tensor<1x3x2x2xi8>
  }

// CHECK-LABEL: func @conv_with_bias_and_relu
// CHECK-DAG: %[[CONST_0:.*]] = "tf.Const"() {value = dense<[1, 2]> : tensor<2xi32>} : () -> tensor<2xi32>
// CHECK-DAG: %[[CONST_1:.*]] = "tf.Const"() {value = dense<1> : tensor<2xi32>} : () -> tensor<2xi32>
// CHECK-DAG: %[[CONST_2:.*]] = "tf.Const"() {value = dense<1> : tensor<i32>} : () -> tensor<i32>
// CHECK-DAG: %[[CONST_3:.*]] = "tf.Const"() {value = dense<0> : tensor<2x2xi32>} : () -> tensor<2x2xi32>
// CHECK-DAG: %[[CONST_4:.*]] = "tf.Const"()
// CHECK-SAME{LITERAL}: {value = dense<[[0, 0], [0, 1], [0, 1], [0, 0]]> : tensor<4x2xi32>} : () -> tensor<4x2xi32>
// CHECK-DAG: %[[CONST_5:.*]] = "tf.Const"() {value = dense<-128> : tensor<i8>} : () -> tensor<i8>
// CHECK-DAG: %[[CONST_6:.*]] = "tf.Const"() {value = dense<{{.*}}> : tensor<2x3x3x2xi8>} : () -> tensor<2x3x3x2xi8>
// CHECK-DAG: %[[CONST_7:.*]] = "tf.Const"() {value = dense<[-24320, -25984]> : tensor<2xi32>} : () -> tensor<2xi32>
// CHECK-DAG: %[[CONST_8:.*]] = "tf.Const"() {value = dense<[162, 160]> : tensor<2xi32>} : () -> tensor<2xi32>
// CHECK: %[[PADV2_0:.*]] = "tf.PadV2"({{.*}}, %[[CONST_4]], %[[CONST_5]]) : (tensor<1x3x4x3xi8>, tensor<4x2xi32>, tensor<i8>) -> tensor<1x4x5x3xi8>
// CHECK: %[[XLACONVV2_0:.*]] = "tf.XlaConvV2"(%[[PADV2_0]], %[[CONST_6]], %[[CONST_0]], %[[CONST_3]], %[[CONST_1]], %[[CONST_1]], %[[CONST_2]])
// CHECK-SAME: (tensor<1x4x5x3xi8>, tensor<2x3x3x2xi8>, tensor<2xi32>, tensor<2x2xi32>, tensor<2xi32>, tensor<2xi32>, tensor<i32>) -> tensor<1x3x2x2xi32>
// CHECK: %[[SUB_0:.*]] = "tf.Sub"(%[[XLACONVV2_0]], %[[CONST_7]]) : (tensor<1x3x2x2xi32>, tensor<2xi32>) -> tensor<1x3x2x2xi32>
// CHECK: %[[ADDV2_1:.*]] = "tf.AddV2"(%[[SUB_0]], %[[CONST_8]]) : (tensor<1x3x2x2xi32>, tensor<2xi32>) -> tensor<1x3x2x2xi32>
}

// -----

module attributes {tf.versions = {bad_consumers = [], min_consumer = 12 : i32, producer = 1195 : i32}} {
  func.func @depthwise_conv_with_bias_and_relu6(%arg0: tensor<1x3x4x3xf32>) -> tensor<1x2x2x3xf32> {
    %cst = "tf.Const"() {value = dense<[129, 166, 221]> : tensor<3xi32>} : () -> tensor<3xi32>
    %cst_0 = "tf.Const"() {value = dense<[[[[-84], [73], [24]], [[-102], [-28], [-94]], [[-127], [-82], [82]]], [[[-56], [67], [120]], [[45], [11], [-88]], [[-106], [77], [123]]]]> : tensor<2x3x3x1xi8>} : () -> tensor<2x3x3x1xi8>
    %cst_1 = "tf.Const"() {value = dense<0.587548196> : tensor<f32>} : () -> tensor<f32>
    %cst_2 = "tf.Const"() {value = dense<-128> : tensor<i32>} : () -> tensor<i32>
    %cst_3 = "tf.Const"() {value = dense<0.0235294122> : tensor<f32>} : () -> tensor<f32>
    %cst_4 = "tf.Const"() {value = dense<0.0751230493> : tensor<1xf32>} : () -> tensor<1xf32>
    %cst_5 = "tf.Const"() {value = dense<0> : tensor<1xi32>} : () -> tensor<1xi32>
    %cst_6 = "tf.Const"() {value = dense<0.0441384129> : tensor<f32>} : () -> tensor<f32>
    %cst_7 = "tf.Const"() {value = dense<0> : tensor<i32>} : () -> tensor<i32>
    %0 = "tf.PartitionedCall"(%arg0, %cst_1, %cst_2) {config = "", config_proto = "", executor_type = "", f = @quantize_i8} : (tensor<1x3x4x3xf32>, tensor<f32>, tensor<i32>) -> tensor<1x3x4x3xi8>
    %1 = "tf.PartitionedCall"(%0, %cst_0, %cst, %cst_1, %cst_2, %cst_4, %cst_5, %cst_6, %cst_7, %cst_3, %cst_2) {config = "", config_proto = "", executor_type = "", f = @quantized_depthwise_conv2d_with_bias_and_relu6_fn_0} : (tensor<1x3x4x3xi8>, tensor<2x3x3x1xi8>, tensor<3xi32>, tensor<f32>, tensor<i32>, tensor<1xf32>, tensor<1xi32>, tensor<f32>, tensor<i32>, tensor<f32>, tensor<i32>) -> tensor<1x2x2x3xi8>
    %2 = "tf.PartitionedCall"(%1, %cst_3, %cst_2) {config = "", config_proto = "", executor_type = "", f = @dequantize_i8} : (tensor<1x2x2x3xi8>, tensor<f32>, tensor<i32>) -> tensor<1x2x2x3xf32>
    return %2 : tensor<1x2x2x3xf32>
  }
  func.func private @quantize_i8(%arg0: tensor<1x3x4x3xf32>, %arg1: tensor<f32>, %arg2: tensor<i32>) -> tensor<1x3x4x3xi8> {
    %0 = "tf.Div"(%arg0, %arg1) : (tensor<1x3x4x3xf32>, tensor<f32>) -> tensor<1x3x4x3xf32>
    %1 = "tf.Round"(%0) : (tensor<1x3x4x3xf32>) -> tensor<1x3x4x3xf32>
    %2 = "tf.Cast"(%1) : (tensor<1x3x4x3xf32>) -> tensor<1x3x4x3xi32>
    %3 = "tf.AddV2"(%2, %arg2) : (tensor<1x3x4x3xi32>, tensor<i32>) -> tensor<1x3x4x3xi32>
    %4 = "tf.Cast"(%3) {Truncate = false} : (tensor<1x3x4x3xi32>) -> tensor<1x3x4x3xi8>
    return %4 : tensor<1x3x4x3xi8>
  }
  func.func private @dequantize_i8(%arg0: tensor<1x2x2x3xi8>, %arg1: tensor<f32>, %arg2: tensor<i32>) -> tensor<1x2x2x3xf32> {
    %0 = "tf.Cast"(%arg0) : (tensor<1x2x2x3xi8>) -> tensor<1x2x2x3xi32>
    %1 = "tf.Sub"(%0, %arg2) : (tensor<1x2x2x3xi32>, tensor<i32>) -> tensor<1x2x2x3xi32>
    %2 = "tf.Cast"(%1) : (tensor<1x2x2x3xi32>) -> tensor<1x2x2x3xf32>
    %3 = "tf.Mul"(%2, %arg1) : (tensor<1x2x2x3xf32>, tensor<f32>) -> tensor<1x2x2x3xf32>
    return %3 : tensor<1x2x2x3xf32>
  }
  func.func private @quantized_depthwise_conv2d_with_bias_and_relu6_fn_0(%arg0: tensor<1x3x4x3xi8>, %arg1: tensor<2x3x3x1xi8>, %arg2: tensor<3xi32>, %arg3: tensor<f32>, %arg4: tensor<i32>, %arg5: tensor<1xf32>, %arg6: tensor<1xi32>, %arg7: tensor<f32>, %arg8: tensor<i32>, %arg9: tensor<f32>, %arg10: tensor<i32>) -> tensor<1x2x2x3xi8> {
    %cst = "tf.Const"() {value = dense<127> : tensor<i32>} : () -> tensor<i32>
    %cst_0 = "tf.Const"() {value = dense<-128> : tensor<i32>} : () -> tensor<i32>
    %cst_1 = "tf.Const"() {value = dense<6.000000e+00> : tensor<f32>} : () -> tensor<f32>
    %0 = "tf.Cast"(%arg0) {Truncate = false} : (tensor<1x3x4x3xi8>) -> tensor<1x3x4x3xi32>
    %1 = "tf.Sub"(%0, %arg4) : (tensor<1x3x4x3xi32>, tensor<i32>) -> tensor<1x3x4x3xi32>
    %2 = "tf.Cast"(%arg1) {Truncate = false} : (tensor<2x3x3x1xi8>) -> tensor<2x3x3x1xi32>
    %3 = "tf.Sub"(%2, %arg6) : (tensor<2x3x3x1xi32>, tensor<1xi32>) -> tensor<2x3x3x1xi32>
    %4 = "tf.Identity"(%3) : (tensor<2x3x3x1xi32>) -> tensor<2x3x3x1xi32>
    %5 = "tf.Cast"(%1) {Truncate = false} : (tensor<1x3x4x3xi32>) -> tensor<1x3x4x3xf32>
    %6 = "tf.Cast"(%4) {Truncate = false} : (tensor<2x3x3x1xi32>) -> tensor<2x3x3x1xf32>
    %7 = "tf.DepthwiseConv2dNative"(%5, %6) {dilations = [1, 1, 1, 1], explicit_paddings = [], padding = "SAME", strides = [1, 2, 2, 1]} : (tensor<1x3x4x3xf32>, tensor<2x3x3x1xf32>) -> tensor<1x2x2x3xf32>
    %8 = "tf.Cast"(%7) : (tensor<1x2x2x3xf32>) -> tensor<1x2x2x3xi32>
    %9 = "tf.AddV2"(%8, %arg2) : (tensor<1x2x2x3xi32>, tensor<3xi32>) -> tensor<1x2x2x3xi32>
    %10 = "tf.Mul"(%arg3, %arg5) : (tensor<f32>, tensor<1xf32>) -> tensor<1xf32>
    %11 = "tf.Div"(%10, %arg9) : (tensor<1xf32>, tensor<f32>) -> tensor<1xf32>
    %12 = "tf.Cast"(%9) {Truncate = false} : (tensor<1x2x2x3xi32>) -> tensor<1x2x2x3xf32>
    %13 = "tf.Mul"(%11, %12) : (tensor<1xf32>, tensor<1x2x2x3xf32>) -> tensor<1x2x2x3xf32>
    %14 = "tf.Round"(%13) : (tensor<1x2x2x3xf32>) -> tensor<1x2x2x3xf32>
    %15 = "tf.Cast"(%14) {Truncate = false} : (tensor<1x2x2x3xf32>) -> tensor<1x2x2x3xi32>
    %16 = "tf.AddV2"(%15, %arg10) : (tensor<1x2x2x3xi32>, tensor<i32>) -> tensor<1x2x2x3xi32>
    %17 = "tf.Div"(%cst_1, %arg9) : (tensor<f32>, tensor<f32>) -> tensor<f32>
    %18 = "tf.Round"(%17) : (tensor<f32>) -> tensor<f32>
    %19 = "tf.Cast"(%18) : (tensor<f32>) -> tensor<i32>
    %20 = "tf.AddV2"(%19, %arg10) : (tensor<i32>, tensor<i32>) -> tensor<i32>
    %21 = "tf.Cast"(%20) : (tensor<i32>) -> tensor<i8>
    %22 = "tf.Cast"(%21) {Truncate = false} : (tensor<i8>) -> tensor<i8>
    %23 = "tf.Cast"(%22) {Truncate = false} : (tensor<i8>) -> tensor<i32>
    %24 = "tf.Maximum"(%cst_0, %arg10) : (tensor<i32>, tensor<i32>) -> tensor<i32>
    %25 = "tf.Minimum"(%cst, %23) : (tensor<i32>, tensor<i32>) -> tensor<i32>
    %26 = "tf.ClipByValue"(%16, %24, %25) : (tensor<1x2x2x3xi32>, tensor<i32>, tensor<i32>) -> tensor<1x2x2x3xi32>
    %27 = "tf.Cast"(%26) {Truncate = false} : (tensor<1x2x2x3xi32>) -> tensor<1x2x2x3xi8>
    return %27 : tensor<1x2x2x3xi8>
  }

// CHECK-LABEL: func @depthwise_conv_with_bias_and_relu6
// CHECK-DAG: %[[CONST_0:.*]] = "tf.Const"() {value = dense<{{.*}}> : tensor<4x2xi32>} : () -> tensor<4x2xi32>
// CHECK-DAG: %[[CONST_1:.*]] = "tf.Const"() {value = dense<-128> : tensor<i8>} : () -> tensor<i8>
// CHECK-DAG: %[[CONST_2:.*]] = "tf.Const"() {value = dense<{{.*}}> : tensor<2x3x1x3xi8>} : () -> tensor<2x3x1x3xi8>
// CHECK-DAG: %[[CONST_3:.*]] = "tf.Const"() {value = dense<2> : tensor<2xi32>} : () -> tensor<2xi32>
// CHECK-DAG: %[[CONST_4:.*]] = "tf.Const"() {value = dense<0> : tensor<2x2xi32>} : () -> tensor<2x2xi32>
// CHECK-DAG: %[[CONST_5:.*]] = "tf.Const"() {value = dense<1> : tensor<2xi32>} : () -> tensor<2xi32>
// CHECK-DAG: %[[CONST_6:.*]] = "tf.Const"() {value = dense<3> : tensor<i32>} : () -> tensor<i32>
// CHECK-DAG: %[[CONST_7:.*]] = "tf.Const"() {value = dense<[55040, -15104, -21376]> : tensor<3xi32>} : () -> tensor<3xi32>
// CHECK-DAG: %[[CONST_8:.*]] = "tf.Const"() {value = dense<[129, 166, 221]> : tensor<3xi32>} : () -> tensor<3xi32>
// CHECK: %[[PADV2_0:.*]] = "tf.PadV2"({{.*}}, %[[CONST_0]], %[[CONST_1]]) : (tensor<1x3x4x3xi8>, tensor<4x2xi32>, tensor<i8>) -> tensor<1x4x5x3xi8>
// CHECK: %[[XLACONVV2_0:.*]] = "tf.XlaConvV2"(%[[PADV2_0]], %[[CONST_2]], %[[CONST_3]], %[[CONST_4]], %[[CONST_5]], %[[CONST_5]], %[[CONST_6]])
// CHECK-SAME: (tensor<1x4x5x3xi8>, tensor<2x3x1x3xi8>, tensor<2xi32>, tensor<2x2xi32>, tensor<2xi32>, tensor<2xi32>, tensor<i32>) -> tensor<1x2x2x3xi32>
// CHECK: %[[SUB_0:.*]] = "tf.Sub"(%[[XLACONVV2_0]], %[[CONST_7]]) : (tensor<1x2x2x3xi32>, tensor<3xi32>) -> tensor<1x2x2x3xi32>
// CHECK: %[[ADDV2_1:.*]] = "tf.AddV2"(%[[SUB_0]], %[[CONST_8]]) : (tensor<1x2x2x3xi32>, tensor<3xi32>) -> tensor<1x2x2x3xi32>
}

