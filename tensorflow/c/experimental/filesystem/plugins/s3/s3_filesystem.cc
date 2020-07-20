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

#include <aws/core/config/AWSProfileConfigLoader.h>
#include <aws/core/utils/FileSystemUtils.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <stdlib.h>
#include <string.h>

#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "tensorflow/c/experimental/filesystem/filesystem_interface.h"
#include "tensorflow/c/experimental/filesystem/plugins/s3/aws_crypto.h"
#include "tensorflow/c/tf_status.h"

// Implementation of a filesystem for S3 environments.
// This filesystem will support `s3://` URI schemes.
constexpr char kS3FileSystemAllocationTag[] = "S3FileSystemAllocation";
constexpr char kS3ClientAllocationTag[] = "S3ClientAllocation";
constexpr int64_t kS3TimeoutMsec = 300000;  // 5 min

constexpr char kExecutorTag[] = "TransferManagerExecutorAllocation";
constexpr int kExecutorPoolSize = 25;

constexpr uint64_t kS3MultiPartUploadChunkSize = 50 * 1024 * 1024;    // 50 MB
constexpr uint64_t kS3MultiPartDownloadChunkSize = 50 * 1024 * 1024;  // 50 MB
constexpr size_t kDownloadRetries = 3;
constexpr size_t kUploadRetries = 3;

constexpr size_t kS3ReadAppendableFileBufferSize = 1024 * 1024;  // 1 MB

static void* plugin_memory_allocate(size_t size) { return calloc(1, size); }
static void plugin_memory_free(void* ptr) { free(ptr); }

static inline void TF_SetStatusFromAWSError(
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error, TF_Status* status) {
  switch (error.GetResponseCode()) {
    case Aws::Http::HttpResponseCode::FORBIDDEN:
      TF_SetStatus(status, TF_FAILED_PRECONDITION,
                   "AWS Credentials have not been set properly. "
                   "Unable to access the specified S3 location");
      break;
    case Aws::Http::HttpResponseCode::REQUESTED_RANGE_NOT_SATISFIABLE:
      TF_SetStatus(status, TF_OUT_OF_RANGE, "Read less bytes than requested");
      break;
    case Aws::Http::HttpResponseCode::NOT_FOUND:
      TF_SetStatus(status, TF_NOT_FOUND, error.GetMessage().c_str());
      break;
    default:
      TF_SetStatus(
          status, TF_UNKNOWN,
          (error.GetExceptionName() + ": " + error.GetMessage()).c_str());
      break;
  }
}

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

static Aws::Client::ClientConfiguration& GetDefaultClientConfig() {
  ABSL_CONST_INIT static absl::Mutex cfg_lock(absl::kConstInit);
  static bool init(false);
  static Aws::Client::ClientConfiguration cfg;

  absl::MutexLock l(&cfg_lock);

  if (!init) {
    const char* endpoint = getenv("S3_ENDPOINT");
    if (endpoint) cfg.endpointOverride = Aws::String(endpoint);
    const char* region = getenv("AWS_REGION");
    // TODO (yongtang): `S3_REGION` should be deprecated after 2.0.
    if (!region) region = getenv("S3_REGION");
    if (region) {
      cfg.region = Aws::String(region);
    } else {
      // Load config file (e.g., ~/.aws/config) only if AWS_SDK_LOAD_CONFIG
      // is set with a truthy value.
      const char* load_config_env = getenv("AWS_SDK_LOAD_CONFIG");
      std::string load_config =
          load_config_env ? absl::AsciiStrToLower(load_config_env) : "";
      if (load_config == "true" || load_config == "1") {
        Aws::String config_file;
        // If AWS_CONFIG_FILE is set then use it, otherwise use ~/.aws/config.
        const char* config_file_env = getenv("AWS_CONFIG_FILE");
        if (config_file_env) {
          config_file = config_file_env;
        } else {
          const char* home_env = getenv("HOME");
          if (home_env) {
            config_file = home_env;
            config_file += "/.aws/config";
          }
        }
        Aws::Config::AWSConfigFileProfileConfigLoader loader(config_file);
        loader.Load();
        auto profiles = loader.GetProfiles();
        if (!profiles["default"].GetRegion().empty())
          cfg.region = profiles["default"].GetRegion();
      }
    }
    const char* use_https = getenv("S3_USE_HTTPS");
    if (use_https) {
      if (use_https[0] == '0')
        cfg.scheme = Aws::Http::Scheme::HTTP;
      else
        cfg.scheme = Aws::Http::Scheme::HTTPS;
    }
    const char* verify_ssl = getenv("S3_VERIFY_SSL");
    if (verify_ssl) {
      if (verify_ssl[0] == '0')
        cfg.verifySSL = false;
      else
        cfg.verifySSL = true;
    }
    // if these timeouts are low, you may see an error when
    // uploading/downloading large files: Unable to connect to endpoint
    int64_t timeout;
    cfg.connectTimeoutMs =
        absl::SimpleAtoi(getenv("S3_CONNECT_TIMEOUT_MSEC"), &timeout)
            ? timeout
            : kS3TimeoutMsec;
    cfg.requestTimeoutMs =
        absl::SimpleAtoi(getenv("S3_REQUEST_TIMEOUT_MSEC"), &timeout)
            ? timeout
            : kS3TimeoutMsec;
    const char* ca_file = getenv("S3_CA_FILE");
    if (ca_file) cfg.caFile = Aws::String(ca_file);
    const char* ca_path = getenv("S3_CA_PATH");
    if (ca_path) cfg.caPath = Aws::String(ca_path);
    init = true;
  }
  return cfg;
};

