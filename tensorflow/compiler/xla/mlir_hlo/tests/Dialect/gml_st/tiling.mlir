// RUN: mlir-hlo-opt %s --split-input-file \
// RUN:     --gml-tiling="tile-sizes=256,512 distribute=false op-label=tile-2d" \
// RUN:     --gml-tiling="tile-sizes=1,1 distribute=false op-label=tile-2d-point" \
// RUN:     --gml-tiling="tile-sizes=256,512 distribute=false op-label=tile-3d" | \
// RUN: FileCheck %s --check-prefix=CHECK-SEQUENTIAL

// RUN: mlir-hlo-opt %s --split-input-file \
// RUN:     --gml-tiling="tile-sizes=256,512 distribute=true op-label=tile-2d" \
// RUN:     --gml-tiling="tile-sizes=1,1 distribute=true op-label=tile-2d-point" | \
// RUN: FileCheck %s --check-prefix=CHECK-PARALLEL


#id_map = affine_map<(d0, d1) -> (d0, d1)>

func.func @add(%lhs: tensor<?x?xf32>, %rhs: tensor<?x?xf32>)
    -> tensor<?x?xf32> {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %d0 = tensor.dim %lhs, %c0 : tensor<?x?xf32>
  %d1 = tensor.dim %lhs, %c1 : tensor<?x?xf32>
  %init = linalg.init_tensor [%d0, %d1] : tensor<?x?xf32>
  %add = linalg.generic {
      indexing_maps = [#id_map, #id_map, #id_map],
      iterator_types = ["parallel", "parallel"],
      op_label = "tile-2d"}
      ins(%lhs, %rhs : tensor<?x?xf32>, tensor<?x?xf32>)
      outs(%init : tensor<?x?xf32>) {
  ^bb0(%lhs_scalar: f32, %rhs_scalar: f32, %_: f32):
    %add_scalar = arith.addf %lhs_scalar, %rhs_scalar : f32
    linalg.yield %add_scalar : f32
  } -> tensor<?x?xf32>
  func.return %add : tensor<?x?xf32>
}


// CHECK-SEQUENTIAL-LABEL: @add
// CHECK-SEQUENTIAL-SAME:  %[[ARG0:.*]]: tensor<?x?xf32>, %[[ARG1:.*]]: tensor<?x?xf32>

// CHECK-SEQUENTIAL:       %[[C0:.*]] = arith.constant 0
// CHECK-SEQUENTIAL:       %[[C1:.*]] = arith.constant 1
// CHECK-SEQUENTIAL:       %[[C256:.*]] = arith.constant 256
// CHECK-SEQUENTIAL:       %[[C512:.*]] = arith.constant 512
// CHECK-SEQUENTIAL:       %[[DIM:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-SEQUENTIAL:       %[[DIM_0:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-SEQUENTIAL:       %[[INIT:.*]] = linalg.init_tensor [%[[DIM]], %[[DIM_0]]]
// CHECK-SEQUENTIAL:       %[[DIM_1:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-SEQUENTIAL:       %[[DIM_2:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-SEQUENTIAL:       %[[FOR:.*]] = gml_st.for (%[[ARG2:.*]], %[[ARG3:.*]]) = (%[[C0]], %[[C0]])
// CHECK-SEQUENTIAL-SAME:      to (%[[DIM_1]], %[[DIM_2]])
// CHECK-SEQUENTIAL-SAME:      step (%[[C256]], %[[C512]])
// CHECK-SEQUENTIAL-SAME:      outs (%[[ARG4:.*]] = %[[INIT]]: tensor<?x?xf32>)
// CHECK-SEQUENTIAL:         %[[MIN:.*]] = affine.min #map0(%[[ARG2]])[%[[C256]], %[[DIM_1]]]
// CHECK-SEQUENTIAL:         %[[MIN_0:.*]] = affine.min #map1(%[[ARG3]])[%[[C512]], %[[DIM_2]]]
// CHECK-SEQUENTIAL:         %[[DIM_3:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-SEQUENTIAL:         %[[DIM_4:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-SEQUENTIAL:         %[[SPACE:.*]] = gml_st.space [%[[DIM_3]], %[[DIM_4]]]
// CHECK-SEQUENTIAL:         %[[TILE:.*]] = gml_st.tile %[[SPACE]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG0]][%[[TILE]]]
// CHECK-SEQUENTIAL:         %[[DIM_5:.*]] = tensor.dim %[[ARG1]], %[[C0]]
// CHECK-SEQUENTIAL:         %[[DIM_6:.*]] = tensor.dim %[[ARG1]], %[[C1]]
// CHECK-SEQUENTIAL:         %[[SPACE_0:.*]] = gml_st.space [%[[DIM_5]], %[[DIM_6]]]
// CHECK-SEQUENTIAL:         %[[TILE_0:.*]] = gml_st.tile %[[SPACE_0]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[ARG1]][%[[TILE_0]]]
// CHECK-SEQUENTIAL:         %[[DIM_7:.*]] = tensor.dim %[[ARG4]], %[[C0]]
// CHECK-SEQUENTIAL:         %[[DIM_8:.*]] = tensor.dim %[[ARG4]], %[[C1]]
// CHECK-SEQUENTIAL:         %[[SPACE_1:.*]] = gml_st.space [%[[DIM_7]], %[[DIM_8]]]
// CHECK-SEQUENTIAL:         %[[TILE_1:.*]] = gml_st.tile %[[SPACE_1]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE_1:.*]] = gml_st.materialize %[[ARG4]][%[[TILE_1]]]
// CHECK-SEQUENTIAL:         %[[GENERIC:.*]] = linalg.generic
// CHECK-SEQUENTIAL-SAME:        iterator_types = ["parallel", "parallel"]
// CHECK-SEQUENTIAL-SAME:        ins(%[[MATERIALIZE]], %[[MATERIALIZE_0]] : tensor<?x?xf32>, tensor<?x?xf32>)
// CHECK-SEQUENTIAL-SAME:        outs(%[[MATERIALIZE_1]] : tensor<?x?xf32>)
// CHECK-SEQUENTIAL-SAME:        attrs =  {op_label = "tile-2d"}
// CHECK-SEQUENTIAL:         ^bb0(%[[ARG5:.*]]: f32, %[[ARG6:.*]]: f32, %[[ARG7:.*]]: f32):
// CHECK-SEQUENTIAL:           %[[ADDF:.*]] = arith.addf %[[ARG5]], %[[ARG6]]
// CHECK-SEQUENTIAL:           linalg.yield %[[ADDF]]
// CHECK-SEQUENTIAL:         gml_st.set_yield %[[GENERIC]] into %[[ARG4]][%[[TILE_1]]]
// CHECK-SEQUENTIAL:       return %[[FOR]]


// CHECK-PARALLEL-LABEL: @add
// CHECK-PARALLEL-SAME:  %[[ARG0:.*]]: tensor<?x?xf32>, %[[ARG1:.*]]: tensor<?x?xf32>

// CHECK-PARALLEL:       %[[C0:.*]] = arith.constant 0
// CHECK-PARALLEL:       %[[C1:.*]] = arith.constant 1
// CHECK-PARALLEL:       %[[C256:.*]] = arith.constant 256
// CHECK-PARALLEL:       %[[C512:.*]] = arith.constant 512
// CHECK-PARALLEL:       %[[DIM:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-PARALLEL:       %[[DIM_0:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-PARALLEL:       %[[INIT:.*]] = linalg.init_tensor [%[[DIM]], %[[DIM_0]]]
// CHECK-PARALLEL:       %[[DIM_1:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-PARALLEL:       %[[DIM_2:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-PARALLEL:       %[[PARALLEL:.*]] = gml_st.parallel (%[[ARG2:.*]], %[[ARG3:.*]]) = (%[[C0]], %[[C0]])
// CHECK-PARALLEL-SAME:      to (%[[DIM_1]], %[[DIM_2]])
// CHECK-PARALLEL-SAME:      step (%[[C256]], %[[C512]])
// CHECK-PARALLEL:         %[[MIN:.*]] = affine.min #map0(%[[ARG2]])[%[[C256]], %[[DIM_1]]]
// CHECK-PARALLEL:         %[[MIN_0:.*]] = affine.min #map1(%[[ARG3]])[%[[C512]], %[[DIM_2]]]
// CHECK-PARALLEL:         %[[DIM_3:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-PARALLEL:         %[[DIM_4:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-PARALLEL:         %[[SPACE:.*]] = gml_st.space [%[[DIM_3]], %[[DIM_4]]]
// CHECK-PARALLEL:         %[[TILE:.*]] = gml_st.tile %[[SPACE]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-PARALLEL:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG0]][%[[TILE]]]
// CHECK-PARALLEL:         %[[DIM_5:.*]] = tensor.dim %[[ARG1]], %[[C0]]
// CHECK-PARALLEL:         %[[DIM_6:.*]] = tensor.dim %[[ARG1]], %[[C1]]
// CHECK-PARALLEL:         %[[SPACE_0:.*]] = gml_st.space [%[[DIM_5]], %[[DIM_6]]]
// CHECK-PARALLEL:         %[[TILE_0:.*]] = gml_st.tile %[[SPACE_0]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-PARALLEL:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[ARG1]][%[[TILE_0]]]
// CHECK-PARALLEL:         %[[DIM_7:.*]] = tensor.dim %[[INIT]], %[[C0]]
// CHECK-PARALLEL:         %[[DIM_8:.*]] = tensor.dim %[[INIT]], %[[C1]]
// CHECK-PARALLEL:         %[[SPACE_1:.*]] = gml_st.space [%[[DIM_7]], %[[DIM_8]]]
// CHECK-PARALLEL:         %[[TILE_1:.*]] = gml_st.tile %[[SPACE_1]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-PARALLEL:         %[[MATERIALIZE_1:.*]] = gml_st.materialize %[[INIT]][%[[TILE_1]]]
// CHECK-PARALLEL:         %[[GENERIC:.*]] = linalg.generic
// CHECK-PARALLEL-SAME:        iterator_types = ["parallel", "parallel"]
// CHECK-PARALLEL-SAME:        ins(%[[MATERIALIZE]], %[[MATERIALIZE_0]] : tensor<?x?xf32>, tensor<?x?xf32>)
// CHECK-PARALLEL-SAME:        outs(%[[MATERIALIZE_1]] : tensor<?x?xf32>)
// CHECK-PARALLEL-SAME:        attrs =  {op_label = "tile-2d"}
// CHECK-PARALLEL:         ^bb0(%[[ARG4:.*]]: f32, %[[ARG5:.*]]: f32, %[[ARG6:.*]]: f32):
// CHECK-PARALLEL:           %[[ADDF:.*]] = arith.addf %[[ARG4]], %[[ARG5]]
// CHECK-PARALLEL:           linalg.yield %[[ADDF]]
// CHECK-PARALLEL:         gml_st.set_yield %[[GENERIC]] into %[[INIT]][%[[TILE_1]]]
// CHECK-PARALLEL:       return %[[PARALLEL]]

// -----

func.func @reduce_row(%lhs: tensor<?x?xf32>,
                      %rhs: tensor<?x?xf32>) -> tensor<?xf32> {
  %cst = arith.constant 0.000000e+00 : f32
  %c0 = arith.constant 0 : index
  %0 = tensor.dim %lhs, %c0 : tensor<?x?xf32>

  %init = linalg.init_tensor [%0] : tensor<?xf32>
  %fill = linalg.fill ins(%cst : f32)
                      outs(%init : tensor<?xf32>) -> tensor<?xf32>
  %sum_of_prod = linalg.generic {
    indexing_maps = [affine_map<(d0, d1) -> (d0, d1)>,
                     affine_map<(d0, d1) -> (d0, d1)>,
                     affine_map<(d0, d1) -> (d0)>],
    iterator_types = ["parallel", "reduction"],
    op_label = "tile-2d"}
    ins(%lhs, %rhs : tensor<?x?xf32>, tensor<?x?xf32>)
    outs(%fill : tensor<?xf32>) {
  ^bb0(%l: f32, %r: f32, %o: f32):
    %prod = arith.mulf %l, %r : f32
    %add = arith.addf %prod, %o : f32
    linalg.yield %add : f32
  } -> tensor<?xf32>
  func.return %sum_of_prod : tensor<?xf32>
}


// CHECK-SEQUENTIAL-LABEL: @reduce_row
// CHECK-SEQUENTIAL-SAME:  %[[ARG0_0:.*]]: tensor<?x?xf32>, %[[ARG1_0:.*]]: tensor<?x?xf32>

// CHECK-SEQUENTIAL:       %[[C0_0:.*]] = arith.constant 0
// CHECK-SEQUENTIAL:       %[[C1_0:.*]] = arith.constant 1
// CHECK-SEQUENTIAL:       %[[C256_0:.*]] = arith.constant 256
// CHECK-SEQUENTIAL:       %[[C512_0:.*]] = arith.constant 512
// CHECK-SEQUENTIAL:       %[[CST:.*]] = arith.constant 0.000000e+00
// CHECK-SEQUENTIAL:       %[[DIM_9:.*]] = tensor.dim %[[ARG0_0]], %[[C0_0]]
// CHECK-SEQUENTIAL:       %[[INIT_0:.*]] = linalg.init_tensor [%[[DIM_9]]]
// CHECK-SEQUENTIAL:       %[[FILL:.*]] = linalg.fill ins(%[[CST]] : f32) outs(%[[INIT_0]] : tensor<?xf32>)
// CHECK-SEQUENTIAL:       %[[DIM_10:.*]] = tensor.dim %[[ARG0_0]], %[[C0_0]]
// CHECK-SEQUENTIAL:       %[[DIM_11:.*]] = tensor.dim %[[ARG0_0]], %[[C1_0]]
// CHECK-SEQUENTIAL:       %[[FOR_0:.*]] = gml_st.for (%[[ARG2_0:.*]], %[[ARG3_0:.*]]) = (%[[C0_0]], %[[C0_0]])
// CHECK-SEQUENTIAL-SAME:      to (%[[DIM_10]], %[[DIM_11]])
// CHECK-SEQUENTIAL-SAME:      step (%[[C256_0]], %[[C512_0]])
// CHECK-SEQUENTIAL-SAME:      outs (%[[ARG4_0:.*]] = %[[FILL]]: tensor<?xf32>)
// CHECK-SEQUENTIAL:         %[[MIN_1:.*]] = affine.min #map0(%[[ARG2_0]])[%[[C256_0]], %[[DIM_10]]]
// CHECK-SEQUENTIAL:         %[[MIN_2:.*]] = affine.min #map1(%[[ARG3_0]])[%[[C512_0]], %[[DIM_11]]]
// CHECK-SEQUENTIAL:         %[[DIM_12:.*]] = tensor.dim %[[ARG0_0]], %[[C0_0]]
// CHECK-SEQUENTIAL:         %[[DIM_13:.*]] = tensor.dim %[[ARG0_0]], %[[C1_0]]
// CHECK-SEQUENTIAL:         %[[SPACE_2:.*]] = gml_st.space [%[[DIM_12]], %[[DIM_13]]]
// CHECK-SEQUENTIAL:         %[[TILE_2:.*]] = gml_st.tile %[[SPACE_2]] [%[[ARG2_0]], %[[ARG3_0]]] [%[[MIN_1]], %[[MIN_2]]] [1, 1]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE_2:.*]] = gml_st.materialize %[[ARG0_0]][%[[TILE_2]]]
// CHECK-SEQUENTIAL:         %[[DIM_14:.*]] = tensor.dim %[[ARG1_0]], %[[C0_0]]
// CHECK-SEQUENTIAL:         %[[DIM_15:.*]] = tensor.dim %[[ARG1_0]], %[[C1_0]]
// CHECK-SEQUENTIAL:         %[[SPACE_3:.*]] = gml_st.space [%[[DIM_14]], %[[DIM_15]]]
// CHECK-SEQUENTIAL:         %[[TILE_3:.*]] = gml_st.tile %[[SPACE_3]] [%[[ARG2_0]], %[[ARG3_0]]] [%[[MIN_1]], %[[MIN_2]]] [1, 1]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE_3:.*]] = gml_st.materialize %[[ARG1_0]][%[[TILE_3]]]
// CHECK-SEQUENTIAL:         %[[DIM_16:.*]] = tensor.dim %[[ARG4_0]], %[[C0_0]]
// CHECK-SEQUENTIAL:         %[[SPACE_4:.*]] = gml_st.space [%[[DIM_16]]]
// CHECK-SEQUENTIAL:         %[[TILE_4:.*]] = gml_st.tile %[[SPACE_4]] [%[[ARG2_0]]] [%[[MIN_1]]] [1]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE_4:.*]] = gml_st.materialize %[[ARG4_0]][%[[TILE_4]]]
// CHECK-SEQUENTIAL:         %[[GENERIC_0:.*]] = linalg.generic
// CHECK-SEQUENTIAL-SAME:        iterator_types = ["parallel", "reduction"]}
// CHECK-SEQUENTIAL-SAME:        ins(%[[MATERIALIZE_2]], %[[MATERIALIZE_3]] : tensor<?x?xf32>, tensor<?x?xf32>)
// CHECK-SEQUENTIAL-SAME:        outs(%[[MATERIALIZE_4]] : tensor<?xf32>)
// CHECK-SEQUENTIAL-SAME:        attrs =  {op_label = "tile-2d"}
// CHECK-SEQUENTIAL:         ^bb0(%[[ARG5_0:.*]]: f32, %[[ARG6_0:.*]]: f32, %[[ARG7_0:.*]]: f32):
// CHECK-SEQUENTIAL:           %[[MULF:.*]] = arith.mulf %[[ARG5_0]], %[[ARG6_0]]
// CHECK-SEQUENTIAL:           %[[ADDF_0:.*]] = arith.addf %[[MULF]], %[[ARG7_0]]
// CHECK-SEQUENTIAL:           linalg.yield %[[ADDF_0]]
// CHECK-SEQUENTIAL:         gml_st.set_yield %[[GENERIC_0]] into %[[ARG4_0]][%[[TILE_4]]]
// CHECK-SEQUENTIAL:       return %[[FOR_0]]


// CHECK-PARALLEL-LABEL: @reduce_row
// CHECK-PARALLEL-SAME:  %[[ARG0_0:.*]]: tensor<?x?xf32>, %[[ARG1_0:.*]]: tensor<?x?xf32>

// CHECK-PARALLEL-NOT:   gml_st.parallel
// CHECK-PARALLEL:       %[[RES:.*]] = linalg.generic
// CHECK-PARALLEL-NOT:   gml_st.parallel
// CHECK-PARALLEL:       return %[[RES]]

// -----

func.func @dynamic_broadcast_in_dim_at_tile(%init : tensor<?x?x?xf32>,
    %arg : tensor<?x?xf32>) -> tensor<?x?x?xf32> {
  %bcast = thlo.dynamic_broadcast_in_dim ins(%arg: tensor<?x?xf32>)
      outs(%init: tensor<?x?x?xf32>) broadcast_dimensions = [0, 2]
      { op_label = "tile-3d" }
  func.return %bcast : tensor<?x?x?xf32>
}


// CHECK-SEQUENTIAL-LABEL: @dynamic_broadcast_in_dim_at_tile
// CHECK-SEQUENTIAL-SAME:  %[[ARG0:.*]]: tensor<?x?x?xf32>, %[[ARG1:.*]]: tensor<?x?xf32>

// CHECK-SEQUENTIAL:       %[[C0:.*]] = arith.constant 0
// CHECK-SEQUENTIAL:       %[[C1:.*]] = arith.constant 1
// CHECK-SEQUENTIAL:       %[[C2:.*]] = arith.constant 2
// CHECK-SEQUENTIAL:       %[[C256:.*]] = arith.constant 256
// CHECK-SEQUENTIAL:       %[[C512:.*]] = arith.constant 512
// CHECK-SEQUENTIAL:       %[[DIM:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-SEQUENTIAL:       %[[DIM_0:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-SEQUENTIAL:       %[[DIM_1:.*]] = tensor.dim %[[ARG0]], %[[C2]]
// CHECK-SEQUENTIAL:       %[[FOR:.*]] = gml_st.for (%[[ARG2:.*]], %[[ARG3:.*]]) = (%[[C0]], %[[C0]])
// CHECK-SEQUENTIAL-SAME:      to (%[[DIM]], %[[DIM_0]])
// CHECK-SEQUENTIAL-SAME:      step (%[[C256]], %[[C512]])
// CHECK-SEQUENTIAL-SAME:      outs (%[[ARG4:.*]] = %[[ARG0]]: tensor<?x?x?xf32>)
// CHECK-SEQUENTIAL:         %[[MIN:.*]] = affine.min #map0(%[[ARG2]])[%[[C256]], %[[DIM]]]
// CHECK-SEQUENTIAL:         %[[MIN_0:.*]] = affine.min #map1(%[[ARG3]])[%[[C512]], %[[DIM_0]]]
// CHECK-SEQUENTIAL:         %[[DIM_2:.*]] = tensor.dim %[[ARG4]], %[[C0]]
// CHECK-SEQUENTIAL:         %[[DIM_3:.*]] = tensor.dim %[[ARG4]], %[[C1]]
// CHECK-SEQUENTIAL:         %[[DIM_4:.*]] = tensor.dim %[[ARG4]], %[[C2]]
// CHECK-SEQUENTIAL:         %[[SPACE:.*]] = gml_st.space [%[[DIM_2]], %[[DIM_3]], %[[DIM_4]]]
// CHECK-SEQUENTIAL:         %[[TILE:.*]] = gml_st.tile %[[SPACE]] [%[[ARG2]], %[[ARG3]], %[[C0]]] [%[[MIN]], %[[MIN_0]], %[[DIM_1]]] [1, 1, 1]
// CHECK-SEQUENTIAL:         %[[DIM_5:.*]] = tensor.dim %[[ARG1]], %[[C0]]
// CHECK-SEQUENTIAL:         %[[DIM_6:.*]] = tensor.dim %[[ARG1]], %[[C1]]
// CHECK-SEQUENTIAL:         %[[SPACE_0:.*]] = gml_st.space [%[[DIM_5]], %[[DIM_6]]]
// CHECK-SEQUENTIAL:         %[[DROP:.*]] = gml_st.drop_dims %[[TILE]], [0, 2]
// CHECK-SEQUENTIAL:         %[[DIM_7:.*]] = tensor.dim %[[ARG4]], %[[C0]]
// CHECK-SEQUENTIAL:         %[[CMPI:.*]] = arith.cmpi ne, %[[DIM_5]], %[[DIM_7]]
// CHECK-SEQUENTIAL:         %[[DIM_8:.*]] = tensor.dim %[[ARG4]], %[[C2]]
// CHECK-SEQUENTIAL:         %[[CMPI_0:.*]] = arith.cmpi ne, %[[DIM_6]], %[[DIM_8]]
// CHECK-SEQUENTIAL:         %[[OFFSET:.*]] = gml_st.offset %[[DROP]][%[[C0]]]
// CHECK-SEQUENTIAL:         %[[SELECT:.*]] = arith.select %[[CMPI]], %[[C0]], %[[OFFSET]]
// CHECK-SEQUENTIAL:         %[[OFFSET_0:.*]] = gml_st.offset %[[DROP]][%[[C1]]]
// CHECK-SEQUENTIAL:         %[[SELECT_0:.*]] = arith.select %[[CMPI_0]], %[[C0]], %[[OFFSET_0]]
// CHECK-SEQUENTIAL:         %[[SIZE:.*]] = gml_st.size %[[DROP]][%[[C0]]]
// CHECK-SEQUENTIAL:         %[[SELECT_1:.*]] = arith.select %[[CMPI]], %[[C1]], %[[SIZE]]
// CHECK-SEQUENTIAL:         %[[SIZE_0:.*]] = gml_st.size %[[DROP]][%[[C1]]]
// CHECK-SEQUENTIAL:         %[[SELECT_2:.*]] = arith.select %[[CMPI_0]], %[[C1]], %[[SIZE_0]]
// CHECK-SEQUENTIAL:         %[[TILE_0:.*]] = gml_st.tile %[[SPACE_0]] [%[[SELECT]], %[[SELECT_0]]] [%[[SELECT_1]], %[[SELECT_2]]] [1, 1]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG4]][%[[TILE]]]
// CHECK-SEQUENTIAL:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[ARG1]][%[[TILE_0]]]
// CHECK-SEQUENTIAL:         %[[DYNAMIC:.*]] = thlo.dynamic_broadcast_in_dim
// CHECK-SEQUENTIAL-SAME:        ins(%[[MATERIALIZE_0]]
// CHECK-SEQUENTIAL-SAME:        outs(%[[MATERIALIZE]]
// CHECK-SEQUENTIAL-SAME:        broadcast_dimensions = [0, 2]
// CHECK-SEQUENTIAL:         gml_st.set_yield %[[DYNAMIC]] into %[[ARG4]][%[[TILE]]]
// CHECK-SEQUENTIAL:       return %[[FOR]]

// -----

func.func @scatter_i32_i64(%indices: tensor<?x2xi32>, %updates: tensor<?xi64>,
                           %init: tensor<?x?xi64>) -> tensor<?x?xi64> {
  %result = thlo.scatter
    ins (%indices: tensor<?x2xi32>, %updates: tensor<?xi64>)
    outs (%init: tensor<?x?xi64>) { op_label = "tile-2d-point" }
  return %result : tensor<?x?xi64>
}

// CHECK-SEQUENTIAL-LABEL: @scatter_i32_i64
// CHECK-SEQUENTIAL-SAME:  (%[[INDICES:.*]]: {{.*}}, %[[UPDATES:.*]]: {{.*}}, %[[INIT:.*]]:
// CHECK-SEQUENTIAL-DAG:   %[[ZERO:.*]] = arith.constant 0 : index
// CHECK-SEQUENTIAL-DAG:   %[[ONE:.*]] = arith.constant 1 : index
// CHECK-SEQUENTIAL-DAG:   %[[ZERO_I64:.*]] = arith.constant 0 : i64
// CHECK-SEQUENTIAL-DAG:   %[[ZERO_TENSOR:.*]] = arith.constant dense<0> : tensor<1xi32>
// CHECK-SEQUENTIAL:       %[[RESULT:.*]] = gml_st.for (%[[I:.*]], %[[J:.*]]) =
// CHECK-SEQUENTIAL-SAME:    outs (%[[INIT_FOR:.*]] = %[[INIT]]
// CHECK-SEQUENTIAL:       %[[INDICES_D0:.*]] = tensor.dim %[[INDICES]], %[[ZERO]]
// CHECK-SEQUENTIAL:       %[[UPD_ACC:.*]] = scf.for %[[K:[^ ]*]] =
// CHECK-SEQUENTIAL-SAME:      %[[ZERO]] to %[[INDICES_D0]] step %[[ONE]]
// CHECK-SEQUENTIAL-SAME:      iter_args(%[[UPD_ACC_VAR:.*]] = %[[ZERO_I64]])
// CHECK-SEQUENTIAL:         %[[IDX_0:.*]] = tensor.extract %[[INDICES]][%[[K]], %[[ZERO]]]
// CHECK-SEQUENTIAL:         %[[IDX_0_CAST:.*]] = arith.index_cast %[[IDX_0]]
// CHECK-SEQUENTIAL:         %[[IDX_0_OK:.*]] = arith.cmpi eq, %[[IDX_0_CAST]], %[[I]]
// CHECK-SEQUENTIAL:         %[[IDX_1:.*]] = tensor.extract %[[INDICES]][%[[K]], %[[ONE]]]
// CHECK-SEQUENTIAL:         %[[IDX_1_CAST:.*]] = arith.index_cast %[[IDX_1]]
// CHECK-SEQUENTIAL:         %[[IDX_1_OK:.*]] = arith.cmpi eq, %[[IDX_1_CAST]], %[[J]]
// CHECK-SEQUENTIAL:         %[[UPD_IDX_OK:.*]] = arith.andi %[[IDX_0_OK]], %[[IDX_1_OK]]
// CHECK-SEQUENTIAL:         %[[UPD_ACC_STEP:.*]] = scf.if %[[UPD_IDX_OK]]
// CHECK-SEQUENTIAL:           %[[UPD_VALUE:.*]] = tensor.extract %[[UPDATES]][%[[K]]]
// CHECK-SEQUENTIAL:           %[[UPD_ACC_SUM:.*]] = arith.addi %[[UPD_ACC_VAR]],
// CHECK-SEQUENTIAL-SAME:                                       %[[UPD_VALUE]]
// CHECK-SEQUENTIAL:           scf.yield %[[UPD_ACC_SUM]]
// CHECK-SEQUENTIAL:         } else {
// CHECK-SEQUENTIAL:           scf.yield %[[UPD_ACC_VAR]]
// CHECK-SEQUENTIAL:         }
// CHECK-SEQUENTIAL:         scf.yield %[[UPD_ACC_STEP]]
// CHECK-SEQUENTIAL:       }
// CHECK-SEQUENTIAL:       %[[UPD_TENSOR:.*]] = tensor.from_elements %[[UPD_ACC]]
// CHECK-SEQUENTIAL:       %[[INIT_TILE:.*]] = gml_st.tile {{.*}} [%[[I]], %[[J]]]
// CHECK-SEQUENTIAL:       %[[INIT_MAT:.*]] = gml_st.materialize
// CHECK-SEQUENTIAL-SAME:                       %[[INIT_FOR]][%[[INIT_TILE]]]
// CHECK-SEQUENTIAL:       %[[YIELD_VAL:.*]] = thlo.scatter
// CHECK-SEQUENTIAL-SAME:     ins(%[[ZERO_TENSOR]] : {{.*}}, %[[UPD_TENSOR]] :
// CHECK-SEQUENTIAL-SAME:     outs(%[[INIT_MAT]] :
// CHECK-SEQUENTIAL:       gml_st.set_yield %[[YIELD_VAL]]
// CHECK-SEQUENTIAL:       return %[[RESULT]]

// PARALLEL-LABEL: @scatter_i32_i64
// PARALLEL: gml_st.parallel

// -----

func.func @scatter_i32_f32(%indices: tensor<?x2xi32>, %updates: tensor<?xf32>,
                           %init: tensor<?x?xf32>) -> tensor<?x?xf32> {
  %result = thlo.scatter
    ins (%indices: tensor<?x2xi32>, %updates: tensor<?xf32>)
    outs (%init: tensor<?x?xf32>) { op_label = "tile-2d-point" }
  return %result : tensor<?x?xf32>
}

// CHECK-SEQUENTIAL-LABEL: @scatter_i32_f32
// CHECK-SEQUENTIAL-SAME:  (%{{.*}}, %[[UPDATES:.*]]: {{.*}}, %{{.*}}:
// CHECK-SEQUENTIAL-DAG:   %[[ZERO_F:.*]] = arith.constant 0.00
// CHECK-SEQUENTIAL:       scf.for %[[K:.*]] = {{.*}} step
// CHECK-SEQUENTIAL-SAME:  iter_args(%[[UPD_ACC_VAR:.*]] = %[[ZERO_F]])
// CHECK-SEQUENTIAL:       %[[UPD_VALUE:.*]] = tensor.extract %[[UPDATES]][%[[K]]]
// CHECK-SEQUENTIAL:       arith.addf %[[UPD_ACC_VAR]], %[[UPD_VALUE]]

// PARALLEL-LABEL: @scatter_i32_f32
// PARALLEL: gml_st.parallel

// -----

func.func @scatter_2d_indices(%indices: tensor<?x?x2xi32>,
    %updates: tensor<?x?xf32>, %init: tensor<?x?xf32>) -> tensor<?x?xf32> {
  %result = thlo.scatter
    ins (%indices: tensor<?x?x2xi32>, %updates: tensor<?x?xf32>)
    outs (%init: tensor<?x?xf32>) { op_label = "tile-2d-point" }
  return %result : tensor<?x?xf32>
}

// CHECK-SEQUENTIAL-LABEL: @scatter_2d_indices
// CHECK-SEQUENTIAL-SAME:  (%[[INDICES:[^ ]*]]:
// CHECK-SEQUENTIAL-DAG:   %[[ZERO:.*]] = arith.constant 0 : index
// CHECK-SEQUENTIAL-DAG:   %[[ONE:.*]] = arith.constant 1 : index
// CHECK-SEQUENTIAL-DAG:   %[[ZERO_F32:.*]] = arith.constant 0.00
// CHECK-SEQUENTIAL:       %[[INDICES_D0:.*]] = tensor.dim %[[INDICES]], %[[ZERO]]
// CHECK-SEQUENTIAL:       %[[OUTER_RESULT:.*]] = scf.for %{{[^ ]*}} = %[[ZERO]] to %[[INDICES_D0]]
// CHECK-SEQUENTIAL-SAME:      iter_args(%[[OUTER_ACC:.*]] = %[[ZERO_F32]])
// CHECK-SEQUENTIAL:         %[[INDICES_D1:.*]] = tensor.dim %[[INDICES]], %[[ONE]]
// CHECK-SEQUENTIAL:         %[[INNER_RESULT:.*]] = scf.for %{{[^ ]*}} = %[[ZERO]] to %[[INDICES_D1]]
// CHECK-SEQUENTIAL-SAME:        iter_args(%[[INNER_ACC:.*]] = %[[OUTER_ACC]])
// CHECK-SEQUENTIAL:           scf.yield
// CHECK-SEQUENTIAL:         scf.yield %[[INNER_RESULT]]
// CHECK-SEQUENTIAL:       tensor.from_elements %[[OUTER_RESULT]]

// PARALLEL-LABEL: @scatter_2d_indices
// PARALLEL: gml_st.parallel

// -----

func.func @gather(%operand: tensor<?x?x?x?xf64>, %indices: tensor<?x?x4xi64>,
    %init: tensor<?x?xf64>) -> tensor<?x?xf64> {
  %result = thlo.gather
    ins (%operand: tensor<?x?x?x?xf64>, %indices: tensor<?x?x4xi64>)
    outs (%init: tensor<?x?xf64>) { op_label = "tile-2d" }
  return %result : tensor<?x?xf64>
}

// CHECK-SEQUENTIAL-LABEL: @gather
// CHECK-SEQUENTIAL-SAME:    %[[OPERAND:.*]]: tensor<?x?x?x?xf64>
// CHECK-SEQUENTIAL-SAME:    %[[INDICES:.*]]: tensor<?x?x4xi64>
// CHECK-SEQUENTIAL-SAME:    %[[INIT:.*]]:
// CHECK-SEQUENTIAL-DAG:   %[[ZERO:.*]] = arith.constant 0 : index
// CHECK-SEQUENTIAL-DAG:   %[[ONE:.*]] = arith.constant 1 : index
// CHECK-SEQUENTIAL:       %[[RESULT:.*]] = gml_st.for (%[[I:.*]], %[[J:.*]]) =
// CHECK-SEQUENTIAL:         %[[SIZE0:.*]] = affine.min {{.*}}%[[I]]
// CHECK-SEQUENTIAL:         %[[SIZE1:.*]] = affine.min {{.*}}%[[J]]
// CHECK-SEQUENTIAL:         %[[INDEX_SLICE:.*]] = tensor.extract_slice
// CHECK-SEQUENTIAL-SAME:       %[[INDICES]][%[[I]], %[[J]], 0]
// CHECK-SEQUENTIAL-SAME:       [%[[SIZE0]], %[[SIZE1]], 4]
// CHECK-SEQUENTIAL-SAME:       [1, 1, 1]
// CHECK-SEQUENTIAL:         %[[TILE:.*]] = gml_st.tile {{.*}} [%[[I]], %[[J]]]
// CHECK-SEQUENTIAL-SAME:       [%[[SIZE0]], %[[SIZE1]]] [1, 1]
// CHECK-SEQUENTIAL:         %[[INIT_SLICE:.*]] = gml_st.materialize 
// CHECK-SEQUENTIAL-SAME:       [%[[TILE]]]
// CHECK-SEQUENTIAL:         %[[GATHER_SLICE:.*]] = thlo.gather
// CHECK-SEQUENTIAL-SAME:       ins(%[[OPERAND]] :
// CHECK-SEQUENTIAL-SAME:         , %[[INDEX_SLICE]]
// CHECK-SEQUENTIAL-SAME:       outs(%[[INIT_SLICE]]
// CHECK-SEQUENTIAL:         gml_st.set_yield %[[GATHER_SLICE]]

// PARALLEL-LABEL: @gather
// PARALLEL: gml_st.parallel
