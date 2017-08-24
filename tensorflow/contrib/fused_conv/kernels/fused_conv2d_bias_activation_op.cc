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

#define EIGEN_USE_THREADS

#if GOOGLE_CUDA
#define EIGEN_USE_GPU
#endif  // GOOGLE_CUDA

#include "tensorflow/contrib/fused_conv/kernels/fused_conv2d_bias_activation_op.h"

#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_slice.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/kernels/conv_2d.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/util/padding.h"
#include "tensorflow/core/util/use_cudnn.h"

#if GOOGLE_CUDA
#include "tensorflow/core/kernels/conv_ops_gpu.h"
#include "tensorflow/core/platform/stream_executor.h"
#include "tensorflow/core/util/activation_mode.h"
#endif  // GOOGLE_CUDA

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

template <typename T>
struct RawType {
  using type = T;
};

template <>
struct RawType<qint8> {
  using type = int8;
};

// T is the element type of the conv_input, filter and side_input tensors.
// BiasType is the element type of the bias tensor, which can be different.
// ScaleType is the type used for conv_input_scale, side_input_scale.
template <typename Device, typename T, typename BiasType, typename ScaleType>
class FusedConv2DBiasActivationOp : public OpKernel {
 public:
  explicit FusedConv2DBiasActivationOp(OpKernelConstruction* context)
      : OpKernel(context) {
    string data_format_str, filter_format_str;
    OP_REQUIRES_OK(context, context->GetAttr("data_format", &data_format_str));
    OP_REQUIRES(context, FormatFromString(data_format_str, &data_format_),
                errors::InvalidArgument("Invalid data format"));
    OP_REQUIRES_OK(context,
                   context->GetAttr("filter_format", &filter_format_str));
    OP_REQUIRES(context,
                FilterFormatFromString(filter_format_str, &filter_format_),
                errors::InvalidArgument("Invalid filter format"));

    std::vector<int32> strides;
    OP_REQUIRES_OK(context, context->GetAttr("strides", &strides));
    OP_REQUIRES(context, strides.size() == 4,
                errors::InvalidArgument("Sliding window strides field must "
                                        "specify 4 dimensions"));

    stride_rows_ = GetTensorDim(strides, data_format_, 'H');
    stride_cols_ = GetTensorDim(strides, data_format_, 'W');
    OP_REQUIRES(
        context,
        (GetTensorDim(strides, data_format_, 'N') == 1 &&
         GetTensorDim(strides, data_format_, 'C') == 1),
        errors::InvalidArgument("Convolutional strides are not supported in "
                                "the batch or depth dimensions."));

    // Note: Only NCHW_VECT_C format is supported for int8.
    // This is because it is expected to be the fastest, and our previous tests
    // found cudnn 6 does not fully support the other formats for int8 mode.
    OP_REQUIRES(
        context,
        (std::is_same<T, qint8>::value == (data_format_ == FORMAT_NCHW_VECT_C)),
        errors::InvalidArgument(
            "qint8 should be used with data_format NCHW_VECT_C."));

    OP_REQUIRES(context,
                (std::is_same<T, qint8>::value ==
                 (filter_format_ == FORMAT_OIHW_VECT_I)),
                errors::InvalidArgument(
                    "qint8 should be used with filter_format OIHW_VECT_I."));

    OP_REQUIRES_OK(context, context->GetAttr("padding", &padding_type_));
    eigen_padding_type_ = BrainPadding2EigenPadding(padding_type_);
    string activation_mode_str;
    OP_REQUIRES_OK(context,
                   context->GetAttr("activation_mode", &activation_mode_str));
    OP_REQUIRES_OK(context, GetActivationModeFromString(activation_mode_str,
                                                        &activation_mode_));
    OP_REQUIRES(context, activation_mode_ == ActivationMode::RELU,
                errors::InvalidArgument("Current implementation only supports "
                                        "RELU as the activation function."));
    cudnn_use_autotune_ = CudnnUseAutotune();
    float conv_input_scale_flt, side_input_scale_flt;
    OP_REQUIRES_OK(context,
                   context->GetAttr("conv_input_scale", &conv_input_scale_flt));
    OP_REQUIRES_OK(context,
                   context->GetAttr("side_input_scale", &side_input_scale_flt));
    conv_input_scale_ = conv_input_scale_flt;
    side_input_scale_ = side_input_scale_flt;
  }