static void GetS3Client(tf_s3_filesystem::S3File* s3_file) {
  absl::MutexLock l(&s3_file->initialization_lock);

  if (s3_file->s3_client.get() == nullptr) {
    Aws::SDKOptions options;
    options.cryptoOptions.sha256Factory_create_fn = []() {
      return Aws::MakeShared<tf_s3_filesystem::AWSSHA256Factory>(
          tf_s3_filesystem::AWSCryptoAllocationTag);
    };
    options.cryptoOptions.sha256HMACFactory_create_fn = []() {
      return Aws::MakeShared<tf_s3_filesystem::AWSSHA256HmacFactory>(
          tf_s3_filesystem::AWSCryptoAllocationTag);
    };
    options.cryptoOptions.secureRandomFactory_create_fn = []() {
      return Aws::MakeShared<tf_s3_filesystem::AWSSecureRandomFactory>(
          tf_s3_filesystem::AWSCryptoAllocationTag);
    };
    Aws::InitAPI(options);

    // The creation of S3Client disables virtual addressing:
    //   S3Client(clientConfiguration, signPayloads, useVirtualAddressing =
    //   true)
    // The purpose is to address the issue encountered when there is an `.`
    // in the bucket name. Due to TLS hostname validation or DNS rules,
    // the bucket may not be resolved. Disabling of virtual addressing
    // should address the issue. See GitHub issue 16397 for details.
    s3_file->s3_client = Aws::MakeShared<Aws::S3::S3Client>(
        kS3ClientAllocationTag, GetDefaultClientConfig(),
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);
  }
}

static void GetExecutor(tf_s3_filesystem::S3File* s3_file) {
  absl::MutexLock l(&s3_file->initialization_lock);

  if (s3_file->executor.get() == nullptr) {
    s3_file->executor =
        Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>(
            kExecutorTag, kExecutorPoolSize);
  }
}

