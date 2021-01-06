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

#include "tensorflow/lite/delegates/gpu/cl/cl_device.h"

#include <algorithm>
#include <string>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "tensorflow/lite/delegates/gpu/cl/util.h"
#include "tensorflow/lite/delegates/gpu/common/status.h"

namespace tflite {
namespace gpu {
namespace cl {

template <>
std::string GetDeviceInfo<std::string>(cl_device_id id, cl_device_info info) {
  size_t size;
  cl_int error = clGetDeviceInfo(id, info, 0, nullptr, &size);
  if (error != CL_SUCCESS) {
    return "";
  }

  std::string result(size - 1, 0);
  error = clGetDeviceInfo(id, info, size, &result[0], nullptr);
  if (error != CL_SUCCESS) {
    return "";
  }
  return result;
}

namespace {
template <typename T>
T GetPlatformInfo(cl_platform_id id, cl_platform_info info) {
  T result;
  cl_int error = clGetPlatformInfo(id, info, sizeof(T), &result, nullptr);
  if (error != CL_SUCCESS) {
    return -1;
  }
  return result;
}

std::string GetPlatformInfo(cl_platform_id id, cl_platform_info info) {
  size_t size;
  cl_int error = clGetPlatformInfo(id, info, 0, nullptr, &size);
  if (error != CL_SUCCESS) {
    return "";
  }

  std::string result(size - 1, 0);
  error = clGetPlatformInfo(id, info, size, &result[0], nullptr);
  if (error != CL_SUCCESS) {
    return "";
  }
  return result;
}

void GetDeviceWorkDimsSizes(cl_device_id id, int3* result) {
  int dims_count =
      GetDeviceInfo<cl_uint>(id, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS);
  if (dims_count < 3) {
    return;
  }
  std::vector<size_t> limits(dims_count);
  cl_int error =
      clGetDeviceInfo(id, CL_DEVICE_MAX_WORK_ITEM_SIZES,
                      sizeof(size_t) * dims_count, limits.data(), nullptr);
  if (error != CL_SUCCESS) {
    return;
  }
  // dims_count must be at least 3 according to spec
  result->x = limits[0];
  result->y = limits[1];
  result->z = limits[2];
}

OpenClVersion ParseCLVersion(const std::string& version) {
  const auto first_dot_pos = version.find_first_of('.');
  if (first_dot_pos == std::string::npos) {
    return OpenClVersion::kCl1_0;
  }
  const int major = version[first_dot_pos - 1] - '0';
  const int minor = version[first_dot_pos + 1] - '0';

  if (major == 1) {
    if (minor == 2) {
      return OpenClVersion::kCl1_2;
    } else if (minor == 1) {
      return OpenClVersion::kCl1_1;
    } else {
      return OpenClVersion::kCl1_0;
    }
  } else if (major == 2) {
    if (minor == 2) {
      return OpenClVersion::kCl2_2;
    } else if (minor == 1) {
      return OpenClVersion::kCl2_1;
    } else {
      return OpenClVersion::kCl2_0;
    }
  } else if (major == 3) {
    return OpenClVersion::kCl3_0;
  } else {
    return OpenClVersion::kCl1_0;
  }
}

// check that gpu_version belong to range min_version-max_version
// min_version is included and max_version is excluded.
bool IsGPUVersionInRange(int gpu_version, int min_version, int max_version) {
  return gpu_version >= min_version && gpu_version < max_version;
}
}  // namespace

GpuInfo GpuInfoFromDeviceID(cl_device_id id) {
  GpuInfo info;
  const auto device_name = GetDeviceInfo<std::string>(id, CL_DEVICE_NAME);
  const auto vendor_name = GetDeviceInfo<std::string>(id, CL_DEVICE_VENDOR);
  const auto opencl_c_version =
      GetDeviceInfo<std::string>(id, CL_DEVICE_OPENCL_C_VERSION);
  const std::string gpu_description =
      absl::StrCat(device_name, " ", vendor_name, " ", opencl_c_version);
  GetGpuInfoFromDeviceDescription(gpu_description, GpuApi::kOpenCl, &info);
  info.opencl_info.cl_version = ParseCLVersion(opencl_c_version);
  info.opencl_info.extensions =
      absl::StrSplit(GetDeviceInfo<std::string>(id, CL_DEVICE_EXTENSIONS), ' ');
  info.opencl_info.supports_fp16 = false;
  info.opencl_info.supports_image3d_writes = false;
  for (const auto& ext : info.opencl_info.extensions) {
    if (ext == "cl_khr_fp16") {
      info.opencl_info.supports_fp16 = true;
    }
    if (ext == "cl_khr_3d_image_writes") {
      info.opencl_info.supports_image3d_writes = true;
    }
  }

  info.opencl_info.supports_images =
      GetDeviceInfo<cl_bool>(id, CL_DEVICE_IMAGE_SUPPORT);

  cl_device_fp_config f32_config =
      GetDeviceInfo<cl_device_fp_config>(id, CL_DEVICE_SINGLE_FP_CONFIG);
  info.opencl_info.supports_fp32_rtn = f32_config & CL_FP_ROUND_TO_NEAREST;

  if (info.opencl_info.supports_fp16) {
    cl_device_fp_config f16_config;
    auto status = GetDeviceInfo<cl_device_fp_config>(
        id, CL_DEVICE_HALF_FP_CONFIG, &f16_config);
    // AMD supports cl_khr_fp16 but CL_DEVICE_HALF_FP_CONFIG is empty.
    if (status.ok() && !info.IsAMD()) {
      info.opencl_info.supports_fp16_rtn = f16_config & CL_FP_ROUND_TO_NEAREST;
    } else {  // happens on PowerVR
      f16_config = f32_config;
      info.opencl_info.supports_fp16_rtn = info.opencl_info.supports_fp32_rtn;
    }
  } else {
    info.opencl_info.supports_fp16_rtn = false;
  }

  if (info.IsPowerVR() && !info.opencl_info.supports_fp16) {
    // PowerVR doesn't have full support of fp16 and so doesn't list this
    // extension. But it can support fp16 in MADs and as buffers/textures types,
    // so we will use it.
    info.opencl_info.supports_fp16 = true;
    info.opencl_info.supports_fp16_rtn = info.opencl_info.supports_fp32_rtn;
  }

  if (!info.opencl_info.supports_image3d_writes &&
      ((info.IsAdreno() && info.adreno_info.IsAdreno4xx()) ||
       info.IsNvidia())) {
    // in local tests Adreno 430 can write in image 3d, at least on small sizes,
    // but it doesn't have cl_khr_3d_image_writes in list of available
    // extensions
    // The same for NVidia
    info.opencl_info.supports_image3d_writes = true;
  }
  info.opencl_info.compute_units_count =
      GetDeviceInfo<cl_uint>(id, CL_DEVICE_MAX_COMPUTE_UNITS);
  info.opencl_info.image2d_max_width =
      GetDeviceInfo<size_t>(id, CL_DEVICE_IMAGE2D_MAX_WIDTH);
  info.opencl_info.image2d_max_height =
      GetDeviceInfo<size_t>(id, CL_DEVICE_IMAGE2D_MAX_HEIGHT);
  info.opencl_info.buffer_max_size =
      GetDeviceInfo<cl_ulong>(id, CL_DEVICE_MAX_MEM_ALLOC_SIZE);
  if (info.opencl_info.cl_version >= OpenClVersion::kCl1_2) {
    info.opencl_info.image_buffer_max_size =
        GetDeviceInfo<size_t>(id, CL_DEVICE_IMAGE_MAX_BUFFER_SIZE);
    info.opencl_info.image_array_max_layers =
        GetDeviceInfo<size_t>(id, CL_DEVICE_IMAGE_MAX_ARRAY_SIZE);
  }
  info.opencl_info.image3d_max_width =
      GetDeviceInfo<size_t>(id, CL_DEVICE_IMAGE3D_MAX_WIDTH);
  info.opencl_info.image3d_max_height =
      GetDeviceInfo<size_t>(id, CL_DEVICE_IMAGE2D_MAX_HEIGHT);
  info.opencl_info.image3d_max_depth =
      GetDeviceInfo<size_t>(id, CL_DEVICE_IMAGE3D_MAX_DEPTH);
  int3 max_work_group_sizes;
  GetDeviceWorkDimsSizes(id, &max_work_group_sizes);
  info.opencl_info.max_work_group_size_x = max_work_group_sizes.x;
  info.opencl_info.max_work_group_size_y = max_work_group_sizes.y;
  info.opencl_info.max_work_group_size_z = max_work_group_sizes.z;
  info.opencl_info.max_work_group_total_size =
      GetDeviceInfo<size_t>(id, CL_DEVICE_MAX_WORK_GROUP_SIZE);

  if (info.IsIntel()) {
    if (info.SupportsExtension("cl_intel_required_subgroup_size")) {
      size_t sub_groups_count;
      cl_int status =
          clGetDeviceInfo(id, 0x4108 /*CL_DEVICE_SUB_GROUP_SIZES_INTEL*/, 0,
                          nullptr, &sub_groups_count);
      if (status == CL_SUCCESS) {
        std::vector<size_t> sub_group_sizes(sub_groups_count);
        status = clGetDeviceInfo(id, 0x4108 /*CL_DEVICE_SUB_GROUP_SIZES_INTEL*/,
                                 sizeof(size_t) * sub_groups_count,
                                 sub_group_sizes.data(), nullptr);
        if (status == CL_SUCCESS) {
          for (int i = 0; i < sub_groups_count; ++i) {
            info.supported_subgroup_sizes.push_back(sub_group_sizes[i]);
          }
        }
      }
    }
  }
  return info;
}

CLDevice::CLDevice(cl_device_id id, cl_platform_id platform_id)
    : info_(GpuInfoFromDeviceID(id)), id_(id), platform_id_(platform_id) {}

CLDevice::CLDevice(const CLDevice& device)
    : info_(device.info_), id_(device.id_), platform_id_(device.platform_id_) {}

CLDevice& CLDevice::operator=(const CLDevice& device) {
  if (this != &device) {
    info_ = device.info_;
    id_ = device.id_;
    platform_id_ = device.platform_id_;
  }
  return *this;
}

CLDevice::CLDevice(CLDevice&& device)
    : info_(std::move(device.info_)),
      id_(device.id_),
      platform_id_(device.platform_id_) {
  device.id_ = nullptr;
  device.platform_id_ = nullptr;
}

CLDevice& CLDevice::operator=(CLDevice&& device) {
  if (this != &device) {
    id_ = nullptr;
    platform_id_ = nullptr;
    info_ = std::move(device.info_);
    std::swap(id_, device.id_);
    std::swap(platform_id_, device.platform_id_);
  }
  return *this;
}

std::string CLDevice::GetPlatformVersion() const {
  return GetPlatformInfo(platform_id_, CL_PLATFORM_VERSION);
}

void CLDevice::DisableOneLayerTextureArray() {
  info_.adreno_info.support_one_layer_texture_array = false;
}

absl::Status CreateDefaultGPUDevice(CLDevice* result) {
  cl_uint num_platforms;
  clGetPlatformIDs(0, nullptr, &num_platforms);
  if (num_platforms == 0) {
    return absl::UnknownError("No supported OpenCL platform.");
  }
  std::vector<cl_platform_id> platforms(num_platforms);
  clGetPlatformIDs(num_platforms, platforms.data(), nullptr);

  cl_platform_id platform_id = platforms[0];
  cl_uint num_devices;
  clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices);
  if (num_devices == 0) {
    return absl::UnknownError("No GPU on current platform.");
  }

  std::vector<cl_device_id> devices(num_devices);
  clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_GPU, num_devices, devices.data(),
                 nullptr);

  *result = CLDevice(devices[0], platform_id);
  return absl::OkStatus();
}

}  // namespace cl
}  // namespace gpu
}  // namespace tflite
