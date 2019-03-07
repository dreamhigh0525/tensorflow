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

#include "tensorflow/core/kernels/data/dataset_test_base.h"

namespace tensorflow {
namespace data {
namespace {

constexpr char kNodeName[] = "repeat_dataset";
constexpr char kOpName[] = "RepeatDataset";

class RepeatDatasetOpTest : public DatasetOpsTestBase {
 protected:
  // Creates a new `RepeatDataset` op kernel.
  Status CreateRepeatDatasetKernel(
      const DataTypeVector &output_types,
      const std::vector<PartialTensorShape> &output_shapes,
      std::unique_ptr<OpKernel> *op_kernel) {
    node_def_ = test::function::NDef(
        kNodeName, kOpName, {"input_dataset", "count"},
        {{"output_types", output_types}, {"output_shapes", output_shapes}});
    TF_RETURN_IF_ERROR(CreateOpKernel(node_def_, op_kernel));
    return Status::OK();
  }

  // Create a new `RepeatDataset` op kernel context.
  Status CreateRepeatDatasetContext(
      OpKernel *op_kernel, gtl::InlinedVector<TensorValue, 4> *const inputs,
      std::unique_ptr<OpKernelContext> *context) {
    TF_RETURN_IF_ERROR(CheckOpKernelInput(*op_kernel, *inputs));
    TF_RETURN_IF_ERROR(CreateOpKernelContext(op_kernel, inputs, context));
    return Status::OK();
  }

 private:
  NodeDef node_def_;
};

struct TestParam {
  std::vector<Tensor> input_tensors;
  int64 count;
  std::vector<Tensor> expected_outputs;
  DataTypeVector expected_output_dtypes;
  std::vector<PartialTensorShape> expected_output_shapes;
  int64 expected_cardinality;
  std::vector<int> breakpoints;
};

// Test case 1: finite repetition.
TestParam TestCase1() {
  return {
      /*input_tensors*/
      {DatasetOpsTestBase::CreateTensor<int64>(TensorShape{2, 2}, {1, 2, 3, 4}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape{2, 1}, {"a", "b"})},
      /*count*/ 2,
      /*expected_outputs*/
      {DatasetOpsTestBase::CreateTensor<int64>(TensorShape{2}, {1, 2}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape{1}, {"a"}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape{2}, {3, 4}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape{1}, {"b"}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape{2}, {1, 2}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape{1}, {"a"}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape{2}, {3, 4}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape{1}, {"b"})},
      /*expected_output_dtypes*/ {DT_INT64, DT_STRING},
      /*expected_output_shapes*/
      {PartialTensorShape({2}), PartialTensorShape({1})},
      /*expected_cardinality*/ 4,
      /*breakpoints*/ {0, 1, 3}};
}

// Test case 2: empty repetition.
TestParam TestCase2() {
  return {
      /*input_tensors*/
      {DatasetOpsTestBase::CreateTensor<int64>(TensorShape{2, 2}, {1, 2, 3, 4}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape{2, 1}, {"a", "b"})},
      /*count*/ 0,
      /*expected_outputs*/
      {},
      /*expected_output_dtypes*/ {DT_INT64, DT_STRING},
      /*expected_output_shapes*/
      {PartialTensorShape({2}), PartialTensorShape({1})},
      /*expected_cardinality*/ 0,
      /*breakpoints*/ {0, 1, 3}};
}

// Test case 3: infinite repetition.
TestParam TestCase3() {
  return {/*input_tensors*/
          {DatasetOpsTestBase::CreateTensor<int64>(TensorShape{2, 1}, {1, 2})},
          /*count*/ -1,
          /*expected_outputs*/
          // Use the first group of the repeated tensors to represent the
          // infinite outputs.
          {DatasetOpsTestBase::CreateTensor<int64>(TensorShape{1}, {1}),
           DatasetOpsTestBase::CreateTensor<int64>(TensorShape{1}, {2})},
          /*expected_output_dtypes*/ {DT_INT64},
          /*expected_output_shapes*/ {PartialTensorShape({1})},
          /*expected_cardinality*/ -1,
          /*breakpoints*/ {0, 1, 3}};
}

class RepeatDatasetOpTestHelper : public RepeatDatasetOpTest {
 public:
  ~RepeatDatasetOpTestHelper() override {
    if (dataset_) dataset_->Unref();
  }

