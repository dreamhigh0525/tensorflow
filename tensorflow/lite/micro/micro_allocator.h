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
#ifndef TENSORFLOW_LITE_MICRO_MICRO_ALLOCATOR_H_
#define TENSORFLOW_LITE_MICRO_MICRO_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>

#include "flatbuffers/flatbuffers.h"  // from @flatbuffers
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/core/api/error_reporter.h"
#include "tensorflow/lite/core/api/flatbuffer_conversions.h"
#include "tensorflow/lite/micro/compatibility.h"
#include "tensorflow/lite/micro/simple_memory_allocator.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace tflite {

namespace internal {

// Sets up all of the data structure members for a TfLiteTensor based on the
// contents of a serialized tensor in the flatbuffer.
// TODO(b/162311891): Drop this method when the interpreter has an API for
// returning buffers on TfLiteEvalTensor.
TfLiteStatus InitializeTfLiteTensorFromFlatbuffer(
    SimpleMemoryAllocator* allocator, bool allocate_temp,
    const tflite::Tensor& flatbuffer_tensor,
    const flatbuffers::Vector<flatbuffers::Offset<Buffer>>* buffers,
    ErrorReporter* error_reporter, TfLiteTensor* result);

// Holds placeholder information for a scratch buffer request from a kernel.
// This struct is only used during the model prepare stage. Each request from a
// kernel is stored in the head section. During the prepare stage, the head
// section will at least hold kMaxScratchBuffersPerOp number of requests plus
// any requests from previous kernel requests.
//
// When the memory plan is finalized, these structs are no longer used in favor
// of a sequential, array of ScratchBufferHandle allocations in the tail
// section. These allocations are indexed by the request API defined in the
// TfLiteContext struct.
typedef struct {
  // Number of bytes required by the buffer. The actual allocated size might be
  // greater than `bytes` due to buffer alignment.
  size_t bytes;
  // Node where the buffer is allocated for. This provides useful information to
  // determine the lifetime of the buffer. In AllocationInfo, this buffer will
  // have `before` = node_idx and `after` = node_idx.
  int node_idx;
} ScratchBufferRequest;

}  // namespace internal

typedef struct {
  TfLiteNode node;
  const TfLiteRegistration* registration;
} NodeAndRegistration;

// Holds a pointer to a buffer for a scratch buffer requested by a kernel during
// the model prepare stage. This struct is allocated in-place and allows for
// quick pointer-indexed lookup for speed during model inference.
typedef struct {
  // Pointer to location of the scratch buffer:
  uint8_t* data;
} ScratchBufferHandle;

// Stores all per-subgraph allocations. This includes the node and registration
// array, tensor list and scratch buffer handles for each subgraph.
typedef struct {
  NodeAndRegistration* node_and_registrations;
  TfLiteEvalTensor* tensors;
} SubgraphAllocations;

// Allocator responsible for allocating memory for all intermediate tensors
// necessary to invoke a model.
//
// The lifetime of the model, tensor arena and error reporter must be at
// least as long as that of the allocator object, since the allocator needs
// them to be accessible during its entire lifetime.
//
// The MicroAllocator simply plans out additional allocations that are required
// to standup a model for inference in TF Micro. This class currently relies on
// an additional allocator - SimpleMemoryAllocator - for all allocations from an
// arena. These allocations are divided into head (non-persistent) and tail
// (persistent) regions:
//
// Memory layout to help understand how it works
// This information could change in the future version.
// ************** .memory_allocator->GetBuffer()
// Tensors/Scratch buffers (head)
// ************** .head_watermark
// unused memory
// ************** .memory_allocator->GetBuffer() + ->GetMaxBufferSize()
//                                               - ->GetDataSize()
// persistent area (tail)
// ************** .memory_allocator->GetBuffer() + ->GetMaxBufferSize()
class MicroAllocator {
 public:
  // Creates a MicroAllocator instance from a given tensor arena. This arena
  // will be managed by the created instance.
  // Note: Please use __declspec(align(16)) to make sure tensor_arena is 16
  // bytes aligned, otherwise some head room will be wasted.
  // TODO(b/157615197): Cleanup constructor + factory usage.
  static MicroAllocator* Create(uint8_t* tensor_arena, size_t arena_size,
                                ErrorReporter* error_reporter);

