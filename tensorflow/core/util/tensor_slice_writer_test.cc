#include "tensorflow/core/util/tensor_slice_writer.h"

#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/util/saved_tensor_slice_util.h"
#include "tensorflow/core/util/tensor_slice_reader.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/platform/test.h"
#include <gtest/gtest.h>

namespace tensorflow {

namespace checkpoint {

class TensorSliceWriteTestHelper {
 public:
  static void CheckEntries(const string& fname);
  static void GetData(TensorSliceReader::Table* table, const string& name,
                      const TensorSlice& slice, SavedSlice* ss);
};

namespace {

// Testing that an array is what is expected
void ExpectIdenticalFloatArrays(const float* expected, int size,
                                const float* actual) {
  // TODO(yangke): copy some of the Dump* functions over
  //  LOG(INFO) << "Expected = " << DumpFloatArray(expected, size);
  //  LOG(INFO) << "Actual   = " << DumpFloatArray(actual, size);
  for (int i = 0; i < size; ++i) {
    EXPECT_NEAR(expected[i], actual[i], 1e-6);
  }
}

template <typename T, typename U>
void ExpectIdenticalIntArrays(const T* expected, int size, const U* actual) {
  for (int i = 0; i < size; ++i) {
    EXPECT_EQ(expected[i], static_cast<T>(actual[i]));
  }
}

// Nifty routine to get the size of an array
template <typename T, unsigned SIZE>
inline size_t ArraySize(const T(&v)[SIZE]) {
  return SIZE;
}

// A simple test on writing a few tensor slices
// TODO(yangke): refactor into smaller tests: will do as we add more stuff to
// the writer.
TEST(TensorSliceWriteTest, SimpleWrite) {
  const string filename = io::JoinPath(testing::TmpDir(), "checkpoint");

  TensorSliceWriter writer(filename, CreateTableTensorSliceBuilder);

  // Add some int32 tensor slices
  {
    TensorShape shape({5, 10});
    TensorSlice slice = TensorSlice::ParseOrDie("-:0,1");
    const int32 data[] = {0, 1, 2, 3, 4};
    TF_CHECK_OK(writer.Add("test", shape, slice, data));
  }

  // Two slices share the same tensor name
  {
    TensorShape shape({5, 10});
    TensorSlice slice = TensorSlice::ParseOrDie("-:3,1");
    const int32 data[] = {10, 11, 12, 13, 14};
    TF_CHECK_OK(writer.Add("test", shape, slice, data));
  }

  // Another slice from a different float tensor -- it has a different name and
  // should be inserted in front of the previous tensor
  {
    TensorShape shape({3, 2});
    TensorSlice slice = TensorSlice::ParseOrDie("-:-");
    const float data[] = {1.2, 1.3, 1.4, 2.1, 2.2, 2.3};
    TF_CHECK_OK(writer.Add("AA", shape, slice, data));
  }

  // A slice with int64 data
  {
    TensorShape shape({5, 10});
    TensorSlice slice = TensorSlice::ParseOrDie("-:3,1");
    const int64 data[] = {10, 11, 12, 13, 14};
    TF_CHECK_OK(writer.Add("int64", shape, slice, data));
  }

  // A slice with int16 data
  {
    TensorShape shape({5, 10});
    TensorSlice slice = TensorSlice::ParseOrDie("-:3,1");
    const int16 data[] = {10, 11, 12, 13, 14};
    TF_CHECK_OK(writer.Add("int16", shape, slice, data));
  }

  TF_CHECK_OK(writer.Finish());

  // Now we examine the checkpoint file manually.
  TensorSliceWriteTestHelper::CheckEntries(filename);
}

}  // namespace

void TensorSliceWriteTestHelper::GetData(TensorSliceReader::Table* table,
                                         const string& name,
                                         const TensorSlice& slice,
                                         SavedSlice* ss) {
  string key = EncodeTensorNameSlice(name, slice);
  string value;
  EXPECT_TRUE(table->Get(key, &value));
  SavedTensorSlices sts;
  EXPECT_TRUE(ParseProtoUnlimited(&sts, value));
  EXPECT_FALSE(sts.has_meta());
  *ss = sts.data();
  EXPECT_EQ(name, ss->name());
  TensorSlice slice2(ss->slice());
  EXPECT_EQ(slice.DebugString(), slice2.DebugString());
}

void TensorSliceWriteTestHelper::CheckEntries(const string& fname) {
  TensorSliceReader::Table* tptr;
  TF_CHECK_OK(OpenTableTensorSliceReader(fname, &tptr));
  std::unique_ptr<TensorSliceReader::Table> table(tptr);
  CHECK_NOTNULL(table.get());

  // We expect a block of SavedTensorSlices
  string value;
  ASSERT_TRUE(table->Get(kSavedTensorSlicesKey, &value));
  {
    SavedTensorSlices sts;
    EXPECT_TRUE(ParseProtoUnlimited(&sts, value));
    // We also expect two entries for the tensors
    EXPECT_TRUE(sts.has_meta());
    EXPECT_EQ(4, sts.meta().tensor_size());
    // We don't expect any data in the first block.
    EXPECT_FALSE(sts.has_data());
    // The two tensors should be stored in the same order as they are first
    // created.
    {
      // The two slices of the "test" tensor
      const SavedSliceMeta& ssm = sts.meta().tensor(0);
      EXPECT_EQ("test", ssm.name());
      EXPECT_EQ(
          "dim { size: 5 } "
          "dim { size: 10 }",
          ssm.shape().ShortDebugString());
      EXPECT_EQ(DT_INT32, ssm.type());
      EXPECT_EQ(2, ssm.slice_size());
      TensorSlice s0(ssm.slice(0));
      TensorSlice s1(ssm.slice(1));
      EXPECT_EQ("-:0,1", s0.DebugString());
      EXPECT_EQ("-:3,1", s1.DebugString());
    }
    {
      // The "AA" tensor
      const SavedSliceMeta& ssm = sts.meta().tensor(1);
      EXPECT_EQ("AA", ssm.name());
      EXPECT_EQ(
          "dim { size: 3 } "
          "dim { size: 2 }",
          ssm.shape().ShortDebugString());
      EXPECT_EQ(DT_FLOAT, ssm.type());
      EXPECT_EQ(1, ssm.slice_size());
      TensorSlice s0(ssm.slice(0));
      EXPECT_EQ("-:-", s0.DebugString());
    }
    {
      // The "int64" tensor
      const SavedSliceMeta& ssm = sts.meta().tensor(2);
      EXPECT_EQ("int64", ssm.name());
      EXPECT_EQ(
          "dim { size: 5 } "
          "dim { size: 10 }",
          ssm.shape().ShortDebugString());
      EXPECT_EQ(DT_INT64, ssm.type());
      EXPECT_EQ(1, ssm.slice_size());
      TensorSlice s0(ssm.slice(0));
      EXPECT_EQ("-:3,1", s0.DebugString());
    }
    {
      // The "int16" tensor
      const SavedSliceMeta& ssm = sts.meta().tensor(3);
      EXPECT_EQ("int16", ssm.name());
      EXPECT_EQ(
          "dim { size: 5 } "
          "dim { size: 10 }",
          ssm.shape().ShortDebugString());
      EXPECT_EQ(DT_INT16, ssm.type());
      EXPECT_EQ(1, ssm.slice_size());
      TensorSlice s0(ssm.slice(0));
      EXPECT_EQ("-:3,1", s0.DebugString());
    }
  }

  // We expect 5 blocks of tensor data
  {
    // Block 1: we expect it to be the full slice of the "AA" tensor
    SavedSlice ss;
    GetData(table.get(), "AA", TensorSlice(2), &ss);
    const float data[] = {1.2, 1.3, 1.4, 2.1, 2.2, 2.3};
    EXPECT_EQ(ArraySize(data), ss.data().float_val_size());
    ExpectIdenticalFloatArrays(data, ArraySize(data),
                               ss.data().float_val().data());
  }

  {
    // Block 2: we expect it to be the first slice of the "test" tensor
    SavedSlice ss;
    GetData(table.get(), "test", TensorSlice({{0, -1}, {0, 1}}), &ss);
    const int32 data[] = {0, 1, 2, 3, 4};
    EXPECT_EQ(ArraySize(data), ss.data().int_val_size());
    ExpectIdenticalIntArrays(data, ArraySize(data), ss.data().int_val().data());
  }

  {
    // Block 3: we expect it to be the second slice of the "test" tensor
    SavedSlice ss;
    GetData(table.get(), "test", TensorSlice({{0, -1}, {3, 1}}), &ss);
    const int32 data[] = {10, 11, 12, 13, 14};
    EXPECT_EQ(ArraySize(data), ss.data().int_val_size());
    ExpectIdenticalIntArrays(data, ArraySize(data), ss.data().int_val().data());
  }

  {
    // Block 4: we expect it to be the slice of the "int64" tensor
    SavedSlice ss;
    GetData(table.get(), "int64", TensorSlice({{0, -1}, {3, 1}}), &ss);
    const int64 data[] = {10, 11, 12, 13, 14};
    EXPECT_EQ(ArraySize(data), ss.data().int64_val_size());
    ExpectIdenticalIntArrays(data, ArraySize(data),
                             ss.data().int64_val().data());
  }

  {
    // Block 5: we expect it to be the slice of the "int16" tensor
    SavedSlice ss;
    GetData(table.get(), "int16", TensorSlice({{0, -1}, {3, 1}}), &ss);
    const int16 data[] = {10, 11, 12, 13, 14};
    EXPECT_EQ(ArraySize(data), ss.data().int_val_size());
    ExpectIdenticalIntArrays(data, ArraySize(data), ss.data().int_val().data());
  }
}

}  // namespace checkpoint

}  // namespace tensorflow
