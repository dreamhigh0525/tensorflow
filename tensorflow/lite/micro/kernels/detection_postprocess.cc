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

#include <numeric>

#define FLATBUFFERS_LOCALE_INDEPENDENT 0
#include "flatbuffers/flexbuffers.h"  // from @flatbuffers

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/op_macros.h"
#include "tensorflow/lite/micro/micro_utils.h"


namespace tflite {
namespace ops {
namespace micro {
namespace custom {
namespace detection_postprocess {

/**
 * This version of detection_postprocess is specific to TFLite Micro. It contains the following
 * differences between the TFLite version:
 *
 * 1.) Temporaries (temporary tensors) - Micro use instead scratch buffer API.
 * 2.) Output dimensions - the TFLite version determines output size
 * and resizes the output tensor. Micro runtime does not support tensor
 * resizing. However if output dimensions are undefined TFLu memory API is
 * used to allocate the new dimensions.
 */


// Input tensors
constexpr int kInputTensorBoxEncodings = 0;
constexpr int kInputTensorClassPredictions = 1;
constexpr int kInputTensorAnchors = 2;

// Output tensors
constexpr int kOutputTensorDetectionBoxes = 0;
constexpr int kOutputTensorDetectionClasses = 1;
constexpr int kOutputTensorDetectionScores = 2;
constexpr int kOutputTensorNumDetections = 3;

constexpr int kNumCoordBox = 4;
constexpr int kBatchSize = 1;

constexpr int kNumDetectionsPerClass = 100;

// Object Detection model produces axis-aligned boxes in two formats:
// BoxCorner represents the lower left corner (xmin, ymin) and
// the upper right corner (xmax, ymax).
// CenterSize represents the center (xcenter, ycenter), height and width.
// BoxCornerEncoding and CenterSizeEncoding are related as follows:
// ycenter = y / y_scale * anchor.h + anchor.y;
// xcenter = x / x_scale * anchor.w + anchor.x;
// half_h = 0.5*exp(h/ h_scale)) * anchor.h;
// half_w = 0.5*exp(w / w_scale)) * anchor.w;
// ymin = ycenter - half_h
// ymax = ycenter + half_h
// xmin = xcenter - half_w
// xmax = xcenter + half_w
struct BoxCornerEncoding {
  float ymin;
  float xmin;
  float ymax;
  float xmax;
};

struct CenterSizeEncoding {
  float y;
  float x;
  float h;
  float w;
};
// We make sure that the memory allocations are contiguous with static assert.
static_assert(sizeof(BoxCornerEncoding) == sizeof(float) * kNumCoordBox,
              "Size of BoxCornerEncoding is 4 float values");
static_assert(sizeof(CenterSizeEncoding) == sizeof(float) * kNumCoordBox,
              "Size of CenterSizeEncoding is 4 float values");

struct OpData {
  int max_detections;
  int max_classes_per_detection;  // Fast Non-Max-Suppression
  int detections_per_class;       // Regular Non-Max-Suppression
  float non_max_suppression_score_threshold;
  float intersection_over_union_threshold;
  int num_classes;
  bool use_regular_non_max_suppression;
  CenterSizeEncoding scale_values;

  // Scratch buffers
  int active_candidate_idx;
  int decoded_boxes_idx;
  int scores_idx;
  int score_buffer_idx;
  int keep_scores_idx;
  int scores_after_regular_non_max_suppression_idx;
  int sorted_values_idx;
  int keep_indices_idx;
  int sorted_indices_idx;
  int buffer_idx;
  int selected_idx;
  uint8_t* active_box_candidate;
  float* decoded_boxes;
  float* scores;
  float* score_buffer;
  float* keep_scores;
  float* scores_after_regular_non_max_suppression;
  float* sorted_values;
  int* keep_indices;
  int* sorted_indices;
  int* buffer;
  int* selected;
};

TfLiteStatus AllocateOutDimensions(TfLiteContext* context, TfLiteIntArray** dims,
                                   int x, int y=0, int z=0) {
  int size = 1;

  size = size * x;
  size = (y > 0) ? size * y : size;
  size = (z > 0) ? size * z : size;

  TF_LITE_ENSURE_STATUS(context->AllocatePersistentBuffer(
      context, TfLiteIntArrayGetSizeInBytes(size),
      reinterpret_cast<void**>(dims)));

  (*dims)->size = size;
  (*dims)->data[0] = x;
  if (y > 0) {
    (*dims)->data[1] = y;
  }
  if (z > 0) {
    (*dims)->data[2] = z;
  }

  return kTfLiteOk;
}

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  void* raw;
  OpData* op_data = nullptr;

