// RUN: mlir-translate -test-spirv-roundtrip %s | FileCheck %s

spv.module "Logical" "GLSL450" {
  // CHECK:       spv.globalVariable @var2 : !spv.ptr<f32, Input>
  // CHECK-NEXT:  spv.globalVariable @var3 : !spv.ptr<f32, Output>
  // CHECK-NEXT:  func @noop({{%.*}}: !spv.ptr<f32, Input>, {{%.*}}: !spv.ptr<f32, Output>)
  // CHECK:       spv.EntryPoint "GLCompute" @noop, @var2, @var3
  spv.globalVariable @var2 : !spv.ptr<f32, Input>
  spv.globalVariable @var3 : !spv.ptr<f32, Output>
  func @noop(%arg0 : !spv.ptr<f32, Input>, %arg1 : !spv.ptr<f32, Output>) -> () {
    spv.Return
  }
  spv.EntryPoint "GLCompute" @noop, @var2, @var3
  spv.ExecutionMode @noop "ContractionOff"
}
