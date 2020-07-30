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
#include "tensorflow/core/data/service/journal.h"

#include "absl/memory/memory.h"
#include "tensorflow/core/data/service/common.pb.h"
#include "tensorflow/core/data/service/journal.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/path.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace data {

namespace {
using ::testing::HasSubstr;

bool NewJournalDir(std::string* journal_dir) {
  std::string filename;
  if (!Env::Default()->LocalTempFilename(&filename)) {
    return false;
  }
  *journal_dir = io::JoinPath(testing::TmpDir(), filename);
  return true;
}

Update MakeCreateJobUpdate() {
  Update update;
  CreateJobUpdate* create_job = update.mutable_create_job();
  create_job->set_dataset_id(3);
  create_job->set_job_id(8);
  create_job->set_processing_mode(ProcessingModeDef::PARALLEL_EPOCHS);
  return update;
}

Update MakeFinishJobUpdate() {
  Update update;
  FinishJobUpdate* finish_job = update.mutable_finish_job();
  finish_job->set_job_id(8);
  return update;
}

Update MakeRegisterDatasetUpdate() {
  Update update;
  RegisterDatasetUpdate* register_dataset = update.mutable_register_dataset();
  register_dataset->set_dataset_id(2);
  register_dataset->set_fingerprint(3);
  return update;
}

Status CheckJournalContent(StringPiece journal_dir,
                           const std::vector<Update>& expected) {
  JournalReader reader(Env::Default(), journal_dir);
  for (const auto& update : expected) {
    Update result;
    bool end_of_journal = true;
    TF_RETURN_IF_ERROR(reader.Read(&result, &end_of_journal));
    EXPECT_FALSE(end_of_journal);
    // We can't use the testing::EqualsProto matcher because it is not available
    // in OSS.
    EXPECT_EQ(result.SerializeAsString(), update.SerializeAsString());
  }
  Update result;
  bool end_of_journal = false;
  TF_RETURN_IF_ERROR(reader.Read(&result, &end_of_journal));
  EXPECT_TRUE(end_of_journal);
  return Status::OK();
}
}  // namespace

TEST(Journal, RoundTripMultiple) {
  std::string journal_dir;
  EXPECT_TRUE(NewJournalDir(&journal_dir));
  std::vector<Update> updates = {MakeCreateJobUpdate(),
                                 MakeRegisterDatasetUpdate(),
                                 MakeFinishJobUpdate()};
  JournalWriter writer(Env::Default(), journal_dir);
  for (const auto& update : updates) {
    TF_EXPECT_OK(writer.Write(update));
  }

  TF_EXPECT_OK(CheckJournalContent(journal_dir, updates));
}

TEST(Journal, AppendExistingFile) {
  std::string journal_dir;
  EXPECT_TRUE(NewJournalDir(&journal_dir));
  std::vector<Update> updates = {MakeCreateJobUpdate(),
                                 MakeRegisterDatasetUpdate(),
                                 MakeFinishJobUpdate()};
  for (const auto& update : updates) {
    JournalWriter writer(Env::Default(), journal_dir);
    TF_EXPECT_OK(writer.Write(update));
  }

  TF_EXPECT_OK(CheckJournalContent(journal_dir, updates));
}

TEST(Journal, MissingFile) {
  std::string journal_dir;
  EXPECT_TRUE(NewJournalDir(&journal_dir));
  JournalReader reader(Env::Default(), journal_dir);
  Update result;
  bool end_of_journal = true;
  Status s = reader.Read(&result, &end_of_journal);
  EXPECT_TRUE(errors::IsNotFound(s));
}

TEST(Journal, NonRecordData) {
  std::string journal_dir;
  EXPECT_TRUE(NewJournalDir(&journal_dir));

  TF_ASSERT_OK(Env::Default()->RecursivelyCreateDir(journal_dir));
  {
    std::unique_ptr<WritableFile> file;
    TF_ASSERT_OK(Env::Default()->NewAppendableFile(
        DataServiceJournalFile(journal_dir), &file));
    TF_ASSERT_OK(file->Append("not record data"));
  }

  JournalReader reader(Env::Default(), journal_dir);
  Update result;
  bool end_of_journal = true;
  Status s = reader.Read(&result, &end_of_journal);
  EXPECT_THAT(s.error_message(), HasSubstr("corrupted record"));
  EXPECT_EQ(s.code(), error::DATA_LOSS);
}

TEST(Journal, InvalidRecordData) {
  std::string journal_dir;
  EXPECT_TRUE(NewJournalDir(&journal_dir));

  TF_ASSERT_OK(Env::Default()->RecursivelyCreateDir(journal_dir));
  {
    std::unique_ptr<WritableFile> file;
    TF_ASSERT_OK(Env::Default()->NewAppendableFile(
        DataServiceJournalFile(journal_dir), &file));
    auto writer = absl::make_unique<io::RecordWriter>(file.get());
    TF_ASSERT_OK(writer->WriteRecord("not serializd proto"));
  }

  JournalReader reader(Env::Default(), journal_dir);
  Update result;
  bool end_of_journal = true;
  Status s = reader.Read(&result, &end_of_journal);
  EXPECT_THAT(s.error_message(), HasSubstr("Failed to parse journal record"));
  EXPECT_EQ(s.code(), error::DATA_LOSS);
}
}  // namespace data
}  // namespace tensorflow