  const uint8_t* buffer_t = reinterpret_cast<const uint8_t*>(buffer);
  const flexbuffers::Map& m = flexbuffers::GetRoot(buffer_t, length).AsMap();

  context->AllocatePersistentBuffer(context, sizeof(OpData), &raw);
  op_data = reinterpret_cast<OpData*>(raw);

  op_data->max_detections = m["max_detections"].AsInt32();
  op_data->max_classes_per_detection = m["max_classes_per_detection"].AsInt32();
  if (m["detections_per_class"].IsNull())
    op_data->detections_per_class = kNumDetectionsPerClass;
  else
    op_data->detections_per_class = m["detections_per_class"].AsInt32();
  if (m["use_regular_nms"].IsNull())
    op_data->use_regular_non_max_suppression = false;
  else
    op_data->use_regular_non_max_suppression = m["use_regular_nms"].AsBool();

  op_data->non_max_suppression_score_threshold =
      m["nms_score_threshold"].AsFloat();
  op_data->intersection_over_union_threshold = m["nms_iou_threshold"].AsFloat();
  op_data->num_classes = m["num_classes"].AsInt32();
  op_data->scale_values.y = m["y_scale"].AsFloat();
  op_data->scale_values.x = m["x_scale"].AsFloat();
  op_data->scale_values.h = m["h_scale"].AsFloat();
  op_data->scale_values.w = m["w_scale"].AsFloat();

  return op_data;
}

void Free(TfLiteContext* context, void* buffer) {}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  auto* op_data = static_cast<OpData*>(node->user_data);

  // Inputs: box_encodings, scores, anchors
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 3);
  const TfLiteTensor* input_box_encodings =
      GetInput(context, node, kInputTensorBoxEncodings);
  const TfLiteTensor* input_class_predictions =
      GetInput(context, node, kInputTensorClassPredictions);
  const TfLiteTensor* input_anchors =
      GetInput(context, node, kInputTensorAnchors);
  TF_LITE_ENSURE_EQ(context, NumDimensions(input_box_encodings), 3);
  TF_LITE_ENSURE_EQ(context, NumDimensions(input_class_predictions), 3);
  TF_LITE_ENSURE_EQ(context, NumDimensions(input_anchors), 2);

  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 4);
  const int num_boxes = input_box_encodings->dims->data[1];
  const int num_classes = op_data->num_classes;

  // Scratch tensors
  context->RequestScratchBufferInArena(context, num_boxes,
                                       &op_data->active_candidate_idx);
  context->RequestScratchBufferInArena(context, num_boxes * kNumCoordBox * sizeof(float),
                                       &op_data->decoded_boxes_idx);
  context->RequestScratchBufferInArena(context,
                                       input_class_predictions->dims->data[1] * input_class_predictions->dims->data[2] *
                                       sizeof(float),
                                       &op_data->scores_idx);

  // Additional buffers
  context->RequestScratchBufferInArena(context, num_boxes * sizeof(float), &op_data->scores_idx);
  context->RequestScratchBufferInArena(context, num_boxes * sizeof(float), &op_data->keep_scores_idx);
  context->RequestScratchBufferInArena(context, op_data->max_detections * num_boxes * sizeof(float),
                                       &op_data->scores_after_regular_non_max_suppression_idx);
  context->RequestScratchBufferInArena(context, op_data->max_detections * num_boxes * sizeof(float),
                                       &op_data->sorted_values_idx);
  context->RequestScratchBufferInArena(context, num_boxes * sizeof(int), &op_data->keep_indices_idx);
  context->RequestScratchBufferInArena(context, op_data->max_detections * num_boxes * sizeof(int),
                                       &op_data->sorted_indices_idx);
  int buffer_size = std::max(num_classes, op_data->max_detections);
  context->RequestScratchBufferInArena(context, buffer_size * num_boxes * sizeof(int),
                                       &op_data->buffer_idx);
  buffer_size = std::min(num_boxes, op_data->max_detections);
  context->RequestScratchBufferInArena(context, buffer_size * num_boxes * sizeof(int),
                                       &op_data->selected_idx);

  // number of detected boxes
  const int num_detected_boxes =
      op_data->max_detections * op_data->max_classes_per_detection;

  // Outputs: detection_boxes, detection_scores, detection_classes,
  // num_detections
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 4);

