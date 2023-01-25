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
#ifndef TENSORFLOW_CORE_DATA_SERVICE_SNAPSHOT_SNAPSHOT_MANAGER_H_
#define TENSORFLOW_CORE_DATA_SERVICE_SNAPSHOT_SNAPSHOT_MANAGER_H_

#include <memory>
#include <string>
#include <vector>

#include "tensorflow/core/data/service/dispatcher.pb.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/protobuf/snapshot.pb.h"
#include "tensorflow/tsl/platform/env.h"
#include "tensorflow/tsl/platform/status.h"
#include "tensorflow/tsl/platform/statusor.h"

namespace tensorflow {
namespace data {

// A helper used by `DataServiceDispatcherImpl` to manage a call to `Snapshot`.
//
// Two mirrored states are maintained:
// - An in-memory state (objects in the `SnapshotManager` instance).
// - An on-disk state (files in the `SnapshotManager::path_`).
//
// The on-disk state has this structure:
// - snapshot_path
//   - DONE
//   - snapshot.metadata
//   - dataset_def.proto
//   - chunks
//     - chunk_<stream_index>_<chunk_index>
//   - streams
//     - stream_0
//       - DONE
//       - splits
//         - source_0
//           - DONE
//           - split_<local_split_index>_<global_split_index>
//       - uncommitted_chucnks
//         - chunk_<chunk_index>
//       - checkpoints
//         - checkpoint_<chunk_index>
//
class SnapshotManager {
 public:
  // Initiates a new snapshot process, creating a fresh in-memory state and
  // writing an on-disk state to `path`. Returns an error if `path` already
  // exists in the filesystem.
  static tsl::StatusOr<std::unique_ptr<SnapshotManager>> Start(
      const SnapshotRequest& request, Env* env);
  // Resumes an existing snapshot process, reading from the on-disk state in
  // `path` to derive an in-memory state. Returns an error if `path` is in a bad
  // state.
  static tsl::StatusOr<std::unique_ptr<SnapshotManager>> Resume(
      absl::string_view path, Env* env);

  // Handles the work pertaining to this snapshot process for the respective
  // `DispatcherService` API calls:
  // - `WorkerHeartbeat`: Returns a stream assignment for the worker.
  // - `GetSnapshotSplit`: Returns a split assignment for the worker.
  tsl::Status WorkerHeartbeat(const WorkerHeartbeatRequest& request,
                              WorkerHeartbeatResponse& response);
  tsl::Status GetSnapshotSplit(const GetSnapshotSplitRequest& request,
                               GetSnapshotSplitResponse& response);

 private:
  SnapshotManager(absl::string_view path, Env* env) : path_(path), env_(env) {}

  // See `Start` above.
  tsl::Status Start(const SnapshotRequest& request);
  tsl::Status WriteOnDiskSkeleton();
  tsl::Status WriteOnDiskMetadata(const SnapshotRequest& request);

  // See `Resume` above.
  tsl::Status Resume();
  tsl::Status ReadOnDiskMetadata();
  tsl::Status ReadOnDiskStreams();
  tsl::Status ReadOnDiskStream(
      int64_t stream_index, absl::flat_hash_set<int64_t>& global_split_indices);
  tsl::Status ReadOnDiskSource(
      int64_t stream_index, int64_t source_index,
      absl::flat_hash_set<int64_t>& global_split_indices);

  // Returns the id of a newly created stream assigned to the worker.
  tsl::StatusOr<int64_t> CreateNewStream(const std::string& worker_address);

  // The filepath of the on-disk state.
  const std::string path_;
  // A tensorflow environment interface used to write to and read from `path_`.
  tsl::Env* const env_;
  // Distributed snapshot metadata.
  experimental::DistributedSnapshotMetadata metadata_;

  // A split provider for each input source of the dataset being snapshotted.
  std::vector<std::unique_ptr<SplitProvider>> split_providers_;
  int64_t num_sources() const { return split_providers_.size(); }

  struct Stream {
    explicit Stream(int64_t num_sources) : num_assigned_splits(num_sources) {}

    // A counter of assigned splits for each source.
    std::vector<int64_t> num_assigned_splits;
  };

  // All streams for this snapshot.
  std::vector<Stream> streams_;
  // Indices of all "assigned" streams, keyed by worker address. A stream is
  // considered to be assigned if the dispatcher knows of a worker
  // processing the stream and that worker is heartbeating.
  absl::flat_hash_map<std::string, int64_t> assignments_;

  // A counter of assigned aplits for this snapshot.
  int64_t num_assigned_splits_ = 0;
};

}  // namespace data
}  // namespace tensorflow

#endif  // TENSORFLOW_CORE_DATA_SERVICE_SNAPSHOT_SNAPSHOT_MANAGER_H_
