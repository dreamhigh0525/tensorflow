/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/framework/dataset.h"

namespace ignite {

class IgniteDataset : public tensorflow::DatasetBase {
 public:
  IgniteDataset(tensorflow::OpKernelContext* ctx, std::string cache_name,
                std::string host, tensorflow::int32 port, bool local,
                tensorflow::int32 part, tensorflow::int32 page_size,
                std::string username, std::string password,
                std::string certfile, std::string keyfile,
                std::string cert_password,
                std::vector<tensorflow::int32> schema,
                std::vector<tensorflow::int32> permutation);
  ~IgniteDataset();
  std::unique_ptr<tensorflow::IteratorBase> MakeIteratorInternal(
      const tensorflow::string& prefix) const override;
  const tensorflow::DataTypeVector& output_dtypes() const override;
  const std::vector<tensorflow::PartialTensorShape>& output_shapes()
      const override;
  tensorflow::string DebugString() const override;

 protected:
  tensorflow::Status AsGraphDefInternal(
      tensorflow::SerializationContext* ctx, DatasetGraphDefBuilder* b,
      tensorflow::Node** output) const override;

 private:
  const std::string cache_name;
  const std::string host;
  const tensorflow::int32 port;
  const bool local;
  const tensorflow::int32 part;
  const tensorflow::int32 page_size;
  const std::string username;
  const std::string password;
  const std::string certfile;
  const std::string keyfile;
  const std::string cert_password;
  const std::vector<tensorflow::int32> schema;
  const std::vector<tensorflow::int32> permutation;

  tensorflow::DataTypeVector dtypes;
  std::vector<tensorflow::PartialTensorShape> shapes;

  void SchemaToTypes();
  void SchemaToShapes();
};

}  // namespace ignite