  // Output Tensor detection_boxes: size is set to (1, num_detected_boxes, 4)
  TfLiteTensor* detection_boxes =
      GetOutput(context, node, kOutputTensorDetectionBoxes);
  if (detection_boxes->dims->size == 0) {
    TF_LITE_ENSURE_STATUS(AllocateOutDimensions(
        context, &detection_boxes->dims, 1, num_detected_boxes, 4));
  }

  // Output Tensor detection_classes: size is set to (1, num_detected_boxes)
  TfLiteTensor* detection_classes =
      GetOutput(context, node, kOutputTensorDetectionClasses);
  if (detection_classes->dims->size == 0) {
    TF_LITE_ENSURE_STATUS(AllocateOutDimensions(
        context, &detection_classes->dims, 1, num_detected_boxes));
  }

  // Output Tensor detection_scores: size is set to (1, num_detected_boxes)
  TfLiteTensor* detection_scores =
      GetOutput(context, node, kOutputTensorDetectionScores);
  if (detection_scores->dims->size == 0) {
    TF_LITE_ENSURE_STATUS(AllocateOutDimensions(
        context, &detection_scores->dims, 1, num_detected_boxes));
  }

  // Output Tensor num_detections: size is set to 1
  TfLiteTensor* num_detections =
      GetOutput(context, node, kOutputTensorNumDetections);
  if (num_detections->dims->size == 0) {
    TF_LITE_ENSURE_STATUS(AllocateOutDimensions(
        context, &num_detections->dims, 1));
  }

  return kTfLiteOk;
}

class Dequantizer {
 public:
  Dequantizer(int zero_point, float scale)
      : zero_point_(zero_point), scale_(scale) {}
  float operator()(uint8 x) {
    return (static_cast<float>(x) - zero_point_) * scale_;
  }

 private:
  int zero_point_;
  float scale_;
};

void DequantizeBoxEncodings(const TfLiteTensor* input_box_encodings, int idx,
                            float quant_zero_point, float quant_scale,
                            int length_box_encoding,
                            CenterSizeEncoding* box_centersize) {
  const uint8* boxes =
      GetTensorData<uint8>(input_box_encodings) + length_box_encoding * idx;
  Dequantizer dequantize(quant_zero_point, quant_scale);
  // See definition of the KeyPointBoxCoder at
  // https://github.com/tensorflow/models/blob/master/research/object_detection/box_coders/keypoint_box_coder.py
  // The first four elements are the box coordinates, which is the same as the
  // FastRnnBoxCoder at
  // https://github.com/tensorflow/models/blob/master/research/object_detection/box_coders/faster_rcnn_box_coder.py
  box_centersize->y = dequantize(boxes[0]);
  box_centersize->x = dequantize(boxes[1]);
  box_centersize->h = dequantize(boxes[2]);
  box_centersize->w = dequantize(boxes[3]);
}

template <class T>
T ReInterpretTensor(const TfLiteTensor* tensor) {
  // TODO (chowdhery): check float
  const float* tensor_base = GetTensorData<float>(tensor);
  return reinterpret_cast<T>(tensor_base);
}

template <class T>
T ReInterpretTensor(TfLiteTensor* tensor) {
  // TODO (chowdhery): check float
  float* tensor_base = GetTensorData<float>(tensor);
  return reinterpret_cast<T>(tensor_base);
}

