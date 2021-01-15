// RUN: mlir-hlo-opt %s -hlo-legalize-to-linalg -split-input-file | FILECHECK_OPTS="" FileCheck %s

// CHECK: #map = affine_map<(d0, d1) -> (d0, d1)>
// CHECK-LABEL: func @float_add
func @float_add(%lhs: tensor<2x2xf32>,
                %rhs: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: ^{{[a-z0-9_]*}}
  // CHECK-SAME: %[[ARG0:[a-zA-Z0-9_]*]]: f32
  // CHECK-SAME: %[[ARG1:[a-zA-Z0-9_]*]]: f32
  // CHECK: %[[RESULT:[a-zA-Z0-9_]*]] = addf %[[ARG0]], %[[ARG1]]
  // CHECK: linalg.yield %[[RESULT]]
  %0 = "mhlo.add"(%lhs, %rhs) : (tensor<2x2xf32>,
                                    tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: integer_add
func @integer_add(%lhs: tensor<2x2xi32>,
                  %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  // CHECK: linalg.generic
  // CHECK: addi
  %0 = "mhlo.add"(%lhs, %rhs) : (tensor<2x2xi32>,
                                    tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: complex_add
func @complex_add(%lhs: tensor<2x2xcomplex<f32>>,
                  %rhs: tensor<2x2xcomplex<f32>>) -> tensor<2x2xcomplex<f32>> {
  // CHECK: linalg.generic
  // CHECK: addcf
  %0 = "mhlo.add"(%lhs, %rhs) : (tensor<2x2xcomplex<f32>>,
      tensor<2x2xcomplex<f32>>) -> tensor<2x2xcomplex<f32>>
  return %0 : tensor<2x2xcomplex<f32>>
}

// -----

// CHECK-LABEL: func @float_mul
func @float_mul(%lhs: tensor<2x2xf32>,
                %rhs: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: mulf
  %0 = "mhlo.multiply"(%lhs, %rhs) : (tensor<2x2xf32>,
                                    tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @integer_mul
func @integer_mul(%lhs: tensor<2x2xi32>,
                  %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  // CHECK: linalg.generic
  // CHECK: muli
  %0 = "mhlo.multiply"(%lhs, %rhs) : (tensor<2x2xi32>,
                                    tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: func @float_remainder
func @float_remainder(%lhs: tensor<2x2xf32>,
                      %rhs: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: remf
  %0 = "mhlo.remainder"(%lhs, %rhs) : (tensor<2x2xf32>,
                                    tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @integer_remainder
func @integer_remainder(%lhs: tensor<2x2xi32>,
                        %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  // CHECK: linalg.generic
  // CHECK: remi_signed
  %0 = "mhlo.remainder"(%lhs, %rhs) : (tensor<2x2xi32>,
                                          tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: func @float_rsqrt
func @float_rsqrt(%operand: tensor<2x2xf32>) -> tensor<2x2xf32> {
  %tensor_result = "mhlo.rsqrt"(%operand)
      : (tensor<2x2xf32>) -> tensor<2x2xf32>
  // CHECK: linalg.generic
  // CHECK: rsqrt
  return %tensor_result : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @float_sub
func @float_sub(%lhs: tensor<2x2xf32>,
                %rhs: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: subf
  %0 = "mhlo.subtract"(%lhs, %rhs) : (tensor<2x2xf32>,
                                    tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @integer_sub
func @integer_sub(%lhs: tensor<2x2xi32>,
                  %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  // CHECK: linalg.generic
  // CHECK: subi
  %0 = "mhlo.subtract"(%lhs, %rhs) : (tensor<2x2xi32>,
                                    tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: complex_sub
func @complex_sub(%lhs: tensor<2x2xcomplex<f32>>,
                  %rhs: tensor<2x2xcomplex<f32>>) -> tensor<2x2xcomplex<f32>> {
  // CHECK: linalg.generic
  // CHECK: subcf
  %0 = "mhlo.subtract"(%lhs, %rhs) : (tensor<2x2xcomplex<f32>>,
      tensor<2x2xcomplex<f32>>) -> tensor<2x2xcomplex<f32>>
  return %0 : tensor<2x2xcomplex<f32>>
}

// -----

// CHECK-LABEL: func @float_abs
func @float_abs(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: absf
  %0 = "mhlo.abs"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @float_exp
func @float_exp(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: exp
  %0 = "mhlo.exponential"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @float_log
func @float_log(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: log
  %0 = "mhlo.log"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @float_ceil
func @float_ceil(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: ceilf
  %0 = "mhlo.ceil"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @floor
func @floor(%input: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: floorf
  %0 = "mhlo.floor"(%input) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @float_neg
func @float_neg(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: negf
  %0 = "mhlo.negate"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @float_tanh
func @float_tanh(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: tanh
  %0 = "mhlo.tanh"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @integer_and
func @integer_and(%lhs: tensor<2x2xi32>,
                  %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  // CHECK: linalg.generic
  // CHECK: and
  %0 = "mhlo.and"(%lhs, %rhs) : (tensor<2x2xi32>,
                                    tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: func @integer_or
func @integer_or(%lhs: tensor<2x2xi32>,
                  %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  // CHECK: linalg.generic
  // CHECK: or
  %0 = "mhlo.or"(%lhs, %rhs) : (tensor<2x2xi32>,
                                    tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: func @integer_xor
func @integer_xor(%lhs: tensor<2x2xi32>,
                  %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  // CHECK: linalg.generic
  // CHECK: xor
  %0 = "mhlo.xor"(%lhs, %rhs) : (tensor<2x2xi32>,
                                    tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-LABEL: func @float_cmp
func @float_cmp(%lhs: tensor<2x2xf32>,
                %rhs: tensor<2x2xf32>) -> (tensor<2x2xi1>) {
  %0 = "mhlo.compare"(%lhs, %rhs) {comparison_direction = "EQ"}
          : (tensor<2x2xf32>, tensor<2x2xf32>) -> tensor<2x2xi1>
  return %0 : tensor<2x2xi1>
}
// CHECK: linalg.init_tensor [2, 2] : tensor<2x2xi1>
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS_IN:.*]]: f32, %[[RHS_IN:.*]]: f32, %{{.*}}: i1):
// CHECK-NEXT:   %[[RESULT:.*]] = cmpf oeq, %[[LHS_IN]], %[[RHS_IN]] : f32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i1

// -----

// CHECK-LABEL: func @float_cmp_ne
func @float_cmp_ne(%lhs: tensor<2x2xf32>,
                %rhs: tensor<2x2xf32>) -> (tensor<2x2xi1>) {
  %0 = "mhlo.compare"(%lhs, %rhs) {comparison_direction = "NE"}
          : (tensor<2x2xf32>, tensor<2x2xf32>) -> tensor<2x2xi1>
  return %0 : tensor<2x2xi1>
}
// CHECK: linalg.init_tensor [2, 2] : tensor<2x2xi1>
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS_IN:.*]]: f32, %[[RHS_IN:.*]]: f32, %{{.*}}: i1):
// CHECK-NEXT:   %[[RESULT:.*]] = cmpf une, %[[LHS_IN]], %[[RHS_IN]] : f32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i1

// -----

// CHECK-LABEL: func @int_cmp
func @int_cmp(%lhs: tensor<2x2xi32>,
              %rhs: tensor<2x2xi32>) -> tensor<2x2xi1> {
  %0 = "mhlo.compare"(%lhs, %rhs) {comparison_direction = "LT"}
          : (tensor<2x2xi32>, tensor<2x2xi32>) -> (tensor<2x2xi1>)
  return %0 : tensor<2x2xi1>
}
// CHECK: linalg.init_tensor [2, 2] : tensor<2x2xi1>
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS_IN:.*]]: i32, %[[RHS_IN:.*]]: i32, %{{.*}}: i1):
// CHECK-NEXT:   %[[RESULT:.*]] = cmpi slt, %[[LHS_IN]], %[[RHS_IN]] : i32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i1

// -----

// CHECK-LABEL: func @float_cos
func @float_cos(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: cos
  %0 = "mhlo.cosine"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @float_sin
func @float_sin(%arg0: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: sin
  %0 = "mhlo.sine"(%arg0) : (tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK-LABEL: func @copy
// CHECK-SAME: [[ARG:%[a-zA-Z0-9]+]]
func @copy(%input: tensor<2x4x8xf32>) -> tensor<2x4x8xf32> {
  %0 = "mhlo.copy"(%input) : (tensor<2x4x8xf32>) -> (tensor<2x4x8xf32>)
  return %0 : tensor<2x4x8xf32>
}
// CHECK: return [[ARG]] : tensor<2x4x8xf32>

// -----

// CHECK-LABEL: func @is_finte
func @is_finte(%input: tensor<2x2xf32>) -> tensor<2x2xi1> {
  %0 = "mhlo.is_finite"(%input) : (tensor<2x2xf32>) -> tensor<2x2xi1>
  return %0 : tensor<2x2xi1>
}
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[OPERAND_IN:.*]]: f32
// CHECK-NEXT:   %[[POS_INF:.+]] = constant 0x7F800000 : f32
// CHECK-NEXT:   %[[ABS_X:.+]] = absf %[[OPERAND_IN]] : f32
// CHECK-NEXT:   %[[RESULT:.+]] = cmpf one, %[[ABS_X]], %[[POS_INF]] : f32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i1

// -----

// CHECK-LABEL: func @select
func @select(%pred: tensor<2x2xi1>, %lhs: tensor<2x2xf32>,
             %rhs: tensor<2x2xf32>) -> tensor<2x2xf32> {
  %0 = "mhlo.select"(%pred, %lhs, %rhs)
         : (tensor<2x2xi1>, tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
  return %0 : tensor<2x2xf32>
}
// CHECK: linalg.init_tensor [2, 2] : tensor<2x2xf32>
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[PRED_IN:.*]]: i1, %[[LHS_IN:.*]]: f32, %[[RHS_IN:.*]]: f32, %{{.*}}: f32):
// CHECK-NEXT:   %[[RESULT:.*]] = select %[[PRED_IN]], %[[LHS_IN]], %[[RHS_IN]] : f32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : f32

// -----

// CHECK-DAG: #[[OPERAND_MAP:.+]] = affine_map<(d0, d1, d2) -> ()>
// CHECK-DAG: #[[RESULT_MAP:.+]] = affine_map<(d0, d1, d2) -> (d0, d1, d2)>
// CHECK-LABEL: func @broadcast_scalar
func @broadcast_scalar(%arg: tensor<f32>) -> tensor<4x2x1xf32> {
  %0 = "mhlo.broadcast"(%arg) {broadcast_sizes = dense<[4, 2, 1]> : tensor<3xi64>} : (tensor<f32>) -> tensor<4x2x1xf32>
  return %0: tensor<4x2x1xf32>
}
// CHECK: linalg.init_tensor [4, 2, 1] : tensor<4x2x1xf32>
// CHECK: linalg.generic {{{.*}}indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]
// CHECK-NEXT: ^bb0(%[[OPERAND:.*]]: f32, %{{.*}}: f32):
// CHECK-NEXT:   linalg.yield %[[OPERAND]] : f32

// -----

// CHECK-DAG: #[[OPERAND_MAP:.+]] = affine_map<(d0, d1, d2, d3, d4, d5) -> (d3, d4, d5)>
// CHECK-DAG: #[[RESULT_MAP:.+]] = affine_map<(d0, d1, d2, d3, d4, d5) -> (d0, d1, d2, d3, d4, d5)>
// CHECK-LABEL: func @broadcast
func @broadcast(%arg: tensor<4x?x16xf32>) -> tensor<4x2x1x4x?x16xf32> {
  %0 = "mhlo.broadcast"(%arg) {broadcast_sizes = dense<[4, 2, 1]> : tensor<3xi64>} : (tensor<4x?x16xf32>) -> tensor<4x2x1x4x?x16xf32>
  return %0: tensor<4x2x1x4x?x16xf32>
}
// CHECK: %[[C1:.*]] = constant 1 : index
// CHECK: %[[D1:.*]] = dim %{{.*}}, %[[C1]] : tensor<4x?x16xf32>
// CHECK: linalg.init_tensor [4, 2, 1, 4, %[[D1]], 16] : tensor<4x2x1x4x?x16xf32>
// CHECK: linalg.generic {{{.*}}indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]
// CHECK-NEXT: ^bb0(%[[OPERAND:.*]]: f32, %{{.*}}: f32):
// CHECK-NEXT:   linalg.yield %[[OPERAND]] : f32

// -----

// CHECK-DAG: #[[OPERAND_MAP:.*]] = affine_map<(d0, d1, d2, d3, d4) -> (d4, d0, 0)>
// CHECK-DAG: #[[RESULT_MAP:.*]] = affine_map<(d0, d1, d2, d3, d4) -> (d0, d1, d2, d3, d4)>
// CHECK-LABEL: func @broadcast_in_dim
func @broadcast_in_dim(%operand: tensor<5x7x1xf32>) -> tensor<7x10x6x4x5xf32> {
  %0 = "mhlo.broadcast_in_dim"(%operand)
         {broadcast_dimensions = dense<[4,0,2]> : tensor<3xi64>}
         : (tensor<5x7x1xf32>) -> tensor<7x10x6x4x5xf32>
  return %0 : tensor<7x10x6x4x5xf32>
}
// CHECK: linalg.init_tensor [7, 10, 6, 4, 5] : tensor<7x10x6x4x5xf32>
// CHECK: linalg.generic {{{.*}}indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]
// CHECK-NEXT: ^bb0(%[[OPERAND:.*]]: f32, %{{.*}}: f32):
// CHECK-NEXT:   linalg.yield %[[OPERAND]] : f32

// -----

// CHECK-DAG: #[[OPERAND_MAP:.+]] = affine_map<(d0, d1) -> (d0)>
// CHECK-DAG: #[[RESULT_MAP:.+]] = affine_map<(d0, d1) -> (d0, d1)>
// CHECK-LABEL: func @broadcast_in_dim_with_one_to_one
func @broadcast_in_dim_with_one_to_one(
         %operand: tensor<1xf32>) -> tensor<1x5xf32> {
  %0 = "mhlo.broadcast_in_dim"(%operand)
         {broadcast_dimensions = dense<[0]> : tensor<1xi64>}
         : (tensor<1xf32>) -> tensor<1x5xf32>
  return %0 : tensor<1x5xf32>
}
// CHECK: linalg.init_tensor [1, 5] : tensor<1x5xf32>
// CHECK: linalg.generic {{{.*}}indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]
// CHECK-NEXT: ^bb0(%[[OPERAND:.*]]: f32, %{{.*}}: f32):
// CHECK-NEXT:   linalg.yield %[[OPERAND]] : f32

// -----

// CHECK-DAG: #[[OPERAND_MAP:.*]] = affine_map<(d0, d1, d2) -> ()>
// CHECK-DAG: #[[RESULT_MAP:.*]] = affine_map<(d0, d1, d2) -> (d0, d1, d2)>
// CHECK-LABEL: func @broadcast_scalar
func @broadcast_scalar(%operand: tensor<f32>) -> tensor<7x10x6xf32> {
  %0 = "mhlo.broadcast_in_dim"(%operand)
        {broadcast_dimensions = dense<[]> : tensor<0xi64>}
        : (tensor<f32>) -> tensor<7x10x6xf32>
  return %0 : tensor<7x10x6xf32>
}
// CHECK: linalg.init_tensor [7, 10, 6] : tensor<7x10x6xf32>
// CHECK: linalg.generic {{{.*}}indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]
// CHECK-NEXT: ^bb0(%[[OPERAND:.*]]: f32, %{{.*}}: f32):
// CHECK-NEXT:   linalg.yield %[[OPERAND]] : f32

// -----

// CHECK-DAG: #[[OPERAND_MAP:.*]] = affine_map<(d0, d1, d2, d3) -> (d1, d0, d3, d2)>
// CHECK-DAG: #[[RESULT_MAP:.*]] = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
// CHECK-LABEL: func @transpose
func @transpose(%arg0: tensor<2x3x9x5xi32>) -> tensor<3x2x5x9xi32> {
  %0 = "mhlo.transpose"(%arg0) {permutation = dense<[1, 0, 3, 2]> : tensor<4xi64>}
        : (tensor<2x3x9x5xi32>) -> tensor<3x2x5x9xi32>
  return %0 : tensor<3x2x5x9xi32>
}
// CHECK: linalg.generic {{{.*}}indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]

// -----

// CHECK-DAG: #[[RESHAPE_MAP1:.*]] = affine_map<(d0, d1, d2) -> (d0, d1)>
// CHECK-DAG: #[[RESHAPE_MAP2:.*]] = affine_map<(d0, d1, d2) -> (d2)>
// CHECK-LABEL: func @reshape_3D_2D
func @reshape_3D_2D(%arg0: tensor<12x1x42xi32>) -> tensor<12x42xi32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<12x1x42xi32>) -> tensor<12x42xi32>
  return %0 : tensor<12x42xi32>
}
// CHECK: linalg.tensor_reshape %{{.*}} [#[[RESHAPE_MAP1]], #[[RESHAPE_MAP2]]]

// -----

// CHECK-DAG: #[[RESHAPE_MAP1:.*]] = affine_map<(d0, d1, d2, d3) -> (d0)>
// CHECK-DAG: #[[RESHAPE_MAP2:.*]] = affine_map<(d0, d1, d2, d3) -> (d1, d2, d3)>
// CHECK-LABEL: func @reshape_4D_2D
func @reshape_4D_2D(%arg0: tensor<12x42x1x1xi32>) -> tensor<12x42xi32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<12x42x1x1xi32>) -> tensor<12x42xi32>
  return %0 : tensor<12x42xi32>
}
// CHECK: linalg.tensor_reshape %{{.*}} [#[[RESHAPE_MAP1]], #[[RESHAPE_MAP2]]]

// -----

// CHECK-DAG: #[[RESHAPE_MAP1:.*]] = affine_map<(d0, d1, d2, d3) -> (d0, d1)>
// CHECK-DAG: #[[RESHAPE_MAP2:.*]] = affine_map<(d0, d1, d2, d3) -> (d2, d3)>
// CHECK-LABEL: func @reshape_2D_4D
func @reshape_2D_4D(%arg0: tensor<12x42xi32>) -> tensor<12x1x42x1xi32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<12x42xi32>) -> tensor<12x1x42x1xi32>
  return %0 : tensor<12x1x42x1xi32>
}
// CHECK: linalg.tensor_reshape %{{.*}} [#[[RESHAPE_MAP1]], #[[RESHAPE_MAP2]]]

// -----

// CHECK-DAG: #[[RESHAPE_MAP1:.*]] = affine_map<(d0, d1, d2) -> (d0, d1, d2)>
// CHECK-DAG: #[[RESHAPE_MAP2:.*]] = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
// CHECK-LABEL: func @reshape_3D_4D
func @reshape_3D_4D(%arg0: tensor<1x49x16xf32>) -> tensor<1x784x1x1xf32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<1x49x16xf32>) -> tensor<1x784x1x1xf32>
  return %0 : tensor<1x784x1x1xf32>
}
// CHECK: linalg.tensor_reshape %{{.*}} [#[[RESHAPE_MAP1]]]
// CHECK: linalg.tensor_reshape %{{.*}} [#[[RESHAPE_MAP2]]]

// -----

// CHECK-DAG: #[[MAP:.*]] = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
// CHECK-LABEL: func @reshape1_4D_4D
func @reshape1_4D_4D(%arg0: tensor<4x512x1x1xi32>) -> tensor<1x4x1x512xi32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<4x512x1x1xi32>) -> tensor<1x4x1x512xi32>
  return %0 : tensor<1x4x1x512xi32>
}
// CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP]]]
// CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP]]]

// -----

// CHECK-DAG: #[[MAP:.*]] = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
// CHECK-LABEL: func @reshape2_4D_4D
func @reshape2_4D_4D(%arg0: tensor<4x1x1x1024xi32>) -> tensor<4x1024x1x1xi32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<4x1x1x1024xi32>) -> tensor<4x1024x1x1xi32>
  return %0 : tensor<4x1024x1x1xi32>
}
// CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP]]]
// CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP]]]

// -----

// CHECK-LABEL: func @minf
func @minf(%lhs: tensor<2x2xf32>, %rhs: tensor<2x2xf32>) -> tensor<2x2xf32> {
  %0 = "mhlo.minimum"(%lhs, %rhs)
          : (tensor<2x2xf32>, tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}
// CHECK: linalg.init_tensor [2, 2] : tensor<2x2xf32>
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS_IN:.*]]: f32, %[[RHS_IN:.*]]: f32, %{{.*}}: f32):
// CHECK-NEXT:   %[[CMP:.*]] = cmpf olt, %[[LHS_IN]], %[[RHS_IN]] : f32
// CHECK-NEXT:   %[[RESULT:.*]] = select %[[CMP]], %[[LHS_IN]], %[[RHS_IN]] : f32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : f32

// -----

// CHECK-LABEL: func @maxi
func @maxi(%lhs: tensor<2x2xi32>, %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  %0 = "mhlo.maximum"(%lhs, %rhs)
          : (tensor<2x2xi32>, tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}
// CHECK: linalg.init_tensor [2, 2] : tensor<2x2xi32>
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS_IN:.*]]: i32, %[[RHS_IN:.*]]: i32, %{{.*}}: i32):
// CHECK-NEXT:   %[[CMP:.*]] = cmpi sgt, %[[LHS_IN]], %[[RHS_IN]] : i32
// CHECK-NEXT:   %[[RESULT:.*]] = select %[[CMP]], %[[LHS_IN]], %[[RHS_IN]] : i32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i32

// -----

// CHECK-DAG: #[[MAP:.*]] = affine_map<() -> ()>
// CHECK-LABEL: func @add_scalar
func @add_scalar(%lhs: tensor<f32>, %rhs: tensor<f32>) -> tensor<f32> {
  %0 = "mhlo.add"(%lhs, %rhs) : (tensor<f32>, tensor<f32>) -> tensor<f32>
  return %0 : tensor<f32>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-SAME: indexing_maps = [#[[MAP]], #[[MAP]], #[[MAP]]]
// CHECK-NEXT: ^bb0(%[[LHS:.*]]: f32, %[[RHS:.*]]: f32, %{{.*}}: f32):
// CHECK: %[[RESULT:.*]] = addf %[[LHS]], %[[RHS]]
// CHECK-NEXT:   linalg.yield %[[RESULT]] : f32

// -----

func @reshape_collapse_single_dim
  (%arg0: tensor<1x28x28x1xf32>) -> tensor<1x784xf32> {
  %0 = "mhlo.reshape"(%arg0) : (tensor<1x28x28x1xf32>) -> tensor<1x784xf32>
  return %0 : tensor<1x784xf32>
}
//   CHECK-DAG: #[[MAP0:.*]] = affine_map<(d0, d1, d2, d3) -> (d0)>
//   CHECK-DAG: #[[MAP1:.*]] = affine_map<(d0, d1, d2, d3) -> (d1, d2, d3)>
// CHECK-LABEL: func @reshape_collapse_single_dim
//       CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP0]], #[[MAP1]]]

// -----

func @reshape_collapse(%arg0: tensor<2x2x2x3xf32>) -> tensor<2x4x3xf32> {
    %0 = "mhlo.reshape"(%arg0) : (tensor<2x2x2x3xf32>) -> tensor<2x4x3xf32>
    return %0 : tensor<2x4x3xf32>
}
//   CHECK-DAG: #[[MAP0:.*]] = affine_map<(d0, d1, d2, d3) -> (d0)>
//   CHECK-DAG: #[[MAP1:.*]] = affine_map<(d0, d1, d2, d3) -> (d1, d2)>
//   CHECK-DAG: #[[MAP2:.*]] = affine_map<(d0, d1, d2, d3) -> (d3)>
// CHECK-LABEL: func @reshape_collapse
//       CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP0]], #[[MAP1]], #[[MAP2]]]

// -----

func @reshape_expand(%arg0: tensor<2x8xf32>) -> tensor<2x4x2xf32> {
    %0 = "mhlo.reshape"(%arg0) : (tensor<2x8xf32>) -> tensor<2x4x2xf32>
    return %0 : tensor<2x4x2xf32>
}
//   CHECK-DAG: #[[MAP0:.*]] = affine_map<(d0, d1, d2) -> (d0)>
//   CHECK-DAG: #[[MAP1:.*]] = affine_map<(d0, d1, d2) -> (d1, d2)>
// CHECK-LABEL: func @reshape_expand
//       CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP0]], #[[MAP1]]]

// -----

func @reshape_single_expand(%arg0 : tensor<8xf32>) -> tensor<1x4x2xf32> {
    %0 = "mhlo.reshape"(%arg0) : (tensor<8xf32>) -> tensor<1x4x2xf32>
    return %0 : tensor<1x4x2xf32>
}
//       CHECK: #[[MAP0:.*]] = affine_map<(d0, d1, d2) -> (d0, d1, d2)>
// CHECK-LABEL: func @reshape_single_expand
//       CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP0]]]

// -----

func @reshape_multiple_collapse
  (%arg0 : tensor<1x2x2x5x3x2xf32>) -> tensor<1x4x5x6xf32> {
    %0 = "mhlo.reshape"(%arg0) : (tensor<1x2x2x5x3x2xf32>) -> tensor<1x4x5x6xf32>
    return %0 : tensor<1x4x5x6xf32>
}
//   CHECK-DAG: #[[MAP0:.*]] = affine_map<(d0, d1, d2, d3, d4, d5) -> (d0)>
//   CHECK-DAG: #[[MAP1:.*]] = affine_map<(d0, d1, d2, d3, d4, d5) -> (d1, d2)>
//   CHECK-DAG: #[[MAP2:.*]] = affine_map<(d0, d1, d2, d3, d4, d5) -> (d3)>
//   CHECK-DAG: #[[MAP3:.*]] = affine_map<(d0, d1, d2, d3, d4, d5) -> (d4, d5)>
// CHECK-LABEL: func @reshape_multiple_collapse
//       CHECK: linalg.tensor_reshape %{{.*}} [#[[MAP0]], #[[MAP1]], #[[MAP2]], #[[MAP3]]]

// -----

// CHECK-LABEL: func @convert_i32_to_f32
func @convert_i32_to_f32(%input: tensor<2x2xi32>) -> tensor<2x2xf32> {
  %result = "mhlo.convert"(%input) : (tensor<2x2xi32>) -> tensor<2x2xf32>
  return %result : tensor<2x2xf32>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[OPERAND_IN:.*]]: i32, %{{.*}}: f32):
// CHECK-NEXT:   %[[RESULT:.*]] = sitofp %[[OPERAND_IN]] : i32 to f32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : f32

// -----

// CHECK-LABEL: func @convert_i16_to_i32
func @convert_i16_to_i32(%input: tensor<2x2xi16>) -> tensor<2x2xi32> {
  %result = "mhlo.convert"(%input) : (tensor<2x2xi16>) -> tensor<2x2xi32>
  return %result : tensor<2x2xi32>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[OPERAND_IN:.*]]: i16, %{{.*}}: i32):
// CHECK-NEXT:   %[[RESULT:.*]] = zexti %[[OPERAND_IN]] : i16 to i32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i32

// -----

// CHECK-LABEL: func @convert_i32_to_i16
func @convert_i32_to_i16(%input: tensor<2x2xi32>) -> tensor<2x2xi16> {
  %result = "mhlo.convert"(%input) : (tensor<2x2xi32>) -> tensor<2x2xi16>
  return %result : tensor<2x2xi16>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[OPERAND_IN:.*]]: i32, %{{.*}}: i16):
// CHECK-NEXT:   %[[RESULT:.*]] = trunci %[[OPERAND_IN]] : i32 to i16
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i16

// -----

// CHECK-LABEL: func @convert_f32_to_f64
func @convert_f32_to_f64(%input: tensor<2x2xf32>) -> tensor<2x2xf64> {
  %result = "mhlo.convert"(%input) : (tensor<2x2xf32>) -> tensor<2x2xf64>
  return %result : tensor<2x2xf64>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[OPERAND_IN:.*]]: f32, %{{.*}}: f64):
// CHECK-NEXT:   %[[RESULT:.*]] = fpext %[[OPERAND_IN]] : f32 to f64
// CHECK-NEXT:   linalg.yield %[[RESULT]] : f64

// -----

// CHECK-LABEL: func @convert_f64_to_f32
func @convert_f64_to_f32(%input: tensor<2x2xf64>) -> tensor<2x2xf32> {
  %result = "mhlo.convert"(%input) : (tensor<2x2xf64>) -> tensor<2x2xf32>
  return %result : tensor<2x2xf32>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[OPERAND_IN:.*]]: f64, %{{.*}}: f32):
// CHECK-NEXT:   %[[RESULT:.*]] = fptrunc %[[OPERAND_IN]] : f64 to f32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : f32

// -----

// CHECK-LABEL: func @convert_f32_to_i32
func @convert_f32_to_i32(%input: tensor<2x2xf32>) -> tensor<2x2xi32> {
  %result = "mhlo.convert"(%input) : (tensor<2x2xf32>) -> tensor<2x2xi32>
  return %result : tensor<2x2xi32>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[OPERAND_IN:.*]]: f32, %{{.*}}: i32):
// CHECK-NEXT:   %[[RESULT:.*]] = fptosi %[[OPERAND_IN]] : f32 to i32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i32

// -----

// CHECK-DAG: #[[OPERAND_MAP:.*]] = affine_map<(d0, d1) -> (d0, -d1 + 2)>
// CHECK-DAG: #[[RESULT_MAP:.*]] = affine_map<(d0, d1) -> (d0, d1)>
// CHECK-LABEL: func @reverse
func @reverse(%input: tensor<2x3xf32>) -> tensor<2x3xf32> {
  %result = "mhlo.reverse"(%input) {
    dimensions = dense<1> : tensor<1xi64>
  } : (tensor<2x3xf32>) -> tensor<2x3xf32>
  return %result : tensor<2x3xf32>
}
// CHECK: linalg.generic
// CHECK-SAME: indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]

// -----

// CHECK: #[[RESULT_MAP:.*]] = affine_map<(d0, d1) -> (d0, d1)>
// CHECK-LABEL: func @iota
func @iota() -> tensor<7x10xf32> {
  %result = "mhlo.iota"() {iota_dimension = 1 : i64} : () -> (tensor<7x10xf32>)
  return %result : tensor<7x10xf32>
}
// CHECK: linalg.init_tensor
// CHECK: linalg.indexed_generic
// CHECK-SAME: indexing_maps = [#[[RESULT_MAP]]]
// CHECK-NEXT: ^bb0(%[[D0:.*]]: index, %[[D1:.*]]: index, %{{.*}}: f32):
// CHECK-NEXT:   %[[INT_CAST:.*]] = index_cast %[[D1]] : index to i32
// CHECK-NEXT:   %[[FLOAT_CAST:.*]] = sitofp %[[INT_CAST]] : i32 to f32
// CHECK-NEXT:   linalg.yield %[[FLOAT_CAST]] : f32

// -----

func @shift_left(%lhs: tensor<2x2xi32>,
                 %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  %result = "mhlo.shift_left"(%lhs, %rhs)
      : (tensor<2x2xi32>, tensor<2x2xi32>) -> tensor<2x2xi32>
  return %result : tensor<2x2xi32>
}
// CHECK-LABEL: func @shift_left
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS:.*]]: i32, %[[RHS:.*]]: i32, %{{.*}}: i32):
// CHECK-NEXT:   %[[RESULT:.*]] = shift_left %[[LHS]], %[[RHS]] : i32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i32

