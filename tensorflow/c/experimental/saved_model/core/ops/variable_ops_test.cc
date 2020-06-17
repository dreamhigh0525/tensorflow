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

#include "tensorflow/c/experimental/saved_model/core/ops/variable_ops.h"

#include <memory>

#include "tensorflow/c/experimental/saved_model/core/ops/owned_eager_context.h"
#include "tensorflow/c/experimental/saved_model/core/ops/owned_tensor.h"
#include "tensorflow/c/experimental/saved_model/core/ops/owned_tensor_handle.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/eager/context.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace {

class VariableOpsTest : public ::testing::Test {
 public:
  VariableOpsTest()
      : device_mgr_(std::make_unique<StaticDeviceMgr>(DeviceFactory::NewDevice(
            "CPU", {}, "/job:localhost/replica:0/task:0"))),
        ctx_(new EagerContext(
            SessionOptions(),
            tensorflow::ContextDevicePlacementPolicy::DEVICE_PLACEMENT_SILENT,
            tensorflow::ContextMirroringPolicy::MIRRORING_NONE,
            /* async= */ false,
            /* lazy_copy_function_remote_inputs= */ false, device_mgr_.get(),
            /* device_mgr_owned= */ false, /* rendezvous= */ nullptr,
            /* custom_kernel_creator= */ nullptr,
            /* cluster_flr= */ nullptr)) {}

  EagerContext* context() { return ctx_.get(); }

 private:
  std::unique_ptr<StaticDeviceMgr> device_mgr_;
  EagerContextPtr ctx_;
};

// Sanity check for variable creation
TEST_F(VariableOpsTest, CreateVariableSuccessful) {
  // Create a DT_Resource TensorHandle that points to a scalar DT_FLOAT tensor
  AbstractTensorHandlePtr handle;
  TF_EXPECT_OK(internal::CreateUninitializedResourceVariable(
      context(), DT_FLOAT, {}, &handle));
  // The created TensorHandle should be a DT_Resource
  EXPECT_EQ(handle->DataType(), DT_RESOURCE);
}

// Sanity check for variable destruction
TEST_F(VariableOpsTest, DestroyVariableSuccessful) {
  // Create a DT_Resource TensorHandle that points to a scalar DT_FLOAT tensor
  AbstractTensorHandlePtr handle;
  TF_EXPECT_OK(internal::CreateUninitializedResourceVariable(
      context(), DT_FLOAT, {}, &handle));

  // Destroy the variable
  TF_EXPECT_OK(internal::DestroyResource(context(), handle.get()));
}

}  // namespace
}  // namespace tensorflow
