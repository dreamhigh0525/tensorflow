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

// This pass hoists a `tf_device.launch` body and assigns a `device` attribute
// to each TensorFlow dialect op in the body based on the `device` attribute on
// the `tf_device.launch`. If a TensorFlow dialect op already has a device
// attribute, that attribute will be overwritten with the `tf_device.launch`
// device.
//
// For example:
//   %island:5 = tf_executor.island {
//     %a = "tf.opA"() : () -> tensor<i1>
//     %launch:2 = "tf_device.launch"() ( {
//       %b = "tf.opB"() : () -> tensor<i32>
//       %c = "tf.opC"() : () -> tensor<f32>
//       tf_device.return %c, %b : tensor<f32>, tensor<i32>
//     }) {device = "CPU:0"} : () -> (tensor<f32>, tensor<i32>)
//     %d = "tf.opD"() : () -> tensor<i1>
//     tf_executor.yield %a, %launch#0, %launch#1, %d :
//                       tensor<i1>, tensor<f32>, tensor<i32>, tensor<i1>
//   }
//
// Will be transformed into:
//   %island:5 = tf_executor.island {
//     %a = "tf.opA"() : () -> tensor<i1>
//     %b = "tf.opB"() {device = "CPU:0"} : () -> tensor<i32>
//     %c = "tf.opC"() {device = "CPU:0"} : () -> tensor<f32>
//     %d = "tf.opD"() : () -> tensor<i1>
//     tf_executor.yield %a, %c, %b, %d :
//                       tensor<i1>, tensor<f32>, tensor<i32>, tensor<i1>
//   }

#include "llvm/Support/Casting.h"
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Dialect.h"  // from @llvm-project
#include "mlir/IR/Function.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_device.h"

namespace mlir {
namespace TFDevice {
namespace {
constexpr char kDeviceAttr[] = "device";

struct LaunchToDeviceAttributePass
    : public PassWrapper<LaunchToDeviceAttributePass, FunctionPass> {
  void runOnFunction() override;
};

// Assign all ops in region with specified device from launch.
LogicalResult AssignDevicesInRegion(const Dialect* tf_dialect,
                                    tf_device::LaunchOp launch,
                                    Region& region) {
  auto result = region.walk([&](Operation* op) -> WalkResult {
    if (op->getDialect() != tf_dialect) return WalkResult::advance();

    auto device_attr = op->getAttr(kDeviceAttr);
    if (!device_attr) {
      op->setAttr(kDeviceAttr, launch.deviceAttr());
      return WalkResult::advance();
    }

    if (auto device_str_attr = device_attr.dyn_cast<StringAttr>()) {
      if (device_str_attr.getValue().empty()) {
        op->setAttr(kDeviceAttr, launch.deviceAttr());
        return WalkResult::advance();
      } else if (device_str_attr.getValue() != launch.device()) {
        return launch.emitOpError()
               << "inner op has conflicting 'device' attribute, "
                  "got '"
               << device_str_attr.getValue() << "' but expected '"
               << launch.device() << "'";
      }
    } else {
      return launch.emitOpError()
             << "inner op has bad 'device' attribute, got " << device_attr;
    }

    return WalkResult::advance();
  });

  return failure(result.wasInterrupted());
}

LogicalResult HoistOpsAndAnnotateWithDevice(const Dialect* tf_dialect,
                                            tf_device::LaunchOp launch) {
  // Forward launch inner op results to launch op results.
  launch.replaceAllUsesWith(launch.GetBody().getTerminator()->getOperands());

  // For all inner ops, assign the launch device as a `device` attribute.
  if (failed(AssignDevicesInRegion(tf_dialect, launch, launch.body())))
    return failure();

  // Move all inner ops of the launch to the block containing the launch.
  auto body = launch.GetBody().without_terminator();
  Operation* launch_op = launch.getOperation();
  launch_op->getBlock()->getOperations().splice(
      launch_op->getIterator(), launch.GetBody().getOperations(), body.begin(),
      body.end());

  launch.erase();

  return success();
}

void LaunchToDeviceAttributePass::runOnFunction() {
  const Dialect* tf_dialect = getContext().getLoadedDialect("tf");
  if (!tf_dialect) {
    getOperation().emitError() << "'tf' dialect is not registered";
    return signalPassFailure();
  }

  auto result = getOperation().walk([&tf_dialect](tf_device::LaunchOp launch) {
    if (failed(HoistOpsAndAnnotateWithDevice(tf_dialect, launch)))
      return WalkResult::interrupt();

    return WalkResult::advance();
  });

  if (result.wasInterrupted()) return signalPassFailure();
}

}  // anonymous namespace

std::unique_ptr<OperationPass<FuncOp>> CreateLaunchToDeviceAttributePass() {
  return std::make_unique<LaunchToDeviceAttributePass>();
}

static PassRegistration<LaunchToDeviceAttributePass> pass(
    "tf-launch-to-device-attribute",
    "Hoists and annotates device launch inner ops with associated device "
    "attribute");

}  // namespace TFDevice
}  // namespace mlir
