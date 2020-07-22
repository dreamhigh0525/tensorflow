// RUN: tf-opt %s -tf-functional-control-flow-to-regions -split-input-file | FileCheck %s

// Simple If
// CHECK: func @testIf1Then{{.+}}
// CHECK: func @testIf1Else{{.+}}
func @testIf1Then(tensor<*xf32>) -> tensor<*xf32>
func @testIf1Else(tensor<*xf32>) -> tensor<*xf32>

// CHECK-LABEL: func @testIf1Result(%arg0: tensor<i1>, %arg1: tensor<*xf32>)
func @testIf1Result(%arg0: tensor<i1>, %arg1: tensor<*xf32>) -> tensor<*xf32> {
  %0 = "tf.If"(%arg0, %arg1) {
    then_branch = @testIf1Then, else_branch = @testIf1Else, is_stateless = false
  } : (tensor<i1>, tensor<*xf32>) -> tensor<*xf32>

  // CHECK: "tf.IfRegion"
  // CHECK-NOT: then_branch
  // CHECK-NOT: else_branch
  // CHECK: [[Result0:%.*]] = call @testIf1Then
  // CHECK: "tf.Yield"([[Result0]])
  // CHECK: [[Result1:%.*]] = call @testIf1Else
  // CHECK: "tf.Yield"([[Result1]])
  return %0 : tensor<*xf32>
}

// -----

// If with mismatching input types

// CHECK: func @testIf1Then{{.+}}
// CHECK: func @testIf1Else{{.+}}
func @testIf1Then(tensor<*xf32>) -> tensor<*xf32>
func @testIf1Else(tensor<*xf32>) -> tensor<*xf32>

// CHECK-LABEL: func @testIf2Result(%arg0: tensor<i1>, %arg1: tensor<2xf32>)
func @testIf2Result(%arg0: tensor<i1>, %arg1: tensor<2xf32>) -> tensor<2xf32> {
  %0 = "tf.If"(%arg0, %arg1) {
    then_branch = @testIf1Then, else_branch = @testIf1Else, is_stateless = false
  } : (tensor<i1>, tensor<2xf32>) -> tensor<2xf32>

  // CHECK: "tf.IfRegion"
  // CHECK: "tf.Cast"
  // CHECK: [[Result0:%.*]] = call @testIf1Then
  // CHECK: "tf.Yield"([[Result0]])
  // CHECK: "tf.Cast"
  // CHECK: [[Result1:%.*]] = call @testIf1Else
  // CHECK: "tf.Yield"([[Result1]])
  return %0 : tensor<2xf32>
}

// -----

// If with no inputs, some outputs
// CHECK: func @testIf1Then{{.+}}
// CHECK: func @testIf1Else{{.+}}
func @testIf1Then() -> tensor<*xf32>
func @testIf1Else() -> tensor<*xf32>

// CHECK-LABEL: func @testIfNoInputs(%arg0: tensor<i1>)
func @testIfNoInputs(%arg0: tensor<i1>) -> tensor<2xf32> {
  %0 = "tf.If"(%arg0) {
    then_branch = @testIf1Then, else_branch = @testIf1Else, is_stateless = false
  } : (tensor<i1>) -> tensor<2xf32>

  // CHECK: "tf.IfRegion"
  // CHECK: [[Result0:%.*]] = call @testIf1Then
  // CHECK: "tf.Yield"([[Result0]])
  // CHECK: [[Result1:%.*]] = call @testIf1Else
  // CHECK: "tf.Yield"([[Result1]])
  return %0 : tensor<2xf32>
}

// -----

// If with no outputs, some inputs
// CHECK: func @testIf1Then{{.+}}
// CHECK: func @testIf1Else{{.+}}
func @testIf1Then(tensor<*xf32>) -> ()
func @testIf1Else(tensor<*xf32>) -> ()

