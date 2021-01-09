/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <cmath>
#include <complex>
#include <functional>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/framework/fake_input.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_testutil.h"
#include "tensorflow/core/kernels/mlir_generated/gpu_ops_test_util.h"
#include "tensorflow/core/kernels/ops_testutil.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace {

class GpuUnaryOpTest : public OpsTestBase {
 protected:
  void SetUp() override {
    std::unique_ptr<tensorflow::Device> device_gpu(
        tensorflow::DeviceFactory::NewDevice("GPU", {},
                                             "/job:a/replica:0/task:0"));
    SetDevice(tensorflow::DEVICE_GPU, std::move(device_gpu));
  }

  template <typename T, typename OutT>
  void SetOpKernel(const std::string& op_name, const TensorShape& shape,
                   const absl::InlinedVector<T, 10>& input, bool add_t,
                   bool add_tout) {
    NodeDefBuilder builder("some_name", op_name);
    builder.Input(FakeInput(DataTypeToEnum<T>::v()));
    if (add_t) {
      builder.Attr("T", DataTypeToEnum<T>::v());
    }
    if (add_tout) {
      builder.Attr("Tout", DataTypeToEnum<OutT>::v());
    }
    TF_ASSERT_OK(builder.Finalize(node_def()));

    TF_ASSERT_OK(InitOp());
    AddInputFromArray<T>(shape, input);
  }

  template <typename T, typename OutT>
  void RunAndExpectResult(const std::string& op_name, const TensorShape& shape,
                          const absl::InlinedVector<T, 10>& input,
                          const absl::InlinedVector<OutT, 10>& expected_output,
                          const test::GpuOpsTestConfig& config) {
    SetOpKernel<T, OutT>(op_name, shape, input, config.add_t, config.add_tout);
    TF_ASSERT_OK(RunOpKernel());

    // Assert buffer reuse if expected.
    if (config.expect_buffer_reuse) {
      void* arg_ptr_on_device = context_->input(0).data();
      void* result_ptr_on_device = context_->mutable_output(0)->data();
      ASSERT_EQ(arg_ptr_on_device, result_ptr_on_device);
    }

    // Assert expected results.
    Tensor expected_tensor(allocator(), DataTypeToEnum<OutT>::value, shape);
    test::FillValues<OutT>(&expected_tensor, expected_output);
    if (config.expect_strictly_equal) {
      test::ExpectEqual(expected_tensor, *GetOutput(0));
    } else {
      test::ExpectClose(expected_tensor, *GetOutput(0));
    }
  }

  template <typename T, typename BaselineT, typename OutT,
            typename BaselineOutT>
  void Test(const std::string op_name, const TensorShape& shape,
            absl::InlinedVector<T, 10> input,
            BaselineOutT (*baseline_callback)(BaselineT),
            const test::GpuOpsTestConfig& config) {
    // Prepare inputs and compute expected results.
    auto repeated_input =
        test::RepeatInputToMatchShape(input, shape.num_elements());
    absl::InlinedVector<OutT, 10> expected_output =
        ComputeExpectedOutput<T, BaselineT, OutT, BaselineOutT>(
            repeated_input, baseline_callback);

    RunAndExpectResult<T, OutT>(op_name, shape, repeated_input, expected_output,
                                config);
  }

 private:
  template <typename T, typename BaselineT, typename OutT,
            typename BaselineOutT>
  absl::InlinedVector<OutT, 10> ComputeExpectedOutput(
      absl::InlinedVector<T, 10> input,
      BaselineOutT (*baseline_callback)(BaselineT)) {
    absl::InlinedVector<OutT, 10> expected_output;
    for (int i = 0; i < input.size(); i++) {
      auto arg = static_cast<BaselineT>(input[i]);
      auto result = static_cast<OutT>(baseline_callback(arg));
      expected_output.push_back(result);
    }
    return expected_output;
  }
};

// Macros to easily generate common test cases. For specific inputs, please
// define your own test fixtures.

#define GENERATE_DEFAULT_TEST(op_name, InT, OutT, baseline_callback, config) \
  GENERATE_DEFAULT_TEST_2(op_name, InT, InT, OutT, OutT, baseline_callback,  \
                          config)

#define GENERATE_DEFAULT_TEST_2(op_name, InT, BaselineT, OutT, BaselineOutT, \
                                baseline_callback, config)                   \
  GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES_2(                        \
      op_name, InT, BaselineT, OutT, BaselineOutT,                           \
      test::DefaultInput<NativeT>(), baseline_callback, config)

#define GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(        \
    op_name, InT, OutT, input_values, baseline_callback, config) \
  GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES_2(            \
      op_name, InT, InT, OutT, OutT, input_values, baseline_callback, config)