TfLiteStatus DecodeCenterSizeBoxes(TfLiteContext* context, TfLiteNode* node,
                                   OpData* op_data) {
  // Parse input tensor boxencodings
  const TfLiteTensor* input_box_encodings =
      GetInput(context, node, kInputTensorBoxEncodings);
  TF_LITE_ENSURE_EQ(context, input_box_encodings->dims->data[0], kBatchSize);
  const int num_boxes = input_box_encodings->dims->data[1];
  TF_LITE_ENSURE(context, input_box_encodings->dims->data[2] >= kNumCoordBox);
  const TfLiteTensor* input_anchors =
      GetInput(context, node, kInputTensorAnchors);

  // Decode the boxes to get (ymin, xmin, ymax, xmax) based on the anchors
  CenterSizeEncoding box_centersize;
  CenterSizeEncoding scale_values = op_data->scale_values;
  CenterSizeEncoding anchor;
  for (int idx = 0; idx < num_boxes; ++idx) {
    switch (input_box_encodings->type) {
        // Quantized
      case kTfLiteUInt8:
        DequantizeBoxEncodings(
            input_box_encodings, idx,
            static_cast<float>(input_box_encodings->params.zero_point),
            static_cast<float>(input_box_encodings->params.scale),
            input_box_encodings->dims->data[2], &box_centersize);
        DequantizeBoxEncodings(
            input_anchors, idx,
            static_cast<float>(input_anchors->params.zero_point),
            static_cast<float>(input_anchors->params.scale), kNumCoordBox,
            &anchor);
        break;
        // Float
      case kTfLiteFloat32: {
        // Please see DequantizeBoxEncodings function for the support detail.
        const int box_encoding_idx = idx * input_box_encodings->dims->data[2];
        const float* boxes =
            &(GetTensorData<float>(input_box_encodings)[box_encoding_idx]);
        box_centersize = *reinterpret_cast<const CenterSizeEncoding*>(boxes);
        anchor =
            ReInterpretTensor<const CenterSizeEncoding*>(input_anchors)[idx];
        break;
      }
      default:
        // Unsupported type.
        return kTfLiteError;
    }

    float ycenter = box_centersize.y / scale_values.y * anchor.h + anchor.y;
    float xcenter = box_centersize.x / scale_values.x * anchor.w + anchor.x;
    float half_h =
        0.5f * static_cast<float>(std::exp(box_centersize.h / scale_values.h)) *
        anchor.h;
    float half_w =
        0.5f * static_cast<float>(std::exp(box_centersize.w / scale_values.w)) *
        anchor.w;

    auto& box = reinterpret_cast<BoxCornerEncoding*>(op_data->decoded_boxes)[idx];
    box.ymin = ycenter - half_h;
    box.xmin = xcenter - half_w;
    box.ymax = ycenter + half_h;
    box.xmax = xcenter + half_w;
  }
  return kTfLiteOk;
}

void DecreasingPartialArgSort(const float* values, int num_values,
                              int num_to_sort, int* indices) {
  std::iota(indices, indices + num_values, 0);
  std::partial_sort(
      indices, indices + num_to_sort, indices + num_values,
      [&values](const int i, const int j) { return values[i] > values[j]; });
}

void DecreasingPartialArgSort2(const float* values, int num_values,
                               int num_to_sort, int* indices, int* ind) {

  std::iota(ind, ind + num_values, 0);
  std::partial_sort(
      ind, ind + num_to_sort, ind + num_values,
      [&values](const int i, const int j) { return values[i] > values[j]; });

  std::iota(indices, indices + num_values, 0);

  std::partial_sort(
      indices, indices + num_to_sort, indices + num_values,
      [&values](const int i, const int j) { return values[i] > values[j]; });

}

int SelectDetectionsAboveScoreThreshold(const float* values,
                                        int size,
                                        const float threshold,
                                        float* keep_values,
                                        int* keep_indices) {
  int counter = 0;
  for (int i = 0; i < size; i++) {
    if (values[i] >= threshold) {
      keep_values[counter++] = values[i];
      keep_indices[i] = i;
    }
  }
  return counter;
}

bool ValidateBoxes(const float* decoded_boxes, const int num_boxes) {
  for (int i = 0; i < num_boxes; ++i) {
    // ymax>=ymin, xmax>=xmin
    auto& box = reinterpret_cast<const BoxCornerEncoding*>(decoded_boxes)[i];
    if (box.ymin >= box.ymax || box.xmin >= box.xmax) {
      return false;
    }
  }
  return true;
}

float ComputeIntersectionOverUnion(const float* decoded_boxes,
                                   const int i, const int j) {
  auto& box_i = reinterpret_cast<const BoxCornerEncoding*>(decoded_boxes)[i];
  auto& box_j = reinterpret_cast<const BoxCornerEncoding*>(decoded_boxes)[j];
  const float area_i = (box_i.ymax - box_i.ymin) * (box_i.xmax - box_i.xmin);
  const float area_j = (box_j.ymax - box_j.ymin) * (box_j.xmax - box_j.xmin);
  if (area_i <= 0 || area_j <= 0) return 0.0;
  const float intersection_ymin = std::max<float>(box_i.ymin, box_j.ymin);
  const float intersection_xmin = std::max<float>(box_i.xmin, box_j.xmin);
  const float intersection_ymax = std::min<float>(box_i.ymax, box_j.ymax);
  const float intersection_xmax = std::min<float>(box_i.xmax, box_j.xmax);
  const float intersection_area =
      std::max<float>(intersection_ymax - intersection_ymin, 0.0) *
      std::max<float>(intersection_xmax - intersection_xmin, 0.0);
  return intersection_area / (area_i + area_j - intersection_area);
}

