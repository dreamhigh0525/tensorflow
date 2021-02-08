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

#include "tensorflow/lite/delegates/gpu/common/tasks/winograd.h"

#include <string>
#include <vector>

#include "absl/strings/str_format.h"
#include "tensorflow/lite/delegates/gpu/common/data_type.h"
#include "tensorflow/lite/delegates/gpu/common/shape.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"
#include "tensorflow/lite/delegates/gpu/common/task/work_group_picking.h"
#include "tensorflow/lite/delegates/gpu/common/winograd_util.h"

namespace tflite {
namespace gpu {

Winograd4x4To36TileX6::Winograd4x4To36TileX6(const OperationDef& definition,
                                             const Padding2D& padding,
                                             const GpuInfo& gpu_info)
    : GPUOperation(definition), padding_(padding) {
  work_group_size_ = int3(32, 1, 1);
  code_ = GetWinograd4x4To36TileX6Code(definition_);
  if (gpu_info.IsAdreno()) {
    compiler_options_.push_back(CompilerOptions::kAdrenoMoreWaves);
  }
  if (definition_.precision == CalculationsPrecision::F16 &&
      gpu_info.IsPowerVR()) {
    compiler_options_.push_back(CompilerOptions::kClPowervrFp16);
  }
}

std::string Winograd4x4To36TileX6::GetWinograd4x4To36TileX6Code(
    const OperationDef& op_def) {
  std::string c;

  const auto src_tensor_type = op_def.src_tensors[0].storage_type;
  const bool is_image_buffer =
      src_tensor_type == TensorStorageType::IMAGE_BUFFER;
  const bool is_buffer = src_tensor_type == TensorStorageType::BUFFER;

  switch (op_def.precision) {
    case CalculationsPrecision::F32:
    case CalculationsPrecision::F32_F16:
      c += "#define ACCUM_FLT float\n";
      break;
    case CalculationsPrecision::F16:
      c += "#define ACCUM_FLT half\n";
      break;
  }

  const DataType accum_type = op_def.precision == CalculationsPrecision::F16
                                  ? DataType::FLOAT16
                                  : DataType::FLOAT32;

  auto bt_mat = BtMatrixForWinograd4x4To6x6();
  c += "constant ACCUM_FLT Bt[36] = {\n";
  for (int y = 0; y < 6; ++y) {
    c += "\t";
    for (int x = 0; x < 6; ++x) {
      c += absl::StrFormat("%.10f", bt_mat[y * 6 + x]) + "f, ";
    }
    c += "\n";
  }
  c += "};\n";

  std::string cl_type = accum_type == DataType::FLOAT16 ? "half" : "float";
  auto src_desc = op_def.src_tensors[0];
  src_desc.SetStateVar("ACCUM_FLT", cl_type);
  AddSrcTensor("src_tensor", src_desc);
  AddDstTensor("dst_tensor", op_def.dst_tensors[0]);
  args_.AddInt("padding_x");
  args_.AddInt("padding_y");
  args_.AddInt("tiles_total");
  args_.AddInt("tiles_x");

  c += "MAIN_FUNCTION($0) {\n";
  c += "  int DST_X = GLOBAL_ID_0;\n";
  c += "  int DST_Y = GLOBAL_ID_1;\n";
  c += "  int DST_Z = GLOBAL_ID_2;\n";
  c += "  if (DST_X >= args.tiles_total || DST_Y >= 6 || DST_Z >= "
       "args.dst_tensor.Slices()) {\n";
  c += "    return; \n";
  c += "  }\n";
  c += "  int tile_x = (DST_X % args.tiles_x) * 4;\n";
  c += "  int tile_y = (DST_X / args.tiles_x) * 4;\n";
  c += "  ACCUM_FLT4 I0, I1, I2, I3, I4, I5;\n";
  c += "  ACCUM_FLT bt_ar[6];\n";
  c += "  ACCUM_FLT4 t0 = TO_ACCUM_TYPE(args.bt.Read(DST_Y * 2 + 0));\n";
  c += "  ACCUM_FLT4 t1 = TO_ACCUM_TYPE(args.bt.Read(DST_Y * 2 + 1));\n";
  c += "  DST_Y *= 6;\n";
  c += "  bt_ar[0] = t0.x;\n";
  c += "  bt_ar[1] = t0.y;\n";
  c += "  bt_ar[2] = t0.z;\n";
  c += "  bt_ar[3] = t0.w;\n";
  c += "  bt_ar[4] = t1.x;\n";
  c += "  bt_ar[5] = t1.y;\n";
  auto read_src = [&](const std::string& src, const std::string& xs) {
    if (is_image_buffer) {
      c += "    ACCUM_FLT4 " + src +
           " = args.src_tensor.Read<ACCUM_FLT>(src_a_" + xs + " + offset);\n";
    } else if (is_buffer) {
      c += "    ACCUM_FLT4 " + src +
           " = args.src_tensor.Read<ACCUM_FLT>(src_a_" + xs + " + offset) * m" +
           xs + "_x;\n";
    } else {
      c += "    ACCUM_FLT4 " + src +
           " = args.src_tensor.Read<ACCUM_FLT>(tile_x + args.padding_x + " +
           xs + ", yc, DST_Z);\n";
    }
  };
  if (is_buffer || is_image_buffer) {
    for (int x = 0; x < 6; ++x) {
      const std::string xs = std::to_string(x);
      c += "  int xc" + xs + " = tile_x + args.padding_x + " + xs + ";\n";
      c += "  ACCUM_FLT m" + xs + "_x = TO_ACCUM_FLT(xc" + xs + " >= 0 && xc" +
           xs + " < args.src_tensor.Width());\n";
      c += "  bool inx" + xs + " = (xc" + xs + " >= 0 && xc" + xs +
           " < args.src_tensor.Width());\n";
      c += "  xc" + xs + " = clamp(xc" + xs +
           ", 0, args.src_tensor.Width() - 1);\n";
      c += "  args.src_tensor.GetAddress(src_a_" + xs + ", xc" + xs +
           ", 0, DST_Z);\n";
      if (is_image_buffer) {
        c += "  src_a_" + xs +
             " = select(-args.src_tensor.Width() * args.src_tensor.Height(), "
             "src_a_" +
             xs + ", inx" + xs + ");\n";
      }
    }
  }
  c += "  {\n";
  c += "    int yc = tile_y + args.padding_y;\n";
  if (is_buffer || is_image_buffer) {
    c += "    bool iny = (yc >= 0 && yc < args.src_tensor.Height());\n";
    c += "    int offset = select(0, yc * args.src_tensor.Width(), iny);\n";
    c += "    ACCUM_FLT bt = bt_ar[0] * TO_ACCUM_FLT(iny);\n";
  } else {
    c += "    ACCUM_FLT bt = bt_ar[0];\n";
  }
  for (int x = 0; x < 6; ++x) {
    const std::string xs = std::to_string(x);
    const std::string src = "src" + xs;
    read_src(src, xs);
    c += "    I" + xs + " = bt * " + src + ";\n";
  }
  c += "  }\n";
  for (int y = 1; y < 6; ++y) {
    const std::string ys = std::to_string(y);
    c += "  {\n";
    c += "    int yc = tile_y + args.padding_y + (" + ys + ");\n";
    if (is_buffer || is_image_buffer) {
      c += "    bool iny = (yc >= 0 && yc < args.src_tensor.Height());\n";
      c += "    int offset = select(0, yc * args.src_tensor.Width(), iny);\n";
      c += "    ACCUM_FLT bt = bt_ar[" + ys + "] * TO_ACCUM_FLT(iny);\n";
    } else {
      c += "    ACCUM_FLT bt = bt_ar[" + ys + "];\n";
    }
    for (int x = 0; x < 6; ++x) {
      const std::string xs = std::to_string(x);
      const std::string src = "src" + xs;
      read_src(src, xs);
      c += "    I" + xs + " += bt * " + src + ";\n";
    }
    c += "  }\n";
  }
  c += "  {\n";
  c += "    FLT4 r0 = TO_FLT4(I0 + Bt[2] * I2 + Bt[4] * I4);\n";
  c += "    args.dst_tensor.Write(r0, DST_X, DST_Y, DST_Z);\n";
  c += "    DST_Y++;\n";
  c += "  }\n";
  c += "  {\n";
  c += "    FLT4 r0 = TO_FLT4(Bt[7] * I1 + Bt[8] * I2 + Bt[9] * I3 + Bt[10] * "
       "I4);\n";
  c += "    args.dst_tensor.Write(r0, DST_X, DST_Y, DST_Z);\n";
  c += "    DST_Y++;\n";
  c += "  }\n";
  c += "  {\n";
  c += "    FLT4 r0 = TO_FLT4(Bt[13] * I1 + Bt[14] * I2 + Bt[15] * I3 + Bt[16] "
       "* "
       "I4);\n";
  c += "    args.dst_tensor.Write(r0, DST_X, DST_Y, DST_Z);\n";
  c += "    DST_Y++;\n";
  c += "  }\n";
  c += "  {\n";
  c += "    FLT4 r0 = TO_FLT4(Bt[19] * I1 + Bt[20] * I2 + Bt[21] * I3 + Bt[22] "
       "* "
       "I4);\n";
  c += "    args.dst_tensor.Write(r0, DST_X, DST_Y, DST_Z);\n";
  c += "    DST_Y++;\n";
  c += "  }\n";
  c += "  {\n";
  c += "    FLT4 r0 = TO_FLT4(Bt[25] * I1 + Bt[26] * I2 + Bt[27] * I3 + Bt[28] "
       "* "
       "I4);\n";
  c += "    args.dst_tensor.Write(r0, DST_X, DST_Y, DST_Z);\n";
  c += "    DST_Y++;\n";
  c += "  }\n";
  c += "  {\n";
  c += "    FLT4 r0 = TO_FLT4(Bt[31] * I1 + Bt[33] * I3 + I5);\n";
  c += "    args.dst_tensor.Write(r0, DST_X, DST_Y, DST_Z);\n";
  c += "    DST_Y++;\n";
  c += "  }\n";
  c += "}\n";
  return c;
}

void Winograd4x4To36TileX6::UploadBt() {
  tflite::gpu::Tensor<Linear, DataType::FLOAT32> bt_aligned;
  bt_aligned.shape = Linear(6 * 8);
  bt_aligned.data.resize(6 * 8);
  auto bt_mat = BtMatrixForWinograd4x4To6x6();
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 6; ++x) {
      bt_aligned.data[y * 8 + x] = bt_mat[y * 6 + x];
    }
    bt_aligned.data[y * 8 + 6] = 0.0f;
    bt_aligned.data[y * 8 + 7] = 0.0f;
  }

