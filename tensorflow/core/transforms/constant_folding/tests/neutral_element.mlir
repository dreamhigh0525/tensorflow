// RUN: tfg-transforms-opt -tfg-constant-folding %s | FileCheck %s


module {
  tfg.graph #tf_type.version<producer = 1010, min_consumer = 0> {
    // CHECK: %[[P:.*]], %[[C:.*]] = Placeholder name("x")
    %Placeholder, %ctl = Placeholder name("x") {dtype = f32, shape = #tf_type.shape<2x2>} : () -> (tensor<2x2xf32>)
    // CHECK: %[[P0:.*]], %[[C0:.*]] = Placeholder name("y")
    %Placeholder_0, %ctl_1 = Placeholder name("y") {dtype = f32, shape = #tf_type.shape<2x2>} : () -> (tensor<2x2xf32>)
    // CHECK: %[[P1:.*]], %[[C1:.*]] = Placeholder name("a")
    %Placeholder_2, %ctl_3 = Placeholder name("a") {dtype = f32, shape = #tf_type.shape<3x2>} : () -> (tensor<3x2xf32>)
    // CHECK: %[[P2:.*]], %[[C2:.*]] = Placeholder name("b")
    %Placeholder_4, %ctl_5 = Placeholder name("b") {dtype = f32, shape = #tf_type.shape<2x3>} : () -> (tensor<2x3xf32>)
    // CHECK: %[[P3:.*]], %[[C3:.*]] = Placeholder name("bias")
    %Placeholder_6, %ctl_7 = Placeholder name("bias") {dtype = f32, shape = #tf_type.shape<2>} : () -> (tensor<2xf32>)
    %Const, %ctl_8 = Const name("zeros_1d") {dtype = f32, value = dense<0.000000e+00> : tensor<2xf32>} : () -> (tensor<2xf32>)
    %Const_9, %ctl_10 = Const name("zeros_const") {dtype = f32, value = dense<0.000000e+00> : tensor<2x2xf32>} : () -> (tensor<2x2xf32>)
    %Const_11, %ctl_12 = Const name("zeros_const_bcast") {dtype = f32, value = dense<0.000000e+00> : tensor<2x2x2xf32>} : () -> (tensor<2x2x2xf32>)
    // CHECK: Const [%[[C]]] name("zeros_like")
    %ZerosLike, %ctl_13 = ZerosLike(%Placeholder) name("zeros_like") {T = f32} : (tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: , %[[CC:.*]] = Const name("Const/Const")
    %Const_14, %ctl_15 = Const name("Const/Const") {dtype = i32, value = dense<2> : tensor<2xi32>} : () -> (tensor<2xi32>)
    // CHECK: , %[[CC1:.*]] = Const name("Const_1/Const")
    %Const_16, %ctl_17 = Const name("Const_1/Const") {dtype = f32, value = dense<0.000000e+00> : tensor<f32>} : () -> (tensor<f32>)
    // CHECK: Const [%[[CC]], %[[CC1]]] name("zeros_fill/const_folded")
    %Fill, %ctl_18 = Fill(%Const_14, %Const_16) name("zeros_fill") {T = f32, index_type = i32} : (tensor<2xi32>, tensor<f32>) -> (tensor<2x2xf32>)
    %Const_19, %ctl_20 = Const name("ones_const") {dtype = f32, value = dense<1.000000e+00> : tensor<2x2xf32>} : () -> (tensor<2x2xf32>)
    %Const_21, %ctl_22 = Const name("ones_const_bcast") {dtype = f32, value = dense<1.000000e+00> : tensor<2x2x2xf32>} : () -> (tensor<2x2x2xf32>)
    // CHECK: Const [%[[C]]] name("ones_like")
    %OnesLike, %ctl_23 = OnesLike(%Placeholder) name("ones_like") {T = f32} : (tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: , %[[CC2:.*]] = Const name("Const_2/Const")
    %Const_24, %ctl_25 = Const name("Const_2/Const") {dtype = i32, value = dense<2> : tensor<2xi32>} : () -> (tensor<2xi32>)
    // CHECK: , %[[CC3:.*]] = Const name("Const_3/Const")
    %Const_26, %ctl_27 = Const name("Const_3/Const") {dtype = f32, value = dense<1.000000e+00> : tensor<f32>} : () -> (tensor<f32>)
    // CHECK: Const [%[[CC2]], %[[CC3]]] name("ones_fill/const_folded")
    %Fill_28, %ctl_29 = Fill(%Const_24, %Const_26) name("ones_fill") {T = f32, index_type = i32} : (tensor<2xi32>, tensor<f32>) -> (tensor<2x2xf32>)
    // CHECK: Const [%[[C]], {{.*}} name("mul1")
    %Mul, %ctl_30 = Mul(%Placeholder, %Fill) name("mul1") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Const {{.*}} name("mul2")
    %Mul_31, %ctl_32 = Mul(%Fill, %Placeholder_0) name("mul2") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: BroadcastTo{{.*}} name("mul1_bcast/broadcastto_shape_0")
    %Mul_33, %ctl_34 = Mul(%Placeholder, %Const_21) name("mul1_bcast") {T = f32} : (tensor<2x2xf32>, tensor<2x2x2xf32>) -> (tensor<2x2x2xf32>)
    // CHECK: BroadcastTo{{.*}} name("mul2_bcast/broadcastto_shape_1")
    %Mul_35, %ctl_36 = Mul(%Const_21, %Placeholder_0) name("mul2_bcast") {T = f32} : (tensor<2x2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2x2xf32>)
    // CHECK: Snapshot{{.*}} name("mul3")
    %Mul_37, %ctl_38 = Mul(%Placeholder, %Fill_28) name("mul3") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Snapshot{{.*}} name("mul4")
    %Mul_39, %ctl_40 = Mul(%Fill_28, %Placeholder_0) name("mul4") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Const{{.*}} name("mul5")
    %MulNoNan, %ctl_41 = MulNoNan(%Placeholder, %Const) name("mul5") {T = f32} : (tensor<2x2xf32>, tensor<2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Const{{.*}} name("mul6")
    %MulNoNan_42, %ctl_43 = MulNoNan(%Const, %Placeholder_0) name("mul6") {T = f32} : (tensor<2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Snapshot{{.*}} name("div1")
    %Div, %ctl_44 = Div(%Placeholder, %Fill_28) name("div1") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Reciprocal{{.*}} name("div2")
    %Div_45, %ctl_46 = Div(%Fill_28, %Placeholder_0) name("div2") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: FloorDiv{{.*}} name("floordiv")
    %FloorDiv, %ctl_47 = FloorDiv(%Placeholder, %Fill_28) name("floordiv") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Const{{.*}} name("matmul1")
    %MatMul, %ctl_48 = MatMul(%Placeholder, %Fill) name("matmul1") {T = f32, transpose_a = false, transpose_b = false} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Const{{.*}} name("matmul2")
    %MatMul_49, %ctl_50 = MatMul(%Fill, %Placeholder_0) name("matmul2") {T = f32, transpose_a = false, transpose_b = false} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Const{{.*}} name("matmul3")
    %MatMul_51, %ctl_52 = MatMul(%Placeholder_2, %Fill) name("matmul3") {T = f32, transpose_a = false, transpose_b = false} : (tensor<3x2xf32>, tensor<2x2xf32>) -> (tensor<3x2xf32>)
    // CHECK: Const{{.*}} name("matmul4")
    %MatMul_53, %ctl_54 = MatMul(%Fill, %Placeholder_4) name("matmul4") {T = f32, transpose_a = false, transpose_b = false} : (tensor<2x2xf32>, tensor<2x3xf32>) -> (tensor<2x3xf32>)
    // CHECK: Snapshot{{.*}} name("add1")
    %Add, %ctl_55 = Add(%Placeholder, %Fill) name("add1") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Snapshot{{.*}} name("add2")
    %Add_56, %ctl_57 = Add(%Fill, %Placeholder_0) name("add2") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: BroadcastTo{{.*}} name("add1_bcast/broadcastto_shape_0")
    %Add_58, %ctl_59 = Add(%Placeholder, %Const_11) name("add1_bcast") {T = f32} : (tensor<2x2xf32>, tensor<2x2x2xf32>) -> (tensor<2x2x2xf32>)
    // CHECK: BroadcastTo{{.*}} name("add2_bcast/broadcastto_shape_1")
    %Add_60, %ctl_61 = Add(%Const_11, %Placeholder_0) name("add2_bcast") {T = f32} : (tensor<2x2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2x2xf32>)
    // CHECK: Snapshot{{.*}} name("bias_add1")
    %BiasAdd, %ctl_62 = BiasAdd(%Placeholder, %Const) name("bias_add1") {T = f32, data_format = "NHWC"} : (tensor<2x2xf32>, tensor<2xf32>) -> (tensor<2x2xf32>)
    // CHECK: BroadcastTo{{.*}} name("bias_add2/broadcastto_shape_1")
    %BiasAdd_63, %ctl_64 = BiasAdd(%Fill, %Placeholder_6) name("bias_add2") {T = f32, data_format = "NHWC"} : (tensor<2x2xf32>, tensor<2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Snapshot{{.*}} name("sub1")
    %Sub, %ctl_65 = Sub(%Placeholder, %Fill) name("sub1") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    // CHECK: Neg{{.*}} name("sub2")
    %Sub_66, %ctl_67 = Sub(%Fill, %Placeholder_0) name("sub2") {T = f32} : (tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<2x2xf32>)
    %Pack, %ctl_68 = Pack(%Mul, %Mul_31, %Mul_37, %Mul_39, %MulNoNan, %MulNoNan_42, %Div, %Div_45, %FloorDiv, %MatMul, %MatMul_49, %Add, %Add_56, %BiasAdd, %BiasAdd_63, %Sub, %Sub_66) name("stack") {N = 17 : i64, T = f32, axis = 0 : i64} : (tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>, tensor<2x2xf32>) -> (tensor<17x2x2xf32>)
  }
}

