//
// Created by skama on 1/24/18.
//

#ifndef TENSORFLOW_CONTRIB_TENSORRT_RESOURCES_TRTINT8CALIBRATOR_H_
#define TENSORFLOW_CONTRIB_TENSORRT_RESOURCES_TRTINT8CALIBRATOR_H_

#include "tensorrt/include/NvInfer.h"
#include <atomic>
#include <string>
#include <unordered_map>
#include <utility>
#include "tensorflow/core/platform/mutex.h"
namespace tensorflow {
namespace trt {

struct TRTInt8Calibrator : public nvinfer1::IInt8EntropyCalibrator {
 public:
  TRTInt8Calibrator(const std::unordered_map<
                        string, std::pair<void*, size_t>>& dev_buffers,
                    int batch_size,
                    string engineName);
  int getBatchSize() const;
  bool getBatch(void* bindings[], const char* names[], int nbBindings) override;
  bool setBatch(const std::unordered_map<string, void*> &data);
  void setDone(){done_=true;}
  const void *readCalibrationCache(std::size_t &length) override;
  void writeCalibrationCache(const void *ptr, std::size_t length) override;
  ~TRTInt8Calibrator();
 private:
  int batch_size_;
  tensorflow::mutex cond_mtx_;
  tensorflow::condition_variable cond_;
  bool done_;
  const std::unordered_map<string, std::pair<void*, size_t>> dev_buffers_;
  std::atomic_bool calib_running_;
  string engine_name_;
};
}  // namespace trt
}  // namespace tensorflow
#endif  // TENSORFLOW_CONTRIB_TENSORRT_RESOURCES_TRTINT8CALIBRATOR_H_
