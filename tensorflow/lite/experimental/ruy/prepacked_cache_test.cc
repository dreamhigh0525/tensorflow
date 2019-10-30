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

#include "tensorflow/lite/experimental/ruy/prepacked_cache.h"

#include <thread>  // NOLINT(build/c++11)

#include <gtest/gtest.h>
#include "tensorflow/lite/experimental/ruy/ruy.h"
#include "tensorflow/lite/experimental/ruy/time.h"

namespace ruy {
namespace {

TEST(PrepackedCacheTest, TestCacheEjection) {
  ruy::Context* context = new ruy::Context();
  // Create the cache.
  PrepackedCache prepacked_cache(32);
  // Allocate the prepacked matrix.
  PrepackedMatrix mat1;
  mat1.data_size = 16;
  mat1.sums_size = 8;
  prepacked_cache.AllocatePrepackedMatrix(&mat1);
  auto cache_key1 = std::make_pair(reinterpret_cast<void*>(0), mat1.data);
  prepacked_cache.Insert(cache_key1, mat1);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  // Get a time point after the insertion into the cache.
  TimePoint current = CoarseNow();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  PrepackedCache::CacheIterator itr = prepacked_cache.FindAndUpdate(cache_key1);
  EXPECT_NE(itr, prepacked_cache.cend());
  // By finding mat1, we updated its timestamp. Verify that `current` is older
  // than the time stamp now associated with mat1.
  EXPECT_LT(current, itr->second.second);
  PrepackedMatrix mat2;
  mat2.data_size = 8;
  mat2.sums_size = 4;
  prepacked_cache.AllocatePrepackedMatrix(&mat2);

  auto cache_key2 = std::make_pair(reinterpret_cast<void*>(0), mat2.data);
  prepacked_cache.Insert(cache_key2, mat2);
  // The cache size was exceeded by inserting mat2. Ensure that mat1 was
  // ejected.
  EXPECT_EQ(prepacked_cache.FindAndUpdate(cache_key1), prepacked_cache.cend());
  delete context;
}

TEST(PrepackedCacheTest, TestCacheBasic) {
  ruy::Context* context = new ruy::Context();
  // Create the cache.
  PrepackedCache prepacked_cache(48);
  // Allocate the prepacked matrix.
  PrepackedMatrix mat1;
  mat1.data_size = 16;
  mat1.sums_size = 8;
  prepacked_cache.AllocatePrepackedMatrix(&mat1);

  auto cache_key1 = std::make_pair(reinterpret_cast<void*>(0), mat1.data);
  prepacked_cache.Insert(cache_key1, mat1);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_NE(prepacked_cache.FindAndUpdate(cache_key1), prepacked_cache.cend());

  PrepackedMatrix mat2;
  mat2.data_size = 8;
  mat2.sums_size = 4;
  prepacked_cache.AllocatePrepackedMatrix(&mat2);

  auto cache_key2 = std::make_pair(reinterpret_cast<void*>(0), mat2.data);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  prepacked_cache.Insert(cache_key2, mat2);
  // The cache size was not exceeded by inserting mat2. Ensure that mat1 was not
  // ejected.
  EXPECT_NE(prepacked_cache.FindAndUpdate(cache_key1), prepacked_cache.cend());
  delete context;
}

TEST(PrepackedCacheTest, TestCacheEjection2) {
  ruy::Context* context = new ruy::Context();
  // Create the cache.
  PrepackedCache prepacked_cache(73);
  // Allocate the prepacked matrix 1.
  PrepackedMatrix mat1;
  mat1.data_size = 16;
  mat1.sums_size = 8;
  prepacked_cache.AllocatePrepackedMatrix(&mat1);
  auto cache_key1 = std::make_pair(reinterpret_cast<void*>(0), mat1.data);
  prepacked_cache.Insert(cache_key1, mat1);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Allocate the prepacked matrix 2.
  PrepackedMatrix mat2;
  mat2.data_size = 16;
  mat2.sums_size = 8;
  prepacked_cache.AllocatePrepackedMatrix(&mat2);
  auto cache_key2 = std::make_pair(reinterpret_cast<void*>(0), mat2.data);
  prepacked_cache.Insert(cache_key2, mat2);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Allocate the prepacked matrix 3.
  PrepackedMatrix mat31;
  mat31.data_size = 16;
  mat31.sums_size = 8;
  prepacked_cache.AllocatePrepackedMatrix(&mat31);
  auto cache_key3 = std::make_pair(reinterpret_cast<void*>(0), mat31.data);
  prepacked_cache.Insert(cache_key3, mat31);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // The next insertion will cause the cache size to go over the ejection
  // threshold. Touch matrix 1 and matrix 3 to make matrix 2 the oldest
  EXPECT_NE(prepacked_cache.FindAndUpdate(cache_key1), prepacked_cache.cend());
  EXPECT_NE(prepacked_cache.FindAndUpdate(cache_key3), prepacked_cache.cend());
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Allocate the prepacked matrix 4.
  PrepackedMatrix mat4;
  mat4.data_size = 16;
  mat4.sums_size = 8;
  prepacked_cache.AllocatePrepackedMatrix(&mat4);
  auto cache_key4 = std::make_pair(reinterpret_cast<void*>(0), mat4.data);
  prepacked_cache.Insert(cache_key4, mat4);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Ensure that mat2 was ejected, but mat1, mat3, and mat4 were not.
  EXPECT_EQ(prepacked_cache.FindAndUpdate(cache_key2), prepacked_cache.cend());
  EXPECT_NE(prepacked_cache.FindAndUpdate(cache_key3), prepacked_cache.cend());
  EXPECT_NE(prepacked_cache.FindAndUpdate(cache_key1), prepacked_cache.cend());
  EXPECT_NE(prepacked_cache.FindAndUpdate(cache_key4), prepacked_cache.cend());
  delete context;
}

}  // namespace
}  // namespace ruy

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
