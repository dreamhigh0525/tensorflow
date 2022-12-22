/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/runtime/ffi.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "tensorflow/compiler/xla/mlir/runtime/transforms/compilation_pipeline_gpu.h"
#include "tensorflow/compiler/xla/runtime/arguments.h"
#include "tensorflow/compiler/xla/runtime/async_runtime.h"
#include "tensorflow/compiler/xla/runtime/custom_call_registry.h"
#include "tensorflow/compiler/xla/runtime/ffi/ffi_api.h"
#include "tensorflow/compiler/xla/runtime/ffi/ffi_c_api.h"
#include "tensorflow/compiler/xla/runtime/jit_executable.h"
#include "tensorflow/compiler/xla/runtime/results.h"
#include "tensorflow/tsl/platform/test.h"

namespace xla {
namespace runtime {

// Diagnostic engine that appends all emitted diagnostics to the `error` string.
static DiagnosticEngine CollectDiagnostic(std::string* error) {
  DiagnosticEngine diagnostic_engine;
  diagnostic_engine.AddHandler([=](Diagnostic& diagnostic) -> LogicalResult {
    error->append(diagnostic.status().message());
    return success();
  });
  return diagnostic_engine;
}

static absl::StatusOr<JitExecutable> Compile(std::string_view source) {
  JitExecutable::Options opts;
  opts.specialization = JitExecutable::Specialization::kDisabled;
  opts.compiler.symbols_binding = ToSymbolsBinding();
  opts.compiler.register_dialects = RegisterDefaultXlaGpuRuntimeDialects;
  opts.compiler.create_compilation_pipeline =
      [&](xla::runtime::PassManager& passes) {
        CompilationPipelineOptions copts;
        CreateDefaultXlaGpuRuntimeCompilationPipeline(passes, copts);
      };

  return JitExecutable::Instantiate(source, opts, {"test"});
}

static absl::Status CompileAndExecute(std::string_view source,
                                      ArgumentsRef args,
                                      const DynamicCustomCallRegistry& registry,
                                      CustomCall::UserData user_data) {
  absl::StatusOr<JitExecutable> jit_executable = Compile(source);
  if (!jit_executable.ok()) return jit_executable.status();

  AsyncValuePtr<Executable> executable = jit_executable->DefaultExecutable();
  if (executable.IsError())
    return absl::InternalError(executable.GetError().message());

  std::string diagnostic;
  DiagnosticEngine diagnostic_engine = CollectDiagnostic(&diagnostic);

  Executable::ExecuteOpts execute_opts;
  execute_opts.custom_call_registry = &registry;
  execute_opts.diagnostic_engine = &diagnostic_engine;
  execute_opts.custom_call_data = &user_data;
  execute_opts.async_task_runner =
      reinterpret_cast<AsyncTaskRunner*>(0XDEADBEEF);

  // Append emited diagnostic if execution failed.
  auto executed = executable->Execute(args, NoResultConverter{}, execute_opts);
  if (!executed.ok()) {
    return absl::InternalError(
        absl::StrFormat("%s: %s", executed.message(), diagnostic));
  }

  return absl::OkStatus();
}

//===----------------------------------------------------------------------===//

using ffi::FfiStatus;

// When FFI module is instantiated for an Xla runtime executable, it creates a
// state object whose lifetime is bound to the executable, and the state can be
// accessed from exported FFI functions. We use this state object to observe
// side effects of executing exported FFI functions in tests.
struct TestModuleState {
  // Test scalar arguments decoding.
  int32_t i32_arg = 0;

  // Test scalar attributes decoding.
  float f32_attr;
  double f64_attr;
  int32_t i32_attr;
  int64_t i64_attr;

  // Test array attributes decoding.
  std::vector<float> f32_arr_attr;
  std::vector<double> f64_arr_attr;
  std::vector<int32_t> i32_arr_attr;
  std::vector<int64_t> i64_arr_attr;
};

// TestModule is a stateful FFI module with every exported function having
// access to the instance of `TestModuleState`. State is optional, it's ok to
// skip it in the FFI binding if it's not needed.
struct TestModule : public ffi::StatefulModule<TestModuleState> {
  using Base = ffi::StatefulModule<TestModuleState>;

