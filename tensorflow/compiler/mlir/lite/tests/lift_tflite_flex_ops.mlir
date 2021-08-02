// RUN: tf-opt %s -tfl-lift-tflite-flex-ops | FileCheck %s

// CHECK-LABEL: TfAdd
func @TfAdd(%arg0: tensor<4xf64>, %arg1: tensor<4xf64>) -> tensor<4xf64> {
  %0 = "tfl.custom"(%arg0, %arg1) {
    custom_code = "FlexAdd",
    custom_option = opaque<"tfl", "0x03416464001412034164641A001A002A070A015412023002320000021B171414042801"> : tensor<35xi8>
  } : (tensor<4xf64>, tensor<4xf64>) -> tensor<4xf64>

// CHECK: "tf.Add"(%arg0, %arg1) {T = f64}  : (tensor<4xf64>, tensor<4xf64>) -> tensor<4xf64>
  return %0 : tensor<4xf64>
}



// CHECK-LABEL: TfBatchMatMulV2
func @TfBatchMatMulV2(%arg0: tensor<4x128x2xf32>, %arg1:  tensor<2x1xf32>) -> tensor<4x128x1xf32> {
  %0 = "tfl.custom"(%arg0, %arg1) {
    custom_code = "FlexBatchMatMulV2",
    custom_option = opaque<"tfl", "0x0D42617463684D61744D756C56320038120D42617463684D61744D756C56321A001A002A070A0154120230012A0B0A0561646A5F78120228002A0B0A0561646A5F791202280032000002493B1414042801"> : tensor<81xi8>
  } : (tensor<4x128x2xf32>, tensor<2x1xf32>) -> tensor<4x128x1xf32>

// CHECK: "tf.BatchMatMulV2"(%arg0, %arg1) {T = f32, adj_x = false, adj_y = false} : (tensor<4x128x2xf32>, tensor<2x1xf32>) -> tensor<4x128x1xf32>
  return %0 : tensor<4x128x1xf32>
}


// CHECK-LABEL: TfTensorArrayV3
func @TfTensorArrayV3(%arg0: tensor<i32>) -> tensor<f32> {
  %0:2 = "tfl.custom"(%arg0) {
    custom_code = "FlexTensorArrayV3",
    custom_option = opaque<"tfl", "0x0D54656E736F724172726179563300A8120D54656E736F72417272617956331A002A1E0A186964656E746963616C5F656C656D656E745F736861706573120228012A120A0C64796E616D69635F73697A65120228002A1D0A1174656E736F725F61727261795F6E616D651208120673616D706C652A160A10636C6561725F61667465725F72656164120228012A0B0A056474797065120230012A1B0A0D656C656D656E745F7368617065120A3A08120208081202080132000002B9AB1414042801"> : tensor<193xi8>
  } : (tensor<i32>) -> (tensor<2xi32>, tensor<*xf32>)

// CHECK: "tf.TensorArrayV3"
// CHECK-SAME: : (tensor<i32>) -> (tensor<2x!tf_type.resource>, tensor<f32>)

  %1 = "tfl.cast"(%0#1) : (tensor<*xf32>) -> tensor<f32>
  return %1 : tensor<f32>
}

// CHECK-LABEL: TfParseExample
func @TfParseExample(%arg0: tensor<1x!tf_type.string>) -> (tensor<1x1x!tf_type.string>, tensor<1x1x!tf_type.string>) {
  %0 = "tfl.pseudo_const"() {value = dense<["image/encoded", "image/text"]> : tensor<2x!tf_type.string>} : () -> tensor<2x!tf_type.string>
  %1 = "tfl.pseudo_const"() {value = dense<"image/encoded"> : tensor<1x!tf_type.string>} : () -> tensor<1x!tf_type.string>
  %2 = "tfl.pseudo_const"() {value = dense<"image/text"> : tensor<1x!tf_type.string>} : () -> tensor<1x!tf_type.string>
  %3 = "tfl.pseudo_const"() {value = dense<""> : tensor<1x!tf_type.string>} : () -> tensor<1x!tf_type.string>
  %4:2 = "tfl.custom"(%arg0, %0, %1, %2, %3, %3) {
    custom_code = "FlexParseExample",
    custom_option = opaque<"tfl", "0x0C50617273654578616D706C65008D120C50617273654578616D706C651A001A001A001A001A001A002A0C0A064E64656E7365120218022A1E0A0C64656E73655F736861706573120E0A0C3A04120208013A04120208012A120A0C7370617273655F747970657312020A002A0D0A074E737061727365120218002A100A065464656E736512060A0432020707320E0A0C50617273654578616D706C6500029D901414042801"> : tensor<165xi8>
  } : (
    tensor<1x!tf_type.string>, tensor<2x!tf_type.string>, tensor<1x!tf_type.string>,
    tensor<1x!tf_type.string>, tensor<1x!tf_type.string>, tensor<1x!tf_type.string>
  ) -> (tensor<1x1x!tf_type.string>, tensor<1x1x!tf_type.string>)
  return %4#0, %4#1 : tensor<1x1x!tf_type.string>, tensor<1x1x!tf_type.string>
// CHECK: "tf.ParseExample"(
// CHECK-SAME: operand_segment_sizes = dense<[1, 1, 0, 2, 2]> : vector<5xi32>, result_segment_sizes = dense<[0, 0, 0, 2]>
}

// CHECK-LABEL: FailureOnInvalidOp
func @FailureOnInvalidOp(%arg0: tensor<4xf64>, %arg1: tensor<4xf64>) -> tensor<4xf64> {
  // expected-error@+1 can't find registered TF op for Nop
  %0 = "tfl.custom"(%arg0, %arg1) {
    custom_code = "FlexNop",
    custom_option = opaque<"tfl", "0x034E6F70001412034E6F701A001A002A070A015412023002320000021B171414042801"> : tensor<35xi8>
  } : (tensor<4xf64>, tensor<4xf64>) -> tensor<4xf64>
  return %0 : tensor<4xf64>
}
