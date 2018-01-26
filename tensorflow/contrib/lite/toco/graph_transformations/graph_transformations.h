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
#ifndef TENSORFLOW_CONTRIB_LITE_TOCO_GRAPH_TRANSFORMATIONS_GRAPH_TRANSFORMATIONS_H_
#define TENSORFLOW_CONTRIB_LITE_TOCO_GRAPH_TRANSFORMATIONS_GRAPH_TRANSFORMATIONS_H_

#include <cstddef>
#include <initializer_list>
#include <unordered_set>
#include <vector>

#include "tensorflow/contrib/lite/toco/model.h"
#include "tensorflow/contrib/lite/toco/toco_port.h"

namespace toco {

class GraphTransformation {
 public:
  virtual bool Run(Model* model, std::size_t op_index) = 0;
  virtual const char* Name() const = 0;
  virtual ~GraphTransformation() {}
  // Returns the list of messages that this graph transformation
  // generated since ClearMessages() was called.
  const std::vector<string>& Messages() const { return messages_; }
  // Clears the list of messages; should be called after every
  // run of this graph transformation.
  void ClearMessages() { return messages_.clear(); }
  // Adds a message; normally only called by the graph transformation
  // itself during its run (this function could be protected).
  template <typename... Args>
  void AddMessageF(const char* format, const Args&... args) {
    return messages_.push_back(toco::port::StringF(format, args...));
  }

 protected:
  GraphTransformation() {}

  // List of messages generated by this graph transformation.
  std::vector<string> messages_;

 private:
  GraphTransformation(const GraphTransformation& other) = delete;
  GraphTransformation(const GraphTransformation&& other) = delete;
};

class GraphTransformationsSet {
 public:
  // The choice of a container with fully-specified iteration order
  // ensures that graph transformations are always run in the same order,
  // which avoids having toco randomly fail or produce different results
  // depending on the toolchain. Ideally success/results should be independent
  // of the order in which graph transformations are run, but that's
  // unfortunately not currently guaranteed to be the case.
  using TransformationsContainer =
      std::vector<std::unique_ptr<GraphTransformation>>;

  GraphTransformationsSet() {}
  GraphTransformationsSet(
      const std::initializer_list<GraphTransformation*> transformations) {
    for (GraphTransformation* t : transformations) {
      Add(t);
    }
  }
  void Add(GraphTransformation* transformation) {
    const string& name = transformation->Name();
    CHECK(!names_.count(name));
    names_.insert(name);
    transformations_.emplace_back(transformation);
  }
  TransformationsContainer::const_iterator begin() const {
    return transformations_.begin();
  }
  TransformationsContainer::const_iterator end() const {
    return transformations_.end();
  }
  bool empty() const { return transformations_.empty(); }

