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

#include "tensorflow/compiler/xla/service/hlo_instructions.h"

#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"

namespace xla {

using ::tensorflow::str_util::Join;
using ::tensorflow::strings::StrCat;

HloBatchNormInstruction::HloBatchNormInstruction(
    HloOpcode opcode, const Shape& shape, HloInstruction* operand,
    HloInstruction* scale, float epsilon, int64 feature_index)
    : HloInstruction(opcode, shape),
      epsilon_(epsilon),
      feature_index_(feature_index) {
  AppendOperand(operand);
  AppendOperand(scale);
}

bool HloBatchNormInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  const auto& casted_other = static_cast<const HloBatchNormInstruction&>(other);
  return feature_index() == casted_other.feature_index() &&
         epsilon() == casted_other.epsilon();
}

HloInstructionProto HloBatchNormInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  proto.set_epsilon(epsilon_);
  proto.set_feature_index(feature_index_);
  return proto;
}

std::vector<string> HloBatchNormInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("epsilon=", epsilon()),
          StrCat("feature_index=", feature_index())};
}

HloBatchNormTrainingInstruction::HloBatchNormTrainingInstruction(
    const Shape& shape, HloInstruction* operand, HloInstruction* scale,
    HloInstruction* offset, float epsilon, int64 feature_index)
    : HloBatchNormInstruction(HloOpcode::kBatchNormTraining, shape, operand,
                              scale, epsilon, feature_index) {
  AppendOperand(offset);
}

std::unique_ptr<HloInstruction>
HloBatchNormTrainingInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 3);
  return MakeUnique<HloBatchNormTrainingInstruction>(
      shape, new_operands[0], new_operands[1], new_operands[2], epsilon(),
      feature_index());
}

HloBatchNormInferenceInstruction::HloBatchNormInferenceInstruction(
    const Shape& shape, HloInstruction* operand, HloInstruction* scale,
    HloInstruction* offset, HloInstruction* mean, HloInstruction* variance,
    float epsilon, int64 feature_index)
    : HloBatchNormInstruction(HloOpcode::kBatchNormInference, shape, operand,
                              scale, epsilon, feature_index) {
  AppendOperand(offset);
  AppendOperand(mean);
  AppendOperand(variance);
}

std::unique_ptr<HloInstruction>
HloBatchNormInferenceInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 5);
  return MakeUnique<HloBatchNormInferenceInstruction>(
      shape, new_operands[0], new_operands[1], new_operands[2], new_operands[3],
      new_operands[4], epsilon(), feature_index());
}

HloBatchNormGradInstruction::HloBatchNormGradInstruction(
    const Shape& shape, HloInstruction* operand, HloInstruction* scale,
    HloInstruction* mean, HloInstruction* variance, HloInstruction* grad_output,
    float epsilon, int64 feature_index)
    : HloBatchNormInstruction(HloOpcode::kBatchNormGrad, shape, operand, scale,
                              epsilon, feature_index) {
  AppendOperand(mean);
  AppendOperand(variance);
  AppendOperand(grad_output);
}

std::unique_ptr<HloInstruction>
HloBatchNormGradInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 5);
  return MakeUnique<HloBatchNormGradInstruction>(
      shape, new_operands[0], new_operands[1], new_operands[2], new_operands[3],
      new_operands[4], epsilon(), feature_index());
}

HloFftInstruction::HloFftInstruction(
    const Shape& shape, HloInstruction* operand, FftType fft_type,
    tensorflow::gtl::ArraySlice<int64> fft_length)
    : HloInstruction(HloOpcode::kFft, shape), fft_type_(fft_type) {
  fft_length_.assign(fft_length.begin(), fft_length.end());
  AppendOperand(operand);
}

HloInstructionProto HloFftInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  proto.set_fft_type(fft_type_);
  for (int64 fft_len : fft_length_) {
    proto.add_fft_length(fft_len);
  }
  return proto;
}

std::vector<string> HloFftInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("fft_type=", FftType_Name(fft_type())),
          StrCat("fft_length={", Join(fft_length(), ","), "}")};
}

bool HloFftInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  const auto& casted_other = static_cast<const HloFftInstruction&>(other);
  return fft_type() == casted_other.fft_type() &&
         fft_length() == casted_other.fft_length();
}

std::unique_ptr<HloInstruction> HloFftInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 1);
  return MakeUnique<HloFftInstruction>(shape, new_operands[0], fft_type_,
                                       fft_length_);
}

HloSendRecvInstruction::HloSendRecvInstruction(HloOpcode opcode,
                                               const Shape& shape,
                                               int64 channel_id)
    : HloInstruction(opcode, shape), channel_id_(channel_id) {}

HloInstructionProto HloSendRecvInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  proto.set_channel_id(channel_id_);
  return proto;
}

std::vector<string> HloSendRecvInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("channel_id=", channel_id_)};
}

bool HloSendRecvInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  // Not yet supported.
  return false;
}