  Status CheckShape(const Tensor& tensor, const string& tensor_name) {
    const int num_dims = tensor.dims();
    for (int i = 0; i < num_dims; i++) {
      if (!FastBoundsCheck(tensor.dim_size(i),
                           std::numeric_limits<int32>::max())) {
        return errors::InvalidArgument(tensor_name, " dimension ", i,
                                       " too large");
      }
    }
    // If there is a 5th dimension it is the VECT_C or VECT_I dimension.
    if (num_dims == 5 && tensor.dim_size(4) != 4) {
      return errors::InvalidArgument("The last dimension of ", tensor_name,
                                     " must be of size 4 for qint8.");
    }
    return Status::OK();
  }

  void Compute(OpKernelContext* context) override {
    // The conv_input tensor is one of the following formats:
    // NHWC, NCHW, NCHW_VECT_C.
    const Tensor& conv_input = context->input(0);
    OP_REQUIRES_OK(context, CheckShape(conv_input, "conv_input"));

    // The filter tensor is one of the following formats:
    // HWIO, OIHW, OIHW_VECT_I.
    const Tensor& filter = context->input(1);
    OP_REQUIRES_OK(context, CheckShape(filter, "filter"));

    // Input bias is a 1-D tensor, with size matching output depth.
    const Tensor& bias = context->input(2);
    OP_REQUIRES_OK(context, CheckShape(bias, "conv_input"));

    // If side_input_scale != 0, then side_input is not ignored and
    // has the same type and dimensions as the output.
    const Tensor& side_input = context->input(3);
    if (side_input_scale_ != 0) {
      OP_REQUIRES_OK(context, CheckShape(side_input, "side_input"));
    }

    // TODO(pauldonnelly): Switch to a more efficient mechanism to access
    // dimension indexes and per-dimension attributes.
    const int32 filter_rows = GetFilterDim(filter, filter_format_, 'H');
    const int32 filter_cols = GetFilterDim(filter, filter_format_, 'W');
    const int32 output_depth = GetFilterDim(filter, filter_format_, 'O');

    const int32 batch_size = GetTensorDim(conv_input, data_format_, 'N');
    const int32 conv_input_rows = GetTensorDim(conv_input, data_format_, 'H');
    const int32 conv_input_cols = GetTensorDim(conv_input, data_format_, 'W');

    int64 output_rows = 0, output_cols = 0, pad_rows = 0, pad_cols = 0;
    OP_REQUIRES_OK(context, GetWindowedOutputSize(conv_input_rows, filter_rows,
                                                  stride_rows_, padding_type_,
                                                  &output_rows, &pad_rows));
    OP_REQUIRES_OK(context, GetWindowedOutputSize(conv_input_cols, filter_cols,
                                                  stride_cols_, padding_type_,
                                                  &output_cols, &pad_cols));
    // Initialize the output tensor shape according to data_format_
    TensorShape output_shape = ShapeFromFormat(
        data_format_, batch_size, output_rows, output_cols, output_depth);
    Tensor* output = nullptr;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &output));

    VLOG(2) << "FusedConv2DBiasActivation: conv_input_cols = "
            << conv_input_cols << ", conv_input_rows = " << conv_input_rows
            << ", filter_cols = " << filter_cols
            << ", filter_rows = " << filter_rows
            << ", stride_cols = " << stride_cols_
            << ", stride_rows = " << stride_rows_
            << ", output_depth = " << output_depth
            << ", output_cols = " << output_cols
            << ", output_rows = " << output_rows
            << ", output_shape.num_elements = " << output_shape.num_elements();

    // If there is nothing to compute, return.
    if (output_shape.num_elements() == 0) {
      return;
    }

    launcher_.launch(context, cudnn_use_autotune_, conv_input,
                     conv_input_scale_, filter, stride_rows_, stride_cols_,
                     eigen_padding_type_, side_input, side_input_scale_, bias,
                     activation_mode_, data_format_, filter_format_, output);
  }

 private:
  int32 stride_rows_, stride_cols_;
  Padding padding_type_;
  Eigen::PaddingType eigen_padding_type_;
  ActivationMode activation_mode_;
  TensorFormat data_format_;
  FilterTensorFormat filter_format_;
  ScaleType conv_input_scale_;
  ScaleType side_input_scale_;
  LaunchFusedConv2DBiasActivationOp<Device, T, BiasType, ScaleType> launcher_;
  bool cudnn_use_autotune_;

  TF_DISALLOW_COPY_AND_ASSIGN(FusedConv2DBiasActivationOp);
};