static void GetTransferManager(
    const Aws::Transfer::TransferDirection& direction,
    tf_s3_filesystem::S3File* s3_file) {
  absl::MutexLock l(&s3_file->initialization_lock);

  if (s3_file->transfer_managers[direction].get() == nullptr) {
    GetS3Client(s3_file);
    GetExecutor(s3_file);
    Aws::Transfer::TransferManagerConfiguration config(s3_file->executor.get());
    config.s3Client = s3_file->s3_client;
    config.bufferSize = s3_file->multi_part_chunk_sizes[direction];
    // must be larger than pool size * multi part chunk size
    config.transferBufferMaxHeapSize =
        (kExecutorPoolSize + 1) * s3_file->multi_part_chunk_sizes[direction];
    s3_file->transfer_managers[direction] =
        Aws::Transfer::TransferManager::Create(config);
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
typedef struct S3File {
  Aws::String bucket;
  Aws::String object;
  std::shared_ptr<Aws::S3::S3Client> s3_client;
  std::shared_ptr<Aws::Transfer::TransferManager> transfer_manager;
  bool use_multi_part_download;
} S3File;

// AWS Streams destroy the buffer (buf) passed, so creating a new
// IOStream that retains the buffer so the calling function
// can control it's lifecycle
class TFS3UnderlyingStream : public Aws::IOStream {
 public:
  using Base = Aws::IOStream;
  TFS3UnderlyingStream(std::streambuf* buf) : Base(buf) {}
  virtual ~TFS3UnderlyingStream() = default;
};

void Cleanup(TF_RandomAccessFile* file) {
  auto s3_file = static_cast<S3File*>(file->plugin_file);
  delete s3_file;
}

static int64_t ReadS3Client(S3File* s3_file, uint64_t offset, size_t n,
                            char* buffer, TF_Status* status) {
  Aws::S3::Model::GetObjectRequest get_object_request;
  get_object_request.WithBucket(s3_file->bucket).WithKey(s3_file->bucket);
  Aws::String bytes =
      absl::StrCat("bytes=", offset, "-", offset + n - 1).c_str();
  get_object_request.SetRange(bytes);
  get_object_request.SetResponseStreamFactory(
      []() { return Aws::New<Aws::StringStream>(kS3FileSystemAllocationTag); });

  auto get_object_outcome = s3_file->s3_client->GetObject(get_object_request);
  if (!get_object_outcome.IsSuccess())
    TF_SetStatusFromAWSError(get_object_outcome.GetError(), status);
  else
    TF_SetStatus(status, TF_OK, "");
  if (TF_GetCode(status) != TF_OK && TF_GetCode(status) != TF_OUT_OF_RANGE)
    return -1;

  int64_t read = get_object_outcome.GetResult().GetContentLength();
  if (read < n)
    TF_SetStatus(status, TF_OUT_OF_RANGE, "Read less bytes than requested");
  get_object_outcome.GetResult().GetBody().read(buffer, read);
  return read;
}

static int64_t ReadS3TransferManager(S3File* s3_file, uint64_t offset, size_t n,
                                     char* buffer, TF_Status* status) {
  auto create_download_stream = [&]() {
    return Aws::New<TFS3UnderlyingStream>(
        "S3ReadStream",
        Aws::New<Aws::Utils::Stream::PreallocatedStreamBuf>(
            "S3ReadStream", reinterpret_cast<unsigned char*>(buffer), n));
  };
  auto handle = s3_file->transfer_manager->DownloadFile(
      s3_file->bucket, s3_file->object, offset, n, create_download_stream);
  handle->WaitUntilFinished();

  size_t retries = 0;
  while (handle->GetStatus() == Aws::Transfer::TransferStatus::FAILED &&
         handle->GetLastError().GetResponseCode() !=
             Aws::Http::HttpResponseCode::REQUESTED_RANGE_NOT_SATISFIABLE &&
         retries++ < kDownloadRetries) {
    // Only failed parts will be downloaded again.
    s3_file->transfer_manager->RetryDownload(handle);
    handle->WaitUntilFinished();
  }

  if (handle->GetStatus() != Aws::Transfer::TransferStatus::COMPLETED)
    TF_SetStatusFromAWSError(handle->GetLastError(), status);
  else
    TF_SetStatus(status, TF_OK, "");
  if (TF_GetCode(status) != TF_OK && TF_GetCode(status) != TF_OUT_OF_RANGE)
    return -1;
  int64_t read = handle->GetBytesTransferred();
  if (read < n)
    TF_SetStatus(status, TF_OUT_OF_RANGE, "Read less bytes than requested");
  return read;
}

int64_t Read(const TF_RandomAccessFile* file, uint64_t offset, size_t n,
             char* buffer, TF_Status* status) {
  auto s3_file = static_cast<S3File*>(file->plugin_file);
  if (s3_file->use_multi_part_download)
    return ReadS3TransferManager(s3_file, offset, n, buffer, status);
  else
    return ReadS3Client(s3_file, offset, n, buffer, status);
}

}  // namespace tf_random_access_file

// SECTION 2. Implementation for `TF_WritableFile`
// ----------------------------------------------------------------------------
namespace tf_writable_file {
typedef struct S3File {
  Aws::String bucket;
  Aws::String object;
  std::shared_ptr<Aws::S3::S3Client> s3_client;
  std::shared_ptr<Aws::Transfer::TransferManager> transfer_manager;
  bool sync_needed;
  std::shared_ptr<Aws::Utils::TempFile> outfile;
  S3File(Aws::String bucket, Aws::String object,
         std::shared_ptr<Aws::S3::S3Client> s3_client,
         std::shared_ptr<Aws::Transfer::TransferManager> transfer_manager)
      : bucket(bucket),
        object(object),
        s3_client(s3_client),
        transfer_manager(transfer_manager),
        outfile(Aws::MakeShared<Aws::Utils::TempFile>(
            kS3FileSystemAllocationTag, nullptr, "_s3_filesystem_XXXXXX",
            std::ios_base::binary | std::ios_base::trunc | std::ios_base::in |
                std::ios_base::out)) {}
} S3File;

void Cleanup(TF_WritableFile* file) {
  auto s3_file = static_cast<S3File*>(file->plugin_file);
  delete s3_file;
}

void Append(const TF_WritableFile* file, const char* buffer, size_t n,
            TF_Status* status) {
  auto s3_file = static_cast<S3File*>(file->plugin_file);
  if (!s3_file->outfile) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION,
                 "The internal temporary file is not writable.");
    return;
  }
  s3_file->sync_needed = true;
  s3_file->outfile->write(buffer, n);
  if (!s3_file->outfile->good())
    TF_SetStatus(status, TF_INTERNAL,
                 "Could not append to the internal temporary file.");
  else
    TF_SetStatus(status, TF_OK, "");
}

