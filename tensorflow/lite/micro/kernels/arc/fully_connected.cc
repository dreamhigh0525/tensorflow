/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/kernels/internal/reference/fully_connected.h"

#include "mli_api.h"  // NOLINT
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/fully_connected.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/arc/mli_tf_utils.h"
#include "tensorflow/lite/micro/kernels/arc/scratch_buffers.h"
#include "tensorflow/lite/micro/kernels/arc/scratch_buf_mgr.h"

#include "mli_api.h"


namespace tflite {
namespace ops {
namespace micro {
namespace fully_connected {
namespace {

struct OpData {
  // The scaling factor from input to output (aka the 'real multiplier') can
  // be represented as a fixed point multiplier plus a left shift.
  int32_t output_multiplier;
  int output_shift;
  // The range of the fused activation layer. For example for kNone and
  // uint8_t these would be 0 and 255.
  int32_t output_activation_min;
  int32_t output_activation_max;
  // The index of the temporary tensor where the quantized inputs are cached.
  int input_quantized_index;
};

constexpr int kInputTensor = 0;
constexpr int kWeightsTensor = 1;
constexpr int kBiasTensor = 2;
constexpr int kOutputTensor = 0;

TfLiteStatus CalculateOpData(TfLiteContext* context,
                             TfLiteFullyConnectedParams* params,
                             TfLiteType data_type, const TfLiteTensor* input,
                             const TfLiteTensor* filter,
                             const TfLiteTensor* bias, TfLiteTensor* output,
                             OpData* data) {
  TfLiteStatus status = kTfLiteOk;
  if (data_type != kTfLiteFloat32) {
    double real_multiplier = 0.0;
    TF_LITE_ENSURE_STATUS(GetQuantizedConvolutionMultipler(
        context, input, filter, bias, output, &real_multiplier));
    int exponent;
    QuantizeMultiplier(real_multiplier, &data->output_multiplier, &exponent);
    data->output_shift = -exponent;
    TF_LITE_ENSURE_STATUS(CalculateActivationRangeQuantized(
        context, params->activation, output, &data->output_activation_min,
        &data->output_activation_max));
  }
  return status;
}

}  // namespace

TfLiteStatus EvalQuantizedInt8(TfLiteContext* context, TfLiteNode* node,
                               TfLiteFullyConnectedParams* params, OpData* data,
                               const TfLiteTensor* input,
                               const TfLiteTensor* filter,
                               const TfLiteTensor* bias, TfLiteTensor* output) {
  // Run Fully Connected MLI kernel
  // MLI optimized version only supports int8 dataype and no fused Relu
  // TODO: subject to add mli_saturate kernel
  // work around for issue #35318, mli fully connect kernel only supports
  // zeropoint == 0 for weights. this check can be removed once issue #35318 is
  // resolved.
  if ((filter->params.zero_point == 0) &&
      (input->type == kTfLiteInt8 && params->activation == kTfLiteActNone)) {
    mli_tensor mli_in = {0};
    mli_tensor mli_weights = {0};
    mli_tensor mli_bias = {0};
    mli_tensor mli_out = {0};

    ConvertToMliTensor<int8_t>(input, &mli_in);
    ConvertToMliTensor<int8_t>(filter, &mli_weights);
    ConvertToMliTensor<int32_t>(bias, &mli_bias);
    ConvertToMliTensor<int8_t>(output, &mli_out);

    mli_point_to_subtsr_cfg subtsr_cfg_in = {{0, 0}, 2, static_cast<uint8_t>(mli_in.shape[1])};
    mli_point_to_subtsr_cfg subtsr_cfg_out = {{0, 0}, 2, static_cast<uint8_t>(mli_out.shape[1])};
    mli_tensor sub_mli_in = {0};
    mli_tensor sub_mli_out = {0};
    mli_hlp_point_to_subtensor(&mli_in, &subtsr_cfg_in, &sub_mli_in);
    mli_hlp_point_to_subtensor(&mli_out, &subtsr_cfg_out, &sub_mli_out);

    // Tensors for data in fast (local) memory and config to copy data from external to local memory
    mli_tensor weights_local = mli_weights;
    mli_tensor bias_local = mli_bias;
    mli_tensor in_local = sub_mli_in;
    mli_tensor out_local = sub_mli_out;
    mli_mov_cfg_t copy_config;
    mli_mov_cfg_for_copy(&copy_config);
    TF_LITE_ENSURE_STATUS(get_arc_scratch_buffer_for_conv_tensors(context, &in_local, &weights_local, &bias_local, &out_local));
    bool in_is_local = in_local.data == sub_mli_in.data;
    bool out_is_local = out_local.data == sub_mli_out.data;

    mli_mov_tensor_sync(&mli_weights, &copy_config, &weights_local);
    mli_mov_tensor_sync(&mli_bias, &copy_config, &bias_local);

    const int batches =
        MatchingDim(GetTensorShape(input), 0, GetTensorShape(output), 0);

    for (int i = 0; i < batches; i++) {
      mli_mov_tensor_sync(&sub_mli_in, &copy_config, &in_local);
      mli_krn_fully_connected_sa8_sa8_sa32(&in_local, &weights_local, &bias_local, &out_local);
      mli_mov_tensor_sync(&out_local, &copy_config, &sub_mli_out);
      subtsr_cfg_in.start_coord[0]++;
      subtsr_cfg_out.start_coord[0]++;
      mli_hlp_point_to_subtensor(&mli_in, &subtsr_cfg_in, &sub_mli_in);
      mli_hlp_point_to_subtensor(&mli_out, &subtsr_cfg_out, &sub_mli_out);
      if (in_is_local) {
        in_local.data = sub_mli_in.data;
      }
      if (out_is_local) {
        out_local.data = sub_mli_out.data;
      }
    }
  } else {
    FullyConnectedParams op_params;
    op_params.input_offset = -input->params.zero_point;
    op_params.weights_offset = -filter->params.zero_point;
    op_params.output_offset = output->params.zero_point;
    op_params.output_multiplier = data->output_multiplier;
    // TODO(b/138810107): Figure out whether output shift should be inverted
    op_params.output_shift = -data->output_shift;
    op_params.quantized_activation_min = data->output_activation_min;
    op_params.quantized_activation_max = data->output_activation_max;

    reference_integer_ops::FullyConnected(
        op_params, GetTensorShape(input), GetTensorData<int8_t>(input),
        GetTensorShape(filter), GetTensorData<int8_t>(filter),
        GetTensorShape(bias), GetTensorData<int32_t>(bias),
        GetTensorShape(output), GetTensorData<int8_t>(output));
  }
  return kTfLiteOk;
}

TfLiteStatus EvalQuantized(TfLiteContext* context, TfLiteNode* node,
                           TfLiteFullyConnectedParams* params, OpData* data,
                           const TfLiteTensor* input,
                           const TfLiteTensor* filter, const TfLiteTensor* bias,
                           TfLiteTensor* output) {
  const int32_t input_offset = -input->params.zero_point;
  const int32_t filter_offset = -filter->params.zero_point;
  const int32_t output_offset = output->params.zero_point;

  tflite::FullyConnectedParams op_params;
  op_params.input_offset = input_offset;
  op_params.weights_offset = filter_offset;
  op_params.output_offset = output_offset;
  op_params.output_multiplier = data->output_multiplier;
  // Legacy ops used mixed left and right shifts. Now all are +ve-means-left.
  op_params.output_shift = -data->output_shift;
  op_params.quantized_activation_min = data->output_activation_min;
  op_params.quantized_activation_max = data->output_activation_max;

#define TF_LITE_FULLY_CONNECTED(output_data_type)                      \
  reference_ops::FullyConnected(                                       \
      op_params, GetTensorShape(input), GetTensorData<uint8_t>(input), \
      GetTensorShape(filter), GetTensorData<uint8_t>(filter),          \
      GetTensorShape(bias), GetTensorData<int32_t>(bias),              \
      GetTensorShape(output), GetTensorData<output_data_type>(output))
  switch (output->type) {
    case kTfLiteUInt8:
      TF_LITE_FULLY_CONNECTED(uint8_t);
      break;
    case kTfLiteInt16:
      TF_LITE_FULLY_CONNECTED(int16_t);
      break;
    default:
      TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                         TfLiteTypeGetName(output->type), output->type);
      return kTfLiteError;
  }

