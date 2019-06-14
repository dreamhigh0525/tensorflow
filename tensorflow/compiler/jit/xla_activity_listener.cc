/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/jit/xla_activity_listener.h"

#include "absl/synchronization/mutex.h"
#include "tensorflow/core/lib/core/errors.h"

namespace tensorflow {
namespace {
// The list of all registered `XlaActivityListener`s.
struct XlaActivityListenerList {
  absl::Mutex mutex;
  std::vector<std::unique_ptr<XlaActivityListener>> listeners GUARDED_BY(mutex);
};

XlaActivityListenerList* GetXlaActivityListenerList() {
  static XlaActivityListenerList* listener_list = new XlaActivityListenerList;
  return listener_list;
}

template <typename FnTy>
Status ForEachListener(FnTy fn) {
  XlaActivityListenerList* listener_list = GetXlaActivityListenerList();
  absl::ReaderMutexLock reader_lock(&listener_list->mutex);

  for (const std::unique_ptr<XlaActivityListener>& listener :
       listener_list->listeners) {
    TF_RETURN_IF_ERROR(fn(listener.get()));
  }

  return Status::OK();
}

struct GlobalProcessIdMakerStorage {
  absl::Mutex mutex;
  GlobalProcessIdMaker global_process_id_maker GUARDED_BY(mutex);

  // True if we have used the process ID generated by `global_process_id_maker`.
  // We disallow setting the global process id maker once we have broadcasted
  // messages with global_process_id set to "unknown".
  bool has_been_used GUARDED_BY(mutex) = false;
};

GlobalProcessIdMakerStorage* GetGlobalProcessIdMakerStorage() {
  static GlobalProcessIdMakerStorage* global_process_id_maker_storage =
      new GlobalProcessIdMakerStorage;
  return global_process_id_maker_storage;
}

GlobalProcessIdMaker GetGlobalProcessIdMaker() {
  GlobalProcessIdMakerStorage* global_process_id_maker_storage =
      GetGlobalProcessIdMakerStorage();
  {
    absl::ReaderMutexLock reader_lock(&global_process_id_maker_storage->mutex);
    if (global_process_id_maker_storage->has_been_used) {
      return global_process_id_maker_storage->global_process_id_maker;
    }
  }

  {
    absl::WriterMutexLock writer_lock(&global_process_id_maker_storage->mutex);
    global_process_id_maker_storage->has_been_used = true;
  }

  {
    absl::ReaderMutexLock reader_lock(&global_process_id_maker_storage->mutex);
    return global_process_id_maker_storage->global_process_id_maker;
  }
}

absl::string_view GetGlobalProcessId() {
  static std::string* cached_process_id = [&] {
    std::string* result = new std::string;
    GlobalProcessIdMaker maker = GetGlobalProcessIdMaker();
    *result = maker ? maker() : "unknown";
    return result;
  }();
  return *cached_process_id;
}
}  // namespace

Status BroadcastXlaActivity(
    XlaAutoClusteringActivity auto_clustering_activity) {
  auto_clustering_activity.set_global_process_id(
      std::string(GetGlobalProcessId()));
  return ForEachListener([&](XlaActivityListener* listener) {
    return listener->Listen(auto_clustering_activity);
  });
}

Status BroadcastXlaActivity(
    XlaJitCompilationActivity jit_compilation_activity) {
  jit_compilation_activity.set_global_process_id(
      std::string(GetGlobalProcessId()));
  return ForEachListener([&](XlaActivityListener* listener) {
    return listener->Listen(jit_compilation_activity);
  });
}

void RegisterXlaActivityListener(
    std::unique_ptr<XlaActivityListener> listener) {
  XlaActivityListenerList* listener_list = GetXlaActivityListenerList();
  absl::WriterMutexLock writer_lock(&listener_list->mutex);

  listener_list->listeners.push_back(std::move(listener));
}

void SetGlobalProcessIdMaker(GlobalProcessIdMaker global_process_id_maker) {
  GlobalProcessIdMakerStorage* global_process_id_maker_storage =
      GetGlobalProcessIdMakerStorage();
  absl::WriterMutexLock writer_lock(&global_process_id_maker_storage->mutex);
  CHECK(!global_process_id_maker_storage->has_been_used);
  global_process_id_maker_storage->global_process_id_maker =
      std::move(global_process_id_maker);
}

XlaActivityListener::~XlaActivityListener() {}

}  // namespace tensorflow