 private:
  GraphTransformationsSet(const GraphTransformationsSet& other) = delete;
  GraphTransformationsSet(const GraphTransformationsSet&& other) = delete;
  std::vector<std::unique_ptr<GraphTransformation>> transformations_;
  // Names of transformations in the set. Only used to guard against dupes.
  std::unordered_set<string> names_;
};

// Run the given list of graph transformations on the model.
// The message is only for logging purposes.
// The transformations is a rvalue reference, indicating that
// nothing else will use these pointers. The user is supposed to
// construct GraphTransformation objects by using 'new', pass us
// the resulting raw pointers, and this RunGraphTransformations
// takes care of delete'ing these pointers.
void RunGraphTransformations(Model* model, const string& message,
                             const GraphTransformationsSet& transformations);

#define DECLARE_GRAPH_TRANSFORMATION(GTName)               \
  class GTName : public GraphTransformation {              \
   public:                                                 \
    bool Run(Model* model, std::size_t op_index) override; \
    const char* Name() const override { return #GTName; }  \
  };

// List of all graph transformations
DECLARE_GRAPH_TRANSFORMATION(ConvertExpandDimsToReshape)
DECLARE_GRAPH_TRANSFORMATION(ConvertPureConvToDepthwise)
DECLARE_GRAPH_TRANSFORMATION(ConvertTrivialAddNToAdd)
DECLARE_GRAPH_TRANSFORMATION(ConvertTrivialTransposeToReshape)
DECLARE_GRAPH_TRANSFORMATION(ConvertReorderAxes)
DECLARE_GRAPH_TRANSFORMATION(EnsureBiasVectors)
DECLARE_GRAPH_TRANSFORMATION(FuseActivationFunctions)
DECLARE_GRAPH_TRANSFORMATION(FuseBinaryIntoFollowingAffine)
DECLARE_GRAPH_TRANSFORMATION(FuseBinaryIntoPrecedingAffine)
DECLARE_GRAPH_TRANSFORMATION(IdentifyL2Normalization)
DECLARE_GRAPH_TRANSFORMATION(IdentifyL2Pool)
DECLARE_GRAPH_TRANSFORMATION(IdentifyLstmCell)
DECLARE_GRAPH_TRANSFORMATION(IdentifyRelu1)
DECLARE_GRAPH_TRANSFORMATION(MakeInitialDequantizeOperator)
DECLARE_GRAPH_TRANSFORMATION(PropagateArrayDataTypes)
DECLARE_GRAPH_TRANSFORMATION(PropagateFixedSizes)
DECLARE_GRAPH_TRANSFORMATION(HardcodeMinMax)
DECLARE_GRAPH_TRANSFORMATION(Quantize)
DECLARE_GRAPH_TRANSFORMATION(RemoveFinalDequantizeOp)
DECLARE_GRAPH_TRANSFORMATION(RemoveTensorFlowAssert)
DECLARE_GRAPH_TRANSFORMATION(RemoveTensorFlowIdentity)
DECLARE_GRAPH_TRANSFORMATION(RemoveTrivialBinaryOperator)
DECLARE_GRAPH_TRANSFORMATION(RemoveTrivialConcatenation)
DECLARE_GRAPH_TRANSFORMATION(RemoveTrivialConcatenationInput)
DECLARE_GRAPH_TRANSFORMATION(RemoveTrivialQuantizedActivationFunc)
DECLARE_GRAPH_TRANSFORMATION(RemoveUnusedOp)
DECLARE_GRAPH_TRANSFORMATION(ResolveBatchNormalization)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantBinaryOperator)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantUnaryOperator)
DECLARE_GRAPH_TRANSFORMATION(CreateIm2colArrays)
DECLARE_GRAPH_TRANSFORMATION(DropIm2colArrays)
DECLARE_GRAPH_TRANSFORMATION(ReadFakeQuantMinMax)
DECLARE_GRAPH_TRANSFORMATION(ResolveReorderAxes)
DECLARE_GRAPH_TRANSFORMATION(ResolveTensorFlowConcat)
DECLARE_GRAPH_TRANSFORMATION(ResolveTensorFlowMatMul)
DECLARE_GRAPH_TRANSFORMATION(ResolveTensorFlowMerge)
DECLARE_GRAPH_TRANSFORMATION(ResolveSqueezeAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolveTensorFlowSwitch)
DECLARE_GRAPH_TRANSFORMATION(ResolveTensorFlowTile)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantFakeQuant)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantConcatenation)
DECLARE_GRAPH_TRANSFORMATION(DropFakeQuant)
DECLARE_GRAPH_TRANSFORMATION(UnfuseActivationFunctions)
DECLARE_GRAPH_TRANSFORMATION(ResolveSpaceToBatchNDAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolveBatchToSpaceNDAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolvePadAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolveStridedSliceAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolveSliceAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolveMeanAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolveTransposeAttributes)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantRange)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantShapeOrRank)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantStack)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantStridedSlice)
DECLARE_GRAPH_TRANSFORMATION(ResolveConstantFill)
DECLARE_GRAPH_TRANSFORMATION(Dequantize)

class ResolveReshapeAttributes : public GraphTransformation {
 public:
  bool Run(Model* model, std::size_t op_index) override;
  const char* Name() const override { return "ResolveReshapeAttributes"; }
};

class RemoveTrivialReshape : public GraphTransformation {
 public:
  bool Run(Model* model, std::size_t op_index) override;
  const char* Name() const override { return "RemoveTrivialReshape"; }
  bool treat_expand_dims_as_trivial() const {
    return treat_expand_dims_as_trivial_;
  }
  void set_treat_expand_dims_as_trivial(bool val) {
    treat_expand_dims_as_trivial_ = val;
  }

 private:
  bool treat_expand_dims_as_trivial_ = false;
};

#undef DECLARE_GRAPH_TRANSFORMATION

}  // end namespace toco

#endif  // TENSORFLOW_CONTRIB_LITE_TOCO_GRAPH_TRANSFORMATIONS_GRAPH_TRANSFORMATIONS_H_
