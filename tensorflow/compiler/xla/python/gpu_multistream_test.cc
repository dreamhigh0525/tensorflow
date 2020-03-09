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

#include "tensorflow/compiler/xla/client/executable_build_options.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/python/local_client.h"
#include "tensorflow/compiler/xla/python/nvidia_gpu_device.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/literal_test_util.h"
#include "tensorflow/core/platform/random.h"

namespace xla {
namespace {

// Regression test that verifies that substreams of a multistream GPU
// computation wait for the inputs to be produced before executing.
TEST(GpuMultiStream, Basics) {
  TF_ASSERT_OK_AND_ASSIGN(
      std::shared_ptr<PyLocalClient> client,
      GetNvidiaGpuClient(/*asynchronous=*/true, GpuAllocatorConfig(),
                         /*distributed_client=*/nullptr, /*node_id=*/0));

  std::shared_ptr<Device> device = client->local_devices().at(0);

  int n = 1024;
  Shape shape = ShapeUtil::MakeShape(S32, {n});
  std::vector<int32> inputs(n);
  std::vector<int32> expected_outputs(n);

  XlaBuilder builder("acomputation");
  auto p0 = Parameter(&builder, 0, shape, "param");
  auto p1 = Parameter(&builder, 1, shape, "param");
  Tuple(&builder, {Neg(p0), Neg(p1)});
  TF_ASSERT_OK_AND_ASSIGN(XlaComputation computation, builder.Build());

  ExecutableBuildOptions build_options;
  build_options.mutable_debug_options()->set_xla_gpu_disable_multi_streaming(
      false);
  build_options.mutable_debug_options()->set_xla_gpu_use_random_streams(true);

  TF_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<PyLocalExecutable> executable,
      PyLocalExecutable::CompileForDevices(computation, {}, &build_options,
                                           client, {{device}}));

  int64 dummy_size = 1 << 20;
  std::vector<int32> dummy_inputs(dummy_size);
  Shape dummy_shape = ShapeUtil::MakeShape(S32, {dummy_size});

  for (int i = 0; i < 100; ++i) {
    for (int i = 0; i < n; ++i) {
      inputs[i] = tensorflow::random::New64();
      expected_outputs[i] = -inputs[i];
    }
    // Transfer a large dummy buffer, behind which the inputs to the computation
    // must wait.
    TF_ASSERT_OK_AND_ASSIGN(
        auto dummy_buffer,
        PyLocalBuffer::FromHostBuffer(
            dummy_inputs.data(), dummy_shape, /*force_copy=*/false,
            /*buffer_reference=*/nullptr, client, device));
    TF_ASSERT_OK_AND_ASSIGN(auto in_buffer0,
                            PyLocalBuffer::FromHostBuffer(
                                inputs.data(), shape, /*force_copy=*/false,
                                /*buffer_reference=*/nullptr, client, device));
    TF_ASSERT_OK_AND_ASSIGN(auto in_buffer1,
                            PyLocalBuffer::FromHostBuffer(
                                inputs.data(), shape, /*force_copy=*/false,
                                /*buffer_reference=*/nullptr, client, device));
    // The execution may be enqueued before the transfers complete, requiring
    // adequate device-side synchronization.
    TF_ASSERT_OK_AND_ASSIGN(
        auto out_buffer,
        executable->Execute({in_buffer0.get(), in_buffer1.get()}));

    TF_ASSERT_OK_AND_ASSIGN(auto out_buffers, out_buffer->DestructureTuple());

    TF_ASSERT_OK_AND_ASSIGN(auto out_literal, out_buffers[0]->ToLiteral());
    LiteralTestUtil::ExpectR1Equal<int32>(expected_outputs, *out_literal);
    TF_ASSERT_OK_AND_ASSIGN(out_literal, out_buffers[1]->ToLiteral());
    LiteralTestUtil::ExpectR1Equal<int32>(expected_outputs, *out_literal);
  }
}

}  // namespace
}  // namespace xla