#define GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES_2(                   \
    op_name, InT, BaselineT, OutT, BaselineOutT, input_values,                \
    baseline_callback, config)                                                \
  TEST_F(GpuUnaryOpTest, op_name##InT) {                                      \
    using NativeT = EnumToDataType<InT>::Type;                                \
    using NativeBaselineT = EnumToDataType<BaselineT>::Type;                  \
    using NativeOutT = EnumToDataType<OutT>::Type;                            \
    using NativeBaselineOutT = EnumToDataType<BaselineOutT>::Type;            \
    Test<NativeT, NativeBaselineT, NativeOutT, NativeBaselineOutT>(           \
        #op_name, test::DefaultInputShape(), input_values, baseline_callback, \
        config);                                                              \
  }

/// Test `tf.Abs`.

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Abs, DT_FLOAT, DT_FLOAT, test::NearZeroAndExtremeInput<float>(), std::abs,
    test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Abs, DT_DOUBLE, DT_DOUBLE, test::NearZeroAndExtremeInput<double>(),
    std::abs, test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES_2(
    Abs, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT,
    test::NearZeroAndExtremeInput<Eigen::half>(), std::abs,
    test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Abs, DT_INT32, DT_INT32, test::NearZeroAndExtremeInput<int32>(), std::abs,
    test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Abs, DT_INT64, DT_INT64, test::NearZeroAndExtremeInput<int64>(), std::abs,
    test::GpuOpsTestConfig().ExpectStrictlyEqual())

/// Test `tf.Ceil`.