 protected:
  // Creates `TensorSliceDataset` variant tensor from the input vector of
  // tensors.
  Status CreateTensorSliceDatasetTensor(
      std::vector<Tensor> *const tensor_vector, Tensor *dataset_tensor) {
    DatasetBase *tensor_slice_dataset;
    TF_RETURN_IF_ERROR(CreateTensorSliceDataset(
        "tensor_slice_node", tensor_vector, &tensor_slice_dataset));
    TF_RETURN_IF_ERROR(
        StoreDatasetInVariantTensor(tensor_slice_dataset, dataset_tensor));
    return Status::OK();
  }

  Status CreateDatasetFromTestCase(const TestParam &test_case) {
    Tensor tensor_slice_dataset_tensor(DT_VARIANT, TensorShape({}));
    std::vector<Tensor> input_tensors = test_case.input_tensors;
    TF_RETURN_IF_ERROR(CreateTensorSliceDatasetTensor(
        &input_tensors, &tensor_slice_dataset_tensor));
    gtl::InlinedVector<TensorValue, 4> inputs;
    Tensor count = CreateTensor<int64>(TensorShape{}, {test_case.count});
    inputs.emplace_back(&tensor_slice_dataset_tensor);
    inputs.emplace_back(&count);
    TF_RETURN_IF_ERROR(CreateRepeatDatasetKernel(
        test_case.expected_output_dtypes, test_case.expected_output_shapes,
        &dataset_kernel_));
    TF_RETURN_IF_ERROR(CreateRepeatDatasetContext(
        dataset_kernel_.get(), &inputs, &dataset_kernel_ctx_));
    TF_RETURN_IF_ERROR(CreateDataset(dataset_kernel_.get(),
                                     dataset_kernel_ctx_.get(), &dataset_));
    return Status::OK();
  }

  Status CreateIteratorFromTestCase(const TestParam &test_case) {
    TF_RETURN_IF_ERROR(CreateDatasetFromTestCase(test_case));
    TF_RETURN_IF_ERROR(
        CreateIteratorContext(dataset_kernel_ctx_.get(), &iterator_ctx_));
    TF_RETURN_IF_ERROR(
        dataset_->MakeIterator(iterator_ctx_.get(), "Iterator", &iterator_));
    return Status::OK();
  }

  std::unique_ptr<OpKernel> dataset_kernel_;
  std::unique_ptr<OpKernelContext> dataset_kernel_ctx_;
  DatasetBase *dataset_ = nullptr;  // owned by this class.
  std::unique_ptr<IteratorContext> iterator_ctx_;
  std::unique_ptr<IteratorBase> iterator_;
};

class ParameterizedDatasetTest
    : public RepeatDatasetOpTestHelper,
      public ::testing::WithParamInterface<TestParam> {};

TEST_P(ParameterizedDatasetTest, GetNext) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  TF_ASSERT_OK(CreateIteratorFromTestCase(test_case));

  auto expected_outputs_it = test_case.expected_outputs.begin();
  bool end_of_sequence = false;
  std::vector<Tensor> out_tensors;

  if (test_case.count < 0) {
    int fake_infinite_repetition = 100;
    while (fake_infinite_repetition > 0) {
      TF_EXPECT_OK(iterator_->GetNext(iterator_ctx_.get(), &out_tensors,
                                      &end_of_sequence));
      for (const auto &tensor : out_tensors) {
        TF_EXPECT_OK(ExpectEqual(tensor, *expected_outputs_it));
        expected_outputs_it++;
        // In the forever-repeat test case, the first group of the repeated
        // tensors is used to represent the expected outputs, so the iterator
        // of the expected outputs needs to be reset once it reaches the end.
        if (expected_outputs_it == test_case.expected_outputs.end()) {
          expected_outputs_it = test_case.expected_outputs.begin();
        }
      }
      fake_infinite_repetition--;
    }
    EXPECT_FALSE(end_of_sequence);
  } else {
    while (!end_of_sequence) {
      TF_EXPECT_OK(iterator_->GetNext(iterator_ctx_.get(), &out_tensors,
                                      &end_of_sequence));
      if (!end_of_sequence) {
        for (const auto &tensor : out_tensors) {
          EXPECT_NE(expected_outputs_it, test_case.expected_outputs.end());
          TF_EXPECT_OK(ExpectEqual(tensor, *expected_outputs_it));
          expected_outputs_it++;
        }
      }
    }
    EXPECT_EQ(expected_outputs_it, test_case.expected_outputs.end());
  }
}

TEST_F(RepeatDatasetOpTestHelper, DatasetName) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  TF_ASSERT_OK(CreateDatasetFromTestCase(TestCase1()));

  EXPECT_EQ(dataset_->type_string(), kOpName);
}

TEST_P(ParameterizedDatasetTest, DatasetOutputDtypes) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  TF_ASSERT_OK(CreateDatasetFromTestCase(test_case));
  TF_EXPECT_OK(VerifyTypesMatch(dataset_->output_dtypes(),
                                test_case.expected_output_dtypes));
}

