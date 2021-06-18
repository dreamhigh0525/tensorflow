/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/validator_runner.h"

#include <fcntl.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif  // !_WIN32

#include <fstream>
#include <memory>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "flatbuffers/flatbuffers.h"  // from @flatbuffers
#include "tensorflow/lite/experimental/acceleration/compatibility/android_info.h"
#include "tensorflow/lite/experimental/acceleration/configuration/configuration_generated.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/embedded_mobilenet_validation_model.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/status_codes.h"
#ifdef __ANDROID__
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/embedded_runner_executable.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/embedded_validator_runner_so_for_tests.h"
#endif  // __ANDROID__

namespace tflite {
namespace acceleration {
namespace {

class ValidatorRunnerTest : public ::testing::Test {
 protected:
  std::string GetTestSrcDir() {
    const char* from_env = getenv("TEST_SRCDIR");
    if (from_env) {
      return from_env;
    }
    return "/data/local/tmp";
  }
  std::string GetTestTmpDir() {
    const char* from_env = getenv("TEST_TMPDIR");
    if (from_env) {
      return from_env;
    }
    return "/data/local/tmp";
  }
  void* LoadEntryPointModule() {
#ifndef _WIN32
    std::string path = GetTestSrcDir() + "/libvalidator_runner_so_for_tests.so";
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    if (!module) {
      path = GetTestSrcDir() +
             "/tensorflow/lite/experimental/acceleration/"
             "mini_benchmark/libvalidator_runner_so_for_tests.so";
      module = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    }
    EXPECT_TRUE(module) << dlerror();
    return module;
#else   // _WIN32
    return nullptr;
#endif  // !_WIN32
  }