// Send instruction produces a tuple of {aliased operand, U32 context}.
HloSendInstruction::HloSendInstruction(HloInstruction* operand,
                                       int64 channel_id)
    : HloSendRecvInstruction(
          HloOpcode::kSend,
          ShapeUtil::MakeTupleShape(
              {CHECK_NOTNULL(operand)->shape(), ShapeUtil::MakeShape(U32, {})}),
          channel_id) {
  AppendOperand(operand);
}

std::unique_ptr<HloInstruction> HloSendInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 1);
  return MakeUnique<HloSendInstruction>(new_operands[0], channel_id());
}

HloSendDoneInstruction::HloSendDoneInstruction(HloSendInstruction* operand)
    : HloSendRecvInstruction(HloOpcode::kSendDone, ShapeUtil::MakeNil(),
                             CHECK_NOTNULL(operand)->channel_id()) {
  AppendOperand(operand);
}

std::unique_ptr<HloInstruction>
HloSendDoneInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 1);
  return MakeUnique<HloSendDoneInstruction>(
      Cast<HloSendInstruction>(new_operands[0]));
}

// Recv instruction produces a tuple of {receive buffer, U32 context}.
HloRecvInstruction::HloRecvInstruction(const Shape& shape, int64 channel_id)
    : HloSendRecvInstruction(
          HloOpcode::kRecv,
          ShapeUtil::MakeTupleShape({shape, ShapeUtil::MakeShape(U32, {})}),
          channel_id) {}

std::unique_ptr<HloInstruction> HloRecvInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 0);
  return MakeUnique<HloRecvInstruction>(
      ShapeUtil::GetTupleElementShape(shape, 0), channel_id());
}

HloRecvDoneInstruction::HloRecvDoneInstruction(HloRecvInstruction* operand)
    : HloSendRecvInstruction(
          HloOpcode::kRecvDone,
          ShapeUtil::GetTupleElementShape(operand->shape(), 0),
          CHECK_NOTNULL(operand)->channel_id()) {
  AppendOperand(operand);
}

std::unique_ptr<HloInstruction>
HloRecvDoneInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 1);
  return MakeUnique<HloRecvDoneInstruction>(
      Cast<HloRecvInstruction>(new_operands[0]));
}

HloReverseInstruction::HloReverseInstruction(
    const Shape& shape, HloInstruction* operand,
    tensorflow::gtl::ArraySlice<int64> dimensions)
    : HloInstruction(HloOpcode::kReverse, shape),
      dimensions_(dimensions.begin(), dimensions.end()) {
  AppendOperand(operand);
}

HloInstructionProto HloReverseInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  for (int64 dimension : dimensions_) {
    proto.add_dimensions(dimension);
  }
  return proto;
}

std::vector<string> HloReverseInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("dimensions={", Join(dimensions(), ","), "}")};
}

bool HloReverseInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  const auto& casted_other = static_cast<const HloReverseInstruction&>(other);
  return dimensions() == casted_other.dimensions();
}

std::unique_ptr<HloInstruction> HloReverseInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 1);
  return MakeUnique<HloReverseInstruction>(shape, new_operands[0],
                                           dimensions());
}

HloConcatenateInstruction::HloConcatenateInstruction(
    const Shape& shape, tensorflow::gtl::ArraySlice<HloInstruction*> operands,
    int64 dimension)
    : HloInstruction(HloOpcode::kConcatenate, shape), dimensions_({dimension}) {
  for (auto operand : operands) {
    AppendOperand(operand);
  }
}

HloInstructionProto HloConcatenateInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  for (int64 dimension : dimensions_) {
    proto.add_dimensions(dimension);
  }
  return proto;
}

std::vector<string> HloConcatenateInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("dimensions={", Join(dimensions(), ","), "}")};
}

bool HloConcatenateInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  const auto& casted_other =
      static_cast<const HloConcatenateInstruction&>(other);
  return dimensions() == casted_other.dimensions();
}

std::unique_ptr<HloInstruction>
HloConcatenateInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  return MakeUnique<HloConcatenateInstruction>(shape, new_operands,
                                               dimensions(0));
}

HloReduceInstruction::HloReduceInstruction(
    const Shape& shape, HloInstruction* arg, HloInstruction* init_value,
    tensorflow::gtl::ArraySlice<int64> dimensions_to_reduce,
    HloComputation* reduce_computation)
    : HloInstruction(HloOpcode::kReduce, shape),
      dimensions_(dimensions_to_reduce.begin(), dimensions_to_reduce.end()) {
  AppendOperand(arg);
  AppendOperand(init_value);
  AppendComputation(reduce_computation);
}

HloInstructionProto HloReduceInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  for (int64 dimension : dimensions_) {
    proto.add_dimensions(dimension);
  }
  return proto;
}

std::vector<string> HloReduceInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("dimensions={", Join(dimensions(), ","), "}")};
}

bool HloReduceInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  const auto& casted_other = static_cast<const HloReduceInstruction&>(other);
  // Reduction results are determined by the reduction dimension and the
  // reduction computation.
  return dimensions() == casted_other.dimensions() &&
         eq_computations(to_apply(), casted_other.to_apply());
}