  // Creates a MicroAllocator instance using the provided SimpleMemoryAllocator
  // intance. This allocator instance will use the SimpleMemoryAllocator
  // instance to manage allocations internally.
  static MicroAllocator* Create(SimpleMemoryAllocator* memory_allocator,
                                ErrorReporter* error_reporter);

  // Allocates internal resources required for model inference for each subgraph
  // from the arena.
  //
  // This method will run through the flatbuffer data supplied in the model to
  // properly allocate tensor, node, and op registration data. This method is
  // expected to be followed with a call to FinishModelAllocation()  Returns a
  // pointer to an array of SubgraphAllocations (also stored in the tail of the
  // arena) where each index corresponds to a different subgraph in the model.
  // Return value is nullptr if the allocations failed.
  SubgraphAllocations* StartModelAllocation(const Model* model);

  // Finish allocating internal resources required for model inference.
  //
  // -Plan the memory for activation tensors and scratch buffers.
  // -Update eval tensors for each subgraph based on planned offsets.
  // -Allocate scratch buffer handles array and update based on planned offsets.
  //
  // This method should be called after assigning model resources
  // in StartModelAllocation(). The subgraph_allocations pointer should be the
  // value passed into this class during StartModelAllocation(). Scratch buffer
  // handles are stored in the out-param `scratch_buffer_handles` array which is
  // allocated in this method. This value will be used in `GetScratchBuffer`
  // call to retrieve scratch buffers.
  TfLiteStatus FinishModelAllocation(
      const Model* model, SubgraphAllocations* subgraph_allocations,
      ScratchBufferHandle** scratch_buffer_handles);

  // Allocates a TfLiteTensor struct and populates the returned value with
  // properties from the model flatbuffer. This struct is allocated from
  // persistent arena memory is only guaranteed for the lifetime of the
  // application. The eval_tensors pointer should be the value passed into this
  // class during StartModelAllocation() and contains the source-of-truth for
  // buffers.
  virtual TfLiteTensor* AllocatePersistentTfLiteTensor(
      const Model* model, const SubgraphAllocations* subgraph_allocations,
      int tensor_index, int subgraph_index);

  // Allocates a TfLiteTensor struct and populates the returned value with
  // properties from the model flatbuffer. This struct is allocated from
  // temporary arena memory is only guaranteed until a call is made to
  // ResetTempAllocations(). Subgraph_allocaitons contains the array of
  // TfLiteEvalTensors. If the newly allocated temp at the specified subgraph
  // and tensor index is already present int the TfLiteEvalTensor array, its
  // data buffer will be re-used.
  virtual TfLiteTensor* AllocateTempTfLiteTensor(
      const Model* model, const SubgraphAllocations* subgraph_allocations,
      int tensor_index, int subgraph_index);

  // Resets all temporary allocations. This method should be called after a
  // chain of temp allocations (e.g. chain of TfLiteTensor objects via
  // AllocateTfLiteTensor()).
  virtual void ResetTempAllocations();

  // Allocates persistent buffer which has the same life time as the allocator.
  // The memory is immediately available and is allocated from the tail of the
  // arena.
  virtual void* AllocatePersistentBuffer(size_t bytes);

  // Register a scratch buffer of size `bytes` for Node with `node_id`.
  // This method only requests a buffer with a given size to be used after a
  // model has finished allocation via FinishModelAllocation(). All requested
  // buffers will be accessible by the out-param in that method.
  TfLiteStatus RequestScratchBufferInArena(size_t bytes, int subgraph_idx,
                                           int* buffer_idx);

  // Finish allocating a specific NodeAndRegistration prepare block (kernel
  // entry for a model) with a given node ID. This call ensures that any scratch
  // buffer requests and temporary allocations are handled and ready for the
  // next node prepare block.
  TfLiteStatus FinishPrepareNodeAllocations(int node_id);

  // Returns the arena usage in bytes, only available after
  // `FinishModelAllocation`. Otherwise, it will return 0.
  size_t used_bytes() const;