TEST_P(ParameterizedDatasetTest, DatasetOutputShapes) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  TF_ASSERT_OK(CreateDatasetFromTestCase(test_case));
  TF_EXPECT_OK(VerifyShapesCompatible(dataset_->output_shapes(),
                                      test_case.expected_output_shapes));
}

TEST_P(ParameterizedDatasetTest, Cardinality) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  TF_ASSERT_OK(CreateDatasetFromTestCase(test_case));

  EXPECT_EQ(dataset_->Cardinality(), GetParam().expected_cardinality);
}

TEST_F(RepeatDatasetOpTestHelper, DatasetSave) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  TF_ASSERT_OK(CreateDatasetFromTestCase(TestCase1()));

  std::unique_ptr<SerializationContext> serialization_ctx;
  TF_ASSERT_OK(CreateSerializationContext(&serialization_ctx));
  VariantTensorData data;
  VariantTensorDataWriter writer(&data);
  TF_ASSERT_OK(dataset_->Save(serialization_ctx.get(), &writer));
  TF_ASSERT_OK(writer.Flush());
}

TEST_P(ParameterizedDatasetTest, IteratorOutputDtypes) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  TF_ASSERT_OK(CreateIteratorFromTestCase(test_case));
  TF_EXPECT_OK(VerifyTypesMatch(iterator_->output_dtypes(),
                                test_case.expected_output_dtypes));
}

TEST_P(ParameterizedDatasetTest, IteratorOutputShapes) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  TF_ASSERT_OK(CreateIteratorFromTestCase(test_case));
  TF_EXPECT_OK(VerifyShapesCompatible(iterator_->output_shapes(),
                                      test_case.expected_output_shapes));
}

TEST_P(ParameterizedDatasetTest, IteratorOutputPrefix) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  TF_ASSERT_OK(CreateIteratorFromTestCase(test_case));
  if (test_case.count < 0) {
    EXPECT_EQ(iterator_->prefix(), "Iterator::ForeverRepeat");
  } else if (test_case.count == 0) {
    EXPECT_EQ(iterator_->prefix(), "Iterator::EmptyRepeat");
  } else {
    EXPECT_EQ(iterator_->prefix(), "Iterator::FiniteRepeat");
  }
}

TEST_P(ParameterizedDatasetTest, Roundtrip) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));
  const TestParam &test_case = GetParam();
  auto expected_outputs_it = test_case.expected_outputs.begin();
  TF_ASSERT_OK(CreateIteratorFromTestCase(test_case));

  std::unique_ptr<SerializationContext> serialization_ctx;
  TF_ASSERT_OK(CreateSerializationContext(&serialization_ctx));

  bool end_of_sequence = dataset_->Cardinality() == 0;
  std::vector<Tensor> out_tensors;
  int cur_iteration = 0;
  std::vector<int> breakpoints = GetParam().breakpoints;
  for (int breakpoint : breakpoints) {
    VariantTensorData data;
    VariantTensorDataWriter writer(&data);
    TF_EXPECT_OK(iterator_->Save(serialization_ctx.get(), &writer));
    TF_EXPECT_OK(writer.Flush());
    VariantTensorDataReader reader(&data);
    TF_EXPECT_OK(iterator_->Restore(iterator_ctx_.get(), &reader));

    while (cur_iteration < breakpoint) {
      TF_EXPECT_OK(iterator_->GetNext(iterator_ctx_.get(), &out_tensors,
                                      &end_of_sequence));
      if (!end_of_sequence) {
        for (auto &tensor : out_tensors) {
          EXPECT_NE(expected_outputs_it, test_case.expected_outputs.end());
          TF_EXPECT_OK(ExpectEqual(tensor, *expected_outputs_it));
          expected_outputs_it++;
        }
      }
      cur_iteration++;
      if (test_case.count < 0 &&
          expected_outputs_it == test_case.expected_outputs.end()) {
        expected_outputs_it = test_case.expected_outputs.begin();
      }
    }

    if (breakpoint >= dataset_->Cardinality()) {
      if (test_case.count < 0) {
        EXPECT_FALSE(end_of_sequence);
      } else {
        EXPECT_TRUE(end_of_sequence);
        EXPECT_EQ(expected_outputs_it, test_case.expected_outputs.end());
      }
    } else {
      EXPECT_FALSE(end_of_sequence);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(RepeatDatasetOpTest, ParameterizedDatasetTest,
                         ::testing::ValuesIn(std::vector<TestParam>(
                             {TestCase1(), TestCase2(), TestCase3()})));

}  // namespace
}  // namespace data
}  // namespace tensorflow