int64_t Tell(const TF_WritableFile* file, TF_Status* status) {
  auto s3_file = static_cast<S3File*>(file->plugin_file);
  auto position = static_cast<int64_t>(s3_file->outfile->tellp());
  if (position == -1)
    TF_SetStatus(status, TF_INTERNAL,
                 "tellp on the internal temporary file failed");
  else
    TF_SetStatus(status, TF_OK, "");
  return position;
}

void Sync(const TF_WritableFile* file, TF_Status* status) {
  auto s3_file = static_cast<S3File*>(file->plugin_file);
  if (!s3_file->outfile) {
    TF_SetStatus(status, TF_FAILED_PRECONDITION,
                 "The internal temporary file is not writable.");
    return;
  }
  if (!s3_file->sync_needed) {
    TF_SetStatus(status, TF_OK, "");
    return;
  }
  auto position = static_cast<int64_t>(s3_file->outfile->tellp());
  auto handle = s3_file->transfer_manager->UploadFile(
      s3_file->outfile, s3_file->bucket, s3_file->object,
      "application/octet-stream", Aws::Map<Aws::String, Aws::String>());
  handle->WaitUntilFinished();

  size_t retries = 0;
  while (handle->GetStatus() == Aws::Transfer::TransferStatus::FAILED &&
         retries++ < kUploadRetries) {
    // if multipart upload was used, only the failed parts will be re-sent
    s3_file->transfer_manager->RetryUpload(s3_file->outfile, handle);
    handle->WaitUntilFinished();
  }
  if (handle->GetStatus() != Aws::Transfer::TransferStatus::COMPLETED)
    return TF_SetStatusFromAWSError(handle->GetLastError(), status);
  s3_file->outfile->clear();
  s3_file->outfile->seekp(position);
  s3_file->sync_needed = false;
  TF_SetStatus(status, TF_OK, "");
}

void Flush(const TF_WritableFile* file, TF_Status* status) {
  Sync(file, status);
}