// NonMaxSuppressionSingleClass() prunes out the box locations with high overlap
// before selecting the highest scoring boxes (max_detections in number)
// It assumes all boxes are good in beginning and sorts based on the scores.
// If lower-scoring box has too much overlap with a higher-scoring box,
// we get rid of the lower-scoring box.
// Complexity is O(N^2) pairwise comparison between boxes
TfLiteStatus NonMaxSuppressionSingleClassHelper(
    TfLiteContext* context, TfLiteNode* node, OpData* op_data,
    const float* scores, int* selected, int* selected_size,
    int max_detections) {

  const TfLiteTensor* input_box_encodings =
      GetInput(context, node, kInputTensorBoxEncodings);
  const int num_boxes = input_box_encodings->dims->data[1];
  const float non_max_suppression_score_threshold =
      op_data->non_max_suppression_score_threshold;
  const float intersection_over_union_threshold =
      op_data->intersection_over_union_threshold;
  // Maximum detections should be positive.
  TF_LITE_ENSURE(context, (max_detections >= 0));
  // intersection_over_union_threshold should be positive
  // and should be less than 1.
  TF_LITE_ENSURE(context, (intersection_over_union_threshold > 0.0f) &&
                              (intersection_over_union_threshold <= 1.0f));
  // Validate boxes
  TF_LITE_ENSURE(context, ValidateBoxes(op_data->decoded_boxes, num_boxes));

  // threshold scores
  int* keep_indices = op_data->keep_indices;
  float* keep_scores = op_data->keep_scores;
  int num_scores_kept = SelectDetectionsAboveScoreThreshold(
      scores, num_boxes, non_max_suppression_score_threshold, keep_scores, keep_indices);

  int* sorted_indices = op_data->sorted_indices;

  DecreasingPartialArgSort(keep_scores, num_scores_kept, num_scores_kept,
                            sorted_indices);

  const int num_boxes_kept = num_scores_kept;
  const int output_size = std::min(num_boxes_kept, max_detections);
  *selected_size = 0;

  int num_active_candidate = num_boxes_kept;
  uint8_t* active_box_candidate = op_data->active_box_candidate;

  for (int row = 0; row < num_boxes_kept; row++) {
    active_box_candidate[row] = 1;
  }
  for (int i = 0; i < num_boxes_kept; ++i) {
    if (num_active_candidate == 0 || *selected_size >= output_size) break;
    if (active_box_candidate[i] == 1) {
      selected[(*selected_size)++] = keep_indices[sorted_indices[i]];
      active_box_candidate[i] = 0;
      num_active_candidate--;
    } else {
      continue;
    }
    for (int j = i + 1; j < num_boxes_kept; ++j) {
      if (active_box_candidate[j] == 1) {
        float intersection_over_union = ComputeIntersectionOverUnion(
            op_data->decoded_boxes, keep_indices[sorted_indices[i]],
            keep_indices[sorted_indices[j]]);

        if (intersection_over_union > intersection_over_union_threshold) {
          active_box_candidate[j] = 0;
          num_active_candidate--;
        }
      }
    }
  }

  return kTfLiteOk;
}