  TensorLinearDescriptor desc;
  desc.storage_type = LinearStorageType::TEXTURE_2D;
  desc.element_type = definition_.GetDataType();
  desc.UploadLinearData(bt_aligned);
  args_.AddObject("bt",
                  absl::make_unique<TensorLinearDescriptor>(std::move(desc)));
}

int3 Winograd4x4To36TileX6::SelectBestWorkGroup(
    const KernelInfo& kernel_info) const {
  const std::vector<int3> wgs = {{8, 6, 4}, {8, 6, 2}, {4, 6, 2},
                                 {4, 6, 2}, {2, 6, 2}, {2, 6, 1},
                                 {1, 6, 1}, {1, 3, 1}, {1, 1, 1}};
  return GetFirstSuitableWorkGroup(wgs, kernel_info.max_work_group_size);
}

absl::Status Winograd4x4To36TileX6::BindArguments(ArgumentsBinder* args) {
  const int tiles_x = DivideRoundUp(
      src_[0]->Width() + padding_.prepended.w + padding_.appended.w - 2, 4);
  const int tiles_y = DivideRoundUp(
      src_[0]->Height() + padding_.prepended.h + padding_.appended.h - 2, 4);
  const int tiles_total = tiles_x * tiles_y;
  RETURN_IF_ERROR(args->SetInt("padding_x", -padding_.prepended.w));
  RETURN_IF_ERROR(args->SetInt("padding_y", -padding_.prepended.h));
  RETURN_IF_ERROR(args->SetInt("tiles_total", tiles_total));
  RETURN_IF_ERROR(args->SetInt("tiles_x", tiles_x));
  return absl::OkStatus();
}