  return kTfLiteOk;
}

TfLiteStatus EvalFloat(TfLiteContext* context, TfLiteNode* node,
                       TfLiteFullyConnectedParams* params, OpData* data,
                       const TfLiteTensor* input, const TfLiteTensor* filter,
                       const TfLiteTensor* bias, TfLiteTensor* output) {
  float output_activation_min, output_activation_max;
  CalculateActivationRange(params->activation, &output_activation_min,
                           &output_activation_max);
  tflite::FullyConnectedParams op_params;
  op_params.float_activation_min = output_activation_min;
  op_params.float_activation_max = output_activation_max;
  tflite::reference_ops::FullyConnected(
      op_params, GetTensorShape(input), GetTensorData<float>(input),
      GetTensorShape(filter), GetTensorData<float>(filter),
      GetTensorShape(bias), GetTensorData<float>(bias), GetTensorShape(output),
      GetTensorData<float>(output));
  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  auto* params =
      reinterpret_cast<TfLiteFullyConnectedParams*>(node->builtin_data);

  const TfLiteTensor* input = GetInput(context, node, kInputTensor);
  const TfLiteTensor* filter = GetInput(context, node, kWeightsTensor);
  const TfLiteTensor* bias = GetOptionalInputTensor(context, node, kBiasTensor);
  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

  TfLiteType data_type = input->type;
  OpData local_data_object;
  OpData* data = &local_data_object;
  TF_LITE_ENSURE_STATUS(CalculateOpData(context, params, data_type, input,
                                        filter, bias, output, data));

  switch (filter->type) {  // Already know in/out types are same.
    case kTfLiteFloat32:
      return EvalFloat(context, node, params, data, input, filter, bias,
                       output);
    case kTfLiteInt8:
      return EvalQuantizedInt8(context, node, params, data, input, filter, bias,
                               output);

    case kTfLiteUInt8:
      return EvalQuantized(context, node, params, data, input, filter, bias,
                           output);

    default:
      TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                         TfLiteTypeGetName(filter->type), filter->type);
      return kTfLiteError;
  }
  return kTfLiteOk;
}

}  // namespace fully_connected

TfLiteRegistration* Register_FULLY_CONNECTED() {
  static TfLiteRegistration r = {/*init=*/nullptr,
                                 /*free=*/nullptr,
                                 /*prepare=*/nullptr,
                                 /*invoke=*/fully_connected::Eval,
                                 /*profiling_string=*/nullptr,
                                 /*builtin_code=*/0,
                                 /*custom_name=*/nullptr,
                                 /*version=*/0};

  return &r;
}

}  // namespace micro
}  // namespace ops
}  // namespace tflite
