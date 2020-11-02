// RUN: kernel-gen-opt %s --buffer-reuse | FileCheck %s

// CHECK-LABEL: @unique_reuse_output
func @unique_reuse_output() -> (index, memref<2x3xi64>) attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_output = 1 : index
  %result_0 = constant 1 : index
  %result_1 = alloc() : memref<2x3xi64>
  return %result_0, %result_1 : index, memref<2x3xi64>
}

// CHECK-LABEL: @ambiguous_reuse_output
func @ambiguous_reuse_output(%pred : i1)
    -> (memref<2x3xi64>, memref<2x3xi64>) attributes {tf_entry} {
  // CHECK: alloc
  // CHECK: reuse_output = -1
  %mem = alloc() : memref<2x3xi64>
  %other_mem = alloc() : memref<2x3xi64>
  cond_br %pred, ^bb0, ^bb1
^bb0:
  return %mem, %other_mem : memref<2x3xi64>, memref<2x3xi64>
^bb1:
  return %other_mem, %mem : memref<2x3xi64>, memref<2x3xi64>
}

// CHECK-LABEL: @direct_reuse
func @direct_reuse(%not_a_memref : index,
                   %smaller : memref<5xi64>,
                   %greater : memref<7xi64>,
                   %different_element_type : memref<2x3xf32>,
                   %reusable_0 : memref<2x3xi64>,
                   %reusable_1 : memref<6xi64>) -> memref<2x3xi64>
                   attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [4 : index, 5 : index]
  %result = alloc() : memref<2x3xi64>
  return %result : memref<2x3xi64>
}

// CHECK-LABEL: @local_reuse_with_memref_maps
func @local_reuse_with_memref_maps(
    %arg : memref<?xi64, offset: 2, strides: [3]>, %n : index)
    -> memref<?xi64, offset: 2, strides: [3]> attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [0 : index]
  %result = alloc(%n) : memref<?xi64, offset: 2, strides: [3]>
  linalg.generic {
    indexing_maps = [affine_map<(i) -> (i)>, affine_map<(i) -> (i)>],
    iterator_types = ["parallel"]
  } ins(%arg : memref<?xi64, offset: 2, strides: [3]>)
    outs(%result : memref<?xi64, offset: 2, strides: [3]>) {
  ^bb0(%a: i64, %b: i64):
    linalg.yield %a : i64
  }
  return %result : memref<?xi64, offset: 2, strides: [3]>
}

// CHECK-LABEL: @indirect_size_equality
func @indirect_size_equality(%arg0 : memref<?xi64>,
                             %arg1 : memref<?xi64>,
                             %n : index) -> memref<?xi64>
                             attributes {tf_entry} {
  // arg0 and arg1 are equal in size.
  linalg.generic {
    indexing_maps = [affine_map<(d0) -> (d0)>, affine_map<(d0) -> (d0)>],
    iterator_types = ["parallel"]
  } ins(%arg0 : memref<?xi64>) outs(%arg1 : memref<?xi64>) {
  ^bb0(%a: i64, %b: i64):
    linalg.yield %a : i64
  }

  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [0 : index, 1 : index]
  %result = alloc(%n) : memref<?xi64>

  // arg0 and result are equal in size.
  linalg.generic {
    indexing_maps = [affine_map<(d0) -> (d0)>, affine_map<(d0) -> (d0)>],
    iterator_types = ["parallel"]
  } ins(%arg0 : memref<?xi64>) outs(%result : memref<?xi64>) {
  ^bb0(%a: i64, %b: i64):
    linalg.yield %a : i64
  }

  return %result : memref<?xi64>
}

// CHECK-LABEL: @livetimes_incompatible
func @livetimes_incompatible(%arg0 : memref<3xi64>)
    -> memref<3xi64> attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = []
  %result = alloc() : memref<3xi64>

  // Use newly allocated buffer.
  %c0 = constant 0 : index
  %0 = load %result[%c0] : memref<3xi64>

  // Use argument buffer again.
  %1 = load %arg0[%c0] : memref<3xi64>

  return %result : memref<3xi64>
}

// CHECK-LABEL: @never_used
func @never_used(%arg0 : memref<3xi64>) -> memref<3xi64> attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [0 : index]
  %result = alloc() : memref<3xi64>
  %c0 = constant 0 : index
  %0 = load %arg0[%c0] : memref<3xi64>
  return %result : memref<3xi64>
}

