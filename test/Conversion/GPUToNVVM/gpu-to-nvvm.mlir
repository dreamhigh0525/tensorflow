// RUN: mlir-opt %s -lower-gpu-ops-to-nvvm-ops -split-input-file | FileCheck %s

module attributes {gpu.kernel_module} {
  // CHECK-LABEL: func @gpu_index_ops()
  func @gpu_index_ops()
      attributes { gpu.kernel } {
    // CHECK: = nvvm.read.ptx.sreg.tid.x : !llvm.i32
    %tIdX = "gpu.thread_id"() {dimension = "x"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.tid.y : !llvm.i32
    %tIdY = "gpu.thread_id"() {dimension = "y"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.tid.z : !llvm.i32
    %tIdZ = "gpu.thread_id"() {dimension = "z"} : () -> (index)

    // CHECK: = nvvm.read.ptx.sreg.ntid.x : !llvm.i32
    %bDimX = "gpu.block_dim"() {dimension = "x"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.ntid.y : !llvm.i32
    %bDimY = "gpu.block_dim"() {dimension = "y"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.ntid.z : !llvm.i32
    %bDimZ = "gpu.block_dim"() {dimension = "z"} : () -> (index)

    // CHECK: = nvvm.read.ptx.sreg.ctaid.x : !llvm.i32
    %bIdX = "gpu.block_id"() {dimension = "x"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.ctaid.y : !llvm.i32
    %bIdY = "gpu.block_id"() {dimension = "y"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.ctaid.z : !llvm.i32
    %bIdZ = "gpu.block_id"() {dimension = "z"} : () -> (index)

    // CHECK: = nvvm.read.ptx.sreg.nctaid.x : !llvm.i32
    %gDimX = "gpu.grid_dim"() {dimension = "x"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.nctaid.y : !llvm.i32
    %gDimY = "gpu.grid_dim"() {dimension = "y"} : () -> (index)
    // CHECK: = nvvm.read.ptx.sreg.nctaid.z : !llvm.i32
    %gDimZ = "gpu.grid_dim"() {dimension = "z"} : () -> (index)

    std.return
  }
}

// -----

module attributes {gpu.kernel_module} {
  // CHECK-LABEL: func @gpu_all_reduce_op()
  func @gpu_all_reduce_op()
      attributes { gpu.kernel } {
    %arg0 = constant 1.0 : f32
    // TODO(csigg): Check full IR expansion once lowering has settled.
    // CHECK: nvvm.shfl.sync.bfly
    // CHECK: nvvm.barrier0
    // CHECK: llvm.fadd
    %result = "gpu.all_reduce"(%arg0) ({}) {op = "add"} : (f32) -> (f32)

    std.return
  }
}

// -----

module attributes {gpu.kernel_module} {
  // CHECK-LABEL: func @gpu_all_reduce_region()
  func @gpu_all_reduce_region()
      attributes { gpu.kernel } {
    %arg0 = constant 1 : i32
    // TODO(csigg): Check full IR expansion once lowering has settled.
    // CHECK: nvvm.shfl.sync.bfly
    // CHECK: nvvm.barrier0
    %result = "gpu.all_reduce"(%arg0) ({
    ^bb(%lhs : i32, %rhs : i32):
      %xor = xor %lhs, %rhs : i32
      "gpu.yield"(%xor) : (i32) -> ()
    }) : (i32) -> (i32)

    std.return
  }
}