#if GOOGLE_CUDA
namespace dnn = ::perftools::gputools::dnn;

// A dummy type to group forward convolution autotune results together.
struct ConvBiasActivationAutoTuneGroup {
  static string name() { return "ConvBiasActivation"; }
};
typedef AutoTuneSingleton<ConvBiasActivationAutoTuneGroup, FusedConvParameters,
                          dnn::AlgorithmConfig>
    AutoTuneConvBiasActivation;

// Allocates 'transformed_tensor' and transforms 'nhwc_tensor' into it
// using the specified 'batch_size', 'rows', 'cols', and 'depth' dimensions.
template <typename T, size_t NDIMS>
Status TransformNHWCToNCHW(OpKernelContext* ctx, const Tensor& nhwc_tensor,
                           int batch_size, int rows, int cols, int depth,
                           Tensor* transformed_tensor, const Tensor** result) {
  TensorShape nchw_shape =
      ShapeFromFormat(FORMAT_NCHW, batch_size, rows, cols, depth);
  if (depth > 1) {
    TF_RETURN_IF_ERROR(ctx->allocate_temp(DataTypeToEnum<T>::value, nchw_shape,
                                          transformed_tensor));
    functor::NHWCToNCHW<GPUDevice, T, NDIMS>()(
        ctx->eigen_device<GPUDevice>(), nhwc_tensor.tensor<T, NDIMS>(),
        transformed_tensor->tensor<T, NDIMS>());
  } else {
    // If depth <= 1, then just reshape.
    CHECK(transformed_tensor->CopyFrom(nhwc_tensor, nchw_shape));
  }
  *result = transformed_tensor;
  return Status::OK();
}

