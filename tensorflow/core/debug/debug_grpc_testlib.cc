/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/debug/debug_grpc_testlib.h"

#include "tensorflow/core/debug/debug_graph_utils.h"
#include "tensorflow/core/debug/debug_io_utils.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/tracing.h"

namespace tensorflow {

namespace test {

::grpc::Status TestEventListenerImpl::SendEvents(
    ::grpc::ServerContext* context,
    ::grpc::ServerReaderWriter< ::tensorflow::EventReply, ::tensorflow::Event>*
        stream) {
  Event event;

  while (stream->Read(&event)) {
    const Summary::Value& val = event.summary().value(0);

    std::vector<string> name_items =
        tensorflow::str_util::Split(val.node_name(), ':');

    const string node_name = name_items[0];
    int32 output_slot = 0;
    tensorflow::strings::safe_strto32(name_items[1], &output_slot);
    const string debug_op = name_items[2];

    const TensorProto& tensor_proto = val.tensor();
    Tensor tensor(tensor_proto.dtype());
    if (!tensor.FromProto(tensor_proto)) {
      return ::grpc::Status::CANCELLED;
    }

    node_names.push_back(node_name);
    output_slots.push_back(output_slot);
    debug_ops.push_back(debug_op);
    debug_tensors.push_back(tensor);
  }

  {
    mutex_lock l(changes_mu_);
    for (size_t i = 0; i < changes_to_enable_.size(); ++i) {
      EventReply event_reply;
      EventReply::DebugOpStateChange* change =
          event_reply.add_debug_op_state_changes();
      change->set_change(changes_to_enable_[i]
                             ? EventReply::DebugOpStateChange::ENABLE
                             : EventReply::DebugOpStateChange::DISABLE);
      change->set_node_name(changes_node_names_[i]);
      change->set_output_slot(changes_output_slots_[i]);
      change->set_debug_op(changes_debug_ops_[i]);
      stream->Write(event_reply);
    }
    changes_to_enable_.clear();
    changes_node_names_.clear();
    changes_output_slots_.clear();
    changes_debug_ops_.clear();
  }

  return ::grpc::Status::OK;
}

void TestEventListenerImpl::ClearReceivedDebugData() {
  node_names.clear();
  output_slots.clear();
  debug_ops.clear();
  debug_tensors.clear();
}

void TestEventListenerImpl::RequestDebugOpStateChangeAtNextStream(
    bool to_enable, const string& node_name, const int32 output_slot,
    const string& debug_op) {
  mutex_lock l(changes_mu_);

  changes_to_enable_.push_back(to_enable);
  changes_node_names_.push_back(node_name);
  changes_output_slots_.push_back(output_slot);
  changes_debug_ops_.push_back(debug_op);
}

void TestEventListenerImpl::RunServer(const int server_port) {
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort(strings::StrCat("localhost:", server_port),
                           ::grpc::InsecureServerCredentials());
  builder.RegisterService(this);
  std::unique_ptr<::grpc::Server> server = builder.BuildAndStart();

  while (!stop_requested_.load()) {
    Env::Default()->SleepForMicroseconds(200 * 1000);
  }
  server->Shutdown();
  stopped_.store(true);
}

void TestEventListenerImpl::StopServer() {
  stop_requested_.store(true);
  while (!stopped_.load()) {
  }
}

}  // namespace test

}  // namespace tensorflow