// CHECK-LABEL: func @testIfNoResult(%arg0: tensor<i1>, %arg1: tensor<2xf32>)
func @testIfNoResult(%arg0: tensor<i1>, %arg1: tensor<2xf32>) -> () {
  "tf.If"(%arg0, %arg1) {
    then_branch = @testIf1Then, else_branch = @testIf1Else, is_stateless = false
  } : (tensor<i1>, tensor<2xf32>) -> ()

  // CHECK: "tf.IfRegion"
  // CHECK: "tf.Cast"
  // CHECK: call @testIf1Then
  // CHECK: "tf.Yield"()
  // CHECK: "tf.Cast"
  // CHECK: call @testIf1Else
  // CHECK: "tf.Yield"()
  return
}

// -----

// If with no outputs, No inputs
// CHECK: func @testIf1Then{{.+}}
// CHECK: func @testIf1Else{{.+}}
func @testIf1Then() -> ()
func @testIf1Else() -> ()

// CHECK-LABEL: func @testIfNoInputAndNoResult(%arg0: tensor<i1>)
func @testIfNoInputAndNoResult(%arg0: tensor<i1>) -> () {
  "tf.If"(%arg0) {
    then_branch = @testIf1Then, else_branch = @testIf1Else, is_stateless = false
  } : (tensor<i1>) -> ()

  // CHECK: "tf.IfRegion"
  // CHECK: call @testIf1Then
  // CHECK: "tf.Yield"()
  // CHECK: call @testIf1Else
  // CHECK: "tf.Yield"()
  return
}

// -----

// Simple While
func @testWhileCond(tensor<*xf32>) -> (tensor<i1>)
func @testWhileBody(tensor<*xf32>) -> (tensor<*xf32>)

// CHECK-LABEL: func @testWhileResult
func @testWhileResult(tensor<*xf32>) -> (tensor<*xf32>) {
^bb0(%arg0: tensor<*xf32>):
  %1 = "tf.While"(%arg0) {
    cond = @testWhileCond,
    body = @testWhileBody,
    is_stateless = false
  } : (tensor<*xf32>) -> (tensor<*xf32>)

  // CHECK: [[Result0:%.*]] = "tf.WhileRegion"
  // CHECK-NOT: cond =
  // CHECK-NOT: body =
  // CHECK: [[Result1:%.*]] = call @testWhileCond
  // CHECK: "tf.Yield"([[Result1]])
  // CHECK: [[Result2:%.*]] = call @testWhileBody
  // CHECK: "tf.Yield"([[Result2]])
  // CHECK: return [[Result0]]
  return %1 : tensor<*xf32>
}

// -----

// While with no inputs & outputs
func @testWhileCond() -> (tensor<i1>)
func @testWhileBody() -> ()

// CHECK-LABEL: func @testWhileResultNoIO
func @testWhileResultNoIO() -> () {
  "tf.While"() {
    cond = @testWhileCond,
    body = @testWhileBody,
    is_stateless = false
  } : () -> ()

  // CHECK: "tf.WhileRegion"
  // CHECK: [[Result1:%.*]] = call @testWhileCond
  // CHECK: "tf.Yield"([[Result1]])
  // CHECK: call @testWhileBody
  // CHECK: "tf.Yield"()
  return
}

// -----

// While with type mismatch
func @testWhileCond(tensor<4xf32>) -> (tensor<i1>)
func @testWhileBody(tensor<4xf32>) -> (tensor<4xf32>)

// CHECK-LABEL: func @testWhileResult
func @testWhileResult(tensor<*xf32>) -> (tensor<*xf32>) {
^bb0(%arg0: tensor<*xf32>):
  %1 = "tf.While"(%arg0) {
    cond = @testWhileCond,
    body = @testWhileBody,
    is_stateless = false
  } : (tensor<*xf32>) -> (tensor<*xf32>)

  // CHECK: [[Result0:%.*]] = "tf.WhileRegion"
  // CHECK: [[ResultCast0:%.*]] = "tf.Cast"
  // CHECK: [[Result1:%.*]] = call @testWhileCond([[ResultCast0]])
  // CHECK: "tf.Yield"([[Result1]])
  // CHECK: [[ResultCast1:%.*]] = "tf.Cast"
  // CHECK: [[Result2:%.*]] = call @testWhileBody([[ResultCast1]])
  // CHECK: "tf.Yield"([[Result2]])
  // CHECK: return [[Result0]]
  return %1 : tensor<*xf32>
}

