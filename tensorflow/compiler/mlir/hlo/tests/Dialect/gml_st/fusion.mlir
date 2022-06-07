// RUN: mlir-hlo-opt %s --split-input-file --gml-fusion | FileCheck %s

// CHECK-LABEL: @dynamic_broadcast_in_dim
// CHECK-SAME:  %[[ARG:.*]]: tensor<?x?xf32>, %[[SHAPE:.*]]: tensor<3xindex>
func.func @dynamic_broadcast_in_dim(%arg : tensor<?x?xf32>,
    %shape : tensor<3xindex>) -> tensor<3x4x5xf32> {
  // CHECK-DAG: %[[C0:.*]] = arith.constant 0
  // CHECK-DAG: %[[C1:.*]] = arith.constant 1
  // CHECK-DAG: %[[C2:.*]] = arith.constant 2

  // Check init tensor.
  // CHECK-DAG: %[[RES_D0:.*]] = tensor.extract %[[SHAPE]][%[[C0]]]
  // CHECK-DAG: %[[RES_D1:.*]] = tensor.extract %[[SHAPE]][%[[C1]]]
  // CHECK-DAG: %[[RES_D2:.*]] = tensor.extract %[[SHAPE]][%[[C2]]]
  // CHECK-DAG: %[[INIT:.*]] = linalg.init_tensor [%[[RES_D0]], %[[RES_D1]], %[[RES_D2]]]

  // Check result space and tile.
  // CHECK-DAG: %[[RES_SPACE:.*]] = gml_st.space [%[[RES_D0]], %[[RES_D1]], %[[RES_D2]]] : !gml_st.tile<?x?x?>
  // CHECK-DAG: %[[RES_TILE:.*]] = gml_st.tile %[[RES_SPACE]] [0, 1, 2] [3, 4, 5] [1, 1, 1] : !gml_st.tile<?x?x?> to !gml_st.tile<3x4x5>

  // Check arg space.
  // CHECK-DAG: %[[ARG_D0:.*]] = tensor.dim %[[ARG]], %[[C0]] : tensor<?x?xf32>
  // CHECK-DAG: %[[ARG_D1:.*]] = tensor.dim %[[ARG]], %[[C1]] : tensor<?x?xf32>
  // CHECK-DAG: %[[ARG_SPACE:.*]] = gml_st.space [%[[ARG_D0]], %[[ARG_D1]]] : !gml_st.tile<?x?>

  // Check collapsing.
  // CHECK-DAG: %[[CED_RES_TILE:.*]] = gml_st.collapse_tile %[[RES_TILE]], [0, 2] : !gml_st.tile<3x4x5> -> !gml_st.tile<3x5>

  // Check first dim of the arg tile.
  // CHECK-DAG: %[[ARG_D0_:.*]] = tensor.dim %[[ARG]], %[[C0]] : tensor<?x?xf32>
  // CHECK-DAG: %[[INIT_D0:.*]] = tensor.dim %[[INIT]], %[[C0]] : tensor<?x?x?xf32>
  // CHECK-DAG: %[[IS_EXPANDING_D0:.*]] = arith.cmpi ne, %[[ARG_D0_]], %[[INIT_D0]] : index
  // CHECK-DAG: %[[CED_RES_OFFSET_D0:.*]] = gml_st.offset %[[CED_RES_TILE]][%[[C0]]] : !gml_st.tile<3x5>
  // CHECK-DAG: %[[CED_RES_SIZE_D0:.*]] = gml_st.size %[[CED_RES_TILE]][%[[C0]]] : !gml_st.tile<3x5>
  // CHECK-DAG: %[[ARG_OFFSET_D0:.*]] = arith.select %[[IS_EXPANDING_D0]], %[[C0]], %[[CED_RES_OFFSET_D0]] : index
  // CHECK-DAG: %[[ARG_SIZE_D0:.*]] = arith.select %[[IS_EXPANDING_D0]], %[[C1]], %[[CED_RES_SIZE_D0]] : index

  // Check second dim of the arg tile.
  // CHECK-DAG: %[[ARG_D1_:.*]] = tensor.dim %[[ARG]], %[[C1]] : tensor<?x?xf32>
  // CHECK-DAG: %[[INIT_D2:.*]] = tensor.dim %[[INIT]], %[[C2]] : tensor<?x?x?xf32>
  // CHECK-DAG: %[[IS_EXPANDING_D1:.*]] = arith.cmpi ne, %[[ARG_D1_]], %[[INIT_D2]] : index
  // CHECK-DAG: %[[CED_RES_OFFSET_D1:.*]] = gml_st.offset %[[CED_RES_TILE]][%[[C1]]] : !gml_st.tile<3x5>
  // CHECK-DAG: %[[CED_RES_SIZE_D1:.*]] = gml_st.size %[[CED_RES_TILE]][%[[C1]]] : !gml_st.tile<3x5>
  // CHECK-DAG: %[[ARG_OFFSET_D1:.*]] = arith.select %[[IS_EXPANDING_D1]], %[[C0]], %[[CED_RES_OFFSET_D1]] : index
  // CHECK-DAG: %[[ARG_SIZE_D1:.*]] = arith.select %[[IS_EXPANDING_D1]], %[[C1]], %[[CED_RES_SIZE_D1]] : index

  // Check arg tile.
  // CHECK-DAG: %[[ARG_TILE:.*]] = gml_st.tile %8 [%[[ARG_OFFSET_D0]], %[[ARG_OFFSET_D1]]] [%[[ARG_SIZE_D0]], %[[ARG_SIZE_D1]]] [1, 1] : !gml_st.tile<?x?> to !gml_st.tile<?x?>

  // Check tiled broadcast.
  // CHECK-DAG: %[[INIT_SUB:.*]] = gml_st.materialize %[[INIT]] at %[[RES_TILE]] : tensor<?x?x?xf32> at !gml_st.tile<3x4x5>
  // CHECK-DAG: %[[ARG_SUB:.*]] = gml_st.materialize %[[ARG]] at %[[ARG_TILE]] : tensor<?x?xf32> at !gml_st.tile<?x?>
  // CHECK-DAG: %[[RES:.*]] = gml_st.dynamic_broadcast_in_dim %[[INIT_SUB]], %[[ARG_SUB]], [0, 2] : tensor<3x4x5xf32>, tensor<?x?xf32> -> tensor<3x4x5xf32>
  // CHECK: return %[[RES]] : tensor<3x4x5xf32>

  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %c2 = arith.constant 2 : index

  // `mhlo.dynamic_broadcast_in_dim` lowered to dst-style op.
  %d0 = tensor.extract %shape[%c0] : tensor<3xindex>
  %d1 = tensor.extract %shape[%c1] : tensor<3xindex>
  %d2 = tensor.extract %shape[%c2] : tensor<3xindex>
  %dst = linalg.init_tensor [%d0, %d1, %d2] : tensor<?x?x?xf32>
  %bcast = gml_st.dynamic_broadcast_in_dim %dst, %arg, [0, 2]
      : tensor<?x?x?xf32>, tensor<?x?xf32> -> tensor<?x?x?xf32>

  // Materialze a tile.
  %space = gml_st.space [%d0, %d1, %d2] : !gml_st.tile<?x?x?>
  %tile = gml_st.tile %space [0, 1, 2] [3, 4, 5] [1, 1, 1]
      : !gml_st.tile<?x?x?> to !gml_st.tile<3x4x5>
  %bcast_sub = gml_st.materialize %bcast at %tile
      : tensor<?x?x?xf32> at !gml_st.tile<3x4x5>

  func.return %bcast_sub : tensor<3x4x5xf32>
}

