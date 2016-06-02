/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_STREAM_EXECUTOR_EXECUTOR_CACHE_H_
#define TENSORFLOW_STREAM_EXECUTOR_EXECUTOR_CACHE_H_

#include "tensorflow/stream_executor/lib/status.h"
#include "tensorflow/stream_executor/lib/statusor.h"
#include "tensorflow/stream_executor/stream_executor_pimpl.h"

namespace perftools {
namespace gputools {

// Utility class to allow Platform objects to manage cached StreamExecutors.
class ExecutorCache {
 public:
  ExecutorCache() {}

  // Inserts a new StreamExecutor with the given configuration into the cache.
  // Will not overwrite if called when a matching element is already present.
  port::Status Insert(const StreamExecutorConfig& config,
                      std::unique_ptr<StreamExecutor> executor);

  // Returns a pointer to the described executor (if one with a matching config
  // has been created), or a NOT_FOUND status.
  port::StatusOr<StreamExecutor*> Get(const StreamExecutorConfig& config);

  // Destroys all Executors and clears the cache.
  // Performs no synchronization - undefined behavior may occur if any executors
  // are active!
  void DestroyAllExecutors();

 private:
  typedef std::pair<StreamExecutorConfig, std::unique_ptr<StreamExecutor>>
      Entry;

  // Maps ordinal number to a list of cached executors for that ordinal.
  // We key off of ordinal (instead of just looking up all fields in the
  // StreamExecutorConfig) for a slight improvement in lookup time.
  std::map<int, std::vector<Entry>> cache_;

  SE_DISALLOW_COPY_AND_ASSIGN(ExecutorCache);
};

}  // namespace gputools
}  // namespace perftools

#endif  // TENSORFLOW_STREAM_EXECUTOR_EXECUTOR_CACHE_H_
