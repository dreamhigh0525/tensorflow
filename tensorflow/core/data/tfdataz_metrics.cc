/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/data/tfdataz_metrics.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "absl/time/time.h"
#include "tensorflow/core/platform/env.h"

namespace tensorflow {
namespace data {

ApproximateLatencyEstimator::ApproximateLatencyEstimator(const Env& env)
    : env_(env),
      last_updated_time_mins_(0),
      latency_value_counter_(0),
      latency_count_counter_(0) {}

void ApproximateLatencyEstimator::AddLatency(const int64_t latency_usec) {
  UpdateRingBuffer();
  mutex_lock l(mu_);
  latency_value_counter_ += latency_usec;
  latency_count_counter_ += 1;
}

void ApproximateLatencyEstimator::UpdateRingBuffer() {
  int64_t now_minutes =
      absl::ToInt64Minutes(absl::Microseconds(env_.NowMicros()));
  int64_t elapsed_minutes = now_minutes - last_updated_time_mins_;

  mutex_lock l(mu_);
  int64_t minutes_to_update = std::min(elapsed_minutes, kSlots);
  for (int i = 0; i < minutes_to_update; ++i) {
    latency_value_[next_slot_] = latency_value_counter_;
    latency_count_[next_slot_] = latency_count_counter_;
    IncrementNextSlot();
  }
  last_updated_time_mins_ = now_minutes;
}

void ApproximateLatencyEstimator::IncrementNextSlot() {
  next_slot_ = (next_slot_ + 1) % kSlots;
}

int ApproximateLatencyEstimator::PrevSlot(int steps) {
  return (next_slot_ - steps + kSlots) % kSlots;
}

double ApproximateLatencyEstimator::GetAverageLatency(Duration duration) {
  UpdateRingBuffer();
  mutex_lock l(mu_);
  double interval_latency =
      static_cast<double>(latency_value_counter_ -
                          latency_value_[PrevSlot(static_cast<int>(duration))]);
  double interval_count =
      static_cast<double>(latency_count_counter_ -
                          latency_count_[PrevSlot(static_cast<int>(duration))]);
  return interval_latency / interval_count;
}

TfDatazMetricsCollector::TfDatazMetricsCollector(const std::string& device_type,
                                                 const Env& env)
    : device_type_(device_type), latency_estimator_(env) {}

void TfDatazMetricsCollector::RecordGetNextLatency(
    int64_t get_next_latency_usec) {
  if (get_next_latency_usec > 0) {
    latency_estimator_.AddLatency(get_next_latency_usec);
  }
}

double TfDatazMetricsCollector::GetAverageLatencyForLastOneMinute() {
  return latency_estimator_.GetAverageLatency(
      ApproximateLatencyEstimator::Duration::kMinute);
}

double TfDatazMetricsCollector::GetAverageLatencyForLastFiveMinutes() {
  return latency_estimator_.GetAverageLatency(
      ApproximateLatencyEstimator::Duration::kFiveMinutes);
}

double TfDatazMetricsCollector::GetAverageLatencyForLastSixtyMinutes() {
  return latency_estimator_.GetAverageLatency(
      ApproximateLatencyEstimator::Duration::kSixtyMinutes);
}

}  // namespace data
}  // namespace tensorflow