// -----

func @shift_right_arithmetic(%lhs: tensor<2x2xi32>,
                             %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  %result = "mhlo.shift_right_arithmetic"(%lhs, %rhs)
      : (tensor<2x2xi32>, tensor<2x2xi32>) -> tensor<2x2xi32>
  return %result : tensor<2x2xi32>
}
// CHECK-LABEL: func @shift_right_arithmetic
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS:.*]]: i32, %[[RHS:.*]]: i32, %{{.*}}: i32):
// CHECK-NEXT:   %[[RESULT:.*]] = shift_right_signed %[[LHS]], %[[RHS]] : i32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i32

// -----

func @shift_right_logical(%lhs: tensor<2x2xi32>,
                          %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
  %result = "mhlo.shift_right_logical"(%lhs, %rhs)
      : (tensor<2x2xi32>, tensor<2x2xi32>) -> tensor<2x2xi32>
  return %result : tensor<2x2xi32>
}
// CHECK-LABEL: func @shift_right_logical
// CHECK: linalg.init_tensor
// CHECK: linalg.generic
// CHECK-NEXT: ^bb0(%[[LHS:.*]]: i32, %[[RHS:.*]]: i32, %{{.*}}: i32):
// CHECK-NEXT:   %[[RESULT:.*]] = shift_right_unsigned %[[LHS]], %[[RHS]] : i32
// CHECK-NEXT:   linalg.yield %[[RESULT]] : i32

