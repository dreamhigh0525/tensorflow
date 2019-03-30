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
#include "tensorflow/lite/tools/evaluation/stages/topk_accuracy_eval_stage.h"

#include <stdint.h>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include "absl/container/flat_hash_map.h"
#include "tensorflow/lite/c/c_api_internal.h"
#include "tensorflow/lite/tools/evaluation/proto/evaluation_config.pb.h"
#include "tensorflow/lite/tools/evaluation/proto/evaluation_stages.pb.h"

namespace tflite {
namespace evaluation {
namespace {

constexpr char kTopkAccuracyEvalStageName[] = "topk_accuracy_eval_stage";
constexpr int kNumCategories = 1001;
// Initializers.
constexpr char kAllLabelsName[] = "all_labels";
constexpr char kModelOutputTypeName[] = "model_output_type";
constexpr char kModelOutputShapeName[] = "model_output_shape";
constexpr char kAllLabelsMapping[] = "ALL_LABELS:all_labels";
constexpr char kModelOutputTypeMapping[] =
    "MODEL_OUTPUT_TYPE:model_output_type";
constexpr char kModelOutputShapeMapping[] =
    "MODEL_OUTPUT_SHAPE:model_output_shape";
// Inputs.
constexpr char kModelOutputName[] = "model_out";
constexpr char kGroundTruthLabelName[] = "ground_truth";
constexpr char kModelOutputMapping[] = "MODEL_OUTPUT:model_out";
constexpr char kGroundTruthLabelMapping[] = "GROUND_TRUTH_LABEL:ground_truth";

EvaluationStageConfig GetTopkAccuracyEvalStageConfig() {
  TopkAccuracyEvalStage_ENABLE();
  EvaluationStageConfig config;
  config.set_name(kTopkAccuracyEvalStageName);
  config.mutable_specification()->set_process_class(TOPK_ACCURACY_EVAL);
  config.add_initializers(kAllLabelsMapping);
  config.add_initializers(kModelOutputTypeMapping);
  config.add_initializers(kModelOutputShapeMapping);
  config.add_inputs(kModelOutputMapping);
  config.add_inputs(kGroundTruthLabelMapping);
  auto* params =
      config.mutable_specification()->mutable_topk_accuracy_eval_params();
  params->set_k(5);
  return config;
}

template <typename T>
T* ResetOutputArray(T array[]) {
  for (int i = 0; i < kNumCategories; i++) {
    array[i] = 0;
  }
  return array;
}

std::vector<std::string> CreateGroundTruthLabels() {
  std::vector<std::string> ground_truth_labels;
  ground_truth_labels.reserve(kNumCategories);
  for (int i = 0; i < kNumCategories; i++) {
    ground_truth_labels.push_back(std::to_string(i));
  }
  return ground_truth_labels;
}

TEST(TopkAccuracyEvalStage, NoK) {
  // Create stage.
  EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
  config.mutable_specification()
      ->mutable_topk_accuracy_eval_params()
      ->clear_k();
  std::unique_ptr<EvaluationStage> stage_ptr = EvaluationStage::Create(config);

  // Initialize.
  std::vector<std::string> ground_truth_labels = CreateGroundTruthLabels();
  TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
  model_output_shape->data[0] = 1;
  model_output_shape->data[1] = kNumCategories;
  TfLiteType model_output_type = kTfLiteFloat32;
  absl::flat_hash_map<std::string, void*> object_map;
  object_map[kAllLabelsName] = &ground_truth_labels;
  object_map[kModelOutputShapeName] = model_output_shape;
  object_map[kModelOutputTypeName] = &model_output_type;
  EXPECT_FALSE(stage_ptr->Init(object_map));
  TfLiteIntArrayFree(model_output_shape);
}

TEST(TopkAccuracyEvalStage, NoGroundTruthLabels) {
  // Create stage.
  EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
  std::unique_ptr<EvaluationStage> stage_ptr = EvaluationStage::Create(config);

  // Initialize.
  std::vector<std::string> ground_truth_labels = {};
  TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
  model_output_shape->data[0] = 1;
  model_output_shape->data[1] = kNumCategories;
  TfLiteType model_output_type = kTfLiteFloat32;
  absl::flat_hash_map<std::string, void*> object_map;
  object_map[kAllLabelsName] = &ground_truth_labels;
  object_map[kModelOutputShapeName] = model_output_shape;
  object_map[kModelOutputTypeName] = &model_output_type;
  EXPECT_FALSE(stage_ptr->Init(object_map));
  TfLiteIntArrayFree(model_output_shape);
}

TEST(TopkAccuracyEvalStage, KTooLarge) {
  // Create stage.
  EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
  config.mutable_specification()->mutable_topk_accuracy_eval_params()->set_k(
      10000);
  std::unique_ptr<EvaluationStage> stage_ptr = EvaluationStage::Create(config);

  // Initialize.
  std::vector<std::string> ground_truth_labels = CreateGroundTruthLabels();
  TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
  model_output_shape->data[0] = 1;
  model_output_shape->data[1] = kNumCategories;
  TfLiteType model_output_type = kTfLiteFloat32;
  absl::flat_hash_map<std::string, void*> object_map;
  object_map[kAllLabelsName] = &ground_truth_labels;
  object_map[kModelOutputShapeName] = model_output_shape;
  object_map[kModelOutputTypeName] = &model_output_type;
  EXPECT_FALSE(stage_ptr->Init(object_map));
  TfLiteIntArrayFree(model_output_shape);
}

TEST(TopkAccuracyEvalStage, WeirdModelOutputShape) {
  // Create stage.
  EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
  std::unique_ptr<EvaluationStage> stage_ptr = EvaluationStage::Create(config);

  // Initialize.
  std::vector<std::string> ground_truth_labels = CreateGroundTruthLabels();
  TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
  model_output_shape->data[0] = 1;
  model_output_shape->data[1] = kNumCategories + 1;
  TfLiteType model_output_type = kTfLiteFloat32;
  absl::flat_hash_map<std::string, void*> object_map;
  object_map[kAllLabelsName] = &ground_truth_labels;
  object_map[kModelOutputShapeName] = model_output_shape;
  object_map[kModelOutputTypeName] = &model_output_type;
  EXPECT_FALSE(stage_ptr->Init(object_map));
  TfLiteIntArrayFree(model_output_shape);
}

TEST(TopkAccuracyEvalStage, UnsupportedModelOutputType) {
  // Create stage.
  EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
  std::unique_ptr<EvaluationStage> stage_ptr = EvaluationStage::Create(config);

  // Initialize.
  std::vector<std::string> ground_truth_labels = CreateGroundTruthLabels();
  TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
  model_output_shape->data[0] = 1;
  model_output_shape->data[1] = kNumCategories + 1;
  TfLiteType model_output_type = kTfLiteComplex64;
  absl::flat_hash_map<std::string, void*> object_map;
  object_map[kAllLabelsName] = &ground_truth_labels;
  object_map[kModelOutputShapeName] = model_output_shape;
  object_map[kModelOutputTypeName] = &model_output_type;
  EXPECT_FALSE(stage_ptr->Init(object_map));
  TfLiteIntArrayFree(model_output_shape);
}

TEST(TopkAccuracyEvalStage, InvalidGroundTruth) {
  // Create stage.
  EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
  std::unique_ptr<EvaluationStage> stage_ptr = EvaluationStage::Create(config);
  // Initialize.
  std::vector<std::string> ground_truth_labels = CreateGroundTruthLabels();
  TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
  model_output_shape->data[0] = 1;
  model_output_shape->data[1] = kNumCategories;
  TfLiteType model_output_type = kTfLiteFloat32;
  absl::flat_hash_map<std::string, void*> object_map;
  object_map[kAllLabelsName] = &ground_truth_labels;
  object_map[kModelOutputShapeName] = model_output_shape;
  object_map[kModelOutputTypeName] = &model_output_type;
  EXPECT_TRUE(stage_ptr->Init(object_map));
  TfLiteIntArrayFree(model_output_shape);

  float array[kNumCategories];
  float* tensor = ResetOutputArray(array);
  tensor[0] = 0.8;
  std::string ground_truth = "XYZ";
  object_map[kModelOutputName] = tensor;
  object_map[kGroundTruthLabelName] = &ground_truth;
  EXPECT_FALSE(stage_ptr->Run(object_map));
}

TEST(TopkAccuracyEvalStage, FloatTest_CorrectLabelsAtLastIndices) {
  // Create stage.
  EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
  std::unique_ptr<EvaluationStage> stage_ptr = EvaluationStage::Create(config);
  // Initialize.
  std::vector<std::string> ground_truth_labels = CreateGroundTruthLabels();
  TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
  model_output_shape->data[0] = 1;
  model_output_shape->data[1] = kNumCategories;
  TfLiteType model_output_type = kTfLiteFloat32;
  absl::flat_hash_map<std::string, void*> object_map;
  object_map[kAllLabelsName] = &ground_truth_labels;
  object_map[kModelOutputShapeName] = model_output_shape;
  object_map[kModelOutputTypeName] = &model_output_type;
  EXPECT_TRUE(stage_ptr->Init(object_map));
  TfLiteIntArrayFree(model_output_shape);

  float array[kNumCategories];

  // The ground truth is index 0, but it is 5th most likely based on model's
  // output.
  float* tensor = ResetOutputArray(array);
  tensor[4] = 0.9;
  tensor[3] = 0.8;
  tensor[2] = 0.7;
  tensor[1] = 0.6;
  tensor[0] = 0.5;
  std::string ground_truth = "0";
  object_map[kModelOutputName] = tensor;
  object_map[kGroundTruthLabelName] = &ground_truth;
  EXPECT_TRUE(stage_ptr->Run(object_map));
  EvaluationStageMetrics metrics = stage_ptr->LatestMetrics();
  EXPECT_EQ(1, metrics.num_runs());
  auto accuracy_metrics = metrics.process_metrics().topk_accuracy_metrics();
  // Only top-5 count is 1.0, rest are 0.0
  EXPECT_FLOAT_EQ(1.0, accuracy_metrics.topk_accuracy_percentages(4));
  for (int i = 0; i < 4; ++i) {
    EXPECT_FLOAT_EQ(0.0, accuracy_metrics.topk_accuracy_percentages(i));
  }

  // The ground truth is index 1, but it is 4th highest based on model's output.
  ground_truth = "1";
  object_map[kGroundTruthLabelName] = &ground_truth;
  EXPECT_TRUE(stage_ptr->Run(object_map));
  metrics = stage_ptr->LatestMetrics();
  EXPECT_EQ(2, metrics.num_runs());
  accuracy_metrics = metrics.process_metrics().topk_accuracy_metrics();
  // 1/2 images had the currect output in top-4, 2/2 has currect output in
  // top-5.
  EXPECT_FLOAT_EQ(1.0, accuracy_metrics.topk_accuracy_percentages(4));
  EXPECT_FLOAT_EQ(0.5, accuracy_metrics.topk_accuracy_percentages(3));
  for (int i = 0; i < 3; ++i) {
    EXPECT_FLOAT_EQ(0.0, accuracy_metrics.topk_accuracy_percentages(i));
  }
}

class CorrectTopkAccuracyEvalTest : public ::testing::Test {
 protected:
  template <typename T>
  void VerifyCorrectBehaviorForType(T ground_truth_0_value,
                                    T ground_truth_1_value,
                                    TfLiteType model_output_type) {
    // Create stage.
    EvaluationStageConfig config = GetTopkAccuracyEvalStageConfig();
    std::unique_ptr<EvaluationStage> stage_ptr =
        EvaluationStage::Create(config);
    // Initialize.
    std::vector<std::string> ground_truth_labels = CreateGroundTruthLabels();
    TfLiteIntArray* model_output_shape = TfLiteIntArrayCreate(2);
    model_output_shape->data[0] = 1;
    model_output_shape->data[1] = kNumCategories;
    absl::flat_hash_map<std::string, void*> object_map;
    object_map[kAllLabelsName] = &ground_truth_labels;
    object_map[kModelOutputShapeName] = model_output_shape;
    object_map[kModelOutputTypeName] = &model_output_type;
    EXPECT_TRUE(stage_ptr->Init(object_map));
    TfLiteIntArrayFree(model_output_shape);

    // Pre-run state.
    EvaluationStageMetrics metrics = stage_ptr->LatestMetrics();
    EXPECT_EQ(0, metrics.num_runs());
    auto accuracy_metrics = metrics.process_metrics().topk_accuracy_metrics();
    EXPECT_EQ(0, accuracy_metrics.topk_accuracy_percentages_size());

    T array[kNumCategories];

    // First image was correctly identified as "0".
    T* tensor = ResetOutputArray(array);
    tensor[0] = ground_truth_0_value;
    std::string ground_truth = "0";
    object_map[kModelOutputName] = tensor;
    object_map[kGroundTruthLabelName] = &ground_truth;
    EXPECT_TRUE(stage_ptr->Run(object_map));
    metrics = stage_ptr->LatestMetrics();
    EXPECT_EQ(1, metrics.num_runs());
    accuracy_metrics = metrics.process_metrics().topk_accuracy_metrics();
    for (int i = 0; i < accuracy_metrics.topk_accuracy_percentages_size();
         ++i) {
      EXPECT_FLOAT_EQ(1.0, accuracy_metrics.topk_accuracy_percentages(i));
    }

    // Second image was also correctly identified as "1".
    // Hence, for the second image as well, the top output ("1") was correct.
    tensor[1] = ground_truth_1_value;
    ground_truth = "1";
    object_map[kModelOutputName] = tensor;
    object_map[kGroundTruthLabelName] = &ground_truth;
    EXPECT_TRUE(stage_ptr->Run(object_map));
    metrics = stage_ptr->LatestMetrics();
    EXPECT_EQ(2, metrics.num_runs());
    accuracy_metrics = metrics.process_metrics().topk_accuracy_metrics();
    for (int i = 0; i < accuracy_metrics.topk_accuracy_percentages_size();
         ++i) {
      EXPECT_FLOAT_EQ(1.0, accuracy_metrics.topk_accuracy_percentages(i));
    }
  }
};

TEST_F(CorrectTopkAccuracyEvalTest, FloatTest) {
  VerifyCorrectBehaviorForType(static_cast<float>(0.8), static_cast<float>(0.9),
                               kTfLiteFloat32);
}

TEST_F(CorrectTopkAccuracyEvalTest, Int8Test) {
  VerifyCorrectBehaviorForType(static_cast<int8_t>(1), static_cast<int8_t>(2),
                               kTfLiteInt8);
}

TEST_F(CorrectTopkAccuracyEvalTest, UInt8Test) {
  VerifyCorrectBehaviorForType(static_cast<uint8_t>(1), static_cast<uint8_t>(2),
                               kTfLiteUInt8);
}

}  // namespace
}  // namespace evaluation
}  // namespace tflite