// This function implements a regular version of Non Maximal Suppression (NMS)
// for multiple classes where
// 1) we do NMS separately for each class across all anchors and
// 2) keep only the highest anchor scores across all classes
// 3) The worst runtime of the regular NMS is O(K*N^2)
// where N is the number of anchors and K the number of
// classes.
TfLiteStatus NonMaxSuppressionMultiClassRegularHelper(TfLiteContext* context,
                                                      TfLiteNode* node,
                                                      OpData* op_data,
                                                      const float* scores) {
  const TfLiteTensor* input_box_encodings =
      GetInput(context, node, kInputTensorBoxEncodings);
  const TfLiteTensor* input_class_predictions =
      GetInput(context, node, kInputTensorClassPredictions);
  TfLiteTensor* detection_boxes =
      GetOutput(context, node, kOutputTensorDetectionBoxes);
  TfLiteTensor* detection_classes =
      GetOutput(context, node, kOutputTensorDetectionClasses);
  TfLiteTensor* detection_scores =
      GetOutput(context, node, kOutputTensorDetectionScores);
  TfLiteTensor* num_detections =
      GetOutput(context, node, kOutputTensorNumDetections);

  const int num_boxes = input_box_encodings->dims->data[1];
  const int num_classes = op_data->num_classes;
  const int num_detections_per_class = op_data->detections_per_class;
  const int max_detections = op_data->max_detections;
  const int num_classes_with_background =
      input_class_predictions->dims->data[2];
  // The row index offset is 1 if background class is included and 0 otherwise.
  int label_offset = num_classes_with_background - num_classes;
  TF_LITE_ENSURE(context, num_detections_per_class > 0);

  // For each class, perform non-max suppression.
  float* class_scores =  op_data->score_buffer;
  int* box_indices_after_regular_non_max_suppression = op_data->buffer;
  float* scores_after_regular_non_max_suppression =
      op_data->scores_after_regular_non_max_suppression;

  int size_of_sorted_indices = 0;
  int* sorted_indices = op_data->sorted_indices;
  float* sorted_values = op_data->sorted_values;

  for (int col = 0; col < num_classes; col++) {
    for (int row = 0; row < num_boxes; row++) {
      // Get scores of boxes corresponding to all anchors for single class
      class_scores[row] =
          *(scores + row * num_classes_with_background + col + label_offset);
    }
    // Perform non-maximal suppression on single class
    int selected_size = 0;
    int* selected = op_data->selected;
    TF_LITE_ENSURE_STATUS(NonMaxSuppressionSingleClassHelper(
        context, node, op_data, class_scores, selected, &selected_size,
        num_detections_per_class));
    // Add selected indices from non-max suppression of boxes in this class
    int output_index = size_of_sorted_indices;
    for (int i = 0; i < selected_size; i++) {
      int selected_index = selected[i];

      box_indices_after_regular_non_max_suppression[output_index] =
          (selected_index * num_classes_with_background + col + label_offset);
      scores_after_regular_non_max_suppression[output_index] =
          class_scores[selected_index];
      output_index++;
    }
    // Sort the max scores among the selected indices
    // Get the indices for top scores
    int num_indices_to_sort = std::min(output_index, max_detections);
    DecreasingPartialArgSort(scores_after_regular_non_max_suppression,
                             output_index, num_indices_to_sort,
                             sorted_indices);

    // Copy values to temporary vectors
    for (int row = 0; row < num_indices_to_sort; row++) {
      int temp = sorted_indices[row];
      sorted_indices[row] = box_indices_after_regular_non_max_suppression[temp];
      sorted_values[row] = scores_after_regular_non_max_suppression[temp];
    }
    // Copy scores and indices from temporary vectors
    for (int row = 0; row < num_indices_to_sort; row++) {
      box_indices_after_regular_non_max_suppression[row] = sorted_indices[row];
      scores_after_regular_non_max_suppression[row] = sorted_values[row];
    }
    size_of_sorted_indices = num_indices_to_sort;
  }

  // Allocate output tensors
  for (int output_box_index = 0; output_box_index < max_detections;
       output_box_index++) {
    if (output_box_index < size_of_sorted_indices) {
      const int anchor_index = floor(
          box_indices_after_regular_non_max_suppression[output_box_index] /
          num_classes_with_background);
      const int class_index =
          box_indices_after_regular_non_max_suppression[output_box_index] -
          anchor_index * num_classes_with_background - label_offset;
      const float selected_score =
          scores_after_regular_non_max_suppression[output_box_index];
      // detection_boxes
      ReInterpretTensor<BoxCornerEncoding*>(detection_boxes)[output_box_index] =
          reinterpret_cast<BoxCornerEncoding*>(op_data->decoded_boxes)[anchor_index];
      // detection_classes
      GetTensorData<float>(detection_classes)[output_box_index] = class_index;
      // detection_scores
      GetTensorData<float>(detection_scores)[output_box_index] = selected_score;
    } else {
      ReInterpretTensor<BoxCornerEncoding*>(
          detection_boxes)[output_box_index] = {0.0f, 0.0f, 0.0f, 0.0f};
      // detection_classes
      GetTensorData<float>(detection_classes)[output_box_index] = 0.0f;
      // detection_scores
      GetTensorData<float>(detection_scores)[output_box_index] = 0.0f;
    }
  }
  GetTensorData<float>(num_detections)[0] = size_of_sorted_indices;

  return kTfLiteOk;
}

