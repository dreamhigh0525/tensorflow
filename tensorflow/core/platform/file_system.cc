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

#include <sys/stat.h>

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/lib/gtl/stl_util.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/lib/strings/scanner.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/file_system.h"
#include "tensorflow/core/platform/protobuf.h"

namespace tensorflow {

FileSystem::~FileSystem() {}

string FileSystem::TranslateName(const string& name) const {
  return io::CleanPath(name);
}

Status FileSystem::IsDirectory(const string& name) {
  // Check if path exists.
  if (!FileExists(name)) {
    return Status(tensorflow::error::NOT_FOUND, "Path not found");
  }
  FileStatistics stat;
  TF_RETURN_IF_ERROR(Stat(name, &stat));
  if (stat.is_directory) {
    return Status::OK();
  }
  return Status(tensorflow::error::FAILED_PRECONDITION, "Not a directory");
}

RandomAccessFile::~RandomAccessFile() {}

WritableFile::~WritableFile() {}

FileSystemRegistry::~FileSystemRegistry() {}

void ParseURI(StringPiece remaining, StringPiece* scheme, StringPiece* host,
              StringPiece* path) {
  // 0. Parse scheme
  // Make sure scheme matches [a-zA-Z][0-9a-zA-Z.]*
  // TODO(keveman): Allow "+" and "-" in the scheme.
  if (!strings::Scanner(remaining)
           .One(strings::Scanner::LETTER)
           .Many(strings::Scanner::LETTER_DIGIT_DOT)
           .StopCapture()
           .OneLiteral("://")
           .GetResult(&remaining, scheme)) {
    // If there's no scheme, assume the entire string is a path.
    scheme->clear();
    host->clear();
    *path = remaining;
    return;
  }

  // 1. Parse host
  if (!strings::Scanner(remaining).ScanUntil('/').GetResult(&remaining, host)) {
    // No path, so the rest of the URI is the host.
    *host = remaining;
    path->clear();
    return;
  }

  // 2. The rest is the path
  *path = remaining;
}

string CreateURI(StringPiece scheme, StringPiece host, StringPiece path) {
  if (scheme.empty()) {
    return path.ToString();
  }
  return strings::StrCat(scheme, "://", host, path);
}

}  // namespace tensorflow
