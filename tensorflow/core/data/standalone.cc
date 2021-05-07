/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/data/standalone.h"

#include <memory>

#include "absl/memory/memory.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/device_mgr.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/graph_constructor.h"
#include "tensorflow/core/common_runtime/graph_runner.h"
#include "tensorflow/core/common_runtime/process_util.h"
#include "tensorflow/core/common_runtime/rendezvous_mgr.h"
#include "tensorflow/core/data/root_dataset.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/refcount.h"
#include "tensorflow/core/public/version.h"
#include "tensorflow/core/util/ptr_util.h"

namespace tensorflow {
namespace data {
namespace standalone {

namespace {

OpKernelContext::Params CreateParams(
    ProcessFunctionLibraryRuntime* pflr, DeviceMgr* device_mgr,
    std::function<void(std::function<void()>)>* runner) {
  OpKernelContext::Params params;
  params.function_library = pflr->GetFLR("/device:CPU:0");
  params.device = device_mgr->ListDevices()[0];
  params.runner = runner;
  return params;
}

}  // namespace

Status Iterator::GetNext(std::vector<Tensor>* outputs, bool* end_of_input) {
  return iterator_->GetNext(ctx_.get(), outputs, end_of_input);
}

Iterator::Iterator(IteratorBase* iterator, IteratorContext* ctx)
    : iterator_(iterator), ctx_(ctx) {}

Status Dataset::FromGraph(Params params, const GraphDef& graph_def,
                          std::unique_ptr<Dataset>* result) {
  Graph graph(OpRegistry::Global());
  TF_RETURN_IF_ERROR(ImportGraphDef({}, graph_def, &graph, nullptr));

  // Instantiate enough of the TF runtime to run `graph` on a single CPU device.
  auto device_mgr = absl::make_unique<StaticDeviceMgr>(DeviceFactory::NewDevice(
      "CPU", params.session_options, "/job:localhost/replica:0/task:0"));
  Device* device = device_mgr->ListDevices()[0];
  // Clone the `FunctionLibraryDefinition` to extend its lifetime extends beyond
  // the lifetime of `graph`.
  auto flib_def =
      absl::make_unique<FunctionLibraryDefinition>(graph.flib_def());
  auto pflr = absl::make_unique<ProcessFunctionLibraryRuntime>(
      device_mgr.get(), Env::Default(), /*config=*/nullptr,
      TF_GRAPH_DEF_VERSION, flib_def.get(), OptimizerOptions{},
      /*thread_pool=*/nullptr, /*parent=*/nullptr,
      /*session_metadata=*/nullptr,
      Rendezvous::Factory{
          [](const int64, const DeviceMgr* device_mgr, Rendezvous** r) {
            *r = new IntraProcessRendezvous(device_mgr);
            return Status::OK();
          }});

  string fetch_node = "";
  for (const auto& node : graph_def.node()) {
    if (node.op() == "_Retval") {
      fetch_node = node.input(0);
    }
  }
  if (fetch_node.empty()) {
    return errors::NotFound("Failed to find a _Retval op in the given dataset");
  }

  // Run graph up to `output_node` and extract the `DatasetBase` stored in the
  // DT_VARIANT output tensor.
  std::vector<Tensor> outputs;
  GraphRunner graph_runner(device);
  TF_RETURN_IF_ERROR(graph_runner.Run(&graph, pflr->GetFLR("/device:CPU:0"), {},
                                      {fetch_node}, &outputs));
  data::DatasetBase* dataset;
  TF_RETURN_IF_ERROR(GetDatasetFromVariantTensor(outputs[0], &dataset));

  data::DatasetBase* finalized_dataset;
  std::unique_ptr<thread::ThreadPool> pool(
      NewThreadPoolFromSessionOptions(params.session_options));
  std::function<void(std::function<void()>)> runner =
      [&pool](std::function<void()> c) { pool->Schedule(std::move(c)); };
  OpKernelContext::Params op_params =
      CreateParams(pflr.get(), device_mgr.get(), &runner);
  OpKernelContext ctx(&op_params, /*num_outputs=*/0);
  TF_RETURN_IF_ERROR(data::FinalizeDataset(&ctx, dataset, &finalized_dataset));
  core::ScopedUnref unref(finalized_dataset);
  *result = WrapUnique(new Dataset(finalized_dataset, device_mgr.release(),
                                   pflr.release(), flib_def.release(),
                                   pool.release(), std::move(runner)));
  return Status::OK();
}  // static

Status Dataset::MakeIterator(std::unique_ptr<SplitProvider> split_provider,
                             std::unique_ptr<Iterator>* result) {
  // Create an `IteratorContext`, which bundles together the necessary runtime
  // support to create and get elements from an iterator.
  std::unique_ptr<IteratorContext> ctx;
  // NOTE(mrry): In the current API, an `IteratorContext` is always initially
  // created from an `OpKernelContext*`, so we need to create `OpKernelContext`
  // with a valid subset of parameters.
  OpKernelContext::Params op_params =
      CreateParams(pflr_.get(), device_mgr_.get(), &runner_);
  OpKernelContext op_ctx(&op_params, /*num_outputs=*/0);
  IteratorContext::Params params(&op_ctx);
  params.function_handle_cache = function_handle_cache_.get();
  params.resource_mgr = &resource_mgr_;
  params.cancellation_manager = &cancellation_manager_;
  params.split_provider = std::move(split_provider);
  ctx = absl::make_unique<IteratorContext>(std::move(params));

  // Create the iterator from the dataset.
  std::unique_ptr<IteratorBase> iterator;
  TF_RETURN_IF_ERROR(dataset_->MakeIterator(ctx.get(), /*parent=*/nullptr,
                                            "Iterator", &iterator));
  *result = WrapUnique(new Iterator(iterator.release(), ctx.release()));

  return Status::OK();
}

Status Dataset::MakeIterator(std::unique_ptr<Iterator>* result) {
  return MakeIterator(/*split_provider=*/nullptr, result);
}

Status Dataset::MakeSplitProvider(std::unique_ptr<SplitProvider>* result) {
  return dataset_->MakeSplitProvider(result);
}

const DatasetBase* Dataset::Get() const { return dataset_; }

Dataset::Dataset(DatasetBase* dataset, DeviceMgr* device_mgr,
                 ProcessFunctionLibraryRuntime* pflr,
                 FunctionLibraryDefinition* flib_def, thread::ThreadPool* pool,
                 std::function<void(std::function<void()>)> runner)
    : dataset_(dataset),
      device_mgr_(device_mgr),
      flib_def_(flib_def),
      pflr_(pflr),
      pool_(pool),
      runner_(std::move(runner)) {
  dataset_->Ref();
  function_handle_cache_ =
      absl::make_unique<FunctionHandleCache>(pflr_->GetFLR("/device:CPU:0"));
}

Dataset::~Dataset() { dataset_->Unref(); }

}  // namespace standalone
}  // namespace data
}  // namespace tensorflow
