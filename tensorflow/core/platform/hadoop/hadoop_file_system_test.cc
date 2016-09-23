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

#include "tensorflow/core/platform/hadoop/hadoop_file_system.h"

#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/lib/gtl/stl_util.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/file_system.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace {

class HadoopFileSystemTest : public ::testing::Test {
 protected:
  HadoopFileSystemTest() {}

  Status WriteString(const string& fname, const string& content) {
    std::unique_ptr<WritableFile> writer;
    TF_RETURN_IF_ERROR(hdfs.NewWritableFile(fname, &writer));
    TF_RETURN_IF_ERROR(writer->Append(content));
    TF_RETURN_IF_ERROR(writer->Close());
    return Status::OK();
  }

  Status ReadAll(const string& fname, string* content) {
    std::unique_ptr<RandomAccessFile> reader;
    TF_RETURN_IF_ERROR(hdfs.NewRandomAccessFile(fname, &reader));

    uint64 file_size = 0;
    TF_RETURN_IF_ERROR(hdfs.GetFileSize(fname, &file_size));

    content->resize(file_size);
    StringPiece result;
    TF_RETURN_IF_ERROR(
        reader->Read(0, file_size, &result, gtl::string_as_array(content)));
    if (file_size != result.size()) {
      return errors::DataLoss("expected ", file_size, " got ", result.size(),
                              " bytes");
    }
    return Status::OK();
  }

  HadoopFileSystem hdfs;
};

TEST_F(HadoopFileSystemTest, RandomAccessFile) {
  const string fname =
      "file://" + io::JoinPath(testing::TmpDir(), "RandomAccessFile");
  const string content = "abcdefghijklmn";
  TF_ASSERT_OK(WriteString(fname, content));

  std::unique_ptr<RandomAccessFile> reader;
  TF_EXPECT_OK(hdfs.NewRandomAccessFile(fname, &reader));

  string got;
  got.resize(content.size());
  StringPiece result;
  TF_EXPECT_OK(
      reader->Read(0, content.size(), &result, gtl::string_as_array(&got)));
  EXPECT_EQ(content.size(), result.size());
  EXPECT_EQ(content, result);

  got.clear();
  got.resize(4);
  TF_EXPECT_OK(reader->Read(2, 4, &result, gtl::string_as_array(&got)));
  EXPECT_EQ(4, result.size());
  EXPECT_EQ(content.substr(2, 4), result);
}

TEST_F(HadoopFileSystemTest, WritableFile) {
  std::unique_ptr<WritableFile> writer;
  const string fname =
      "file://" + io::JoinPath(testing::TmpDir(), "WritableFile");
  TF_EXPECT_OK(hdfs.NewWritableFile(fname, &writer));
  TF_EXPECT_OK(writer->Append("content1,"));
  TF_EXPECT_OK(writer->Append("content2"));
  TF_EXPECT_OK(writer->Flush());
  TF_EXPECT_OK(writer->Sync());
  TF_EXPECT_OK(writer->Close());

  string content;
  TF_EXPECT_OK(ReadAll(fname, &content));
  EXPECT_EQ("content1,content2", content);
}

TEST_F(HadoopFileSystemTest, FileExists) {
  const string fname =
      "file://" + io::JoinPath(testing::TmpDir(), "FileExists");
  EXPECT_FALSE(hdfs.FileExists(fname));
  TF_ASSERT_OK(WriteString(fname, "test"));
  EXPECT_TRUE(hdfs.FileExists(fname));
}

TEST_F(HadoopFileSystemTest, GetChildren) {
  const string base =
      "file://" + io::JoinPath(testing::TmpDir(), "GetChildren");
  TF_EXPECT_OK(hdfs.CreateDir(base));

  const string file = io::JoinPath(base, "testfile.csv");
  TF_EXPECT_OK(WriteString(file, "blah"));
  const string subdir = io::JoinPath(base, "subdir");
  TF_EXPECT_OK(hdfs.CreateDir(subdir));

  std::vector<string> children;
  TF_EXPECT_OK(hdfs.GetChildren(base, &children));
  std::sort(children.begin(), children.end());
  EXPECT_EQ(std::vector<string>({"subdir", "testfile.csv"}), children);
}

TEST_F(HadoopFileSystemTest, DeleteFile) {
  const string fname =
      "file://" + io::JoinPath(testing::TmpDir(), "DeleteFile");
  EXPECT_FALSE(hdfs.DeleteFile(fname).ok());
  TF_ASSERT_OK(WriteString(fname, "test"));
  TF_EXPECT_OK(hdfs.DeleteFile(fname));
}

TEST_F(HadoopFileSystemTest, GetFileSize) {
  const string fname =
      "file://" + io::JoinPath(testing::TmpDir(), "GetFileSize");
  TF_ASSERT_OK(WriteString(fname, "test"));
  uint64 file_size = 0;
  TF_EXPECT_OK(hdfs.GetFileSize(fname, &file_size));
  EXPECT_EQ(4, file_size);
}

TEST_F(HadoopFileSystemTest, CreateDirStat) {
  const string dir =
      "file://" + io::JoinPath(testing::TmpDir(), "CreateDirStat");
  TF_EXPECT_OK(hdfs.CreateDir(dir));
  FileStatistics stat;
  TF_EXPECT_OK(hdfs.Stat(dir, &stat));
  EXPECT_TRUE(stat.is_directory);
}

TEST_F(HadoopFileSystemTest, DeleteDir) {
  const string dir = "file://" + io::JoinPath(testing::TmpDir(), "DeleteDir");
  EXPECT_FALSE(hdfs.DeleteDir(dir).ok());
  TF_EXPECT_OK(hdfs.CreateDir(dir));
  TF_EXPECT_OK(hdfs.DeleteDir(dir));
  FileStatistics stat;
  EXPECT_FALSE(hdfs.Stat(dir, &stat).ok());
}

TEST_F(HadoopFileSystemTest, RenameFile) {
  const string fname1 =
      "file://" + io::JoinPath(testing::TmpDir(), "RenameFile1");
  const string fname2 =
      "file://" + io::JoinPath(testing::TmpDir(), "RenameFile2");
  TF_ASSERT_OK(WriteString(fname1, "test"));
  TF_EXPECT_OK(hdfs.RenameFile(fname1, fname2));
  string content;
  TF_EXPECT_OK(ReadAll(fname2, &content));
  EXPECT_EQ("test", content);
}

TEST_F(HadoopFileSystemTest, StatFile) {
  const string fname = "file://" + io::JoinPath(testing::TmpDir(), "StatFile");
  TF_ASSERT_OK(WriteString(fname, "test"));
  FileStatistics stat;
  TF_EXPECT_OK(hdfs.Stat(fname, &stat));
  EXPECT_EQ(4, stat.length);
  EXPECT_FALSE(stat.is_directory);
}

// NewAppendableFile() is not testable. Local filesystem maps to
// ChecksumFileSystem in Hadoop, where appending is an unsupported operation.

}  // namespace
}  // namespace tensorflow