template <typename T, typename BiasType, typename ScaleType>
void LaunchFusedConv2DBiasActivationOp<GPUDevice, T, BiasType, ScaleType>::
    launch(OpKernelContext* ctx, bool cudnn_use_autotune,
           const Tensor& conv_input_param, ScaleType conv_input_scale,
           const Tensor& filter_param, int32 row_stride, int32 col_stride,
           const Eigen::PaddingType& padding, const Tensor& side_input_param,
           ScaleType side_input_scale, const Tensor& bias,
           ActivationMode activation_mode, TensorFormat data_format,
           FilterTensorFormat filter_format, Tensor* output_param) {
  auto* stream = ctx->op_device_context()->stream();
  OP_REQUIRES(ctx, stream, errors::Internal("No GPU stream available."));

  // TODO(yangzihao): refactor all the complicated/duplicated code in regular
  // conv ops to a shared conv utility.

  // Assuming qint8 <--> NCHW_VECT_C, OIHW_VECT_I here.
  constexpr int rank = std::is_same<T, qint8>::value ? 5 : 4;
  constexpr int vect = std::is_same<T, qint8>::value ? 4 : 1;

  const int batch_size = GetTensorDim(conv_input_param, data_format, 'N');
  int conv_input_rows = GetTensorDim(conv_input_param, data_format, 'H');
  int conv_input_cols = GetTensorDim(conv_input_param, data_format, 'W');

  const int conv_input_depth =
      GetTensorDim(conv_input_param, data_format, 'C') * vect;
  const int output_rows = GetTensorDim(*output_param, data_format, 'H');
  const int output_cols = GetTensorDim(*output_param, data_format, 'W');
  const int output_depth = GetFilterDim(filter_param, filter_format, 'O');
  const int filter_rows = GetFilterDim(filter_param, filter_format, 'H');
  const int filter_cols = GetFilterDim(filter_param, filter_format, 'W');

  int padding_rows = 0;
  int padding_cols = 0;
  const Tensor* conv_input = &conv_input_param;
  Tensor maybe_padded_conv_input;
  if (padding == Eigen::PADDING_SAME) {
    // Total padding on rows and cols is
    // Pr = (R' - 1) * S + Kr - R
    // Pc = (C' - 1) * S + Kc - C
    // where (R', C') are output dimensions, (R, C) are input dimensions, S
    // is stride, (Kr, Kc) are filter dimensions.
    // We pad Pr/2 on the left and Pr - Pr/2 on the right, Pc/2 on the top
    // and Pc - Pc/2 on the bottom.  When Pr or Pc is odd, this means
    // we pad more on the right and bottom than on the top and left.
    padding_rows = std::max<int>(
        0, (output_rows - 1) * row_stride + filter_rows - conv_input_rows);
    padding_cols = std::max<int>(
        0, (output_cols - 1) * col_stride + filter_cols - conv_input_cols);
    const int padding_rows_parity = padding_rows & 1;
    const int padding_cols_parity = padding_cols & 1;
    if ((padding_rows_parity | padding_cols_parity) != 0) {
      Tensor transformed_input;
      const int new_conv_input_rows = conv_input_rows + padding_rows_parity;
      const int new_conv_input_cols = conv_input_cols + padding_cols_parity;

      OP_REQUIRES_OK(
          ctx, ctx->allocate_temp(
                   DataTypeToEnum<T>::value,
                   ShapeFromFormat(data_format, batch_size, new_conv_input_rows,
                                   new_conv_input_cols, conv_input_depth),
                   &maybe_padded_conv_input));

      functor::PadInput<GPUDevice, T, int, rank>()(
          ctx->eigen_device<GPUDevice>(),
          To32Bit(conv_input_param.tensor<T, rank>()), {{0, 0}},
          {{padding_rows_parity, padding_cols_parity}},
          To32Bit(maybe_padded_conv_input.tensor<T, rank>()), data_format);

      conv_input = &maybe_padded_conv_input;
      conv_input_rows = new_conv_input_rows;
      conv_input_cols = new_conv_input_cols;
    }
  }

  Tensor maybe_transformed_conv_input, maybe_transformed_side_input;
  Tensor maybe_transformed_output;
  const Tensor* side_input = &side_input_param;
  Tensor* output = output_param;

  // Assuming qint8 <--> NCHW_VECT_C, OIHW_VECT_I here.
  if (!std::is_same<T, qint8>::value && (data_format == FORMAT_NHWC)) {
    OP_REQUIRES_OK(ctx, (TransformNHWCToNCHW<T, rank>(
                            ctx, *conv_input, batch_size, conv_input_rows,
                            conv_input_cols, conv_input_depth,
                            &maybe_transformed_conv_input, &conv_input)));
    if (side_input_scale != 0) {
      OP_REQUIRES_OK(
          ctx, (TransformNHWCToNCHW<T, rank>(
                   ctx, side_input_param, batch_size, output_rows, output_cols,
                   output_depth, &maybe_transformed_side_input, &side_input)));
    }
    if (output_depth > 1) {
      // Allocate a tensor for the NCHW output of the kernel and point output
      // to it. Afterwards, we will transform it to NHWC while copying back to
      // 'output_param'.
      TensorShape nchw_shape = ShapeFromFormat(
          FORMAT_NCHW, batch_size, output_rows, output_cols, output_depth);
      OP_REQUIRES_OK(ctx,
                     ctx->allocate_temp(DataTypeToEnum<T>::value, nchw_shape,
                                        &maybe_transformed_output));
      output = &maybe_transformed_output;
    }
  }

  // Assuming qint8 <--> NCHW_VECT_C, OIHW_VECT_I here.
  constexpr auto data_layout = std::is_same<T, qint8>::value
                                   ? dnn::DataLayout::kBatchDepthYX4
                                   : dnn::DataLayout::kBatchDepthYX;
  constexpr auto filter_layout = std::is_same<T, qint8>::value
                                     ? dnn::FilterLayout::kOutputInputYX4
                                     : dnn::FilterLayout::kOutputInputYX;

  dnn::BatchDescriptor conv_input_desc;
  conv_input_desc.set_count(batch_size)
      .set_feature_map_count(conv_input_depth)
      .set_height(conv_input_rows)
      .set_width(conv_input_cols)
      .set_layout(data_layout);
  dnn::FilterDescriptor filter_desc;
  filter_desc.set_input_filter_height(filter_rows)
      .set_input_filter_width(filter_cols)
      .set_input_feature_map_count(conv_input_depth)
      .set_output_feature_map_count(output_depth)
      .set_layout(filter_layout);
  dnn::BatchDescriptor side_input_desc;
  side_input_desc.set_count(batch_size)
      .set_height(output_rows)
      .set_width(output_cols)
      .set_feature_map_count(output_depth)
      .set_layout(data_layout);
  dnn::BatchDescriptor bias_desc;
  bias_desc.set_count(1)
      .set_height(1)
      .set_width(1)
      .set_feature_map_count(output_depth)
      .set_layout(dnn::DataLayout::kBatchDepthYX);
  dnn::BatchDescriptor output_desc;
  output_desc.set_count(batch_size)
      .set_height(output_rows)
      .set_width(output_cols)
      .set_feature_map_count(output_depth)
      .set_layout(data_layout);
  dnn::ConvolutionDescriptor conv_desc;
  conv_desc.set_vertical_filter_stride(row_stride)
      .set_horizontal_filter_stride(col_stride)
      .set_zero_padding_height(padding_rows / 2)
      .set_zero_padding_width(padding_cols / 2);

  Tensor maybe_transformed_filter;
  const Tensor* filter;
  if (std::is_same<T, qint8>::value) {
    // We have already checked filter is OIHW_VECT_I in the constructor.
    filter = &filter_param;
  } else if (filter_format == FORMAT_HWIO) {
    // Shuffle filter tensor from HWIO to OIHW:
    OP_REQUIRES_OK(ctx, ctx->allocate_temp(
                            DataTypeToEnum<T>::value,
                            ShapeFromFilterFormat(
                                FORMAT_OIHW, filter_param.shape(), FORMAT_HWIO),
                            &maybe_transformed_filter));
    functor::TransformFilter<GPUDevice, T, int, 4>()(
        ctx->eigen_device<GPUDevice>(), To32Bit(filter_param.tensor<T, 4>()),
        To32Bit(maybe_transformed_filter.tensor<T, 4>()));
    filter = &maybe_transformed_filter;
  }

  auto conv_input_ptr =
      AsDeviceMemory(reinterpret_cast<const typename RawType<T>::type*>(
                         conv_input->template flat<T>().data()),
                     conv_input->template flat<T>().size());
  auto filter_ptr =
      AsDeviceMemory(reinterpret_cast<const typename RawType<T>::type*>(
                         filter->template flat<T>().data()),
                     filter->template flat<T>().size());
  auto side_input_ptr =
      AsDeviceMemory(reinterpret_cast<const typename RawType<T>::type*>(
                         side_input->template flat<T>().data()),
                     side_input->template flat<T>().size());
  auto output_ptr =
      AsDeviceMemory(reinterpret_cast<const typename RawType<T>::type*>(
                         output->template flat<T>().data()),
                     output->template flat<T>().size());
  auto bias_ptr = AsDeviceMemory(bias.template flat<BiasType>().data(),
                                 bias.template flat<BiasType>().size());

  static int64 ConvolveScratchSize = GetCudnnWorkspaceLimit(
      // default value is in bytes despite the name of the environment variable
      "TF_CUDNN_WORKSPACE_LIMIT_IN_MB", 1LL << 32  // 4GB
  );

  int device_id = stream->parent()->device_ordinal();
  FusedConvParameters fused_conv_parameters = {
      batch_size,
      conv_input_depth,
      {{conv_input_rows, conv_input_cols}},
      output_depth,
      {{filter_rows, filter_cols}},
      {{row_stride, col_stride}},
      {{padding_rows, padding_cols}},
      conv_input->dtype(),
      device_id,
      (side_input_scale != 0),
      activation_mode,
  };

  dnn::AlgorithmConfig algorithm_config;
  if (cudnn_use_autotune && !AutoTuneConvBiasActivation::GetInstance()->Find(
                                fused_conv_parameters, &algorithm_config)) {
    std::vector<dnn::AlgorithmType> algorithms;
    CHECK(stream->parent()->GetConvolveAlgorithms(
        fused_conv_parameters.ShouldIncludeWinogradNonfusedAlgo<T>(),
        &algorithms));
    dnn::ProfileResult best_result;
    dnn::ProfileResult best_result_no_scratch;
    for (auto profile_algorithm : algorithms) {
      // TODO(zhengxq): profile each algorithm multiple times to better
      // accuracy.
      CudnnScratchAllocator scratch_allocator(ConvolveScratchSize, ctx);
      dnn::ProfileResult profile_result;
      bool cudnn_launch_status =
          stream
              ->ThenFusedConvolveWithAlgorithm(
                  conv_input_desc, conv_input_ptr, conv_input_scale,
                  filter_desc, filter_ptr, conv_desc, side_input_ptr,
                  side_input_scale, bias_desc, bias_ptr,
                  dnn::ActivationMode::kRelu, output_desc, &output_ptr,
                  &scratch_allocator, dnn::AlgorithmConfig(profile_algorithm),
                  &profile_result)
              .ok();
      if (cudnn_launch_status) {
        if (profile_result.is_valid()) {
          if (profile_result.elapsed_time_in_ms() <
              best_result.elapsed_time_in_ms()) {
            best_result = profile_result;
          }
          if (scratch_allocator.TotalByteSize() == 0 &&
              profile_result.elapsed_time_in_ms() <
                  best_result_no_scratch.elapsed_time_in_ms()) {
            best_result_no_scratch = profile_result;
          }
        }
      }
    }
    OP_REQUIRES(ctx,
                best_result.is_valid() || best_result_no_scratch.is_valid(),
                errors::NotFound("No algorithm worked!"));
    if (best_result.is_valid()) {
      algorithm_config.set_algorithm(best_result.algorithm());
    }
    if (best_result_no_scratch.is_valid()) {
      algorithm_config.set_algorithm_no_scratch(
          best_result_no_scratch.algorithm());
    }
    AutoTuneConvBiasActivation::GetInstance()->Insert(fused_conv_parameters,
                                                      algorithm_config);
  }

  CudnnScratchAllocator scratch_allocator(ConvolveScratchSize, ctx);
  bool cudnn_launch_status =
      stream
          ->ThenFusedConvolveWithAlgorithm(
              conv_input_desc, conv_input_ptr, conv_input_scale, filter_desc,
              filter_ptr, conv_desc, side_input_ptr, side_input_scale,
              bias_desc, bias_ptr, dnn::ActivationMode::kRelu, output_desc,
              &output_ptr, &scratch_allocator, algorithm_config,
              /*output_profile_result=*/nullptr)
          .ok();

  if (!cudnn_launch_status) {
    ctx->SetStatus(errors::Internal("cuDNN launch failure : conv_input shape(",
                                    conv_input->shape().DebugString(),
                                    ") filter shape(",
                                    filter->shape().DebugString(), ")"));
  }

  // Convert the output tensor back from NCHW to NHWC if necessary.
  if (!std::is_same<T, qint8>::value && (data_format == FORMAT_NHWC) &&
      (output_depth > 1)) {
    functor::NCHWToNHWC<GPUDevice, T, 4>()(
        ctx->eigen_device<GPUDevice>(),
        const_cast<const Tensor*>(output)->tensor<T, 4>(),
        output_param->tensor<T, 4>());
  }
}

// Registration of the GPU implementations.

REGISTER_KERNEL_BUILDER(
    Name("FusedConv2DBiasActivation")
        .Device(DEVICE_GPU)
        .TypeConstraint<float>("T")
        .TypeConstraint<float>("Tbias"),
    FusedConv2DBiasActivationOp<GPUDevice, float, float, float>);

REGISTER_KERNEL_BUILDER(
    Name("FusedConv2DBiasActivation")
        .Device(DEVICE_GPU)
        .TypeConstraint<qint8>("T")
        .TypeConstraint<float>("Tbias"),
    FusedConv2DBiasActivationOp<GPUDevice, qint8, float, float>);

#endif  // GOOGLE_CUDA

}  // namespace tensorflow
