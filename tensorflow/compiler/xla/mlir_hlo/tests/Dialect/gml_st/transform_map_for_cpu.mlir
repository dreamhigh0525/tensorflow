// RUN: mlir-hlo-opt %s --gml-st-cpu-transform-map="tile-size=8" \ 
// RUN: --split-input-file \
// RUN: | FileCheck %s

func.func @map_unary(%input: tensor<?x?xf32>, %init: tensor<?x?xf32>)
                  -> tensor<?x?xf32> {
  %abs = linalg.map
         ins(%input:tensor<?x?xf32>)
         outs(%init:tensor<?x?xf32>)
         (%input_elem: f32) {
           %0 = math.absf %input_elem: f32
           linalg.yield %0: f32
         }
  func.return %abs : tensor<?x?xf32>
}

// CHECK-LABEL: func.func @map_unary(
// CHECK-SAME:      %[[INPUT:.*]]: tensor<?x?xf32>,
// CHECK-SAME:      %[[INIT:.*]]: tensor<?x?xf32>)

// CHECK-DAG:   %[[C0:.*]] = arith.constant 0
// CHECK-DAG:   %[[C1:.*]] = arith.constant 1
// CHECK-DAG:   %[[C8:.*]] = arith.constant 8

// CHECK-DAG:  %[[DIM_0:.*]] = tensor.dim %[[INPUT]], %[[C0]]
// CHECK-DAG:  %[[DIM_1:.*]] = tensor.dim %[[INPUT]], %[[C1]]
// CHECK-DAG:  %[[MAP_DIM_1:.*]] = affine.apply {{.*}}%[[DIM_1]]

// CHECK-NEXT: %[[MAIN_PAR:.*]] = gml_st.parallel (%[[MAIN_I:.*]], %[[MAIN_J:.*]]) =
// CHECK-SAME:     (%[[C0]], %[[C0]]) to (%[[DIM_0]], %[[MAP_DIM_1]])
// CHECK-SAME:     step (%[[C1]], %[[C8]]) {
// CHECK-NEXT:   %[[INPUT_SLICE:.*]] = gml_st.materialize %[[INPUT]]
// CHECK-NEXT:   %[[INIT_SLICE:.*]] = gml_st.materialize %[[INIT]]
// CHECK-NEXT:   %[[MAPPED:.*]] = linalg.map
// CHECK-SAME:     ins(%[[INPUT_SLICE]] : tensor<1x?xf32>)
// CHECK-SAME:     outs(%[[INIT_SLICE]] : tensor<1x?xf32>)
// CHECK-NEXT:     (%[[IN_ELEM:.*]]: f32) {
// CHECK-NEXT:       %[[RES_ELEM:.*]] = math.absf %[[IN_ELEM]] : f32
// CHECK-NEXT:       linalg.yield %[[RES_ELEM]] : f32
// CHECK-NEXT:     }
// CHECK-NEXT:   %[[TILE:.*]] = gml_st.tile [%[[MAIN_I]], %[[MAIN_J]]]
// CHECK-SAME:                          [1, %[[C8]]] [1, 1]
// CHECK-NEXT:   gml_st.set_yield %[[MAPPED]] into %[[INIT]][%[[TILE]]]
// CHECK-NEXT: }

// CHECK-NEXT: %[[RESULT:.*]] = gml_st.parallel (%[[I:.*]], %[[J:.*]]) =
// CHECK-SAME:     (%[[C0]], %[[MAP_DIM_1]]) to (%[[DIM_0]], %[[DIM_1]])
// CHECK-SAME:     step (%[[C1]], %[[C8]]) {
// CHECK:        %[[MAP_DIM:.*]] = affine.apply #{{.*}}(%[[J]])[%[[DIM_1]]]
// CHECK-NEXT:   %[[INPUT_SLICE:.*]] = gml_st.materialize %[[INPUT]]
// CHECK-NEXT:   %[[INIT_SLICE:.*]] = gml_st.materialize %[[MAIN_PAR]]
// CHECK-NEXT:   %[[MAPPED:.*]] = linalg.map
// CHECK-SAME:     ins(%[[INPUT_SLICE]] : tensor<1x?xf32>)
// CHECK-SAME:     outs(%[[INIT_SLICE]] : tensor<1x?xf32>)
// CHECK-NEXT:     (%[[IN_ELEM:.*]]: f32) {
// CHECK-NEXT:       %[[RES_ELEM:.*]] = math.absf %[[IN_ELEM]] : f32
// CHECK-NEXT:       linalg.yield %[[RES_ELEM]] : f32
// CHECK-NEXT:     }
// CHECK-NEXT:   %[[TILE:.*]] = gml_st.tile [%[[I]], %[[J]]]
// CHECK-SAME:                          [1, %[[MAP_DIM]]] [1, 1]
// CHECK-NEXT:   gml_st.set_yield %[[MAPPED]] into %[[MAIN_PAR]][%[[TILE]]]
// CHECK-NEXT: }
// CHECK-NEXT: return %[[RESULT]]