// -----

// CHECK-LABEL: @add
// CHECK-SAME:  %[[LHS:.*]]: tensor<32x32xf32>, %[[RHS:.*]]: tensor<32x32xf32>, %[[TILE:.*]]: !gml_st.tile<?x?>
func.func @add(%lhs: tensor<32x32xf32>, %rhs: tensor<32x32xf32>,
    %tile: !gml_st.tile<?x?>) -> tensor<?x?xf32> {
  // CHECK-DAG: %[[LHS_SUB:.*]] = gml_st.materialize %[[LHS]] at %[[TILE]] : tensor<32x32xf32> at !gml_st.tile<?x?>
  // CHECK-DAG: %[[RHS_SUB:.*]] = gml_st.materialize %[[RHS]] at %[[TILE]] : tensor<32x32xf32> at !gml_st.tile<?x?>
  // CHECK-DAG: %[[RES:.*]] = mhlo.add %[[LHS_SUB]], %[[RHS_SUB]] : tensor<?x?xf32>
  // CHECK:     return %[[RES]]
  %0 = mhlo.add %lhs, %rhs : tensor<32x32xf32>
  %1 = gml_st.materialize %0 at %tile : tensor<32x32xf32> at !gml_st.tile<?x?>
  func.return %1 : tensor<?x?xf32>
}
