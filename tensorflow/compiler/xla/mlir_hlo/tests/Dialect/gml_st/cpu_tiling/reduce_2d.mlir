// RUN: mlir-hlo-opt %s --split-input-file --gml-st-cpu-tiling-pipeline \
// RUN: | FileCheck %s

func.func @reduce_static(%input: tensor<100x10xf32>,
                        %output: tensor<10xf32>) -> tensor<10xf32> {
  %res = linalg.reduce { arith.addf }
           ins(%input: tensor<100x10xf32>)
           outs(%output: tensor<10xf32>)
           dimensions = [0]
  return %res : tensor<10xf32>
}
// CHECK-LABEL: @reduce_static

//       CHECK: gml_st.parallel
//       CHECK:   scf.for
//       CHECK:     vector.multi_reduction
//  CHECK-SAME:       : vector<4x4xf32> to vector<4xf32>
//  CHECK-NEXT:     scf.yield %{{.*}} : vector<4xf32>
//       CHECK:   gml_st.set_yield

// -----

func.func @reduce_dynamic(%input: tensor<?x?xf32>,
                      %output: tensor<?xf32>) -> tensor<?xf32> {
  %c0 = arith.constant 0 : index
  %0 = tensor.dim %output, %c0 : tensor<?xf32>
  %1 = tensor.empty(%0) : tensor<?xf32>
  %cst = arith.constant 0.000000e+00 : f32
  %2 = linalg.fill ins(%cst : f32) outs(%1 : tensor<?xf32>) -> tensor<?xf32>
  %res = linalg.reduce { arith.mulf }
           ins(%input: tensor<?x?xf32>)
           outs(%2: tensor<?xf32>)
           dimensions = [1]
  return %res : tensor<?xf32>
}
// CHECK-LABEL: @reduce_dynamic

// CHECK:      gml_st.parallel
// CHECK:        scf.for
// CHECK:          vector.multi_reduction
// CHECK-SAME:       : vector<4x4xf32> to vector<4xf32>
// CHECK-NEXT:     scf.yield %{{.*}} : vector<4xf32>

// CHECK:        scf.for
// CHECK:          vector.multi_reduction
// CHECK-SAME:       : vector<4x1xf32> to vector<4xf32>
// CHECK-NEXT:     scf.yield %{{.*}} : vector<4xf32>
// CHECK:        gml_st.set_yield

// CHECK:      gml_st.parallel
// CHECK:        linalg.fill
// CHECK:        scf.for
// CHECK:          linalg.reduce
// CHECK:          scf.yield
// CHECK:        gml_st.set_yield