// This function implements a fast version of Non Maximal Suppression for
// multiple classes where
// 1) we keep the top-k scores for each anchor and
// 2) during NMS, each anchor only uses the highest class score for sorting.
// 3) Compared to standard NMS, the worst runtime of this version is O(N^2)
// instead of O(KN^2) where N is the number of anchors and K the number of
// classes.
TfLiteStatus NonMaxSuppressionMultiClassFastHelper(TfLiteContext* context,
                                                   TfLiteNode* node,
                                                   OpData* op_data,
                                                   const float* scores) {
  const TfLiteTensor* input_box_encodings =
      GetInput(context, node, kInputTensorBoxEncodings);
  const TfLiteTensor* input_class_predictions =
      GetInput(context, node, kInputTensorClassPredictions);
  TfLiteTensor* detection_boxes =
      GetOutput(context, node, kOutputTensorDetectionBoxes);

  TfLiteTensor* detection_classes =
      GetOutput(context, node, kOutputTensorDetectionClasses);
  TfLiteTensor* detection_scores =
      GetOutput(context, node, kOutputTensorDetectionScores);
  TfLiteTensor* num_detections =
      GetOutput(context, node, kOutputTensorNumDetections);

  const int num_boxes = input_box_encodings->dims->data[1];
  const int num_classes = op_data->num_classes;
  const int max_categories_per_anchor = op_data->max_classes_per_detection;
  const int num_classes_with_background =
      input_class_predictions->dims->data[2];

  // The row index offset is 1 if background class is included and 0 otherwise.
  int label_offset = num_classes_with_background - num_classes;
  TF_LITE_ENSURE(context, (max_categories_per_anchor > 0));
  const int num_categories_per_anchor =
      std::min(max_categories_per_anchor, num_classes);
  float* max_scores = op_data->score_buffer;
  int* sorted_class_indices = op_data->buffer;
  for (int row = 0; row < num_boxes; row++) {
    const float* box_scores =
        scores + row * num_classes_with_background + label_offset;
    int* class_indices = sorted_class_indices + row * num_classes;
    DecreasingPartialArgSort(box_scores, num_classes, num_categories_per_anchor,
                             class_indices);
    max_scores[row] = box_scores[class_indices[0]];
  }

  // Perform non-maximal suppression on max scores
  int selected_size = 0;
  int* selected = op_data->selected;
  TF_LITE_ENSURE_STATUS(NonMaxSuppressionSingleClassHelper(
      context, node, op_data, max_scores, selected, &selected_size, op_data->max_detections));

  // Allocate output tensors
  int output_box_index = 0;

  for (int i = 0; i < selected_size; i++) {
    int selected_index = selected[i];

    const float* box_scores =
        scores + selected_index * num_classes_with_background + label_offset;
    const int* class_indices =
        sorted_class_indices + selected_index * num_classes;

    for (int col = 0; col < num_categories_per_anchor; ++col) {
      int box_offset = num_categories_per_anchor * output_box_index + col;

      // detection_boxes
      ReInterpretTensor<BoxCornerEncoding*>(detection_boxes)[box_offset] =
          reinterpret_cast<BoxCornerEncoding*>(op_data->decoded_boxes)[selected_index];

      // detection_classes
      GetTensorData<float>(detection_classes)[box_offset] = class_indices[col];

      // detection_scores
      GetTensorData<float>(detection_scores)[box_offset] =
          box_scores[class_indices[col]];

      output_box_index++;
    }
  }

  GetTensorData<float>(num_detections)[0] = output_box_index;
  return kTfLiteOk;
}

void DequantizeClassPredictions(const TfLiteTensor* input_class_predictions,
                                const int num_boxes,
                                const int num_classes_with_background,
                                float* scores) {
  float quant_zero_point =
      static_cast<float>(input_class_predictions->params.zero_point);
  float quant_scale = static_cast<float>(input_class_predictions->params.scale);
  Dequantizer dequantize(quant_zero_point, quant_scale);
  const uint8* scores_quant = GetTensorData<uint8>(input_class_predictions);
  for (int idx = 0; idx < num_boxes * num_classes_with_background; ++idx) {
    scores[idx] = dequantize(scores_quant[idx]);
  }
}