void Close(const TF_WritableFile* file, TF_Status* status) {
  auto s3_file = static_cast<S3File*>(file->plugin_file);
  if (s3_file->outfile) {
    Sync(file, status);
    if (TF_GetCode(status) != TF_OK) return;
    s3_file->outfile.reset();
  }
  TF_SetStatus(status, TF_OK, "");
}

}  // namespace tf_writable_file

// SECTION 3. Implementation for `TF_ReadOnlyMemoryRegion`
// ----------------------------------------------------------------------------
namespace tf_read_only_memory_region {
typedef struct S3MemoryRegion {
  std::unique_ptr<char[]> data;
  uint64_t length;
} S3MemoryRegion;

void Cleanup(TF_ReadOnlyMemoryRegion* region) {
  auto r = static_cast<S3MemoryRegion*>(region->plugin_memory_region);
  delete r;
}

const void* Data(const TF_ReadOnlyMemoryRegion* region) {
  auto r = static_cast<S3MemoryRegion*>(region->plugin_memory_region);
  return reinterpret_cast<const void*>(r->data.get());
}

uint64_t Length(const TF_ReadOnlyMemoryRegion* region) {
  auto r = static_cast<S3MemoryRegion*>(region->plugin_memory_region);
  return r->length;
}

}  // namespace tf_read_only_memory_region