// -----

// CHECK-LABEL: func @constant
func @constant() {
  %result = "mhlo.constant"() {
    value = dense<10> : tensor<i32>
  } : () -> (tensor<i32>)
  return
}
// CHECK: %[[CONSTANT:.*]] = constant dense<10> : tensor<i32>

// -----

// CHECK: #map = affine_map<(d0, d1) -> (d0, d1)>
// CHECK-LABEL: func @float_pow
func @float_pow(%lhs: tensor<2x2xf32>,
                %rhs: tensor<2x2xf32>) -> tensor<2x2xf32> {
  // CHECK: linalg.generic
  // CHECK: ^{{[a-z0-9_]*}}
  // CHECK-SAME: %[[ARG0:[a-zA-Z0-9_]*]]: f32
  // CHECK-SAME: %[[ARG1:[a-zA-Z0-9_]*]]: f32
  // CHECK: %[[RESULT:[a-zA-Z0-9_]*]] = powf %[[ARG0]], %[[ARG1]]
  // CHECK: linalg.yield %[[RESULT]]
  %0 = "mhlo.power"(%lhs, %rhs) : (tensor<2x2xf32>,
                                   tensor<2x2xf32>) -> tensor<2x2xf32>
  return %0 : tensor<2x2xf32>
}

// -----