int3 Winograd4x4To36TileX6::GetGridSize() const {
  const int grid_x = dst_[0]->Width() * dst_[0]->Batch();
  const int grid_y = 6;
  const int grid_z = dst_[0]->Slices();
  return int3(grid_x, grid_y, grid_z);
}

void Winograd4x4To36TileX6::GetPossibleKernelWorkGroups(
    TuningType tuning_type, const GpuInfo& gpu_info,
    const KernelInfo& kernel_info, std::vector<int3>* work_groups) const {
  if (gpu_info.IsIntel()) {
    work_groups->push_back(int3(4, 6, 1));
    return;
  }
  switch (tuning_type) {
    case TuningType::kExhaustive:
      GetPossibleWorkGroups(tuning_type, gpu_info, kernel_info, grid_size_,
                            work_groups);
      return;
    case TuningType::kFast:
    default:
      work_groups->push_back(SelectBestWorkGroup(kernel_info));
      return;
  }
}

Winograd4x4To36TileX6 CreateWinograd4x4To36TileX6(
    const GpuInfo& gpu_info, const OperationDef& definition,
    const Padding2D& padding) {
  Winograd4x4To36TileX6 result(definition, padding, gpu_info);
  result.UploadBt();
  return result;
}

Winograd36To4x4Tile4x1::Winograd36To4x4Tile4x1(const OperationDef& definition,
                                               const GpuInfo& gpu_info)
    : GPUOperation(definition) {
  work_group_size_ = int3(32, 1, 1);
  if (definition_.precision == CalculationsPrecision::F16 &&
      gpu_info.IsPowerVR()) {
    compiler_options_.push_back(CompilerOptions::kClPowervrFp16);
  }
  code_ = GetWinograd36To4x4Tile4x1Code(definition_);
}