// CHECK-LABEL: @branching_reuse
func @branching_reuse(%pred : i1, %arg : memref<6xi64>) -> memref<6xi64>
    attributes {tf_entry} {
  cond_br %pred, ^bb0, ^bb1
^bb0:
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [1 : index]
  %mem0 = alloc() : memref<6xi64>

  // Keep buffer argument live in this branch. Reuse is still possible because
  // the newly allocated buffer was not used yet.
  %c0 = constant 0 : index
  load %arg[%c0] : memref<6xi64>

  br ^bb2(%mem0 : memref<6xi64>)
^bb1:
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [1 : index]
  %mem1 = alloc() : memref<6xi64>
  br ^bb2(%mem1 : memref<6xi64>)
^bb2(%result : memref<6xi64>):
  return %result : memref<6xi64>
}

// CHECK-LABEL: @branching_no_reuse
func @branching_no_reuse(%pred : i1, %arg : memref<6xi64>) -> memref<6xi64>
    attributes {tf_entry} {
  cond_br %pred, ^bb0, ^bb1
^bb0:
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = []
  %mem0 = alloc() : memref<6xi64>

  // Use newly allocated memory immediately.
  %c0 = constant 0 : index
  load %mem0[%c0] : memref<6xi64>

  // Keep buffer argument live in this branch and prevent reuse.
  load %arg[%c0] : memref<6xi64>

  br ^bb2(%mem0 : memref<6xi64>)
^bb1:
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [1 : index]
  %mem1 = alloc() : memref<6xi64>
  br ^bb2(%mem1 : memref<6xi64>)
^bb2(%result : memref<6xi64>):
  return %result : memref<6xi64>
}

// CHECK-LABEL: @branching_reuse_if
func @branching_reuse_if(%pred : i1, %arg : memref<6xi64>)
    -> memref<6xi64> attributes {tf_entry} {
  %result = scf.if %pred -> (memref<6xi64>) {
    // CHECK: alloc
    // CHECK-SAME: reuse_input_candidates = [1 : index]
    %mem0 = alloc() : memref<6xi64>

    // Keep buffer argument live in this branch. Reuse is still possible because
    // the newly allocated buffer was not used yet.
    %c0 = constant 0 : index
    load %arg[%c0] : memref<6xi64>

    scf.yield %mem0 : memref<6xi64>
  } else {
    // CHECK: alloc
    // CHECK-SAME: reuse_input_candidates = [1 : index]
    %mem1 = alloc() : memref<6xi64>
    scf.yield %mem1 : memref<6xi64>
  }
  return %result : memref<6xi64>
}

// CHECK-LABEL: @branching_no_reuse_if
func @branching_no_reuse_if(%pred : i1, %arg : memref<6xi64>) -> memref<6xi64>
    attributes {tf_entry} {
  %result = scf.if %pred -> (memref<6xi64>) {
    // CHECK: alloc
    // CHECK-SAME: reuse_input_candidates = []
    %mem0 = alloc() : memref<6xi64>

    // Use newly allocated memory immediately.
    %c0 = constant 0 : index
    load %mem0[%c0] : memref<6xi64>

    // Keep buffer argument live in this branch and prevent reuse.
    load %arg[%c0] : memref<6xi64>

    scf.yield %mem0 : memref<6xi64>
  } else {
    // CHECK: alloc
    // CHECK-SAME: reuse_input_candidates = [1 : index]
    %mem1 = alloc() : memref<6xi64>
    scf.yield %mem1 : memref<6xi64>
  }
  return %result : memref<6xi64>
}

// CHECK-LABEL: @alloc_before_branching
// New buffer is first used in the blocks succeeding its allocation block. In
// both/all cases the newly allocated buffer is used after the buffer argument
// is no longer live. Because these first uses are not block-local the analysis
// does not detect this case (yet). It is correct but incomplete.
func @alloc_before_branching(%pred : i1, %arg : memref<6xi64>) -> memref<6xi64>
    attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = []
  %mem = alloc() : memref<6xi64>
  %c0 = constant 0 : index
  cond_br %pred, ^bb0, ^bb1
^bb0:
  // Last use of `arg` before first use of `mem` (can reuse).
  load %arg[%c0] : memref<6xi64>
  load %mem[%c0] : memref<6xi64>
  return %mem : memref<6xi64>
^bb1:
  // Last use of `arg` before first use of `mem` (can reuse).
  load %arg[%c0] : memref<6xi64>
  load %mem[%c0] : memref<6xi64>
  return %mem : memref<6xi64>
}