// CHECK: #map = affine_map<(d0, d1) -> (d0, d1)>
// CHECK-LABEL: func @integer_pow
func @integer_pow(%lhs: tensor<2x2xi32>,
                %rhs: tensor<2x2xi32>) -> tensor<2x2xi32> {
                    // CHECK: linalg.generic
  // CHECK: ^{{[a-z0-9_]*}}
  // CHECK-SAME: %[[ARG0:[a-zA-Z0-9_]*]]: i32
  // CHECK-SAME: %[[ARG1:[a-zA-Z0-9_]*]]: i32
  // CHECK: %[[UPPER:.*]] = index_cast %[[ARG1]]
  // CHECK: %[[RESULT:.*]] = scf.for {{.*}} to %[[UPPER]]
  // CHECK-SAME: step %c1{{[a-zA-Z0-9_]*}}
  // CHECK-SAME: iter_args(%[[ITER:.*]] = %c1{{.*}}) -> (i32) {
  //   CHECK: %[[ACCUM:[a-zA-Z0-9_]*]] = muli %[[ARG0]], %[[ITER]]
  //   CHECK: scf.yield %[[ACCUM]]
  // CHECK: linalg.yield %[[RESULT]]
  %0 = "mhlo.power"(%lhs, %rhs) : (tensor<2x2xi32>,
                                   tensor<2x2xi32>) -> tensor<2x2xi32>
  return %0 : tensor<2x2xi32>
}