GENERATE_DEFAULT_TEST(Ceil, DT_FLOAT, DT_FLOAT, std::ceil,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST(Ceil, DT_DOUBLE, DT_DOUBLE, std::ceil,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST_2(Ceil, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT, std::ceil,
                        test::GpuOpsTestConfig().ExpectStrictlyEqual())

/// Test `tf.Conj`.

template <typename T>
T baseline_conj(T x) {
  return std::conj(x);
}

GENERATE_DEFAULT_TEST(Conj, DT_COMPLEX64, DT_COMPLEX64, baseline_conj,
                      test::GpuOpsTestConfig().NoBufferReuse())

GENERATE_DEFAULT_TEST(Conj, DT_COMPLEX128, DT_COMPLEX128, baseline_conj,
                      test::GpuOpsTestConfig().NoBufferReuse())

/// Test `tf.Cos`.

GENERATE_DEFAULT_TEST(Cos, DT_FLOAT, DT_FLOAT, std::cos,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST(Cos, DT_DOUBLE, DT_DOUBLE, std::cos,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_2(Cos, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT, std::cos,
                        test::GpuOpsTestConfig())

/// Test `tf.Exp`.

GENERATE_DEFAULT_TEST(Exp, DT_FLOAT, DT_FLOAT, std::exp,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST(Exp, DT_DOUBLE, DT_DOUBLE, std::exp,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_2(Exp, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT, std::exp,
                        test::GpuOpsTestConfig())

/// Test `tf.Floor`.

GENERATE_DEFAULT_TEST(Floor, DT_FLOAT, DT_FLOAT, std::floor,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST(Floor, DT_DOUBLE, DT_DOUBLE, std::floor,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST_2(Floor, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT, std::floor,
                        test::GpuOpsTestConfig().ExpectStrictlyEqual())

/// Test `tf.Imag`.

template <typename T>
typename T::value_type baseline_imag(T x) {
  return std::imag(x);
}

GENERATE_DEFAULT_TEST(Imag, DT_COMPLEX64, DT_FLOAT, baseline_imag,
                      test::GpuOpsTestConfig().AddTout().NoBufferReuse())

GENERATE_DEFAULT_TEST(Imag, DT_COMPLEX128, DT_DOUBLE, baseline_imag,
                      test::GpuOpsTestConfig().AddTout().NoBufferReuse())

/// Test `tf.IsInf`.

// TODO(b/162575339): The tests currently still fails with CUDA_ILLEGAL_ADDRESS
// when Test with unranked kernels.
TEST_F(GpuUnaryOpTest, DISABLED_IsInfFloat) {
  Test<float, float, bool, bool>(
      /*op_name=*/"IsInf", test::DefaultInputShape(),
      test::DefaultInput<float>(),
      /*baseline_callback=*/std::isinf,
      test::GpuOpsTestConfig().ExpectStrictlyEqual());
}

TEST_F(GpuUnaryOpTest, DISABLED_IsInfDouble) {
  // Workaround for gcc bug, it would fail with "unresolved overloaded function
  // type" if passing std::isinf with type double. So we use type float for
  // comparing expected values.
  Test<double, float, bool, bool>(
      /*op_name=*/"IsInf", test::DefaultInputShape(),
      test::DefaultInput<double>(),
      /*baseline_callback=*/std::isinf,
      test::GpuOpsTestConfig().ExpectStrictlyEqual());
}

TEST_F(GpuUnaryOpTest, DISABLED_IsInfHalf) {
  Test<Eigen::half, float, bool, bool>(
      /*op_name=*/"IsInf", test::DefaultInputShape(),
      test::DefaultInput<Eigen::half>(),
      /*baseline_callback=*/std::isinf,
      test::GpuOpsTestConfig().ExpectStrictlyEqual());
}

/// Test `tf.Log`.

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Log, DT_FLOAT, DT_FLOAT, test::DefaultInputGreaterThanZero<float>(),
    std::log, test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Log, DT_DOUBLE, DT_DOUBLE, test::DefaultInputGreaterThanZero<double>(),
    std::log, test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES_2(
    Log, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT,
    test::DefaultInputGreaterThanZero<Eigen::half>(), std::log,
    test::GpuOpsTestConfig())

/// Test `tf.LogicalNot`

bool baseline_logical_not(bool x) { return !x; }

GENERATE_DEFAULT_TEST(LogicalNot, DT_BOOL, DT_BOOL, baseline_logical_not,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual().NoT())

/// Test `tf.Neg`.

/// Reference implementation.
template <typename T>
T baseline_neg(T x) {
  return -x;
}

GENERATE_DEFAULT_TEST(Neg, DT_FLOAT, DT_FLOAT, baseline_neg,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST(Neg, DT_DOUBLE, DT_DOUBLE, baseline_neg,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST_2(Neg, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT, baseline_neg,
                        test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST(Neg, DT_INT8, DT_INT8, baseline_neg,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST(Neg, DT_INT16, DT_INT16, baseline_neg,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST(Neg, DT_INT64, DT_INT64, baseline_neg,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

/// Test `tf.Real`.

template <typename T>
typename T::value_type baseline_real(T x) {
  return std::real(x);
}

GENERATE_DEFAULT_TEST(Real, DT_COMPLEX64, DT_FLOAT, baseline_real,
                      test::GpuOpsTestConfig().AddTout().NoBufferReuse())

GENERATE_DEFAULT_TEST(Real, DT_COMPLEX128, DT_DOUBLE, baseline_real,
                      test::GpuOpsTestConfig().AddTout().NoBufferReuse())

/// Test `tf.Rsqrt`.

/// Reference implementation.
template <typename T>
T baseline_rsqrt(T x) {
  return 1.0 / std::sqrt(x);
}

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Rsqrt, DT_FLOAT, DT_FLOAT, test::DefaultInputGreaterThanZero<float>(),
    baseline_rsqrt, test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Rsqrt, DT_DOUBLE, DT_DOUBLE, test::DefaultInputGreaterThanZero<double>(),
    baseline_rsqrt, test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES_2(
    Rsqrt, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT,
    test::DefaultInputGreaterThanZero<Eigen::half>(), baseline_rsqrt,
    test::GpuOpsTestConfig())

/// Test `tf.Sign`.

// Reference implementation
template <typename T>
T baseline_sign(T x) {
  if (x == 0) return 0;
  if (x < 0) return -1;
  return 1;
}

GENERATE_DEFAULT_TEST(Sign, DT_FLOAT, DT_FLOAT, baseline_sign,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

GENERATE_DEFAULT_TEST(Sign, DT_DOUBLE, DT_DOUBLE, baseline_sign,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

// TODO(b/162577610): We should actually use ExpectStrictlyEqual()
// here. This requires returning 0.0 for input -0.0.
GENERATE_DEFAULT_TEST_2(Sign, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT,
                        baseline_sign, test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST(Sign, DT_INT64, DT_INT64, baseline_sign,
                      test::GpuOpsTestConfig().ExpectStrictlyEqual())

/// Test `tf.Sin`.

GENERATE_DEFAULT_TEST(Sin, DT_FLOAT, DT_FLOAT, std::sin,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST(Sin, DT_DOUBLE, DT_DOUBLE, std::sin,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_2(Sin, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT, std::sin,
                        test::GpuOpsTestConfig())

/// Test `tf.Sqrt`.

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Sqrt, DT_FLOAT, DT_FLOAT, test::DefaultInputGreaterOrEqualToZero<float>(),
    std::sqrt, test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES(
    Sqrt, DT_DOUBLE, DT_DOUBLE,
    test::DefaultInputGreaterOrEqualToZero<double>(), std::sqrt,
    test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_WITH_SPECIFIC_INPUT_VALUES_2(
    Sqrt, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT,
    test::DefaultInputGreaterOrEqualToZero<Eigen::half>(), std::sqrt,
    test::GpuOpsTestConfig())

/// Test `tf.Tanh`.

GENERATE_DEFAULT_TEST(Tanh, DT_FLOAT, DT_FLOAT, std::tanh,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST(Tanh, DT_DOUBLE, DT_DOUBLE, std::tanh,
                      test::GpuOpsTestConfig())

GENERATE_DEFAULT_TEST_2(Tanh, DT_HALF, DT_FLOAT, DT_HALF, DT_FLOAT, std::tanh,
                        test::GpuOpsTestConfig())

}  // namespace
}  // end namespace tensorflow