  void WriteFile(const std::string rootdir, const std::string& filename,
                 const unsigned char* data, size_t length) {
    std::string dir = rootdir +
                      "/tensorflow/lite/experimental/"
                      "acceleration/mini_benchmark";
    system((std::string("mkdir -p ") + dir).c_str());
    std::string path = dir + "/" + filename;
    (void)unlink(path.c_str());
    std::string contents(reinterpret_cast<const char*>(data), length);
    std::ofstream f(path, std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f << contents;
    f.close();
    ASSERT_EQ(chmod(path.c_str(), 0500), 0);
  }

  void SetUp() override {
#ifdef __ANDROID__
    AndroidInfo android_info;
    auto status = RequestAndroidInfo(&android_info);
    ASSERT_TRUE(status.ok());
    if (android_info.is_emulator) {
      return;
    }
    ASSERT_NO_FATAL_FAILURE(
        WriteFile(GetTestSrcDir(), "librunner_main.so",
                  g_tflite_acceleration_embedded_runner,
                  g_tflite_acceleration_embedded_runner_len));
    ASSERT_NO_FATAL_FAILURE(WriteFile(
        GetTestSrcDir(), "libvalidator_runner_so_for_tests.so",
        g_tflite_acceleration_embedded_validator_runner_so_for_tests,
        g_tflite_acceleration_embedded_validator_runner_so_for_tests_len));
    EXPECT_TRUE(LoadEntryPointModule());
#endif
    ASSERT_NO_FATAL_FAILURE(WriteFile(
        GetTestTmpDir(), "mobilenet_quant_with_validation.tflite",
        g_tflite_acceleration_embedded_mobilenet_validation_model,
        g_tflite_acceleration_embedded_mobilenet_validation_model_len));
  }

  void CheckConfigurations(bool use_path = true) {
    AndroidInfo android_info;
    auto status = RequestAndroidInfo(&android_info);
    ASSERT_TRUE(status.ok());
#ifdef __ANDROID__
    if (android_info.is_emulator) {
      return;
    }
#endif  // __ANDROID__

    std::unique_ptr<ValidatorRunner> validator, validator2;
    std::string model_path =
        GetTestTmpDir() +
        "/tensorflow/lite/experimental/acceleration/"
        "mini_benchmark/mobilenet_quant_with_validation.tflite";

    std::string storage_path = GetTestTmpDir() + "/storage_path.fb";
    (void)unlink(storage_path.c_str());
    if (use_path) {
      validator = std::make_unique<ValidatorRunner>(model_path, storage_path,
                                                    GetTestTmpDir());
      validator2 = std::make_unique<ValidatorRunner>(model_path, storage_path,
                                                     GetTestTmpDir());
    } else {
      int fd = open(model_path.c_str(), O_RDONLY);
      ASSERT_GE(fd, 0);
      struct stat stat_buf = {0};
      ASSERT_EQ(fstat(fd, &stat_buf), 0);
      validator = std::make_unique<ValidatorRunner>(
          fd, 0, stat_buf.st_size, storage_path, GetTestTmpDir());
      validator2 = std::make_unique<ValidatorRunner>(
          fd, 0, stat_buf.st_size, storage_path, GetTestTmpDir());
    }
    ASSERT_EQ(validator->Init(), kMinibenchmarkSuccess);
    ASSERT_EQ(validator2->Init(), kMinibenchmarkSuccess);

    std::vector<const BenchmarkEvent*> events =
        validator->GetAndFlushEventsToLog();
    ASSERT_TRUE(events.empty());

    std::vector<const TFLiteSettings*> settings;
    flatbuffers::FlatBufferBuilder fbb_cpu, fbb_nnapi, fbb_gpu;
    fbb_cpu.Finish(CreateTFLiteSettings(fbb_cpu, Delegate_NONE,
                                        CreateNNAPISettings(fbb_cpu)));
    settings.push_back(
        flatbuffers::GetRoot<TFLiteSettings>(fbb_cpu.GetBufferPointer()));
    if (android_info.android_sdk_version >= "28") {
      fbb_nnapi.Finish(CreateTFLiteSettings(fbb_nnapi, Delegate_NNAPI,
                                            CreateNNAPISettings(fbb_nnapi)));
      settings.push_back(
          flatbuffers::GetRoot<TFLiteSettings>(fbb_nnapi.GetBufferPointer()));
    }
    fbb_gpu.Finish(CreateTFLiteSettings(fbb_gpu, Delegate_GPU));
#ifdef __ANDROID__
    if (!android_info.is_emulator) {
      // GPU doesn't run on emulators.
      settings.push_back(
          flatbuffers::GetRoot<TFLiteSettings>(fbb_gpu.GetBufferPointer()));
    }
#endif  // __ANDROID__

    ASSERT_EQ(validator->TriggerMissingValidation(settings), settings.size());

    int event_count = 0;
    while (event_count < settings.size()) {
      events = validator->GetAndFlushEventsToLog();
      event_count += events.size();
      for (const BenchmarkEvent* event : events) {
        std::string delegate_name = "CPU";
        if (event->tflite_settings()->delegate() == Delegate_GPU) {
          delegate_name = "GPU";
        } else if (event->tflite_settings()->delegate() == Delegate_NNAPI) {
          delegate_name = "NNAPI";
        }
        if (event->event_type() == BenchmarkEventType_END) {
          if (event->result()->ok()) {
            std::cout << "Validation passed on " << delegate_name << std::endl;
          } else {
            std::cout << "Validation did not pass on " << delegate_name
                      << std::endl;
          }
        } else if (event->event_type() == BenchmarkEventType_ERROR) {
          std::cout << "Failed to run validation on " << delegate_name
                    << std::endl;
        }
      }
#ifndef _WIN32
      sleep(1);
#endif  // !_WIN32
    }
#if !defined(__aarch64__)
    // Out-of-process running doesn't work on 64-bit arm emulators.
    EXPECT_EQ(validator2->TriggerMissingValidation(settings), 0);
#endif
  }
};

TEST_F(ValidatorRunnerTest, AllConfigurationsWithFilePath) {
  CheckConfigurations(true);
}

TEST_F(ValidatorRunnerTest, AllConfigurationsWithFd) {
  CheckConfigurations(false);
}

}  // namespace
}  // namespace acceleration
}  // namespace tflite