// CHECK-LABEL: @alloc_before_branching_2
func @alloc_before_branching_2(%pred : i1, %arg : memref<6xi64>)
    -> memref<6xi64> attributes {tf_entry} {
  // CHECK: alloc()
  // CHECK-SAME: reuse_input_candidates = []
  %mem = alloc() : memref<6xi64>
  %c0 = constant 0 : index
  cond_br %pred, ^bb0, ^bb1
^bb0:
  // Last use of `arg` after first use of `mem` (cannot reuse).
  load %mem[%c0] : memref<6xi64>
  load %arg[%c0] : memref<6xi64>
  return %mem : memref<6xi64>
^bb1:
  // Last use of `arg` before first use of `mem` (can reuse).
  load %arg[%c0] : memref<6xi64>
  load %mem[%c0] : memref<6xi64>
  return %mem : memref<6xi64>
}

// CHECK-LABEL: @alloc_before_branching_if
// New buffer is first used in the blocks succeeding its allocation block. In
// both/all cases the newly allocated buffer is used after the buffer argument
// is no longer live. Because these first uses are not block-local the analysis
// does not detect this case (yet). It is correct but incomplete.
func @alloc_before_branching_if(%pred : i1, %arg : memref<6xi64>) -> memref<6xi64>
    attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = []
  %mem = alloc() : memref<6xi64>
  %result = scf.if %pred -> (memref<6xi64>) {
    // Last use of `arg` before first use of `mem` (can reuse).
    %c0 = constant 0 : index
    load %arg[%c0] : memref<6xi64>
    load %mem[%c0] : memref<6xi64>
    scf.yield %mem : memref<6xi64>
  } else {
    // Last use of `arg` before first use of `mem` (can reuse).
    %c0 = constant 0 : index
    load %arg[%c0] : memref<6xi64>
    load %mem[%c0] : memref<6xi64>
    scf.yield %mem : memref<6xi64>
  }
  return %result : memref<6xi64>
}

// CHECK-LABEL: @alloc_before_branching_2_if
func @alloc_before_branching_2_if(%pred : i1, %arg : memref<6xi64>)
    -> memref<6xi64> attributes {tf_entry} {
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = []
  %mem = alloc() : memref<6xi64>
  %result = scf.if %pred -> (memref<6xi64>) {
    // Last use of `arg` after first use of `mem` (cannot reuse).
    %c0 = constant 0 : index
    load %mem[%c0] : memref<6xi64>
    load %arg[%c0] : memref<6xi64>
    scf.yield %mem : memref<6xi64>
  } else {
    // Last use of `arg` before first use of `mem` (can reuse).
    %c0 = constant 0 : index
    load %arg[%c0] : memref<6xi64>
    load %mem[%c0] : memref<6xi64>
    scf.yield %mem : memref<6xi64>
  }
  return %result : memref<6xi64>
}

// CHECK-LABEL: @abs_unranked_i64
func @abs_unranked_i64(%arg : memref<*xi64>,
                       %arg_shape : memref<?xindex>,
                       %flat_shape : memref<1xindex>,
                       %arg_size : index) -> memref<*xi64>
                       attributes {tf_entry} {
  %flat_arg = lmhlo.reshape_memref_cast %arg(%flat_shape)
      : (memref<*xi64>, memref<1xindex>) -> memref<?xi64>
  // CHECK: alloc
  // CHECK-SAME: reuse_input_candidates = [0 : index], reuse_output = 0 : index
  %flat_result = alloc(%arg_size) : memref<?xi64>
  linalg.generic {
    indexing_maps = [affine_map<(d0) -> (d0)>, affine_map<(d0) -> (d0)>],
    iterator_types = ["parallel"]
  } ins(%flat_arg : memref<?xi64>) outs(%flat_result : memref<?xi64>) {
  ^bb0(%a: i64, %b: i64):
    %c0 = constant 0 : i64
    %a_pos = cmpi "sge", %a, %c0 : i64
    %a_neg = subi %c0, %a : i64
    %a_abs = select %a_pos, %a, %a_neg : i64
    linalg.yield %a_abs : i64
  }
  %result = lmhlo.reshape_memref_cast %flat_result(%arg_shape)
      : (memref<?xi64>, memref<?xindex>) -> memref<*xi64>
  return %result : memref<*xi64>
}