TfLiteStatus NonMaxSuppressionMultiClass(TfLiteContext* context,
                                         TfLiteNode* node, OpData* op_data) {
  // Get the input tensors
  const TfLiteTensor* input_box_encodings =
      GetInput(context, node, kInputTensorBoxEncodings);
  const TfLiteTensor* input_class_predictions =
      GetInput(context, node, kInputTensorClassPredictions);
  const int num_boxes = input_box_encodings->dims->data[1];
  const int num_classes = op_data->num_classes;

  TF_LITE_ENSURE_EQ(context, input_class_predictions->dims->data[0],
                    kBatchSize);
  TF_LITE_ENSURE_EQ(context, input_class_predictions->dims->data[1], num_boxes);
  const int num_classes_with_background =
      input_class_predictions->dims->data[2];

  TF_LITE_ENSURE(context, (num_classes_with_background - num_classes <= 1));
  TF_LITE_ENSURE(context, (num_classes_with_background >= num_classes));

  const float* scores;
  switch (input_class_predictions->type) {
    case kTfLiteUInt8: {
      float* temporary_scores = op_data->scores;
      DequantizeClassPredictions(input_class_predictions, num_boxes,
                                 num_classes_with_background, temporary_scores);
      scores = temporary_scores;
    } break;
    case kTfLiteFloat32:
      scores = GetTensorData<float>(input_class_predictions);
      break;
    default:
      // Unsupported type.
      return kTfLiteError;
  }

  if (op_data->use_regular_non_max_suppression)
    TF_LITE_ENSURE_STATUS(NonMaxSuppressionMultiClassRegularHelper(context, node, op_data, scores));
  else
    TF_LITE_ENSURE_STATUS(NonMaxSuppressionMultiClassFastHelper(context, node, op_data, scores));

  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  // TODO(chowdhery): Generalize for any batch size
  TF_LITE_ENSURE(context, (kBatchSize == 1));

  // Set up scratch buffers
  void *raw;
  auto* op_data = static_cast<OpData*>(node->user_data);
  raw = context->GetScratchBuffer(context, op_data->active_candidate_idx);
  op_data->active_box_candidate = reinterpret_cast<uint8_t*>(raw);
  raw = context->GetScratchBuffer(context, op_data->decoded_boxes_idx);
  op_data->decoded_boxes = reinterpret_cast<float*>(raw);
  raw = context->GetScratchBuffer(context, op_data->scores_idx);
  op_data->scores = reinterpret_cast<float*>(raw);
  raw = context->GetScratchBuffer(context, op_data->score_buffer_idx);
  op_data->score_buffer = reinterpret_cast<float*>(raw);
  raw = context->GetScratchBuffer(context, op_data->keep_scores_idx);
  op_data->keep_scores = reinterpret_cast<float*>(raw);
  raw = context->GetScratchBuffer(context, op_data->scores_after_regular_non_max_suppression_idx);
  op_data->scores_after_regular_non_max_suppression = reinterpret_cast<float*>(raw);
  raw = context->GetScratchBuffer(context, op_data->sorted_values_idx);
  op_data->sorted_values = reinterpret_cast<float*>(raw);
  raw = context->GetScratchBuffer(context, op_data->keep_indices_idx);
  op_data->keep_indices = reinterpret_cast<int*>(raw);
  raw = context->GetScratchBuffer(context, op_data->sorted_indices_idx);
  op_data->sorted_indices = reinterpret_cast<int*>(raw);
  raw = context->GetScratchBuffer(context, op_data->buffer_idx);
  op_data->buffer = reinterpret_cast<int*>(raw);
  raw = context->GetScratchBuffer(context, op_data->selected_idx);
  op_data->selected = reinterpret_cast<int*>(raw);

  // These two functions correspond to two blocks in the Object Detection model.
  // In future, we would like to break the custom op in two blocks, which is
  // currently not feasible because we would like to input quantized inputs
  // and do all calculations in float. Mixed quantized/float calculations are
  // currently not supported in TFLite.

  // This fills in temporary decoded_boxes
  // by transforming input_box_encodings and input_anchors from
  // CenterSizeEncodings to BoxCornerEncoding
  TF_LITE_ENSURE_STATUS(DecodeCenterSizeBoxes(context, node, op_data));

  // This fills in the output tensors
  // by choosing effective set of decoded boxes
  // based on Non Maximal Suppression, i.e. selecting
  // highest scoring non-overlapping boxes.
  TF_LITE_ENSURE_STATUS(NonMaxSuppressionMultiClass(context, node, op_data));

  // TODO(chowdhery): Generalize for any batch size

  return kTfLiteOk;
}

}  // namespace detection_postprocess

TfLiteRegistration* Register_DETECTION_POSTPROCESS() {
  static TfLiteRegistration r = {
      detection_postprocess::Init, detection_postprocess::Free,
      detection_postprocess::Prepare, detection_postprocess::Eval};
  return &r;
}

}  // namespace custom
}  // namespace micro
}  // namespace ops
}  // namespace tflite