// -----

// CHECK-DAG: #[[OPERAND_MAP:.*]] = affine_map<(d0) -> ()>
// CHECK-DAG: #[[RESULT_MAP:.*]] = affine_map<(d0) -> (d0)>

// CHECK-LABEL: func @dynamic_broadcast_in_dim(
// CHECK-SAME: [[SHAPE:%.*]]: tensor<1xindex>
func @dynamic_broadcast_in_dim(%shape: tensor<1xindex>) -> tensor<?xf32> {
  %cst = mhlo.constant dense<0x7F800000> : tensor<f32>
  %result = "mhlo.dynamic_broadcast_in_dim"(%cst, %shape) {
     broadcast_dimensions = dense<> : tensor<0xi64>
  } : (tensor<f32>, tensor<1xindex>) -> tensor<?xf32>
  return %result : tensor<?xf32>
}
// CHECK: [[CST:%.*]] = constant
// CHECK: [[INIT:%.*]] = linalg.init_tensor
// CHECK: linalg.generic
// CHECK-SAME: indexing_maps = [#[[OPERAND_MAP]], #[[RESULT_MAP]]]
// CHECK-SAME: ins([[CST]] : tensor<f32>) outs([[INIT]] : tensor<?xf32>)
// CHECK-NEXT: ^bb0(%[[OPERAND:.*]]: f32, %[[RESULT:.*]]: f32):
// CHECK-NEXT:   linalg.yield %[[OPERAND]] : f32