  explicit TestModule(const XLA_FFI_Api* api)
      : Base(api, "ffi-module",
             {{"ffi.attrs_decoding", FFI_AttrsDecoding},
              {"ffi.fill", FFI_Fill}}) {}

  // Creates a new TestModule state for each executable.
  std::unique_ptr<TestModuleState> CreateState() final {
    return std::make_unique<TestModuleState>();
  }

  // Function that tests that we can successfully decode various kinds of
  // attributes attached to custom calls.
  XLA_FFI_DEFINE_FUNCTION(FFI_AttrsDecoding, AttrsDecoding,
                          ffi::Ffi::Bind("ffi.attrs")
                              .State<TestModuleState>()  // state
                              .Attr<float>("f32")
                              .Attr<double>("f64")
                              .Attr<int32_t>("i32")
                              .Attr<int64_t>("i64")
                              .Attr<ffi::Span<const float>>("f32_arr")
                              .Attr<ffi::Span<const double>>("f64_arr")
                              .Attr<ffi::Span<const int32_t>>("i32_arr")
                              .Attr<ffi::Span<const int64_t>>("i64_arr"));

  // Function that tests that we can successfully decode various kinds of
  // arguments passed to custom calls.
  XLA_FFI_DEFINE_FUNCTION(FFI_Fill, Fill,
                          ffi::Ffi::Bind("ffi.fill")
                              .State<TestModuleState>()  // state
                              .Arg<int32_t>()            // arg0
                              .Arg<ffi::BufferArg>()     // arg1
                              .Attr<float>("attr"));

  static FfiStatus AttrsDecoding(TestModuleState* state, float f32, double f64,
                                 int32_t i32, int64_t i64,
                                 ffi::Span<const float> f32_arr,
                                 ffi::Span<const double> f64_arr,
                                 ffi::Span<const int32_t> i32_arr,
                                 ffi::Span<const int64_t> i64_arr);

  static FfiStatus Fill(TestModuleState* state, int32_t arg0,
                        ffi::BufferArg arg1, float attr0);
};

FfiStatus TestModule::AttrsDecoding(TestModuleState* state, float f32,
                                    double f64, int32_t i32, int64_t i64,
                                    ffi::Span<const float> f32_arr,
                                    ffi::Span<const double> f64_arr,
                                    ffi::Span<const int32_t> i32_arr,
                                    ffi::Span<const int64_t> i64_arr) {
  state->f32_attr = f32;
  state->f64_attr = f64;
  state->i32_attr = i32;
  state->i64_attr = i64;
  state->f32_arr_attr.assign(f32_arr.begin(), f32_arr.end());
  state->f64_arr_attr.assign(f64_arr.begin(), f64_arr.end());
  state->i32_arr_attr.assign(i32_arr.begin(), i32_arr.end());
  state->i64_arr_attr.assign(i64_arr.begin(), i64_arr.end());
  return FfiStatus::Ok();
}

FfiStatus TestModule::Fill(TestModuleState* state, int32_t arg0,
                           ffi::BufferArg arg1, float attr0) {
  // Update state to observe side effects.
  state->i32_arg = arg0;

  // Write attribute value into the buffer argument.
  if (arg1.dtype != ffi::PrimitiveType::F32)
    return FfiStatus::InvalidArgument("Unsupported buffer type");
  if (arg1.sizes.size() != 2)
    return FfiStatus::InvalidArgument("Unsupported buffer rank");

  size_t size = arg1.sizes[0] * arg1.sizes[1];
  float* data = reinterpret_cast<float*>(arg1.data);
  std::fill(data, data + size, attr0);

  return FfiStatus::Ok();
}

//----------------------------------------------------------------------------//

static TestModule* RegisterModule() {
  static TestModule* module = new TestModule(GetXlaFfiApi());
  return module;
}

// When test is instantiated it automatically registers FFI module with the XLA
// runtime.
class FfiTest : public ::testing::Test {
 public:
  FfiTest() : module_(*RegisterModule()) { ffi::ExportFfiModules(registry_); }

  TestModule& module() { return module_; }
  DynamicCustomCallRegistry& registry() { return registry_; }

