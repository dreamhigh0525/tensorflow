// RUN: mlir-hlo-opt %s --split-input-file \
// RUN: --gml-tiling="tile-sizes=256,512 distribute=false op-label=tile-2d" \
// RUN: --gml-tiling="tile-sizes=1,1 distribute=false op-label=tile-2d-point" \
// RUN: --gml-tiling="tile-sizes=1 distribute=false op-label=tile-1d-point" \
// RUN: --gml-tiling="tile-sizes=256,512 distribute=false op-label=tile-3d" \
// RUN: --gml-tiling="tile-sizes=10 distribute=false op-label=tile-1d" \
// RUN: --gml-tiling="tile-sizes=2,4 distribute=false op-label=tile-pad" \
// RUN: --cse | \
// RUN: FileCheck %s --check-prefix=CHECK-FOR

// RUN: mlir-hlo-opt %s --split-input-file \
// RUN: --gml-tiling="tile-sizes=256,512 distribute=true op-label=tile-2d" \
// RUN: --cse | \
// RUN: FileCheck %s --check-prefix=CHECK-PARALLEL

#id_map = affine_map<(d0, d1) -> (d0, d1)>

func.func @add_static(%lhs: tensor<1024x1024xf32>, %rhs: tensor<1024x1024xf32>)
    -> tensor<1024x1024xf32> {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %init = tensor.empty() : tensor<1024x1024xf32>
  %add = linalg.generic {
      indexing_maps = [#id_map, #id_map, #id_map],
      iterator_types = ["parallel", "parallel"],
      op_label = "tile-2d"}
      ins(%lhs, %rhs : tensor<1024x1024xf32>, tensor<1024x1024xf32>)
      outs(%init : tensor<1024x1024xf32>) {
  ^bb0(%lhs_scalar: f32, %rhs_scalar: f32, %_: f32):
    %add_scalar = arith.addf %lhs_scalar, %rhs_scalar : f32
    linalg.yield %add_scalar : f32
  } -> tensor<1024x1024xf32>
  func.return %add : tensor<1024x1024xf32>
}

// CHECK-FOR-LABEL: @add_static
// CHECK-FOR-SAME:  %[[ARG0:.*]]: tensor<1024x1024xf32>, %[[ARG1:.*]]: tensor<1024x1024xf32>

// CHECK-FOR-DAG:   %[[C0:.*]] = arith.constant 0
// CHECK-FOR-DAG:   %[[C256:.*]] = arith.constant 256
// CHECK-FOR-DAG:   %[[C512:.*]] = arith.constant 512
// CHECK-FOR-DAG:   %[[C1024:.*]] = arith.constant 1024
// CHECK-FOR:       %[[INIT:.*]] = tensor.empty()
// CHECK-FOR:       %[[FOR:.*]] = gml_st.for (%[[I:.*]], %[[J:.*]]) = (%[[C0]], %[[C0]])
// CHECK-FOR-SAME:      to (%[[C1024]], %[[C1024]])
// CHECK-FOR-SAME:      step (%[[C256]], %[[C512]])
// CHECK-FOR-SAME:      outs (%[[ARG4:.*]] = %[[INIT]]: tensor<1024x1024xf32>)
// CHECK-FOR:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG0]] [%[[I]], %[[J]]] [256, 512] [1, 1]
// CHECK-FOR:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[ARG1]] [%[[I]], %[[J]]] [256, 512] [1, 1]
// CHECK-FOR:         %[[MATERIALIZE_1:.*]] = gml_st.materialize %[[ARG4]] [%[[I]], %[[J]]] [256, 512] [1, 1]
// CHECK-FOR:         %[[GENERIC:.*]] = linalg.generic
// CHECK-FOR-SAME:        iterator_types = ["parallel", "parallel"]
// CHECK-FOR-SAME:        ins(%[[MATERIALIZE]], %[[MATERIALIZE_0]] : tensor<256x512xf32>, tensor<256x512xf32>)
// CHECK-FOR-SAME:        outs(%[[MATERIALIZE_1]] : tensor<256x512xf32>)
// CHECK-FOR-SAME:        attrs =  {op_label = "tile-2d"}
// CHECK-FOR:         ^bb0(%[[ARG5:.*]]: f32, %[[ARG6:.*]]: f32, %[[ARG7:.*]]: f32):
// CHECK-FOR:           %[[ADDF:.*]] = arith.addf %[[ARG5]], %[[ARG6]]
// CHECK-FOR:           linalg.yield %[[ADDF]]
// CHECK-FOR:         %[[TILE:.*]] = gml_st.tile [%[[I]], %[[J]]] [256, 512] [1, 1]
// CHECK-FOR:         gml_st.set_yield %[[GENERIC]] into %[[ARG4]][%[[TILE]]]
// CHECK-FOR:       return %[[FOR]]

// -----

#id_map = affine_map<(d0, d1) -> (d0, d1)>

func.func @add(%lhs: tensor<?x?xf32>, %rhs: tensor<?x?xf32>)
    -> tensor<?x?xf32> {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %d0 = tensor.dim %lhs, %c0 : tensor<?x?xf32>
  %d1 = tensor.dim %lhs, %c1 : tensor<?x?xf32>
  %init = tensor.empty(%d0, %d1) : tensor<?x?xf32>
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


// CHECK-FOR-LABEL: @add(
// CHECK-FOR-SAME:  %[[ARG0:.*]]: tensor<?x?xf32>, %[[ARG1:.*]]: tensor<?x?xf32>

// CHECK-FOR:       %[[C0:.*]] = arith.constant 0
// CHECK-FOR:       %[[C1:.*]] = arith.constant 1
// CHECK-FOR:       %[[C256:.*]] = arith.constant 256
// CHECK-FOR:       %[[C512:.*]] = arith.constant 512
// CHECK-FOR:       %[[LHS_DIM_0:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-FOR:       %[[LHS_DIM_1:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-FOR:       %[[INIT:.*]] = tensor.empty(%[[LHS_DIM_0]], %[[LHS_DIM_1]])
// CHECK-FOR:       %[[FOR:.*]] = gml_st.for (%[[ARG2:.*]], %[[ARG3:.*]]) = (%[[C0]], %[[C0]])
// CHECK-FOR-SAME:      to (%[[LHS_DIM_0]], %[[LHS_DIM_1]])
// CHECK-FOR-SAME:      step (%[[C256]], %[[C512]])
// CHECK-FOR-SAME:      outs (%[[OUT:.*]] = %[[INIT]]: tensor<?x?xf32>)
// CHECK-FOR:         %[[MIN:.*]] = affine.min #map{{[0-9]*}}(%[[ARG2]])[%[[LHS_DIM_0]]]
// CHECK-FOR:         %[[MIN_0:.*]] = affine.min #map{{[0-9]*}}(%[[ARG3]])[%[[LHS_DIM_1]]]
// CHECK-FOR:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG0]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-FOR:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[ARG1]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-FOR:         %[[MATERIALIZE_1:.*]] = gml_st.materialize %[[OUT]] [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-FOR:         %[[GENERIC:.*]] = linalg.generic
// CHECK-FOR-SAME:        iterator_types = ["parallel", "parallel"]
// CHECK-FOR-SAME:        ins(%[[MATERIALIZE]], %[[MATERIALIZE_0]] : tensor<?x?xf32>, tensor<?x?xf32>)
// CHECK-FOR-SAME:        outs(%[[MATERIALIZE_1]] : tensor<?x?xf32>)
// CHECK-FOR-SAME:        attrs =  {op_label = "tile-2d"}
// CHECK-FOR:         ^bb0(%[[ARG5:.*]]: f32, %[[ARG6:.*]]: f32, %[[ARG7:.*]]: f32):
// CHECK-FOR:           %[[ADDF:.*]] = arith.addf %[[ARG5]], %[[ARG6]]
// CHECK-FOR:           linalg.yield %[[ADDF]]
// CHECK-FOR:         %[[TILE:.*]] = gml_st.tile [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-FOR:         gml_st.set_yield %[[GENERIC]] into %[[OUT]][%[[TILE]]]
// CHECK-FOR:       return %[[FOR]]


// CHECK-PARALLEL-LABEL: @add(
// CHECK-PARALLEL-SAME:  %[[LHS:.*]]: tensor<?x?xf32>, %[[RHS:.*]]: tensor<?x?xf32>

// CHECK-PARALLEL:       %[[C0:.*]] = arith.constant 0
// CHECK-PARALLEL:       %[[C1:.*]] = arith.constant 1
// CHECK-PARALLEL:       %[[C256:.*]] = arith.constant 256
// CHECK-PARALLEL:       %[[C512:.*]] = arith.constant 512
// CHECK-PARALLEL:       %[[LHS_DIM_0:.*]] = tensor.dim %[[LHS]], %[[C0]]
// CHECK-PARALLEL:       %[[LHS_DIM_1:.*]] = tensor.dim %[[LHS]], %[[C1]]
// CHECK-PARALLEL:       %[[INIT:.*]] = tensor.empty(%[[LHS_DIM_0]], %[[LHS_DIM_1]])
// CHECK-PARALLEL:       %[[PARALLEL:.*]] = gml_st.parallel (%[[ARG2:.*]], %[[ARG3:.*]]) = (%[[C0]], %[[C0]])
// CHECK-PARALLEL-SAME:      to (%[[LHS_DIM_0]], %[[LHS_DIM_1]])
// CHECK-PARALLEL-SAME:      step (%[[C256]], %[[C512]])
// CHECK-PARALLEL:         %[[MIN:.*]] = affine.min #map{{[0-9]*}}(%[[ARG2]])[%[[LHS_DIM_0]]]
// CHECK-PARALLEL:         %[[MIN_0:.*]] = affine.min #map{{[0-9]*}}(%[[ARG3]])[%[[LHS_DIM_1]]]
// CHECK-PARALLEL:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[LHS]]
// CHECK-PARALLEL-SAME:      [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-PARALLEL:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[RHS]]
// CHECK-PARALLEL-SAME:      [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-PARALLEL:         %[[MATERIALIZE_1:.*]] = gml_st.materialize %[[INIT]]
// CHECK-PARALLEL-SAME:      [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-PARALLEL:         %[[GENERIC:.*]] = linalg.generic
// CHECK-PARALLEL-SAME:        iterator_types = ["parallel", "parallel"]
// CHECK-PARALLEL-SAME:        ins(%[[MATERIALIZE]], %[[MATERIALIZE_0]] : tensor<?x?xf32>, tensor<?x?xf32>)
// CHECK-PARALLEL-SAME:        outs(%[[MATERIALIZE_1]] : tensor<?x?xf32>)
// CHECK-PARALLEL-SAME:        attrs =  {op_label = "tile-2d"}
// CHECK-PARALLEL:         ^bb0(%[[OUT:.*]]: f32, %[[ARG5:.*]]: f32, %[[ARG6:.*]]: f32):
// CHECK-PARALLEL:           %[[ADDF:.*]] = arith.addf %[[OUT]], %[[ARG5]]
// CHECK-PARALLEL:           linalg.yield %[[ADDF]]
// CHECK-PARALLEL:         %[[TILE:.*]] = gml_st.tile [%[[ARG2]], %[[ARG3]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-PARALLEL:         gml_st.set_yield %[[GENERIC]] into %[[INIT]][%[[TILE]]]
// CHECK-PARALLEL:       return %[[PARALLEL]]

// -----

func.func @reduce_row(%lhs: tensor<?x?xf32>,
                      %rhs: tensor<?x?xf32>) -> tensor<?xf32> {
  %cst = arith.constant 0.000000e+00 : f32
  %c0 = arith.constant 0 : index
  %0 = tensor.dim %lhs, %c0 : tensor<?x?xf32>

  %init = tensor.empty(%0) : tensor<?xf32>
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


// CHECK-FOR-LABEL: @reduce_row
// CHECK-FOR-SAME:  %[[LHS:.*]]: tensor<?x?xf32>, %[[RHS:.*]]: tensor<?x?xf32>

// CHECK-FOR-DAG:   %[[C0_0:.*]] = arith.constant 0
// CHECK-FOR-DAG:   %[[C1_0:.*]] = arith.constant 1
// CHECK-FOR-DAG:   %[[LHS_DIM_0:.*]] = tensor.dim %[[LHS]], %[[C0_0]]
// CHECK-FOR-DAG:   %[[LHS_DIM_1:.*]] = tensor.dim %[[LHS]], %[[C1_0]]
// CHECK-FOR-DAG:   %[[C256_0:.*]] = arith.constant 256
// CHECK-FOR-DAG:   %[[C512_0:.*]] = arith.constant 512
// CHECK-FOR-DAG:   %[[CST:.*]] = arith.constant 0.000000e+00
// CHECK-FOR-DAG:   %[[INIT_0:.*]] = tensor.empty(%[[LHS_DIM_0]])
// CHECK-FOR-DAG:   %[[FILL:.*]] = linalg.fill ins(%[[CST]] : f32) outs(%[[INIT_0]] : tensor<?xf32>)
// CHECK-FOR:       %[[FOR_0:.*]] = gml_st.for (%[[ARG2_0:.*]], %[[ARG3_0:.*]]) = (%[[C0_0]], %[[C0_0]])
// CHECK-FOR-SAME:      to (%[[LHS_DIM_0]], %[[LHS_DIM_1]])
// CHECK-FOR-SAME:      step (%[[C256_0]], %[[C512_0]])
// CHECK-FOR-SAME:      outs (%[[OUT_0:.*]] = %[[FILL]]: tensor<?xf32>)
// CHECK-FOR:         %[[MIN_1:.*]] = affine.min #map{{[0-9]*}}(%[[ARG2_0]])[%[[LHS_DIM_0]]]
// CHECK-FOR:         %[[MIN_2:.*]] = affine.min #map{{[0-9]*}}(%[[ARG3_0]])[%[[LHS_DIM_1]]]
// CHECK-FOR:         %[[MATERIALIZE_2:.*]] = gml_st.materialize %[[LHS]]
// CHECK-FOR-SAME:      [%[[ARG2_0]], %[[ARG3_0]]] [%[[MIN_1]], %[[MIN_2]]] [1, 1]
// CHECK-FOR:         %[[MATERIALIZE_3:.*]] = gml_st.materialize %[[RHS]]
// CHECK-FOR-SAME:      [%[[ARG2_0]], %[[ARG3_0]]] [%[[MIN_1]], %[[MIN_2]]] [1, 1]
// CHECK-FOR:         %[[MATERIALIZE_4:.*]] = gml_st.materialize %[[OUT_0]]
// CHECK-FOR-SAME:      [%[[ARG2_0]]] [%[[MIN_1]]] [1]
// CHECK-FOR:         %[[GENERIC_0:.*]] = linalg.generic
// CHECK-FOR-SAME:        iterator_types = ["parallel", "reduction"]}
// CHECK-FOR-SAME:        ins(%[[MATERIALIZE_2]], %[[MATERIALIZE_3]] : tensor<?x?xf32>, tensor<?x?xf32>)
// CHECK-FOR-SAME:        outs(%[[MATERIALIZE_4]] : tensor<?xf32>)
// CHECK-FOR-SAME:        attrs =  {op_label = "tile-2d"}
// CHECK-FOR:         ^bb0(%[[ARG5_0:.*]]: f32, %[[ARG6_0:.*]]: f32, %[[ARG7_0:.*]]: f32):
// CHECK-FOR:           %[[MULF:.*]] = arith.mulf %[[ARG5_0]], %[[ARG6_0]]
// CHECK-FOR:           %[[ADDF_0:.*]] = arith.addf %[[MULF]], %[[ARG7_0]]
// CHECK-FOR:           linalg.yield %[[ADDF_0]]
// CHECK-FOR:         %[[TILE_4:.*]] = gml_st.tile [%[[ARG2_0]]] [%[[MIN_1]]] [1]
// CHECK-FOR:         gml_st.set_yield %[[GENERIC_0]] into %[[OUT_0]][%[[TILE_4]]]
// CHECK-FOR:       return %[[FOR_0]]

// -----

func.func @dynamic_broadcast_in_dim_at_tile(%init : tensor<?x?x?xf32>,
    %arg : tensor<?x?xf32>) -> tensor<?x?x?xf32> {
  %bcast = thlo.dynamic_broadcast_in_dim ins(%arg: tensor<?x?xf32>)
      outs(%init: tensor<?x?x?xf32>) broadcast_dimensions = [0, 2]
      { op_label = "tile-3d" }
  func.return %bcast : tensor<?x?x?xf32>
}


// CHECK-FOR-LABEL: @dynamic_broadcast_in_dim_at_tile
// CHECK-FOR-SAME:  %[[INIT:.*]]: tensor<?x?x?xf32>, %[[ARG:.*]]: tensor<?x?xf32>

// CHECK-FOR:       %[[C0:.*]] = arith.constant 0
// CHECK-FOR:       %[[C1:.*]] = arith.constant 1
// CHECK-FOR:       %[[C2:.*]] = arith.constant 2
// CHECK-FOR:       %[[C256:.*]] = arith.constant 256
// CHECK-FOR:       %[[C512:.*]] = arith.constant 512
// CHECK-FOR:       %[[INIT_DIM_0:.*]] = tensor.dim %[[INIT]], %[[C0]]
// CHECK-FOR:       %[[INIT_DIM_1:.*]] = tensor.dim %[[INIT]], %[[C1]]
// CHECK-FOR:       %[[INIT_DIM_2:.*]] = tensor.dim %[[INIT]], %[[C2]]
// CHECK-FOR:       %[[FOR:.*]] = gml_st.for (%[[I:.*]], %[[J:.*]]) = (%[[C0]], %[[C0]])
// CHECK-FOR-SAME:      to (%[[INIT_DIM_0]], %[[INIT_DIM_1]])
// CHECK-FOR-SAME:      step (%[[C256]], %[[C512]])
// CHECK-FOR-SAME:      outs (%[[OUT:.*]] = %[[INIT]]: tensor<?x?x?xf32>)
// CHECK-FOR:         %[[MIN:.*]] = affine.min #map{{[0-9]*}}(%[[I]])[%[[INIT_DIM_0]]]
// CHECK-FOR:         %[[MIN_0:.*]] = affine.min #map{{[0-9]*}}(%[[J]])[%[[INIT_DIM_1]]]
// CHECK-FOR:         %[[ARG_DIM_0:.*]] = tensor.dim %[[ARG]], %[[C0]]
// CHECK-FOR:         %[[ARG_DIM_1:.*]] = tensor.dim %[[ARG]], %[[C1]]
// CHECK-FOR:         %[[CMPI:.*]] = arith.cmpi ne, %[[ARG_DIM_0]], %[[INIT_DIM_0]]
// CHECK-FOR:         %[[CMPI_0:.*]] = arith.cmpi ne, %[[ARG_DIM_1]], %[[INIT_DIM_2]]
// CHECK-FOR:         %[[SELECT:.*]] = arith.select %[[CMPI]], %[[C0]], %[[I]]
// CHECK-FOR:         %[[SELECT_0:.*]] = arith.select %[[CMPI]], %[[C1]], %[[MIN]]
// CHECK-FOR:         %[[SELECT_1:.*]] = arith.select %[[CMPI_0]], %[[C1]], %[[INIT_DIM_2]]
// CHECK-FOR:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[OUT]]
// CHECK-FOR-SAME:      [%[[I]], %[[J]], %[[C0]]] [%[[MIN]], %[[MIN_0]], %[[INIT_DIM_2]]] [1, 1, 1]
// CHECK-FOR:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[ARG]]
// CHECK-FOR-SAME:      [%[[SELECT]], %[[C0]]] [%[[SELECT_0]], %[[SELECT_1]]] [1, 1]
// CHECK-FOR:         %[[DYNAMIC:.*]] = thlo.dynamic_broadcast_in_dim
// CHECK-FOR-SAME:        ins(%[[MATERIALIZE_0]]
// CHECK-FOR-SAME:        outs(%[[MATERIALIZE]]
// CHECK-FOR-SAME:        broadcast_dimensions = [0, 2]
// CHECK-FOR:         %[[TILE:.*]] = gml_st.tile [%[[I]], %[[J]], %[[C0]]] [%[[MIN]], %[[MIN_0]], %[[INIT_DIM_2]]] [1, 1, 1]
// CHECK-FOR:         gml_st.set_yield %[[DYNAMIC]] into %[[OUT]][%[[TILE]]]
// CHECK-FOR:       return %[[FOR]]

// CHECK-PARALLEL-LABEL: @dynamic_broadcast_in_dim_at_tile

// -----

func.func @scatter_i64(%indices: tensor<?x2xindex>,
    %updates: tensor<?x?x?xi64>, %init: tensor<?x?xi64>) -> tensor<?x?xi64> {
  %result = thlo.scatter
    ins (%indices: tensor<?x2xindex>, %updates: tensor<?x?x?xi64>)
    outs (%init: tensor<?x?xi64>) { op_label = "tile-1d-point" }
    (%in: i64, %out: i64) {
      %0 = arith.addi %in, %out: i64
      thlo.yield %0: i64
    }
  return %result : tensor<?x?xi64>
}

// CHECK-FOR-LABEL: func.func @scatter_i64(
// CHECK-FOR-SAME:    %[[INDICES:.*]]: tensor<?x2xindex>,
// CHECK-FOR-SAME:    %[[UPDATES:.*]]: tensor<?x?x?xi64>,
// CHECK-FOR-SAME:    %[[INIT:.*]]: tensor<?x?xi64>

// CHECK-FOR-DAG:   %[[C0:.*]] = arith.constant 0 : index
// CHECK-FOR-DAG:   %[[C1:.*]] = arith.constant 1 : index
// CHECK-FOR-DAG:   %[[C2:.*]] = arith.constant 2 : index

// CHECK-FOR:       %[[INDICES_COUNT:.*]] = tensor.dim %[[INDICES]], %c0
// CHECK-FOR:       gml_st.for (%{{.*}}) = (%[[C0]]) to (%[[INDICES_COUNT]])

// CHECK-FOR:       %[[UPDATE_SUB:.*]] = gml_st.materialize %[[UPDATES]]
// CHECK-FOR-SAME:    : tensor<?x?x?xi64>
// CHECK-FOR:       %[[INDICES_SUB:.*]] = gml_st.materialize %[[INDICES]]
// CHECK-FOR-SAME:    : tensor<?x2xindex>
// CHECK-FOR:       %[[INIT_SUB:.*]] = gml_st.materialize
// CHECK-FOR-SAME:    : tensor<?x?xi64>

// CHECK-FOR:       %[[SCATTER:.*]] = thlo.scatter
// CHECK-FOR-SAME:    ins(%[[INDICES_SUB]] : tensor<1x2xindex>,
// CHECK-FOR-SAME:        %[[UPDATE_SUB]] : tensor<1x?x?xi64>)
// CHECK-FOR-SAME:    outs(%[[INIT_SUB]] : tensor<?x?xi64>)
// CHECK-FOR:           arith.addi
// CHECK-FOR:           thlo.yield
// CHECK-FOR:       gml_st.set_yield %[[SCATTER:.*]]

// -----

func.func @gather(%operand: tensor<?x?x?x?xf64>, %indices: tensor<?x4xindex>,
    %init: tensor<?x10xf64>) -> tensor<?x10xf64> {
  %result = thlo.gather
    ins (%operand: tensor<?x?x?x?xf64>, %indices: tensor<?x4xindex>)
    outs (%init: tensor<?x10xf64>) { op_label = "tile-1d-point" }
  return %result : tensor<?x10xf64>
}

// CHECK-FOR-LABEL: @gather
// CHECK-FOR-SAME:    %[[OPERAND:.*]]: tensor<?x?x?x?xf64>
// CHECK-FOR-SAME:    %[[INDICES:.*]]: tensor<?x4xindex>
// CHECK-FOR-SAME:    %[[INIT:.*]]:
// CHECK-FOR-DAG:   %[[ZERO:.*]] = arith.constant 0
// CHECK-FOR-DAG:   %[[ONE:.*]] = arith.constant 1
// CHECK-FOR:       %[[RESULT:.*]] = gml_st.for (%[[I:.*]]) =
// CHECK-FOR-SAME:      (%[[INIT_:[a-z0-9]+]] = %[[INIT]]: tensor<?x10xf64>)

// CHECK-FOR:         %[[INDEX_SLICE:.*]] = gml_st.materialize %[[INDICES]]
// CHECK-FOR-SAME:      [%[[I]], 0] [1, 4] [1, 1]

// CHECK-FOR:         %[[INIT_SLICE:.*]] = gml_st.materialize %[[INIT_]]
// CHECK-FOR-SAME:      [%[[I]], 0] [1, 10] [1, 1]
// CHECK-FOR:         %[[GATHER_SLICE:.*]] = thlo.gather
// CHECK-FOR-SAME:       ins(%[[OPERAND]] : tensor<?x?x?x?xf64>,
// CHECK-FOR-SAME:           %[[INDEX_SLICE]] : tensor<1x4xindex>)
// CHECK-FOR-SAME:       outs(%[[INIT_SLICE]] : tensor<1x10xf64>)
// CHECK-FOR:         gml_st.set_yield %[[GATHER_SLICE]]

// -----

func.func @concatenate_at_tile(%init : tensor<?x?xi32>, %a: tensor<?x?xi32>,
    %b: tensor<?x?xi32>, %c: tensor<?x?xi32>)
    -> tensor<?x?xi32> {
  %concat = thlo.concatenate
      ins(%a : tensor<?x?xi32>, %b : tensor<?x?xi32>, %c : tensor<?x?xi32>)
      outs(%init : tensor<?x?xi32>)
      dimension = 1
      { op_label = "tile-2d" }
  func.return %concat : tensor<?x?xi32>
}


// CHECK-FOR-LABEL: @concatenate_at_tile
// CHECK-FOR-SAME:  %[[ARG0:.*]]: tensor<?x?xi32>, %[[ARG1:.*]]: tensor<?x?xi32>, %[[ARG2:.*]]: tensor<?x?xi32>, %[[ARG3:.*]]: tensor<?x?xi32>

// CHECK-FOR:       %[[C0:.*]] = arith.constant 0
// CHECK-FOR:       %[[C1:.*]] = arith.constant 1
// CHECK-FOR:       %[[C256:.*]] = arith.constant 256
// CHECK-FOR:       %[[C512:.*]] = arith.constant 512
// CHECK-FOR:       %[[DIM:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-FOR:       %[[DIM_0:.*]] = tensor.dim %[[ARG0]], %[[C1]]
// CHECK-FOR:       %[[FOR:.*]] = gml_st.for (%[[ARG4:.*]], %[[ARG5:.*]]) = (%[[C0]], %[[C0]])
// CHECK-FOR-SAME:      to (%[[DIM]], %[[DIM_0]])
// CHECK-FOR-SAME:      step (%[[C256]], %[[C512]])
// CHECK-FOR-SAME:      outs (%[[ARG6:.*]] = %[[ARG0]]: tensor<?x?xi32>)
// CHECK-FOR:         %[[MIN:.*]] = affine.min #map{{[0-9]*}}(%[[ARG4]])[%[[DIM]]]
// CHECK-FOR:         %[[MIN_0:.*]] = affine.min #map{{[0-9]*}}(%[[ARG5]])[%[[DIM_0]]]
// CHECK-FOR:         %[[DIM_4:.*]] = tensor.dim %[[ARG1]], %[[C1]]
// CHECK-FOR:         %[[MINUI:.*]] = arith.minui %[[ARG5]], %[[DIM_4]]
// CHECK-FOR:         %[[SUBI:.*]] = arith.subi %[[DIM_4]], %[[MINUI]]
// CHECK-FOR:         %[[MINUI_0:.*]] = arith.minui %[[SUBI]], %[[MIN_0]]
// CHECK-FOR:         %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG1]]
// CHECK-FOR-SAME:      [%[[ARG4]], %[[MINUI]]] [%[[MIN]], %[[MINUI_0]]] [%[[C1]], %[[C1]]]
// CHECK-FOR:         %[[CMPI:.*]] = arith.cmpi ule, %[[ARG5]], %[[DIM_4]]
// CHECK-FOR:         %[[SUBI_0:.*]] = arith.subi %[[ARG5]], %[[DIM_4]]
// CHECK-FOR:         %[[SELECT:.*]] = arith.select %[[CMPI]], %[[C0]], %[[SUBI_0]]
// CHECK-FOR:         %[[DIM_5:.*]] = tensor.dim %[[ARG2]], %[[C1]]
// CHECK-FOR:         %[[MINUI_1:.*]] = arith.minui %[[SELECT]], %[[DIM_5]]
// CHECK-FOR:         %[[SUBI_1:.*]] = arith.subi %[[DIM_5]], %[[MINUI_1]]
// CHECK-FOR:         %[[MINUI_2:.*]] = arith.minui %[[SUBI_1]], %[[MIN_0]]
// CHECK-FOR:         %[[MATERIALIZE_0:.*]] = gml_st.materialize %[[ARG2]]
// CHECK-FO-SAME:       [%[[ARG4]], %[[MINUI_1]]] [%[[MIN]], %[[MINUI_2]]] [%[[C1]], %[[C1]]]
// CHECK-FOR:         %[[CMPI_0:.*]] = arith.cmpi ule, %[[SELECT]], %[[DIM_5]]
// CHECK-FOR:         %[[SUBI_2:.*]] = arith.subi %[[SELECT]], %[[DIM_5]]
// CHECK-FOR:         %[[SELECT_0:.*]] = arith.select %[[CMPI_0]], %[[C0]], %[[SUBI_2]]
// CHECK-FOR:         %[[DIM_6:.*]] = tensor.dim %[[ARG3]], %[[C1]]
// CHECK-FOR:         %[[MINUI_3:.*]] = arith.minui %[[SELECT_0]], %[[DIM_6]]
// CHECK-FOR:         %[[SUBI_3:.*]] = arith.subi %[[DIM_6]], %[[MINUI_3]]
// CHECK-FOR:         %[[MINUI_4:.*]] = arith.minui %[[SUBI_3]], %[[MIN_0]]
// CHECK-FOR:         %[[MATERIALIZE_1:.*]] = gml_st.materialize %[[ARG3]]
// CHECK-FO-SAME:       [%[[ARG4]], %[[MINUI_3]]] [%[[MIN]], %[[MINUI_4]]] [%[[C1]], %[[C1]]]
// CHECK-FOR:         %[[MATERIALIZE_2:.*]] = gml_st.materialize %[[ARG6]]
// CHECK-FOR:         [%[[ARG4]], %[[ARG5]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-FOR:         %[[CONCATENATE:.*]] = thlo.concatenate
// CHECK-FOR-SAME:        ins(%[[MATERIALIZE]] : tensor<?x?xi32>, %[[MATERIALIZE_0]] : tensor<?x?xi32>, %[[MATERIALIZE_1]] : tensor<?x?xi32>)
// CHECK-FOR-SAME:        outs(%[[MATERIALIZE_2]] : tensor<?x?xi32>)
// CHECK-FOR-SAME:        dimension = 1
// CHECK-FOR:         %[[TILE:.*]] = gml_st.tile [%[[ARG4]], %[[ARG5]]] [%[[MIN]], %[[MIN_0]]] [1, 1]
// CHECK-FOR:         gml_st.set_yield %[[CONCATENATE]] into %[[ARG6]][%[[TILE]]]
// CHECK-FOR:       return %[[FOR]]

// CHECK-PARALLEL-LABEL: @concatenate_at_tile

// -----

func.func @sort(%input1: tensor<?x?x?xf32>, %input2: tensor<?x?x?xi32>,
                %init1: tensor<?x?x?xf32>, %init2: tensor<?x?x?xi32>)
    -> (tensor<?x?x?xf32>, tensor<?x?x?xi32>) {
  %sorted1, %sorted2 = thlo.sort
      ins(%input1: tensor<?x?x?xf32>, %input2: tensor<?x?x?xi32>)
      outs(%init1: tensor<?x?x?xf32>, %init2: tensor<?x?x?xi32>)
      dimension = 1
      is_stable = true
      {op_label = "tile-3d" }
      (%e11: f32, %e12: f32, %e21: i32, %e22: i32) {
        %gt = arith.cmpf ogt, %e11, %e12: f32
        thlo.yield %gt : i1
      }
  func.return %sorted1, %sorted2 : tensor<?x?x?xf32>, tensor<?x?x?xi32>
}

// CHECK-FOR-LABEL: func.func @sort
// CHECK-FOR-SAME:    (%[[IN0:[a-zA-Z_0-9]*]]: tensor<?x?x?xf32>,
// CHECK-FOR-SAME:     %[[IN1:[a-zA-Z_0-9]*]]: tensor<?x?x?xi32>,
// CHECK-FOR-SAME:     %[[INIT0:[a-zA-Z_0-9]*]]: tensor<?x?x?xf32>,
// CHECK-FOR-SAME:     %[[INIT1:[a-zA-Z_0-9]*]]: tensor<?x?x?xi32>)
// CHECK-FOR-DAG:   %[[C0:[a-zA-Z_0-9]*]] = arith.constant 0
// CHECK-FOR-DAG:   %[[C2:.*]] = arith.constant 2
// CHECK-FOR-DAG:   %[[C1:.*]] = arith.constant 1
// CHECK-FOR-DAG:   %[[DIM0:.*]] = tensor.dim %[[INIT0]], %[[C0]]
// CHECK-FOR-DAG:   %[[DIM2:.*]] = tensor.dim %[[INIT0]], %[[C2]]
// CHECK-FOR:       gml_st.for
// CHECK-FOR-SAME:      (%[[START0:.*]], %[[START2:.*]]) = (%[[C0]], %[[C0]]) to (%[[DIM0]], %[[DIM2]])
// CHECK-FOR-SAME:      outs (%[[INIT0_:.*]] = %[[INIT0]]: tensor<?x?x?xf32>,
// CHECK-FOR-SAME:            %[[INIT1_:.*]] = %[[INIT1]]: tensor<?x?x?xi32>) {
// CHECK-FOR-DAG:     %[[TILE_SIZE0:.*]] = affine.min #map{{[0-9]*}}(%[[START0]])[%[[DIM0]]]
// CHECK-FOR-DAG:     %[[TILE_SIZE2:.*]] = affine.min #map{{[0-9]*}}(%[[START2]])[%[[DIM2]]]
// CHECK-FOR-DAG:     %[[DIM1:.*]] = tensor.dim %[[IN0]], %[[C1]]
// CHECK-FOR-DAG:     %[[IN0_SUB:.*]] = gml_st.materialize %[[IN0]]
// CHECK-FOR-SAME:        [%[[START0]], 0, %[[START2]]]
// CHECK-FOR-SAME:        [%[[TILE_SIZE0]], %[[DIM1]], %[[TILE_SIZE2]]]
// CHECK-FOR-SAME:        [1, 1, 1]
// CHECK-FOR-DAG:     %[[IN1_SUB:.*]] = gml_st.materialize %[[IN1]]
// CHECK-FOR-SAME:        [%[[START0]], 0, %[[START2]]]
// CHECK-FOR-SAME:        [%[[TILE_SIZE0]], %[[DIM1]], %[[TILE_SIZE2]]]
// CHECK-FOR-SAME:        [1, 1, 1]
// CHECK-FOR-DAG:     %[[INIT0_SUB:.*]] = gml_st.materialize %[[INIT0_]]
// CHECK-FOR-SAME:        [%[[START0]], 0, %[[START2]]]
// CHECK-FOR-SAME:        [%[[TILE_SIZE0]], %[[DIM1]], %[[TILE_SIZE2]]]
// CHECK-FOR-SAME:        [1, 1, 1]
// CHECK-FOR-DAG:     %[[INIT1_SUB:.*]] = gml_st.materialize %[[INIT1_]]
// CHECK-FOR-SAME:        [%[[START0]], 0, %[[START2]]]
// CHECK-FOR-SAME:        [%[[TILE_SIZE0]], %[[DIM1]], %[[TILE_SIZE2]]]
// CHECK-FOR-SAME:        [1, 1, 1]
// CHECK-FOR:         thlo.sort
// CHECK-FOR-SAME:        ins(%[[IN0_SUB]] : tensor<?x?x?xf32>, %[[IN1_SUB]] : tensor<?x?x?xi32>)
// CHECK-FOR-SAME:        outs(%[[INIT0_SUB]] : tensor<?x?x?xf32>, %[[INIT1_SUB]] : tensor<?x?x?xi32>)
// CHECK-FOR:         %[[TILE:.*]] = gml_st.tile
// CHECK-FOR-SAME:        [%[[START0]], 0, %[[START2]]]
// CHECK-FOR-SAME:        [%[[TILE_SIZE0]], %[[DIM1]], %[[TILE_SIZE2]]]
// CHECK-FOR-SAME:        [1, 1, 1]
// CHECK-FOR:         gml_st.set_yield
// CHECK-FOR-SAME:        %[[RESULT_TILE:.*]]0 into %[[INIT0_]][%[[TILE]]]
// CHECK-FOR:             %[[RESULT_TILE]]1 into %[[INIT1_]][%[[TILE]]]

// -----

func.func @sort2(%input1: tensor<1024x2048x4096xf32>,
                %input2: tensor<1024x2048x4096xi32>,
                %init1: tensor<1024x2048x4096xf32>,
                %init2: tensor<1024x2048x4096xi32>)
    -> (tensor<1024x2048x4096xf32>, tensor<1024x2048x4096xi32>) {
  %sorted1, %sorted2 = thlo.sort
      ins(%input1: tensor<1024x2048x4096xf32>,
          %input2: tensor<1024x2048x4096xi32>)
      outs(%init1: tensor<1024x2048x4096xf32>,
           %init2: tensor<1024x2048x4096xi32>)
      dimension = 1
      is_stable = true
      { op_label = "tile-3d" }
      (%e11: f32, %e12: f32, %e21: i32, %e22: i32) {
        %gt = arith.cmpf ogt, %e11, %e12: f32
        thlo.yield %gt : i1
      }
  func.return
    %sorted1, %sorted2 : tensor<1024x2048x4096xf32>, tensor<1024x2048x4096xi32>
}

// CHECK-FOR-LABEL: func.func @sort2

// -----

func.func @reverse_static(%input: tensor<100xf32>, %init: tensor<100xf32>)
  -> tensor<100xf32> {
  %res = thlo.reverse
         ins(%input: tensor<100xf32>)
         outs(%init: tensor<100xf32>)
         reverse_dimensions = [0]
         { op_label = "tile-1d" }
  func.return %res : tensor<100xf32>
}

// CHECK-FOR-LABEL: func @reverse_static
//  CHECK-FOR-SAME: %[[ARG0:.*]]: tensor<100xf32>, %[[ARG1:.*]]: tensor<100xf32>
//   CHECK-FOR-DAG:   %[[C10:.*]] = arith.constant 10
//   CHECK-FOR-DAG:   %[[C100:.*]] = arith.constant 100
//       CHECK-FOR:   %[[FOR:.*]] = gml_st.for (%[[I:.*]]) =
//  CHECK-FOR-SAME:   outs (%[[ARG3:.*]] = %[[ARG1]]
//       CHECK-FOR:     %[[TEMP_SUB_RES:.*]] = arith.subi %[[C100]], %[[I]]
//       CHECK-FOR:     %[[IN_TILE_DIM:.*]] = arith.subi %[[TEMP_SUB_RES]], %[[C10]]
//   CHECK-FOR-DAG:     %[[IN_SLICE:.*]] = gml_st.materialize %[[ARG0]] [%[[IN_TILE_DIM]]]
//   CHECK-FOR-DAG:     %[[INIT_SLICE:.*]] = gml_st.materialize %[[ARG3]] [%[[I]]]
//       CHECK-FOR:     %[[REVERSED:.*]] = thlo.reverse ins(%[[IN_SLICE]]
//       CHECK-FOR:       outs(%[[INIT_SLICE]]
//   CHECK-FOR-DAG:     %[[INIT_TILE:.*]] = gml_st.tile [%[[I]]]
//       CHECK-FOR:   gml_st.set_yield %[[REVERSED]] into %[[ARG3]][%[[INIT_TILE]]]
//       CHECK-FOR:   return %[[FOR]]

// -----

func.func @reverse_dynamic(%input: tensor<?x?xf32>, %init: tensor<?x?xf32>)
  -> tensor<?x?xf32> {
  %res = thlo.reverse
         ins(%input: tensor<?x?xf32>)
         outs(%init: tensor<?x?xf32>)
         reverse_dimensions = [0, 1]
         { op_label = "tile-2d" }
  func.return %res : tensor<?x?xf32>
}

// CHECK-FOR-LABEL: func @reverse_dynamic(
//  CHECK-FOR-SAME: %[[ARG0:.*]]: tensor<?x?xf32>, %[[ARG1:.*]]: tensor<?x?xf32>
//   CHECK-FOR-DAG:   %[[C0:.*]] = arith.constant 0
//   CHECK-FOR-DAG:   %[[C1:.*]] = arith.constant 1
//   CHECK-FOR-DAG:   %[[DIM:.*]] = tensor.dim %[[ARG1]], %[[C0]]
//   CHECK-FOR-DAG:   %[[DIM0:.*]] = tensor.dim %[[ARG1]], %[[C1]]
//       CHECK-FOR:   %[[FOR:.*]] = gml_st.for (%[[I:.*]], %[[J:.*]]) =
//  CHECK-FOR-SAME:       (%[[C0]], %[[C0]]) to (%[[DIM]], %[[DIM0]])
//  CHECK-FOR-SAME:       outs (%[[ARG4:.*]] = %[[ARG1]]
//   CHECK-FOR-DAG:     %[[AFFINE_MIN1:.*]] = affine.min
//   CHECK-FOR-DAG:     %[[AFFINE_MIN2:.*]] = affine.min
//   CHECK-FOR-DAG:     %[[DIM1:.*]] = tensor.dim %[[ARG0]], %[[C0]]
//   CHECK-FOR-DAG:     %[[DIM2:.*]] = tensor.dim %[[ARG0]], %[[C1]]
//   CHECK-FOR-DAG:     %[[TEMP_SUB_RES0:.*]] = arith.subi %[[DIM1]], %[[I]]
//   CHECK-FOR-DAG:     %[[IN_TILE_DIM0:.*]] = arith.subi %[[TEMP_SUB_RES0]], %[[AFFINE_MIN1]]
//   CHECK-FOR-DAG:     %[[TEMP_SUB_RES1:.*]] = arith.subi %[[DIM2]], %[[J]]
//   CHECK-FOR-DAG:     %[[IN_TILE_DIM1:.*]] = arith.subi %[[TEMP_SUB_RES1]], %[[AFFINE_MIN2]]
//   CHECK-FOR-DAG:     %[[IN_SLICE:.*]] = gml_st.materialize %[[ARG0]]
//   CHECK-FOR-SAME:      [%[[IN_TILE_DIM0]], %[[IN_TILE_DIM1]]]
//   CHECK-FOR-DAG:     %[[INIT_SLICE:.*]] = gml_st.materialize %[[ARG4]]
//   CHECK-FOR-SAME:      [%[[I]], %[[J]]]
//       CHECK-FOR:     %[[REVERSED:.*]] = thlo.reverse ins(%[[IN_SLICE]]
//  CHECK-FOR-SAME:     outs(%[[INIT_SLICE]]
//       CHECK-FOR:   %[[INIT_TILE:.*]] = gml_st.tile [%[[I]], %[[J]]]
//       CHECK-FOR:   gml_st.set_yield %[[REVERSED]] into %[[ARG4]][%[[INIT_TILE]]]
//       CHECK-FOR:   return %[[FOR]]

// -----

func.func @static_pad_tensor(%input_tensor: tensor<7x9xf32>,
    %pad_value: f32) -> tensor<8x16xf32> {
  %0 = tensor.pad %input_tensor low[0, 0] high[1, 7] {
    ^bb0(%arg1: index, %arg2: index):
      tensor.yield %pad_value : f32
    } { op_label = "tile-pad" }
    : tensor<7x9xf32> to tensor<8x16xf32>
  return %0 : tensor<8x16xf32>
}
// CHECK-FOR-LABEL: func @static_pad_tensor(
//  CHECK-FOR-SAME:     %[[IN:.*]]: tensor<7x9xf32>

// CHECK-FOR-DAG:      %[[C8:.*]] = arith.constant 8
// CHECK-FOR-DAG:      %[[C16:.*]] = arith.constant 16
// CHECK-FOR-DAG:      %[[C0:.*]] = arith.constant 0
// CHECK-FOR-DAG:      %[[C2:.*]] = arith.constant 2
// CHECK-FOR-DAG:      %[[C4:.*]] = arith.constant 4
// CHECK-FOR-DAG:      %[[C7:.*]] = arith.constant 7
// CHECK-FOR-DAG:      %[[C9:.*]] = arith.constant 9
// CHECK-FOR-DAG:      %[[OUT:.*]] = tensor.empty() : tensor<8x16xf32>

// CHECK-FOR:       gml_st.for (%[[I:.*]], %[[J:.*]]) = (%[[C0]], %[[C0]]) to (%[[C8]], %[[C16]])
// CHECK-FOR-SAME:      step (%[[C2]], %[[C4]])
// CHECK-FOR-SAME:      outs (%[[OUT_:.*]] = %[[OUT]]: tensor<8x16xf32>) {
// CHECK-FOR:         %[[SLICE:.*]] = gml_st.materialize %[[IN]]
// CHECK-FOR-SAME:      : tensor<7x9xf32> to tensor<?x?xf32>

// CHECK-FOR:         %[[PAD:.*]] = tensor.pad %[[SLICE]] low[0, 0]
// CHECK-FOR:         ^bb0(%[[VAL_20:.*]]: index, %[[VAL_21:.*]]: index):
// CHECK-FOR:           tensor.yield %[[VAL_22:.*]] : f32
// CHECK-FOR:         } : tensor<?x?xf32> to tensor<?x?xf32>

// CHECK-FOR:         %[[CAST:.*]] = tensor.cast %[[PAD]]
// CHECK-FOR-SAME:      : tensor<?x?xf32> to tensor<2x4xf32>
// CHECK-FOR:         %[[OUT_TILE:.*]] = gml_st.tile [%[[I]], %[[J]]] [2, 4] [1, 1]
// CHECK-FOR-SAME:      : !gml_st.tile<2x4>
// CHECK-FOR:         gml_st.set_yield %[[CAST]] into %[[OUT_]][%[[OUT_TILE]]]
// CHECK-FOR-SAME:      tensor<2x4xf32> into tensor<8x16xf32>[!gml_st.tile<2x4>]
// CHECK-FOR:       } : tensor<8x16xf32>