// -----

func @dot_matmul(%arg0: tensor<2x3xf32>,
                 %arg1: tensor<3x?xf32>) -> tensor<2x?xf32> {
  %0 = "mhlo.dot"(%arg0, %arg1) : (tensor<2x3xf32>,
                                   tensor<3x?xf32>) -> tensor<2x?xf32>
  return %0 : tensor<2x?xf32>
}
// CHECK: func @dot_matmul(%[[ARG0:.*]]: tensor<2x3xf32>, %[[ARG1:.*]]: tensor<3x?xf32>)
// CHECK: %[[INIT:.*]] = dynamic_tensor_from_elements
// CHECK: linalg.matmul
// CHECK-SAME: ins(%[[ARG0]], %[[ARG1]] : tensor<2x3xf32>, tensor<3x?xf32>)
// CHECK-SAME: outs(%[[INIT]] : tensor<2x?xf32>)

// -----

func @dot_matvec(%arg0: tensor<?x3xf32>,
                 %arg1: tensor<3xf32>) -> tensor<?xf32> {
  %0 = "mhlo.dot"(%arg0, %arg1) : (tensor<?x3xf32>,
                                   tensor<3xf32>) -> tensor<?xf32>
  return %0 : tensor<?xf32>
}
// CHECK: func @dot_matvec(%[[ARG0:.*]]: tensor<?x3xf32>, %[[ARG1:.*]]: tensor<3xf32>)
// CHECK: %[[INIT:.*]] = dynamic_tensor_from_elements
// CHECK: linalg.matvec
// CHECK-SAME: ins(%[[ARG0]], %[[ARG1]] : tensor<?x3xf32>, tensor<3xf32>)
// CHECK-SAME: outs(%[[INIT]] : tensor<?xf32>)

