// RUN: mlir-opt -test-patterns -mlir-print-debuginfo %s | FileCheck %s

// CHECK-LABEL: verifyConstantAttr
func @verifyConstantAttr(%arg0 : i32) -> i32 {
  %0 = "test.op_c"(%arg0) : (i32) -> i32 loc("a")

  // CHECK: "test.op_b"(%arg0) {attr = 17 : i32} : (i32) -> i32 loc("a")
  return %0 : i32
}

// CHECK-LABEL: verifyFusedLocs
func @verifyFusedLocs(%arg0 : i32) -> i32 {
  %0 = "test.op_a"(%arg0) {attr = 10 : i32} : (i32) -> i32 loc("a")
  %result = "test.op_a"(%0) {attr = 20 : i32} : (i32) -> i32 loc("b")

  // CHECK: "test.op_b"(%arg0) {attr = 10 : i32} : (i32) -> i32 loc("a")
  // CHECK: "test.op_b"(%arg0) {attr = 20 : i32} : (i32) -> i32 loc(fused["b", "a"])
  return %result : i32
}

// CHECK-LABEL: verifyBenefit
func @verifyBenefit(%arg0 : i32) -> i32 {
  %0 = "test.op_d"(%arg0) : (i32) -> i32
  %1 = "test.op_g"(%arg0) : (i32) -> i32
  %2 = "test.op_g"(%1) : (i32) -> i32

  // CHECK: "test.op_f"(%arg0)
  // CHECK: "test.op_b"(%arg0) {attr = 34 : i32}
  return %0 : i32
}

// CHECK-LABEL: verifyStrEnumAttr
func @verifyStrEnumAttr() -> i32 {
  // CHECK: "test.str_enum_attr"() {attr = "B"}
  %0 = "test.str_enum_attr"() {attr = "A"} : () -> i32
  return %0 : i32
}

// CHECK-LABEL: verifyI32EnumAttr
func @verifyI32EnumAttr() -> i32 {
  // CHECK: "test.i32_enum_attr"() {attr = 10 : i32}
  %0 = "test.i32_enum_attr"() {attr = 5: i32} : () -> i32
  return %0 : i32
}

// CHECK-LABEL: verifyI64EnumAttr
func @verifyI64EnumAttr() -> i32 {
  // CHECK: "test.i64_enum_attr"() {attr = 10 : i64}
  %0 = "test.i64_enum_attr"() {attr = 5: i64} : () -> i32
  return %0 : i32
}

//===----------------------------------------------------------------------===//
// Test Multi-result Ops
//===----------------------------------------------------------------------===//

// CHECK-LABEL: @useMultiResultOpToReplaceWhole
func @useMultiResultOpToReplaceWhole() -> (i32, f32, f32) {
  // CHECK: %0:3 = "test.another_three_result"()
  // CHECK: return %0#0, %0#1, %0#2
  %0:3 = "test.three_result"() {kind = 1} : () -> (i32, f32, f32)
  return %0#0, %0#1, %0#2 : i32, f32, f32
}

// CHECK-LABEL: @useMultiResultOpToReplacePartial1
func @useMultiResultOpToReplacePartial1() -> (i32, f32, f32) {
  // CHECK: %0:2 = "test.two_result"()
  // CHECK: %1 = "test.one_result"()
  // CHECK: return %0#0, %0#1, %1
  %0:3 = "test.three_result"() {kind = 2} : () -> (i32, f32, f32)
  return %0#0, %0#1, %0#2 : i32, f32, f32
}

// CHECK-LABEL: @useMultiResultOpToReplacePartial2
func @useMultiResultOpToReplacePartial2() -> (i32, f32, f32) {
  // CHECK: %0 = "test.another_one_result"()
  // CHECK: %1:2 = "test.another_two_result"()
  // CHECK: return %0, %1#0, %1#1
  %0:3 = "test.three_result"() {kind = 3} : () -> (i32, f32, f32)
  return %0#0, %0#1, %0#2 : i32, f32, f32
}

// CHECK-LABEL: @useMultiResultOpResultsSeparately
func @useMultiResultOpResultsSeparately() -> (i32, f32, f32) {
  // CHECK: %0:2 = "test.two_result"()
  // CHECK: %1 = "test.one_result"()
  // CHECK: %2:2 = "test.two_result"()
  // CHECK: return %0#0, %1, %2#1
  %0:3 = "test.three_result"() {kind = 4} : () -> (i32, f32, f32)
  return %0#0, %0#1, %0#2 : i32, f32, f32
}
