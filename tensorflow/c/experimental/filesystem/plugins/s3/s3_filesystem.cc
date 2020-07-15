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
#include "tensorflow/c/experimental/filesystem/plugins/s3/s3_filesystem.h"

#include <stdlib.h>
#include <string.h>

#include "tensorflow/c/experimental/filesystem/filesystem_interface.h"
#include "tensorflow/c/tf_status.h"

// Implementation of a filesystem for S3 environments.
// This filesystem will support `s3://` URI schemes.

static void* plugin_memory_allocate(size_t size) { return calloc(1, size); }
static void plugin_memory_free(void* ptr) { free(ptr); }

static void ParseS3Path(const Aws::String& fname, bool object_empty_ok,
                        Aws::String* bucket, Aws::String* object,
                        TF_Status* status) {
  size_t scheme_end = fname.find("://") + 2;
  if (fname.substr(0, scheme_end + 1) != "s3://") {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "S3 path doesn't start with 's3://'.");
    return;
  }

  size_t bucket_end = fname.find("/", scheme_end + 1);
  if (bucket_end == std::string::npos) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "S3 path doesn't contain a bucket name.");
    return;
  }

  *bucket = fname.substr(scheme_end + 1, bucket_end - scheme_end - 1);
  *object = fname.substr(bucket_end + 1);

  if (object->empty() && !object_empty_ok) {
    TF_SetStatus(status, TF_INVALID_ARGUMENT,
                 "S3 path doesn't contain an object name.");
  }
}

static void ShutdownClient(Aws::S3::S3Client* s3_client) {
  if (s3_client != nullptr) {
    delete s3_client;
    Aws::SDKOptions options;
    Aws::ShutdownAPI(options);
  }
}

// SECTION 1. Implementation for `TF_RandomAccessFile`
// ----------------------------------------------------------------------------
namespace tf_random_access_file {

// TODO(vnvo2409): Implement later

}  // namespace tf_random_access_file

// SECTION 2. Implementation for `TF_WritableFile`
// ----------------------------------------------------------------------------
namespace tf_writable_file {

// TODO(vnvo2409): Implement later

}  // namespace tf_writable_file

// SECTION 3. Implementation for `TF_ReadOnlyMemoryRegion`
// ----------------------------------------------------------------------------
namespace tf_read_only_memory_region {

// TODO(vnvo2409): Implement later

}  // namespace tf_read_only_memory_region

// SECTION 4. Implementation for `TF_Filesystem`, the actual filesystem
// ----------------------------------------------------------------------------
namespace tf_s3_filesystem {
S3File::S3File()
    : s3_client(nullptr, ShutdownClient),
      executor(nullptr),
      initialization_lock() {}
void Init(TF_Filesystem* filesystem, TF_Status* status) {
  filesystem->plugin_filesystem = new S3File();
  TF_SetStatus(status, TF_OK, "");
}

void Cleanup(TF_Filesystem* filesystem) {
  auto s3_file = static_cast<S3File*>(filesystem->plugin_filesystem);
  delete s3_file;
}

// TODO(vnvo2409): Implement later

}  // namespace tf_s3_filesystem

static void ProvideFilesystemSupportFor(TF_FilesystemPluginOps* ops,
                                        const char* uri) {
  TF_SetFilesystemVersionMetadata(ops);
  ops->scheme = strdup(uri);
}

void TF_InitPlugin(TF_FilesystemPluginInfo* info) {
  info->plugin_memory_allocate = plugin_memory_allocate;
  info->plugin_memory_free = plugin_memory_free;
  info->num_schemes = 1;
  info->ops = static_cast<TF_FilesystemPluginOps*>(
      plugin_memory_allocate(info->num_schemes * sizeof(info->ops[0])));
  ProvideFilesystemSupportFor(&info->ops[0], "s3");
}