 private:
  TestModule& module_;
  DynamicCustomCallRegistry registry_;
};

TEST_F(FfiTest, ModuleRegistered) {
  std::vector<const Module*> modules = ffi::FfiModules();
  ASSERT_EQ(modules.size(), 1);
  EXPECT_EQ(modules[0]->name(), "ffi-module");
}

TEST_F(FfiTest, ModuleExported) {
  EXPECT_TRUE(registry().Find("ffi.attrs_decoding"));
  EXPECT_TRUE(registry().Find("ffi.fill"));
}

TEST_F(FfiTest, CreateState) {
  auto state = ffi::FfiModulesState::Instantiate();
  ASSERT_TRUE(state.ok());

  ffi::FfiStateVector state_vector = state->state_vector();
  ASSERT_EQ(state_vector.state.size(), 1);
}

TEST_F(FfiTest, AttrsDecoding) {
  absl::string_view source = R"(
    func.func private @attrs_decoding()
      attributes { rt.dynamic, rt.custom_call = "ffi.attrs_decoding" }

    func.func @test() {
      call @attrs_decoding() {
        f32 = 42.0 : f32,
        f64 = 43.0 : f64,
        i32 = 42 : i32,
        i64 = 43 : i64,
        f32_arr = array<f32: 1.0, 2.0, 3.0, 4.0>,
        f64_arr = array<f64: 5.0, 6.0, 7.0, 8.0>,
        i32_arr = array<i32: 1, 2, 3, 4>,
        i64_arr = array<i64: 5, 6, 7, 8>
      } : () -> ()
      return
    }
  )";

  auto state = ffi::FfiModulesState::Instantiate();
  auto state_vector = state->state_vector();
  CustomCall::UserData user_data(&state_vector);

  ASSERT_TRUE(CompileAndExecute(source, {}, registry(), user_data).ok());
  auto* attrs = reinterpret_cast<TestModuleState*>(state_vector.state[0]);

  EXPECT_EQ(attrs->f32_attr, 42.0);
  EXPECT_EQ(attrs->f64_attr, 43.0);
  EXPECT_EQ(attrs->i32_attr, 42);
  EXPECT_EQ(attrs->i64_attr, 43);
  EXPECT_EQ(attrs->f32_arr_attr, std::vector<float>({1.0, 2.0, 3.0, 4.0}));
  EXPECT_EQ(attrs->f64_arr_attr, std::vector<double>({5.0, 6.0, 7.0, 8.0}));
  EXPECT_EQ(attrs->i32_arr_attr, std::vector<int32_t>({1, 2, 3, 4}));
  EXPECT_EQ(attrs->i64_arr_attr, std::vector<int64_t>({5, 6, 7, 8}));
}

TEST_F(FfiTest, ScalarAndBufferArgs) {
  absl::string_view source = R"(
    func.func private @fill(%arg0: i32, %arg1: memref<?x?xf32>)
      attributes { rt.dynamic, rt.custom_call = "ffi.fill" }

    func.func @test(%arg0: memref<?x?xf32>) {
      %0 = arith.constant 42 : i32
      call @fill(%0, %arg0) { attr = 42.0 : f32 } : (i32, memref<?x?xf32>) -> ()
      return
    }
  )";

  // Instantiate state for all registered FFI modules.
  auto state = ffi::FfiModulesState::Instantiate();
  ASSERT_TRUE(state.ok());

  // Add an FFI state vector to the UserData.
  ffi::FfiStateVector state_vector = state->state_vector();
  CustomCall::UserData user_data(&state_vector);
  ASSERT_EQ(state_vector.state.size(), 1);

  // Use vector as buffer storage.
  std::vector<float> buffer(16);

  // Use row major layout.
  std::array<int64_t, 2> sizes = {8, 2};
  std::array<int64_t, 2> strides = {2, 1};

  // Pass a single memref argument to the executable.
  std::vector<MemrefDesc> args;
  args.emplace_back(PrimitiveType::F32, buffer.data(), 0, sizes, strides);

  ASSERT_TRUE(CompileAndExecute(source, args, registry(), user_data).ok());

  // Check that the FFI function updated the corresponding module state.
  auto* state_ptr = reinterpret_cast<TestModuleState*>(state_vector.state[0]);
  EXPECT_EQ(state_ptr->i32_arg, 42);

  // Check that FFI function filled the buffer argument with data.
  EXPECT_EQ(buffer, std::vector<float>(16, 42.0));
}

}  // namespace runtime
}  // namespace xla
