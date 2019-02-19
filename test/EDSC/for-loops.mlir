// RUN: mlir-opt -lower-edsc-test %s | FileCheck %s

// Maps used in dynamic_for below.
// CHECK-DAG: #[[idmap:.*]] = (d0) -> (d0)
// CHECK-DAG: #[[diffmap:.*]] = (d0, d1) -> (d0 - d1)
// CHECK-DAG: #[[addmap:.*]] = (d0, d1) -> (d0 + d1)

// This function will be detected by the test pass that will insert
// EDSC-constructed blocks with arguments.
// CHECK-LABEL: @blocks
func @blocks() {
  return
//CHECK:      ^bb1(%0: i32, %1: i32):	// no predecessors
//CHECK-NEXT:   %2 = addi %0, %1 : i32
//CHECK-NEXT:   return
//CHECK:      ^bb2(%3: i32, %4: i32):	// no predecessors
//CHECK-NEXT:   %5 = subi %3, %4 : i32
//CHECK-NEXT:   return
}

// This function will be detected by the test pass that will insert an
// EDSC-constructed empty `for` loop that corresponds to
//   for %arg0 to %arg1 step 2
// before the `return` instruction.
// CHECK-LABEL: func @dynamic_for_func_args(%arg0: index, %arg1: index) {
// CHECK:  for %i0 = #[[idmap]](%arg0) to #[[idmap]](%arg1) step 3 {
func @dynamic_for_func_args(%arg0 : index, %arg1 : index) {
  return
}

// This function will be detected by the test pass that will insert an
// EDSC-constructed empty `for` loop that corresponds to
//   for (%arg0 - %arg1) to (%arg2 + %arg3) step 2
// before the `return` instruction.
//
// CHECK-LABEL: func @dynamic_for(%arg0: index, %arg1: index, %arg2: index, %arg3: index) {
// CHECK:  %[[step:.*]] = constant 2 : index
// CHECK:  %[[lb:.*]] = affine.apply #[[diffmap]](%arg0, %arg1)
// CHECK:  %[[ub:.*]] = affine.apply #[[addmap]](%arg2, %arg3)
// CHECK:  for %i0 = #[[idmap]](%[[lb]]) to #[[idmap]](%[[ub]]) step 2 {
func @dynamic_for(%arg0 : index, %arg1 : index, %arg2 : index, %arg3 : index) {
  return
}