std::string Winograd36To4x4Tile4x1::GetWinograd36To4x4Tile4x1Code(
    const OperationDef& op_def) {
  std::string c;

  switch (op_def.precision) {
    case CalculationsPrecision::F32:
    case CalculationsPrecision::F32_F16:
      c += "#define ACCUM_FLT float\n";
      break;
    case CalculationsPrecision::F16:
      c += "#define ACCUM_FLT half\n";
      break;
  }

  const DataType accum_type = op_def.precision == CalculationsPrecision::F16
                                  ? DataType::FLOAT16
                                  : DataType::FLOAT32;

  std::string cl_type = accum_type == DataType::FLOAT16 ? "half" : "float";
  auto src_desc = op_def.src_tensors[0];
  src_desc.SetStateVar("ACCUM_FLT", cl_type);
  AddSrcTensor("src_tensor", src_desc);
  AddDstTensor("dst_tensor", op_def.dst_tensors[0]);
  args_.AddInt("tiles_x");

  auto at_mat = AtMatrixForWinograd4x4To6x6();
  c += "constant ACCUM_FLT At[24] = {\n";
  for (int y = 0; y < 4; ++y) {
    c += "\t";
    for (int x = 0; x < 6; ++x) {
      c += absl::StrFormat("%.10f", at_mat[y * 6 + x]) + "f, ";
    }
    c += "\n";
  }
  c += "};\n";

  c += "MAIN_FUNCTION($0) {\n";
  c += "  int tile_id = GLOBAL_ID_0;\n";
  c += "  int DST_Y = GLOBAL_ID_1;\n";
  c += "  int DST_Z = GLOBAL_ID_2;\n";
  c += "  int tile_x = (tile_id % args.tiles_x) * 4;\n";
  c += "  int tile_y = (tile_id / args.tiles_x) * 4 + DST_Y;\n";

  c += "  if (tile_x >= args.dst_tensor.Width() || tile_y >= "
       "args.dst_tensor.Height() || DST_Z >= args.dst_tensor.Slices()) {\n";
  c += "    return; \n";
  c += "  }\n";
  c += "  ACCUM_FLT4 I0, I1, I2, I3, I4, I5;\n";
  c += "  ACCUM_FLT at_ar[6];\n";
  c += "  ACCUM_FLT4 t00 = TO_ACCUM_TYPE(args.at.Read(DST_Y * 2 + 0));\n";
  c += "  ACCUM_FLT4 t01 = TO_ACCUM_TYPE(args.at.Read(DST_Y * 2 + 1));\n";
  c += "  at_ar[0] = t00.x;\n";
  c += "  at_ar[1] = t00.y;\n";
  c += "  at_ar[2] = t00.z;\n";
  c += "  at_ar[3] = t00.w;\n";
  c += "  at_ar[4] = t01.x;\n";
  c += "  at_ar[5] = t01.y;\n";
  c += "  {\n";
  c += "    ACCUM_FLT at = at_ar[0];\n";
  for (int x = 0; x < 6; ++x) {
    const std::string yc = std::to_string(x);
    const std::string src = "src" + std::to_string(x);
    c += "    ACCUM_FLT4 " + src +
         " = args.src_tensor.Read<ACCUM_FLT>(tile_id, " + yc + ", DST_Z);\n";
    c += "    I" + std::to_string(x) + " = at * " + src + ";\n";
  }
  c += "  }\n";
  for (int y = 1; y < 6; ++y) {
    c += "  {\n";
    c += "    ACCUM_FLT at = at_ar[" + std::to_string(y) + "];\n";
    for (int x = 0; x < 6; ++x) {
      const std::string yc = std::to_string(y * 6 + x);
      const std::string src = "src" + std::to_string(x);
      c += "    ACCUM_FLT4 " + src +
           " = args.src_tensor.Read<ACCUM_FLT>(tile_id, " + yc + ", DST_Z);\n";
      c += "    I" + std::to_string(x) + " += at * " + src + ";\n";
    }
    c += "  }\n";
  }
  c += "  ACCUM_FLT4 t0 = I1 + I2;\n";
  c += "  ACCUM_FLT4 t1 = I3 + I4;\n";
  c += "  FLT4 bias_val = args.biases.Read(DST_Z);\n";
  c += "  {\n";
  c += "    FLT4 r0 = TO_FLT4(I0 + t0 + t1) + bias_val;\n";
  c += "    args.dst_tensor.Write(r0, tile_x, tile_y, DST_Z);\n";
  c += "    tile_x++;\n";
  c += "  }\n";
  c += "  ACCUM_FLT4 t2 = I1 - I2;\n";
  c += "  ACCUM_FLT4 t3 = I3 - I4;\n";
  c += "  if (tile_x < args.dst_tensor.Width()) {\n";
  c += "    FLT4 r0 = TO_FLT4(t2 * At[7] + t3 * At[9]) + bias_val;\n";
  c += "    args.dst_tensor.Write(r0, tile_x, tile_y, DST_Z);\n";
  c += "    tile_x++;\n";
  c += "  }\n";
  c += "  if (tile_x < args.dst_tensor.Width()) {\n";
  c += "    FLT4 r0 = TO_FLT4(t0 * At[13] + t1 * At[15]) + bias_val;\n";
  c += "    args.dst_tensor.Write(r0, tile_x, tile_y, DST_Z);\n";
  c += "    tile_x++;\n";
  c += "  }\n";
  c += "  if (tile_x < args.dst_tensor.Width()) {\n";
  c += "    FLT4 r0 = TO_FLT4(t2 * At[19] + t3 * At[21] + I5) + bias_val;\n";
  c += "    args.dst_tensor.Write(r0, tile_x, tile_y, DST_Z);\n";
  c += "    tile_x++;\n";
  c += "  }\n";
  c += "}\n";
  return c;
}