// -----

func.func @map_broadcast_fuse(%arg0: tensor<?xf32>, %arg1: tensor<?x?x?xf32>,
                              %init0: tensor<?xf32>,
                              %init1: tensor<?x?x?xf32>) -> tensor<?x?x?xf32> {
  %abs = linalg.map
         ins(%arg0:tensor<?xf32>)
         outs(%init0:tensor<?xf32>)
         (%input_elem: f32) {
           %0 = math.absf %input_elem: f32
           linalg.yield %0: f32
         }

  %bcast = linalg.broadcast
           ins(%abs : tensor<?xf32>)
           outs(%init1 : tensor<?x?x?xf32>)
           dimensions = [1, 2]

  %mapped = linalg.map
            ins(%bcast, %arg1 : tensor<?x?x?xf32>, tensor<?x?x?xf32>)
            outs(%init1:tensor<?x?x?xf32>)
            (%lhs: f32, %rhs: f32) {
              %0 = arith.addf %lhs, %rhs: f32
              linalg.yield %0: f32
            }

  func.return %mapped : tensor<?x?x?xf32>
}

// CHECK-LABEL: func.func @map_broadcast_fuse(
// CHECK-SAME:      %[[ARG0:[0-9a-zA-Z]*]]: tensor<?xf32>,
// CHECK-SAME:      %[[ARG1:[0-9a-zA-Z]*]]: tensor<?x?x?xf32>,
// CHECK-SAME:      %[[INIT0:[0-9a-zA-Z]*]]: tensor<?xf32>,
// CHECK-SAME:      %[[INIT1:[0-9a-zA-Z]*]]: tensor<?x?x?xf32>)

// CHECK-DAG:   %[[C0:.*]] = arith.constant 0
// CHECK-DAG:   %[[C1:.*]] = arith.constant 1
// CHECK-DAG:   %[[C2:.*]] = arith.constant 2
// CHECK-DAG:   %[[C8:.*]] = arith.constant 8

// CHECK-DAG:  %[[DIM_0:.*]] = tensor.dim %[[ARG0]], %[[C0]]
// CHECK-DAG:  %[[DIM_1:.*]] = tensor.dim %[[INIT1]], %[[C1]]
// CHECK-DAG:  %[[DIM_2:.*]] = tensor.dim %[[INIT1]], %[[C2]]
// CHECK-DAG:  %[[MAP_DIM_2:.*]] = affine.apply {{.*}}%[[DIM_2]]

// CHECK-NEXT: %[[MAIN_PAR:.*]] = gml_st.parallel
// CHECK-SAME:     (%[[MAIN_I:.*]], %[[MAIN_J:.*]], %[[MAIN_K:.*]]) =
// CHECK-SAME:     (%[[C0]], %[[C0]], %[[C0]]) to
// CHECK-SAME:     (%[[DIM_0]], %[[DIM_1]], %[[MAP_DIM_2]])
// CHECK-SAME:     step (%[[C1]], %[[C1]], %[[C8]]) {
// CHECK:        %[[ARG0_SLICE:.*]] = gml_st.materialize %[[ARG0]] [%[[MAIN_I]]]
// CHECK:        %[[INIT0_SLICE:.*]] = gml_st.materialize %[[INIT0]] [%[[MAIN_I]]]

// CHECK:        %[[ABS:.*]] = linalg.map
// CHECK-SAME:     ins(%[[ARG0_SLICE]]
// CHECK-SAME:     outs(%[[INIT0_SLICE]]

// CHECK:        %[[INIT1_SLICE:.*]] = gml_st.materialize %[[INIT1]]
// CHECK-SAME:     [%[[MAIN_I]], %[[MAIN_J]], %[[MAIN_K]]]
// CHECK:        %[[BCAST:.*]] = linalg.broadcast
// CHECK-SAME:     ins(%[[ABS]]
// CHECK-SAME:     outs(%[[INIT1_SLICE]]
// CHECK:        %[[ARG1_SLICE:.*]] = gml_st.materialize %[[ARG1]]
// CHECK-NEXT:   %[[MAPPED:.*]] = linalg.map
// CHECK-SAME:     ins(%[[BCAST]], %[[ARG1_SLICE]] : tensor<1x1x?xf32>
// CHECK-SAME:     outs(%[[INIT1_SLICE]] : tensor<1x1x?xf32>)
// CHECK:        %[[INIT1_TILE:.*]] = gml_st.tile [%[[MAIN_I]], %[[MAIN_J]], %[[MAIN_K]]]
// CHECK-NEXT:   gml_st.set_yield %[[MAPPED]] into %[[INIT1]][%[[INIT1_TILE]]]
// CHECK-NEXT: }

