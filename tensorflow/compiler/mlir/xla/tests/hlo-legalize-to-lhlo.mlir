// RUN: tf-opt -hlo-legalize-to-lhlo %s -o - | FileCheck %s

// CHECK-LABEL: func @fusion
func @fusion(%multiplier: memref<2x2xf32>, %summand_1: memref<2x2xf32>,
             %summand_2: memref<2x2xf32>, %result: memref<2x2xf32>) {
  // CHECK-NEXT:  %[[ADD_RESULT:.*]] = alloc() {temp = true} : memref<2x2xf32>
  %tensor_summand_1 = tensor_load %summand_1 : memref<2x2xf32>
  %tensor_summand_2 = tensor_load %summand_2 : memref<2x2xf32>
  %sum = "xla_hlo.add"(%tensor_summand_1, %tensor_summand_2) {name = "add"}
      : (tensor<2x2xf32>, tensor<2x2xf32>) -> tensor<2x2xf32>
  // CHECK-NEXT: "xla_lhlo.add"(%{{.*}}, %{{.*}}, %[[ADD_RESULT]])
  %tensor_multiplier = tensor_load %multiplier : memref<2x2xf32>
  %tensor_result = "xla_hlo.mul"(%sum, %tensor_multiplier) {name = "multiply"}
      : (tensor<2x2xf32>, tensor<2x2xf32>) -> tensor<2x2xf32>
  // CHECK-NEXT: "xla_lhlo.mul"(%[[ADD_RESULT]], %{{.*}}, %{{.*}})
  tensor_store %tensor_result, %result : memref<2x2xf32>
  // CHECK-NEXT:  dealloc %[[ADD_RESULT]] : memref<2x2xf32>
  // CHECK-NEXT: "xla_lhlo.terminator"() : () -> ()
  "xla_lhlo.terminator"() : () -> ()
}

// CHECK-LABEL: func @exp
func @exp(%operand: memref<2x2xf32>, %result: memref<2x2xf32>) {
  %tensor_operand = tensor_load %operand : memref<2x2xf32>
  %tensor_result = "xla_hlo.exp"(%tensor_operand) {name = "exp"}
      : (tensor<2x2xf32>) -> tensor<2x2xf32>
  // CHECK-NEXT: "xla_lhlo.exp"(%{{.*}}, %{{.*}})
  tensor_store %tensor_result, %result : memref<2x2xf32>
  "xla_lhlo.terminator"() : () -> ()
}

// CHECK-LABEL: func @select
func @select(%pred: memref<2x2xi1>, %lhs: memref<2x2xf32>,
             %rhs: memref<2x2xf32>, %result: memref<2x2xf32>) {
  %tensor_pred = tensor_load %pred : memref<2x2xi1>
  %tensor_lhs = tensor_load %lhs : memref<2x2xf32>
  %tensor_rhs = tensor_load %rhs : memref<2x2xf32>
  %tensor_result = "xla_hlo.select"(%tensor_pred, %tensor_lhs, %tensor_rhs)
      {name = "select"}
      : (tensor<2x2xi1>, tensor<2x2xf32>, tensor<2x2xf32>) -> tensor<2x2xf32>
  // CHECK-NEXT: "xla_lhlo.select"(%{{.*}}, %{{.*}}, %{{.*}}, %{{.*}})
  tensor_store %tensor_result, %result : memref<2x2xf32>
  "xla_lhlo.terminator"() : () -> ()
}

// CHECK-LABEL: func @compare
func @compare(%lhs: memref<2x2xf32>, %rhs: memref<2x2xf32>, %result: memref<2x2xi1>) {
  %tensor_lhs = tensor_load %lhs : memref<2x2xf32>
  %tensor_rhs = tensor_load %rhs : memref<2x2xf32>
  %tensor_result = "xla_hlo.compare"(%tensor_lhs, %tensor_rhs)
      {comparison_direction = "EQ"}
      : (tensor<2x2xf32>, tensor<2x2xf32>) -> tensor<2x2xi1>
  // CHECK-NEXT: "xla_lhlo.compare"(%{{.*}}, %{{.*}}, %{{.*}}) {comparison_direction = "EQ"}
  tensor_store %tensor_result, %result : memref<2x2xi1>
  "xla_lhlo.terminator"() : () -> ()
}