void Winograd36To4x4Tile4x1::UploadAt() {
  tflite::gpu::Tensor<Linear, DataType::FLOAT32> at_aligned;
  at_aligned.shape = Linear(4 * 8);
  at_aligned.data.resize(4 * 8);
  auto at_mat = AtMatrixForWinograd4x4To6x6();
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 6; ++x) {
      at_aligned.data[y * 8 + x] = at_mat[y * 6 + x];
    }
    at_aligned.data[y * 8 + 6] = 0.0f;
    at_aligned.data[y * 8 + 7] = 0.0f;
  }

  TensorLinearDescriptor desc;
  desc.storage_type = LinearStorageType::TEXTURE_2D;
  desc.element_type = definition_.GetDataType();
  desc.UploadLinearData(at_aligned);
  args_.AddObject("at",
                  absl::make_unique<TensorLinearDescriptor>(std::move(desc)));
}

int3 Winograd36To4x4Tile4x1::SelectBestWorkGroup(
    const KernelInfo& kernel_info) const {
  const std::vector<int3> wgs = {{32, 4, 2}, {16, 4, 2}, {16, 4, 1},
                                 {8, 4, 1},  {4, 4, 1},  {2, 4, 1},
                                 {1, 4, 1},  {1, 2, 1},  {1, 1, 1}};
  return GetFirstSuitableWorkGroup(wgs, kernel_info.max_work_group_size);
}