// SECTION 4. Implementation for `TF_Filesystem`, the actual filesystem
// ----------------------------------------------------------------------------
namespace tf_s3_filesystem {
S3File::S3File()
    : s3_client(nullptr, ShutdownClient),
      executor(nullptr),
      transfer_managers(),
      multi_part_chunk_sizes(),
      use_multi_part_download(true),
      initialization_lock() {
  uint64_t temp_value;
  multi_part_chunk_sizes[Aws::Transfer::TransferDirection::UPLOAD] =
      absl::SimpleAtoi(getenv("S3_MULTI_PART_UPLOAD_CHUNK_SIZE"), &temp_value)
          ? temp_value
          : kS3MultiPartUploadChunkSize;
  multi_part_chunk_sizes[Aws::Transfer::TransferDirection::DOWNLOAD] =
      absl::SimpleAtoi(getenv("S3_MULTI_PART_DOWNLOAD_CHUNK_SIZE"), &temp_value)
          ? temp_value
          : kS3MultiPartDownloadChunkSize;
  use_multi_part_download =
      absl::SimpleAtoi(getenv("S3_DISABLE_MULTI_PART_DOWNLOAD"), &temp_value)
          ? (temp_value != 1)
          : use_multi_part_download;
  transfer_managers.emplace(Aws::Transfer::TransferDirection::UPLOAD, nullptr);
  transfer_managers.emplace(Aws::Transfer::TransferDirection::DOWNLOAD,
                            nullptr);
}
void Init(TF_Filesystem* filesystem, TF_Status* status) {
  filesystem->plugin_filesystem = new S3File();
  TF_SetStatus(status, TF_OK, "");
}

void Cleanup(TF_Filesystem* filesystem) {
  auto s3_file = static_cast<S3File*>(filesystem->plugin_filesystem);
  delete s3_file;
}

void NewRandomAccessFile(const TF_Filesystem* filesystem, const char* path,
                         TF_RandomAccessFile* file, TF_Status* status) {
  Aws::String bucket, object;
  ParseS3Path(path, false, &bucket, &object, status);
  if (TF_GetCode(status) != TF_OK) return;

  auto s3_file = static_cast<S3File*>(filesystem->plugin_filesystem);
  GetS3Client(s3_file);
  GetTransferManager(Aws::Transfer::TransferDirection::DOWNLOAD, s3_file);
  file->plugin_file = new tf_random_access_file::S3File(
      {bucket, object, s3_file->s3_client,
       s3_file->transfer_managers[Aws::Transfer::TransferDirection::DOWNLOAD],
       s3_file->use_multi_part_download});
  TF_SetStatus(status, TF_OK, "");
}

void NewWritableFile(const TF_Filesystem* filesystem, const char* path,
                     TF_WritableFile* file, TF_Status* status) {
  Aws::String bucket, object;
  ParseS3Path(path, false, &bucket, &object, status);
  if (TF_GetCode(status) != TF_OK) return;

  auto s3_file = static_cast<S3File*>(filesystem->plugin_filesystem);
  GetS3Client(s3_file);
  GetTransferManager(Aws::Transfer::TransferDirection::UPLOAD, s3_file);
  file->plugin_file = new tf_writable_file::S3File(
      bucket, object, s3_file->s3_client,
      s3_file->transfer_managers[Aws::Transfer::TransferDirection::UPLOAD]);
  TF_SetStatus(status, TF_OK, "");
}

void NewAppendableFile(const TF_Filesystem* filesystem, const char* path,
                       TF_WritableFile* file, TF_Status* status) {
  Aws::String bucket, object;
  ParseS3Path(path, false, &bucket, &object, status);
  if (TF_GetCode(status) != TF_OK) return;

  auto s3_file = static_cast<S3File*>(filesystem->plugin_filesystem);
  GetS3Client(s3_file);
  GetTransferManager(Aws::Transfer::TransferDirection::UPLOAD, s3_file);

  // We need to delete `file->plugin_file` in case of errors.
  std::unique_ptr<TF_WritableFile, void (*)(TF_WritableFile*)> writer(
      file, [](TF_WritableFile* file) {
        if (file != nullptr && file->plugin_file != nullptr) {
          tf_writable_file::Cleanup(file);
        }
      });
  writer->plugin_file = new tf_writable_file::S3File(
      bucket, object, s3_file->s3_client,
      s3_file->transfer_managers[Aws::Transfer::TransferDirection::UPLOAD]);
  TF_SetStatus(status, TF_OK, "");

  // Wraping inside a `std::unique_ptr` to prevent memory-leaking.
  std::unique_ptr<TF_RandomAccessFile, void (*)(TF_RandomAccessFile*)> reader(
      new TF_RandomAccessFile, [](TF_RandomAccessFile* file) {
        if (file != nullptr) {
          tf_random_access_file::Cleanup(file);
          delete file;
        }
      });
  NewRandomAccessFile(filesystem, path, reader.get(), status);
  if (TF_GetCode(status) != TF_OK) return;

  uint64_t offset = 0;
  std::string buffer(kS3ReadAppendableFileBufferSize, {});
  while (true) {
    auto read = tf_random_access_file::Read(reader.get(), offset,
                                            kS3ReadAppendableFileBufferSize,
                                            &buffer[0], status);
    if (TF_GetCode(status) == TF_NOT_FOUND) {
      break;
    } else if (TF_GetCode(status) == TF_OK) {
      offset += read;
      tf_writable_file::Append(file, buffer.c_str(), read, status);
      if (TF_GetCode(status) != TF_OK) return;
    } else if (TF_GetCode(status) == TF_OUT_OF_RANGE) {
      offset += read;
      tf_writable_file::Append(file, buffer.c_str(), read, status);
      if (TF_GetCode(status) != TF_OK) return;
      break;
    } else {
      return;
    }
  }
  writer.release();
  TF_SetStatus(status, TF_OK, "");
}

void Stat(const TF_Filesystem* filesystem, const char* path,
          TF_FileStatistics* stats, TF_Status* status) {
  Aws::String bucket, object;
  ParseS3Path(path, true, &bucket, &object, status);
  if (TF_GetCode(status) != TF_OK) return;
  auto s3_file = static_cast<S3File*>(filesystem->plugin_filesystem);
  GetS3Client(s3_file);

  if (object.empty()) {
    Aws::S3::Model::HeadBucketRequest head_bucket_request;
    head_bucket_request.WithBucket(bucket);
    auto head_bucket_outcome =
        s3_file->s3_client->HeadBucket(head_bucket_request);
    if (!head_bucket_outcome.IsSuccess())
      return TF_SetStatusFromAWSError(head_bucket_outcome.GetError(), status);
    stats->length = 0;
    stats->is_directory = 1;
    stats->mtime_nsec = 0;
    return TF_SetStatus(status, TF_OK, "");
  }

  bool found = false;
  Aws::S3::Model::HeadObjectRequest head_object_request;
  head_object_request.WithBucket(bucket).WithKey(object);
  head_object_request.SetResponseStreamFactory(
      []() { return Aws::New<Aws::StringStream>(kS3FileSystemAllocationTag); });
  auto head_object_outcome =
      s3_file->s3_client->HeadObject(head_object_request);
  if (head_object_outcome.IsSuccess()) {
    stats->length = head_object_outcome.GetResult().GetContentLength();
    stats->is_directory = 0;
    stats->mtime_nsec =
        head_object_outcome.GetResult().GetLastModified().Millis() * 1e6;
    found = true;
  } else {
    return TF_SetStatusFromAWSError(head_object_outcome.GetError(), status);
  }

  auto prefix = object;
  if (prefix.back() != '/') {
    prefix.push_back('/');
  }
  Aws::S3::Model::ListObjectsRequest list_objects_request;
  list_objects_request.WithBucket(bucket).WithPrefix(prefix).WithMaxKeys(1);
  list_objects_request.SetResponseStreamFactory(
      []() { return Aws::New<Aws::StringStream>(kS3FileSystemAllocationTag); });
  auto list_objects_outcome =
      s3_file->s3_client->ListObjects(list_objects_request);
  if (list_objects_outcome.IsSuccess()) {
    auto objects = list_objects_outcome.GetResult().GetContents();
    if (objects.size() > 0) {
      stats->length = 0;
      stats->is_directory = 1;
      stats->mtime_nsec = objects[0].GetLastModified().Millis() * 1e6;
      found = true;
    }
  } else {
    TF_SetStatusFromAWSError(list_objects_outcome.GetError(), status);
    if (TF_GetCode(status) == TF_FAILED_PRECONDITION) return;
  }
  if (!found)
    return TF_SetStatus(
        status, TF_NOT_FOUND,
        absl::StrCat("Object ", path, " does not exist").c_str());
  TF_SetStatus(status, TF_OK, "");
}

void PathExists(const TF_Filesystem* filesystem, const char* path,
                TF_Status* status) {
  TF_FileStatistics stats;
  Stat(filesystem, path, &stats, status);
}

int64_t GetFileSize(const TF_Filesystem* filesystem, const char* path,
                    TF_Status* status) {
  TF_FileStatistics stats;
  Stat(filesystem, path, &stats, status);
  return stats.length;
}

void NewReadOnlyMemoryRegionFromFile(const TF_Filesystem* filesystem,
                                     const char* path,
                                     TF_ReadOnlyMemoryRegion* region,
                                     TF_Status* status) {
  Aws::String bucket, object;
  ParseS3Path(path, true, &bucket, &object, status);
  if (TF_GetCode(status) != TF_OK) return;

  auto s3_file = static_cast<S3File*>(filesystem->plugin_filesystem);
  GetS3Client(s3_file);
  GetTransferManager(Aws::Transfer::TransferDirection::UPLOAD, s3_file);

  auto size = GetFileSize(filesystem, path, status);
  if (TF_GetCode(status) != TF_OK) return;
  if (size == 0)
    return TF_SetStatus(status, TF_INVALID_ARGUMENT, "File is empty");

  std::unique_ptr<char[]> data(new char[size]);
  // Wraping inside a `std::unique_ptr` to prevent memory-leaking.
  std::unique_ptr<TF_RandomAccessFile, void (*)(TF_RandomAccessFile*)> reader(
      new TF_RandomAccessFile, [](TF_RandomAccessFile* file) {
        if (file != nullptr) {
          tf_random_access_file::Cleanup(file);
          delete file;
        }
      });
  NewRandomAccessFile(filesystem, path, reader.get(), status);
  if (TF_GetCode(status) != TF_OK) return;
  auto read =
      tf_random_access_file::Read(reader.get(), 0, size, data.get(), status);
  if (TF_GetCode(status) != TF_OK) return;

  region->plugin_memory_region = new tf_read_only_memory_region::S3MemoryRegion(
      {std::move(data), static_cast<uint64_t>(read)});
  TF_SetStatus(status, TF_OK, "");
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