  // Converts a flatbuffer int32_t array to a TfLiteIntArray, accounting for
  // endiannes.
  TfLiteStatus FlatBufferVectorToTfLiteTypeArray(
      const flatbuffers::Vector<int32_t>* flatbuffer_array,
      TfLiteIntArray** result);

  BuiltinDataAllocator* GetBuiltinDataAllocator();

 protected:
  MicroAllocator(SimpleMemoryAllocator* memory_allocator,
                 ErrorReporter* error_reporter);
  virtual ~MicroAllocator();

  // Allocates an array in the arena to hold pointers to the node and
  // registration pointers required to represent the inference graph of the
  // model.
  virtual TfLiteStatus AllocateNodeAndRegistrations(
      const Model* model, SubgraphAllocations* subgraph_allocations);

  // Allocates the list of persistent TfLiteEvalTensors that are used for the
  // "eval" phase of model inference. These structs will be the source of truth
  // for all tensor buffers.
  virtual TfLiteStatus AllocateTfLiteEvalTensors(
      const Model* model, SubgraphAllocations* subgraph_allocations);
  // Allocates persistent tensor buffers for variable tensors in the subgraph.
  virtual TfLiteStatus AllocateVariables(const SubGraph* subgraph,
                                         TfLiteEvalTensor* eval_tensors);

  // Allocate and return a persistent TfLiteTensor.
  // TODO(b/162311891): Drop this method when the interpreter has an API for
  // accessing TfLiteEvalTensor structs.
  virtual TfLiteTensor* AllocatePersistentTfLiteTensorInternal();

  // Populates a TfLiteTensor struct with data from the model flatbuffer. Any
  // quantization data is allocated from either the tail (persistent) or temp
  // sections of the arena based on the allocation flag.
  virtual TfLiteStatus PopulateTfLiteTensorFromFlatbuffer(const Model* model,
                                                          TfLiteTensor* tensor,
                                                          int tensor_index,
                                                          int subgraph_idx,
                                                          bool allocate_temp);

  ErrorReporter* error_reporter() const;

 private:
  // Commits a memory plan for all non-persistent buffer allocations in the
  // 'head' section of the memory arena. The eval_tensors pointer is the list of
  // pre-allocated TfLiteEvalTensor structs that will point to the buffers that
  // will be allocated into the head section in this function call. The
  // scratch_buffer_handles pointer is the array of pre-allocated
  // ScratchBufferHandle structs that will point to allocated buffers also in
  // the head section.
  virtual TfLiteStatus CommitStaticMemoryPlan(
      const Model* model, TfLiteEvalTensor* eval_tensors,
      ScratchBufferHandle* scratch_buffer_handles, int subgraph_idx);

  // Allocates an array of ScratchBufferHandle structs in the tail section for a
  // given number of handles.
  virtual TfLiteStatus AllocateScratchBufferHandles(
      ScratchBufferHandle** scratch_buffer_handles, size_t handle_count);

  // Clears all internal scratch buffer request counts and resets the head to
  // prepare for kernels to request scratch buffer data when a model is
  // preparing.
  TfLiteStatus InitScratchBufferData();

  // Returns the pointer for the array of ScratchBufferRequest allocations in
  // the head section.
  internal::ScratchBufferRequest* GetScratchBufferRequests();

  // A simple memory allocator that always allocate from the arena tail or head.
  SimpleMemoryAllocator* memory_allocator_;

  // Allocator used to allocate persistent builtin data.
  BuiltinDataAllocator* builtin_data_allocator_;

  ErrorReporter* error_reporter_;
  bool model_is_allocating_;

  // Holds the number of ScratchBufferRequest instances stored in the head
  // section when a model is allocating.
  size_t scratch_buffer_request_count_ = 0;

  // Holds the byte length of the memory plan with the largest head usage. Used
  // to ensure that multi-tenant allocations can share the head for buffers.
  size_t max_head_buffer_usage_ = 0;

  TF_LITE_REMOVE_VIRTUAL_DELETE
};

}  // namespace tflite
#endif  // TENSORFLOW_LITE_MICRO_MICRO_ALLOCATOR_H_