std::unique_ptr<HloInstruction> HloReduceInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 2);
  return MakeUnique<HloReduceInstruction>(
      shape, new_operands[0], new_operands[1], dimensions(), to_apply());
}

HloTransposeInstruction::HloTransposeInstruction(
    const Shape& shape, HloInstruction* operand,
    tensorflow::gtl::ArraySlice<int64> dimensions)
    : HloInstruction(HloOpcode::kTranspose, shape),
      dimensions_(dimensions.begin(), dimensions.end()) {
  CHECK_EQ(shape.dimensions().size(), dimensions.size());
  CHECK_EQ(shape.dimensions().size(), operand->shape().dimensions().size());
  CHECK(std::equal(operand->shape().dimensions().begin(),
                   operand->shape().dimensions().end(),
                   Permute(dimensions, shape.dimensions()).begin()))
      << "shape: " << ShapeUtil::HumanString(shape)
      << ", operand->shape(): " << ShapeUtil::HumanString(shape)
      << ", dimensions: {" << Join(dimensions, ", ") << "}";
  AppendOperand(operand);
}

bool HloTransposeInstruction::IsRank2Transpose() const {
  return dimensions() == std::vector<int64>({1, 0}) &&
         shape().dimensions_size() == 2 &&
         std::equal(shape().dimensions().begin(), shape().dimensions().end(),
                    operand(0)->shape().dimensions().rbegin());
}

HloInstructionProto HloTransposeInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  for (int64 dimension : dimensions_) {
    proto.add_dimensions(dimension);
  }
  return proto;
}

std::vector<string> HloTransposeInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("dimensions={", Join(dimensions(), ","), "}")};
}

bool HloTransposeInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  const auto& casted_other = static_cast<const HloTransposeInstruction&>(other);
  return dimensions() == casted_other.dimensions();
}

std::unique_ptr<HloInstruction>
HloTransposeInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 1);
  return MakeUnique<HloTransposeInstruction>(shape, new_operands[0],
                                             dimensions());
}

HloBroadcastInstruction::HloBroadcastInstruction(
    const Shape& shape, HloInstruction* operand,
    tensorflow::gtl::ArraySlice<int64> broadcast_dimension)
    : HloInstruction(HloOpcode::kBroadcast, shape),
      dimensions_(broadcast_dimension.begin(), broadcast_dimension.end()) {
  AppendOperand(operand);
}

HloInstructionProto HloBroadcastInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  for (int64 dimension : dimensions_) {
    proto.add_dimensions(dimension);
  }
  return proto;
}

std::vector<string> HloBroadcastInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("dimensions={", Join(dimensions(), ","), "}")};
}

bool HloBroadcastInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  const auto& casted_other = static_cast<const HloBroadcastInstruction&>(other);
  return dimensions() == casted_other.dimensions();
}

std::unique_ptr<HloInstruction>
HloBroadcastInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  CHECK_EQ(new_operands.size(), 1);
  return MakeUnique<HloBroadcastInstruction>(shape, new_operands[0],
                                             dimensions());
}

HloMapInstruction::HloMapInstruction(
    const Shape& shape, tensorflow::gtl::ArraySlice<HloInstruction*> operands,
    HloComputation* map_computation,
    tensorflow::gtl::ArraySlice<HloInstruction*> static_operands)
    : HloInstruction(HloOpcode::kMap, shape) {
  CHECK(static_operands.empty()) << "static_operands not yet supported";
  for (auto operand : operands) {
    AppendOperand(operand);
  }
  AppendComputation(map_computation);
  // TODO(b/65689298) Remove code below once Map is generalized to accept
  // arbitrary map dimensions.
  dimensions_.resize(ShapeUtil::Rank(shape));
  std::iota(dimensions_.begin(), dimensions_.end(), 0);
}

HloInstructionProto HloMapInstruction::ToProto() const {
  HloInstructionProto proto = HloInstruction::ToProto();
  for (int64 dimension : dimensions_) {
    proto.add_dimensions(dimension);
  }
  return proto;
}

bool HloMapInstruction::IsElementwise() const {
  if (!dimensions().empty()) {
    // Check that the map is executed in elementwise compatible dimensions.
    if (dimensions().size() != shape().dimensions_size()) {
      return false;
    }
    for (int i = 0; i < dimensions().size(); ++i) {
      if (dimensions()[i] != i) {
        return false;
      }
    }
  }
  return true;
}

std::vector<string> HloMapInstruction::ExtraAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {StrCat("dimensions={", Join(dimensions(), ","), "}")};
}

bool HloMapInstruction::IdenticalSlowPath(
    const HloInstruction& other,
    const std::function<bool(const HloComputation*, const HloComputation*)>&
        eq_computations) const {
  return eq_computations(to_apply(), other.to_apply());
}

std::unique_ptr<HloInstruction> HloMapInstruction::CloneWithNewOperandsImpl(
    const Shape& shape,
    tensorflow::gtl::ArraySlice<HloInstruction*> new_operands,
    HloCloneContext* context) const {
  return MakeUnique<HloMapInstruction>(shape, new_operands, to_apply());
}
}  // namespace xla