// -----

func @dot_dot(%arg0: tensor<?xf32>,
              %arg1: tensor<?xf32>) -> tensor<f32> {
  %0 = "mhlo.dot"(%arg0, %arg1) : (tensor<?xf32>, tensor<?xf32>) -> tensor<f32>
  return %0 : tensor<f32>
}
// CHECK: func @dot_dot(%[[ARG0:.*]]: tensor<?xf32>, %[[ARG1:.*]]: tensor<?xf32>)
// CHECK: %[[INIT:.*]] = dynamic_tensor_from_elements
// CHECK: linalg.dot
// CHECK-SAME: ins(%[[ARG0]], %[[ARG1]] : tensor<?xf32>, tensor<?xf32>)
// CHECK-SAME: outs(%[[INIT]] : tensor<f32>)

// -----

func @dot_general(%arg0: tensor<?x?x3xf32>,
                  %arg1: tensor<?x3x?xf32>) -> tensor<?x?x?xf32> {
  %0 ="mhlo.dot_general"(%arg0, %arg1) {
      dot_dimension_numbers = {
          lhs_batching_dimensions = dense<0> : tensor<1xi64>,
          lhs_contracting_dimensions = dense<2> : tensor<1xi64>,
          rhs_batching_dimensions = dense<0> : tensor<1xi64>,
          rhs_contracting_dimensions = dense<1> : tensor<1xi64>
      },
      precision_config = ["DEFAULT", "DEFAULT"]
  } : (tensor<?x?x3xf32>, tensor<?x3x?xf32>) -> tensor<?x?x?xf32>
  return %0 : tensor<?x?x?xf32>
}
// CHECK: func @dot_general(%[[ARG0:.*]]: tensor<?x?x3xf32>, %[[ARG1:.*]]: tensor<?x3x?xf32>)
// CHECK: %[[INIT:.*]] = dynamic_tensor_from_elements
// CHECK: linalg.batch_matmul
// CHECK-SAME: ins(%[[ARG0]], %[[ARG1]] : tensor<?x?x3xf32>, tensor<?x3x?xf32>)
// CHECK-SAME: outs(%[[INIT]] : tensor<?x?x?xf32>)
