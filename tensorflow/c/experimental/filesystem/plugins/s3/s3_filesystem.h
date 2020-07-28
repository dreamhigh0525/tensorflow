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
#ifndef TENSORFLOW_C_EXPERIMENTAL_FILESYSTEM_PLUGINS_S3_S3_FILESYSTEM_H_
#define TENSORFLOW_C_EXPERIMENTAL_FILESYSTEM_PLUGINS_S3_S3_FILESYSTEM_H_

#include <aws/core/Aws.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/transfer/TransferManager.h>

#include "absl/synchronization/mutex.h"
#include "tensorflow/c/experimental/filesystem/filesystem_interface.h"
#include "tensorflow/c/tf_status.h"

void ParseS3Path(const Aws::String& fname, bool object_empty_ok,
                 Aws::String* bucket, Aws::String* object, TF_Status* status);

namespace tf_random_access_file {
void Cleanup(TF_RandomAccessFile* file);
int64_t Read(const TF_RandomAccessFile* file, uint64_t offset, size_t n,
             char* buffer, TF_Status* status);
}  // namespace tf_random_access_file

namespace tf_writable_file {
void Cleanup(TF_WritableFile* file);
void Append(const TF_WritableFile* file, const char* buffer, size_t n,
            TF_Status* status);
int64_t Tell(const TF_WritableFile* file, TF_Status* status);
void Sync(const TF_WritableFile* file, TF_Status* status);
void Flush(const TF_WritableFile* file, TF_Status* status);
void Close(const TF_WritableFile* file, TF_Status* status);
}  // namespace tf_writable_file

namespace tf_s3_filesystem {
typedef struct S3File {
  std::shared_ptr<Aws::S3::S3Client> s3_client;
  std::shared_ptr<Aws::Utils::Threading::PooledThreadExecutor> executor;
  // We need 2 `TransferManager`, for multipart upload/download.
  Aws::Map<Aws::Transfer::TransferDirection,
           std::shared_ptr<Aws::Transfer::TransferManager>>
      transfer_managers;
  // Sizes to split objects during multipart upload/download.
  Aws::Map<Aws::Transfer::TransferDirection, uint64_t> multi_part_chunk_sizes;
  bool use_multi_part_download;
  absl::Mutex initialization_lock;
  S3File();
} S3File;

void Init(TF_Filesystem* filesystem, TF_Status* status);
void Cleanup(TF_Filesystem* filesystem);
void NewRandomAccessFile(const TF_Filesystem* filesystem, const char* path,
                         TF_RandomAccessFile* file, TF_Status* status);
void NewWritableFile(const TF_Filesystem* filesystem, const char* path,
                     TF_WritableFile* file, TF_Status* status);
void NewAppendableFile(const TF_Filesystem* filesystem, const char* path,
                       TF_WritableFile* file, TF_Status* status);
int64_t GetFileSize(const TF_Filesystem* filesystem, const char* path,
                    TF_Status* status);
}  // namespace tf_s3_filesystem

#endif  // TENSORFLOW_C_EXPERIMENTAL_FILESYSTEM_PLUGINS_S3_S3_FILESYSTEM_H_