absl::Status Winograd36To4x4Tile4x1::BindArguments(ArgumentsBinder* args) {
  const int tiles_x = DivideRoundUp(dst_[0]->Width(), 4);
  RETURN_IF_ERROR(args->SetInt("tiles_x", tiles_x));
  return absl::OkStatus();
}

int3 Winograd36To4x4Tile4x1::GetGridSize() const {
  const int tiles_x = DivideRoundUp(dst_[0]->Width(), 4);
  const int tiles_y = DivideRoundUp(dst_[0]->Height(), 4);
  const int grid_x = tiles_x * tiles_y * dst_[0]->Batch();
  const int grid_y = 4;
  const int grid_z = dst_[0]->Slices();
  return int3(grid_x, grid_y, grid_z);
}

void Winograd36To4x4Tile4x1::GetPossibleKernelWorkGroups(
    TuningType tuning_type, const GpuInfo& gpu_info,
    const KernelInfo& kernel_info, std::vector<int3>* work_groups) const {
  if (gpu_info.IsIntel()) {
    work_groups->push_back(int3(8, 4, 1));
    return;
  }
  switch (tuning_type) {
    case TuningType::kExhaustive:
      GetPossibleWorkGroups(tuning_type, gpu_info, kernel_info, grid_size_,
                            work_groups);
      return;
    case TuningType::kFast:
    default:
      work_groups->push_back(SelectBestWorkGroup(kernel_info));
      return;
  }
}

Winograd36To4x4Tile4x1 CreateWinograd36To4x4Tile4x1(
    const GpuInfo& gpu_info, const OperationDef& definition,
    const tflite::gpu::Tensor<Linear, DataType::FLOAT32>& biases) {
  Winograd36To4x4Tile4x1 result(definition, gpu_info);
  TensorLinearDescriptor desc;
  desc.storage_type = LinearStorageType::TEXTURE_2D;
  desc.element_type = definition.GetDataType();
  desc.UploadLinearData(biases);
  result.args_.AddObject(
      "biases", absl::make_unique<TensorLinearDescriptor>(std::move(desc)));
  result.UploadAt();
  return result;
}

}  // namespace gpu
}  // namespace tflite