// CHECK-NEXT: %[[RESULT:.*]] = gml_st.parallel
// CHECK-SAME:     (%[[I:.*]], %[[J:.*]], %[[K:.*]]) =
// CHECK-SAME:     (%[[C0]], %[[C0]], %[[MAP_DIM_2]]) to
// CHECK-SAME:     (%[[DIM_0]], %[[DIM_1]], %[[DIM_2]])
// CHECK-SAME:     step (%[[C1]], %[[C1]], %[[C8]]) {
// CHECK:        %[[MAP_DIM:.*]] = affine.apply #{{.*}}(%[[K]])[%[[DIM_2]]]
// CHECK-DAG:    %[[ARG0_SLICE:.*]] = gml_st.materialize %[[ARG0]] [%[[I]]]
// CHECK-DAG:    %[[INIT0_SLICE:.*]] = gml_st.materialize %[[INIT0]] [%[[I]]]

// CHECK:        %[[ABS:.*]] = linalg.map
// CHECK-SAME:     ins(%[[ARG0_SLICE]]
// CHECK-SAME:     outs(%[[INIT0_SLICE]]

// CHECK:        %[[INIT1_SLICE:.*]] = gml_st.materialize %[[MAIN_PAR]]
// CHECK:        %[[BCAST:.*]] = linalg.broadcast
// CHECK-SAME:     ins(%[[ABS]]
// CHECK-SAME:     outs(%[[INIT1_SLICE]]
// CHECK:        %[[ARG1_SLICE:.*]] = gml_st.materialize %[[ARG1]]
// CHECK-NEXT:   %[[MAPPED:.*]] = linalg.map
// CHECK-SAME:     ins(%[[BCAST]], %[[ARG1_SLICE]] : tensor<1x1x?xf32>
// CHECK-SAME:     outs(%[[INIT1_SLICE]] : tensor<1x1x?xf32>)
// CHECK-DAG:    %[[INIT1_TILE:.*]] = gml_st.tile [%[[I]], %[[J]], %[[K]]]
// CHECK:        gml_st.set_yield %[[MAPPED]] into %[[MAIN_PAR]][%[[INIT1_TILE]]]
// CHECK-NEXT: }
// CHECK-NEXT: return %[[RESULT]]

// -----

func.func @map_non_unique_users(%arg: tensor<?x?xf32>,
                              %init: tensor<?x?xf32>) -> tensor<?x?xf32> {

  %exp = linalg.map
         ins(%arg: tensor<?x?xf32>)
         outs(%init: tensor<?x?xf32>)
         (%input1: f32) {
           %0 = math.exp %input1 : f32
           linalg.yield %0: f32
         }

  %mul = linalg.map
         ins(%exp, %exp: tensor<?x?xf32>, tensor<?x?xf32>)
         outs(%init: tensor<?x?xf32>)
         (%input1: f32, %input2: f32) {
           %0 = arith.mulf %input1, %input2 : f32
           linalg.yield %0: f32
         }

  %abs = linalg.map
         ins(%mul: tensor<?x?xf32>)
         outs(%init: tensor<?x?xf32>)
         (%input1: f32) {
           %0 = math.absf %input1 : f32
           linalg.yield %0: f32
         }
  func.return %abs : tensor<?x?xf32>
}

// CHECK-LABEL: func.func @map_non_unique_users(
// CHECK:          gml_st.parallel
// CHECK:            math.exp
// CHECK-NOT:        math.exp
// CHECK:            arith.mulf
// CHECK:            math.absf
// CHECK:          gml_st.parallel
// CHECK:            math.exp
// CHECK-NOT:        math.exp
// CHECK:            arith.mulf
// CHECK:            math.absf

// -----

func.func @fill() -> tensor<16x10xf32> {
  %cst = arith.constant 0.000000e+00 : f32
  %0 = tensor.empty() : tensor<16x10xf32>
  %1 = linalg.fill ins(%cst : f32) outs(%0 : tensor<16x10xf32>) -> tensor<16x10xf32>
  return %1 : tensor<16x10xf32>
}

// CHECK-LABEL: func.func @fill(

// CHECK-DAG:   %[[C0:.*]] = arith.constant 0
// CHECK-DAG:   %[[C1:.*]] = arith.constant 1
// CHECK-DAG:   %[[C8:.*]] = arith.constant 8
// CHECK-DAG:   %[[C10:.*]] = arith.constant 10
// CHECK-DAG:   %[[C16:.*]] = arith.constant 16

// CHECK:      gml_st.parallel (%[[I:.*]], %[[J:.*]]) = (%[[C0]], %[[C0]]) to
// CHECK-SAME:     (%[[C16]], %[[C8]]) step (%[[C1]], %[[C8]]) {
// CHECK:        linalg.fill
// CHECK:        gml_st.set_yield

// CHECK:      gml_st.parallel (%[[I:.*]], %[[J:.*]]) = (%[[C0]], %[[C8]]) to
// CHECK-SAME:     (%[[C16]], %[[C10]]) step (%[[C1]], %[[C8]]) {
// CHECK:        linalg.fill
// CHECK:        gml_st.set_yield
