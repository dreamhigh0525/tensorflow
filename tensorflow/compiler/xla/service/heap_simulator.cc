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

#include "tensorflow/compiler/xla/service/heap_simulator.h"

#include <algorithm>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "tensorflow/compiler/xla/map_util.h"
#include "tensorflow/compiler/xla/util.h"

namespace xla {

using absl::flat_hash_map;
using absl::flat_hash_set;

namespace {
// FlattenSchedule walks through the instruction, and recurse into each called
// computations. As it walks it also tracks down the ordinal number of each
// instruction in the schedule and store it in the `instruction_schedule`. The
// end of each computation is tracked in `computation_schedule`.
int64 FlattenSchedule(
    const HloComputation& computation,
    const HloInstructionSequence& instruction_sequence,
    const HloSchedule* schedule, int64 start_time,
    absl::flat_hash_map<const HloInstruction*, int64>* instruction_schedule,
    absl::flat_hash_map<const HloComputation*, int64>* computation_schedule) {
  int64 time = start_time;
  for (const HloInstruction* instruction :
       instruction_sequence.instructions()) {
    if (schedule != nullptr) {
      // Recurse into sub computations if we have a module-scoped schedule.
      if (instruction->opcode() == HloOpcode::kCall ||
          instruction->opcode() == HloOpcode::kConditional) {
        for (const HloComputation* called_computation :
             instruction->called_computations()) {
          const HloInstructionSequence& called_sequence =
              schedule->sequence(called_computation);
          time =
              FlattenSchedule(*called_computation, called_sequence, schedule,
                              time, instruction_schedule, computation_schedule);
          computation_schedule->insert({called_computation, time});
        }
      }
      if (instruction->opcode() == HloOpcode::kWhile) {
        const HloInstructionSequence& condition_sequence =
            schedule->sequence(instruction->while_condition());
        time = FlattenSchedule(*instruction->while_condition(),
                               condition_sequence, schedule, time,
                               instruction_schedule, computation_schedule);
        computation_schedule->insert({instruction->while_condition(), time});
        const HloInstructionSequence& body_sequence =
            schedule->sequence(instruction->while_body());
        time =
            FlattenSchedule(*instruction->while_body(), body_sequence, schedule,
                            time, instruction_schedule, computation_schedule);
      }
    }
    if (instruction_schedule->count(instruction) != 0) {
      continue;
    }
    instruction_schedule->insert({instruction, time++});
  }
  computation_schedule->insert({&computation, time});
  return time;
}

// The aliased buffers could have overlapping live ranges.
// NormalizeAliasedBuffers normalizes the buffer such that each alias buffer has
// disjoint live range while keeping the live range union the same. This avoid
// double counting aliased buffer sizes.
//
// Before(buffer1 and 2 are aliased):
//
//           +----+          live range of buffer1
//   +------------------+    live range of buffer2
//
// After:
//
//           +----------+    live range of buffer1
//   +------+                live range of buffer2
//
// Before(buffer1 and 2 are aliased):
//
//           +----------+    live range of buffer1
//   +------------+          live range of buffer2
//
// After:
//
//           +----------+    live range of buffer1
//   +------+                live range of buffer2
//
// Before(buffer1 and 2 are aliased):
//
//           +----------+    live range of buffer1
//   +---+                   live range of buffer2
//
// After(unchanged):
//
//           +----------+    live range of buffer1
//   +---+                   live range of buffer2
//
// As another example, imagine we have the following code sequence with live
// ranges of each while-aliased buffers:
//
//                     a      p1    p2    e     b
// a = ...             +
//                     |
// {                   |
//   p1 = param        |       +
//   ROOT true         |       |
// }                   |       +
// { // body           |
//   p2 = param        +             +
//   c = p2 + 1                      +
//   d = c + 1
//   ROOT e = d + 1                       +
// }                                      |
//                                        |
// b = while (a)                          +     +
//                                              |
// f = b + 1                                    +
//
// After normalization it becomes:
//
//                     a      p1    p2    e     b
// a = ...             +
//                     |
// {                   +
//   p1 = param                +
//   ROOT true                 |
// }                           +
// { // body
//   p2 = param                      +
//   c = p2 + 1                      +
//   d = c + 1
//   ROOT e = d + 1                       +
// }                                      |
//                                        |
// b = while (a)                          +
//                                              +
// f = b + 1                                    +
//
// Note there is no overlap of live ranges after normalization.
void NormalizeAliasedBuffers(
    absl::flat_hash_map<const HloValue*, int64>* buffer_start_map,
    absl::flat_hash_map<const HloValue*, int64>* buffer_end_map,
    const std::vector<const HloValue*>& values_to_assign,
    const HloAliasAnalysis& alias_analysis) {
  absl::flat_hash_set<const HloValue*> values_to_assign_set(
      values_to_assign.begin(), values_to_assign.end());
  for (const HloBuffer& hlo_buffer : alias_analysis.buffers()) {
    std::vector<const HloValue*> aliased_buffers;
    for (const HloValue* hlo_value : hlo_buffer.values()) {
      if (values_to_assign_set.count(hlo_value) != 0) {
        aliased_buffers.push_back(hlo_value);
        CHECK_NE(buffer_start_map->count(hlo_value), 0);
        CHECK_NE(buffer_end_map->count(hlo_value), 0);
      }
    }
    absl::c_sort(
        aliased_buffers, [&](const HloValue* value1, const HloValue* value2) {
          if ((*buffer_start_map)[value1] != (*buffer_start_map)[value2]) {
            return (*buffer_start_map)[value1] < (*buffer_start_map)[value2];
          }
          return (*buffer_end_map)[value1] < (*buffer_end_map)[value2];
        });

    for (int64 i = 0; i < aliased_buffers.size(); ++i) {
      // We can't use aliased_buffers.size() - 1 since aliased_buffers.size() is
      // an unsigned integer and can be 0.
      if (i + 1 == aliased_buffers.size()) {
        break;
      }

      const HloValue* value1 = aliased_buffers[i];
      const HloValue* value2 = aliased_buffers[i + 1];
      if ((*buffer_start_map)[value1] == (*buffer_start_map)[value2]) {
        // If value1 has the same start time as value2, make value1 disappear by
        // setting the end time same as start time:
        //
        // Before:
        // +----+           value1
        // +----------+     value2
        //
        // After:
        // +                value1
        // +----------+     value2
        //
        // Note that only when heap simulator runs before copy insertion can
        // this happen where one instruction defines multiple aliased buffers --
        // This is illegle to execute and can be fixed by copy insertion later.
        (*buffer_end_map)[value1] = (*buffer_start_map)[value1];
        continue;
      }

      if ((*buffer_end_map)[value1] < (*buffer_start_map)[value2]) {
        continue;
      }

      if ((*buffer_end_map)[value1] > (*buffer_end_map)[value2]) {
        (*buffer_end_map)[value2] = (*buffer_end_map)[value1];
      }
      (*buffer_end_map)[value1] = (*buffer_start_map)[value2] - 1;
    }
  }
}
}  // namespace

/*static*/
StatusOr<int64> HeapSimulator::MinimumMemoryForModule(
    const HloSchedule& schedule,
    const LogicalBuffer::SizeFunction& size_function) {
  if (schedule.empty()) {
    return 0;
  }
  const HloModule* module = schedule.module();

  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloAliasAnalysis> alias_analysis,
                      HloAliasAnalysis::Run(module));

  // The absolute minimum memory required for a given sequence of instructions
  // is determined by the sequence of Alloc and Free calls on a simulated heap,
  // ignoring fragmentation. We run the heap simulation on the whole module,
  // rather than summing each computation, since it gives us a better lower
  // bound, by minimizing the liveness of sub-computations.
  TF_ASSIGN_OR_RETURN(
      HeapSimulator::Result result,
      HeapSimulator::Run(absl::make_unique<NoFragmentationStatsHeap>(), *module,
                         schedule, *alias_analysis, size_function));
  return result.heap_size;
}

/*static*/
StatusOr<int64> HeapSimulator::MinimumMemoryForComputation(
    const HloComputation& computation, const HloInstructionSequence& sequence,
    const HloAliasAnalysis& alias_analysis,
    const LogicalBuffer::SizeFunction& size_function,
    const absl::flat_hash_map<const HloComputation*, int64>*
        memory_by_computation) {
  TF_ASSIGN_OR_RETURN(
      HeapSimulator::Result result,
      HeapSimulator::Run(absl::make_unique<NoFragmentationStatsHeap>(),
                         computation, sequence, alias_analysis, size_function,
                         HeapSimulator::Options(), memory_by_computation));
  return result.heap_size;
}

StatusOr<int64> HeapSimulator::MinimumMemoryForComputation(
    const HloComputation& computation, const HloInstructionSequence& sequence,
    const HloAliasAnalysis& alias_analysis,
    const LogicalBuffer::SizeFunction& size_function,
    const HloSchedule* schedule) {
  TF_ASSIGN_OR_RETURN(
      HeapSimulator::Result result,
      HeapSimulator::Run(absl::make_unique<NoFragmentationStatsHeap>(),
                         computation, sequence, alias_analysis, size_function,
                         schedule, HeapSimulator::Options()));
  return result.heap_size;
}

/*static*/
StatusOr<HeapSimulator::Result> HeapSimulator::Run(
    std::unique_ptr<HeapAlgorithm> algorithm, const HloModule& module,
    const HloSchedule& schedule, const HloAliasAnalysis& alias_analysis,
    const BufferValue::SizeFunction& size_fn, const Options& options) {
  HeapSimulator heap(std::move(algorithm), size_fn, options, &schedule);
  const HloComputation* entry_computation = module.entry_computation();
  const HloInstructionSequence& instruction_sequence =
      schedule.sequence(entry_computation);
  TF_RETURN_IF_ERROR(heap.RunComputation(*entry_computation,
                                         instruction_sequence, alias_analysis));
  return heap.Finish();
}

/*static*/
StatusOr<HeapSimulator::Result> HeapSimulator::Run(
    std::unique_ptr<HeapAlgorithm> algorithm, const HloComputation& computation,
    const HloInstructionSequence& instruction_sequence,
    const HloAliasAnalysis& alias_analysis,
    const BufferValue::SizeFunction& size_fn, const Options& options,
    const absl::flat_hash_map<const HloComputation*, int64>*
        memory_by_computation) {
  HeapSimulator heap(std::move(algorithm), size_fn, options,
                     /*schedule=*/nullptr, memory_by_computation);
  TF_RETURN_IF_ERROR(
      heap.RunComputation(computation, instruction_sequence, alias_analysis));
  return heap.Finish();
}

/*static*/
StatusOr<HeapSimulator::Result> HeapSimulator::Run(
    std::unique_ptr<HeapAlgorithm> algorithm, const HloComputation& computation,
    const HloInstructionSequence& instruction_sequence,
    const HloAliasAnalysis& alias_analysis,
    const BufferValue::SizeFunction& size_fn, const HloSchedule* schedule,
    const Options& options) {
  HeapSimulator heap(std::move(algorithm), size_fn, options,
                     /*schedule=*/schedule, nullptr);
  TF_RETURN_IF_ERROR(
      heap.RunComputation(computation, instruction_sequence, alias_analysis));
  return heap.Finish();
}

// Runs a heap simulation for the given 'computation', assuming the given
// 'instruction_sequence'.
Status HeapSimulator::RunComputation(
    const HloComputation& computation,
    const HloInstructionSequence& instruction_sequence,
    const HloAliasAnalysis& alias_analysis) {
  XLA_VLOG_LINES(1, computation.parent()->ToString());
  XLA_VLOG_LINES(2, computation.ToString());

  HloDataflowAnalysis& dataflow_analysis = alias_analysis.dataflow_analysis();

  // instruction_schedule and computation_schedule are the maps that track each
  // instruction/computation and their ordinal in the schedule.
  absl::flat_hash_map<const HloInstruction*, int64> instruction_schedule;
  absl::flat_hash_map<const HloComputation*, int64> computation_schedule;

  // program_end_time is the time of the last instruction scheduled. It is equal
  // to the number of instructions in a computation.
  int64 program_end_time =
      FlattenSchedule(computation, instruction_sequence, schedule_, 0,
                      &instruction_schedule, &computation_schedule);

  VLOG(1) << "Program end time: " << program_end_time;

  // We track the definition and free events for each buffer, then we go through
  // each step and reply those events in program order.
  absl::flat_hash_map<const HloValue*, int64> buffer_start_map;
  absl::flat_hash_map<const HloValue*, int64> buffer_end_map;

  // Record the buffer define/free event for each time step. We free all
  // remaining buffers (entry parameter, etc) after the program has finished
  // running, so we set the size of to program_end_time + 1.
  std::vector<std::vector<const HloValue*>> buffers_defined(program_end_time +
                                                            1);
  std::vector<std::vector<const HloValue*>> buffers_freed(program_end_time + 1);

  // values_to_assign tracks the HloValues that we need to assign a buffer to.
  // Note that we only need to assign a buffer to a value when both of the
  // following conditions are met:
  //
  // - The user specifically asks us to assign a buffer to a set of HloValues,
  // and the value is in the set. If the user don't provide such a set, by
  // default we assign buffer to all HloValues.
  //
  // - If the instruction is in a nested call of the current computation, only
  // assign a buffer if we are doing global heap simulation.
  std::vector<const HloValue*> values_to_assign;

  // Keeps track of buffer start time and buffer end time.
  for (const HloValue* value : dataflow_analysis.values()) {
    // Ignore buffers that are not defined.
    if (instruction_schedule.count(value->defining_instruction()) == 0) {
      continue;
    }
    if (IgnoreBuffer(value)) {
      continue;
    }
    values_to_assign.push_back(value);
    int64 buffer_start_time = instruction_schedule[value->instruction()];

    int64 buffer_end_time = -1;
    // A buffer's live range ends when the last user finishes executing.
    for (const HloUse& use : value->uses()) {
      const HloInstruction* used = use.instruction;
      // As an optimization, we deem a while's init value's live range ends as
      // soon as the loop body starts. This optimization is only applicable to
      // the whole module simulation.
      if (schedule_ != nullptr && used->opcode() == HloOpcode::kWhile) {
        // The current live range is at the end of the while, move it to the
        // beginning of the body.
        used = used->while_body()->parameter_instruction(0);
        VLOG(1) << "Moved value " << value->ToShortString()
                << " to while param: " << used->ToString();
      }
      if (instruction_schedule.count(used) == 0) {
        // We didn't track the instruction `used`. This happens when we do
        // computation scope (versus module scope) heap simulation and when the
        // used instruction is outside of the computation being simulated.
        continue;
      }
      buffer_end_time = std::max(buffer_end_time, instruction_schedule[used]);
    }

    if (buffer_end_time == -1) {
      buffer_end_time = buffer_start_time;
    }

    for (const HloPosition& position : value->positions()) {
      const HloComputation* position_comp = position.instruction->parent();
      // If this instruction lives out, the live range of the instruction should
      // be extended to the end of the computation.
      if (position.instruction == position_comp->root_instruction()) {
        if (schedule_ == nullptr && &computation != position_comp) {
          continue;
        }
        if (computation_schedule.count(position_comp) == 0) {
          continue;
        }
        buffer_end_time =
            std::max(buffer_end_time, computation_schedule[position_comp]);
      }
    }

    // Entry parameters live across whole computation.
    if (value->instruction()->opcode() == HloOpcode::kParameter &&
        value->instruction()->parent() ==
            computation.parent()->entry_computation()) {
      buffer_end_time = program_end_time;
    }

    CHECK(buffer_start_time <= buffer_end_time);

    buffer_start_map[value] = buffer_start_time;
    buffer_end_map[value] = buffer_end_time;
  }

  NormalizeAliasedBuffers(&buffer_start_map, &buffer_end_map, values_to_assign,
                          alias_analysis);

  absl::c_sort(values_to_assign,
               [&](const HloValue* value1, const HloValue* value2) {
                 if (buffer_start_map[value1] != buffer_start_map[value2]) {
                   return buffer_start_map[value1] < buffer_start_map[value2];
                 }

                 if (buffer_end_map[value1] != buffer_end_map[value2]) {
                   return buffer_end_map[value1] < buffer_end_map[value2];
                 }
                 return value1->id() < value2->id();
               });

  // For each value that we need to assign a buffer to, add the define and free
  // events.
  for (const HloValue* value : values_to_assign) {
    buffers_defined[buffer_start_map[value]].push_back(value);
    buffers_freed[buffer_end_map[value]].push_back(value);
  }

  // All HloValues in a hlo buffer should be allocated to the same address. This
  // map tracks the first value that got allocated in a buffer.
  absl::flat_hash_map<const HloBuffer*, const HloValue*> first_allocated_value;

  VLOG(1) << "Program time" << program_end_time;

  // Go through each step in the program and replay each buffer define and free
  // events.
  for (int64 i = 0; i < program_end_time + 1; ++i) {
    VLOG(1) << "Time step: " << i;

    for (const HloValue* value : buffers_defined[i]) {
      bool shared = false;
      VLOG(1) << "Start buffer: " << value->ToShortString();
      const HloBuffer* hlo_buffer =
          &alias_analysis.GetBufferContainingValue(*value);
      if (first_allocated_value.count(hlo_buffer) != 0) {
        // We've already assigned an address for another value in this HloBuffer
        // (HloBuffer holds several aliased HloValues). All values in a buffer
        // should be assigned the same address. Find the one that's already
        // allocated and reuse its address.
        ShareBuffer(value, first_allocated_value[hlo_buffer],
                    value->instruction());
        VLOG(1) << "  ShareWith"
                << first_allocated_value[hlo_buffer]->ToShortString();
        continue;
      }
      if (options_.may_reuse_operand_buffers &&
          hlo_buffer->values().size() == 1) {
        // We don't support sharing an aliased buffer
        // (hlo_buffer->values().size() > 1) with its operand.
        for (const HloInstruction* operand : value->instruction()->operands()) {
          const HloValueSet operand_value_set =
              dataflow_analysis.GetValueSet(operand);
          for (const HloValue* operand_value : operand_value_set.values()) {
            const HloBuffer* operand_buffer =
                &alias_analysis.GetBufferContainingValue(*operand_value);
            if (operand_buffer->values().size() > 1) {
              continue;
            }
            if (buffer_end_map.count(operand_value) == 0) {
              continue;
            }
            // Can only share buffers that are about to be freed.
            if (buffer_end_map[operand_value] != i) {
              continue;
            }

            // The instruction that defines the operand value can be different
            // from the actual operand, if directly passing the defining
            // instruction into "CanShareOperandBufferWithUser" it creates a
            // check failure. The first condition guards against that case.
            if (value->instruction()->IsUserOf(operand_value->instruction()) &&
                value->instruction()->opcode() != HloOpcode::kCopy &&
                dataflow_analysis.CanShareOperandBufferWithUser(
                    operand_value->instruction(), operand_value->index(),
                    value->instruction(), value->index())) {
              // Remove the operand buffer right before sharing (allocating) a
              // new one.
              Free(operand_value, operand_value->instruction());
              buffers_freed[i].erase(
                  std::remove(buffers_freed[i].begin(), buffers_freed[i].end(),
                              operand_value),
                  buffers_freed[i].end());
              ShareBuffer(value, operand_value, value->instruction());
              // The live range of the operand buffer is now extended to the end
              // of the current instruction.
              buffer_end_map[operand_value] = buffer_end_map[value];
              VLOG(1) << "Sharing " << value->ToShortString() << " with "
                      << operand_value->ToShortString()
                      << ", size:" << size_fn_(*value);
              shared = true;
              break;
            }
          }
          if (shared) {
            break;
          }
        }
      }
      if (!shared) {
        Alloc(value, value->instruction());
        first_allocated_value[hlo_buffer] = value;
      }
    }

    if (!buffers_freed[i].empty()) {
      VLOG(1) << "Free Buffer: ";
    }
    for (const HloValue* value : buffers_freed[i]) {
      VLOG(1) << "  " << value->ToShortString();

      Free(value, value->instruction());
    }
  }
  return Status::OK();
}

HeapSimulator::HeapSimulator(
    std::unique_ptr<HeapAlgorithm> algorithm,
    const BufferValue::SizeFunction& size_fn, const Options& options,
    const HloSchedule* schedule,
    const absl::flat_hash_map<const HloComputation*, int64>*
        memory_by_computation)
    : no_fragmentation_stats_(absl::make_unique<NoFragmentationStatsHeap>()),
      algorithm_(std::move(algorithm)),
      size_fn_(size_fn),
      options_(options),
      schedule_(schedule),
      memory_by_computation_(memory_by_computation) {
  for (const BufferValueFlatSet& value_set : options.must_alias_sets) {
    auto group = std::make_shared<SharedGroup>();
    group->refcount = 0;
    VLOG(2) << "Shared buffers:";
    for (const BufferValue* buffer_value : value_set) {
      VLOG(2) << "    " << buffer_value->ToString();
      shared_buffers_.emplace(buffer_value, group);
      // Refcounts are not incremented here as buffers are shared but not
      // referenced yet.
    }
  }
  debug_trace_.set_whole_module_simulation(schedule_ != nullptr);
}

HeapSimulator::~HeapSimulator() {}

bool HeapSimulator::IgnoreBuffer(const BufferValue* buffer) const {
  // Buffers for constants are ignored unless the alloc_constants option is
  // set. Also ignore buffers that we're not meant to assign.
  //
  // TODO(b/32248867): For consistency, constants should get allocations.
  if (!options_.alloc_constants &&
      buffer->instruction()->opcode() == HloOpcode::kConstant) {
    return true;
  }
  return options_.buffers_to_assign != nullptr &&
         !options_.buffers_to_assign->contains(buffer);
}

// Alloc always calls the underlying heap algorithm.
void HeapSimulator::Alloc(const BufferValue* buffer,
                          const HloInstruction* instruction) {
  CHECK(!allocated_buffers_.contains(buffer))
      << "Alloc called on allocated buffer: " << *buffer;
  CHECK(!freed_buffers_.contains(buffer))
      << "Alloc called on freed buffer: " << *buffer;

  allocated_buffers_.insert(buffer);
  const int64 size = size_fn_(*buffer);
  algorithm_->Alloc(buffer, size);
  no_fragmentation_stats_->Alloc(buffer, size);
  FillDebugTrace(HeapSimulatorTrace::Event::ALLOC, buffer, instruction,
                 nullptr);
}

// Free calls the underlying algorithm for non-shared buffers, and for shared
// buffers whose group liveness has expired.  Shared group liveness is tracked
// by maintaining a refcount; the Free call on the last buffer in the group
// causes Free to be called on the underlying algorithm.
void HeapSimulator::Free(const BufferValue* buffer,
                         const HloInstruction* instruction) {
  const int64 size = size_fn_(*buffer);
  algorithm_->Free(buffer, size);
  no_fragmentation_stats_->Free(buffer, size);
  FillDebugTrace(HeapSimulatorTrace::Event::FREE, buffer, instruction, nullptr);
}

// ShareBuffer associates buffers with their SharedGroup in shared_buffers_.
// The 'buffer' must be a non-allocated, non-freed buffer, just like in calls
// to Alloc.  The 'shared' buffer must be a previously allocated or shared
// buffer. Both 'buffer' and 'shared' will be associated with the same
// SharedGroup.
void HeapSimulator::ShareBuffer(const BufferValue* buffer,
                                const BufferValue* shared,
                                const HloInstruction* instruction) {
  algorithm_->ShareWith(buffer, shared, size_fn_(*shared));
  no_fragmentation_stats_->ShareWith(buffer, shared, size_fn_(*shared));
  FillDebugTrace(HeapSimulatorTrace::Event::SHARE_WITH, buffer, instruction,
                 shared);
}

HeapSimulator::Result HeapSimulator::Finish() {
  Result result = algorithm_->Finish();

  // Post-process the result to add chunks for shared buffers.  An empty chunk
  // map means that either no buffers were allocated, or the heap was only
  // collecting statistics, e.g. NoFragmentationStatsHeap.
  if (!result.chunk_map.empty()) {
    // If we were told to assign specific buffers, make sure we've assigned
    // exactly that many buffers.
    if (options_.buffers_to_assign != nullptr) {
      CHECK_EQ(options_.buffers_to_assign->size(), result.chunk_map.size());
    }
  }

  // Fragmentation is the difference between the actual and ideal sizes.
  const Result no_frag_result = no_fragmentation_stats_->Finish();
  result.fragmentation_size = result.heap_size - no_frag_result.heap_size;

  // Copy the debug trace we collected to the final result.
  result.debug_trace.Swap(&debug_trace_);

  return result;
}

void HeapSimulator::FillDebugTrace(HeapSimulatorTrace::Event::Kind kind,
                                   const BufferValue* buffer,
                                   const HloInstruction* instruction,
                                   const BufferValue* share_with_canonical) {
  HeapSimulatorTrace::Event* event = debug_trace_.add_events();
  event->set_kind(kind);
  event->set_buffer_id(buffer->id());
  event->set_computation_name(instruction->parent()->name());
  event->set_instruction_name(instruction->name());
  if (kind == HeapSimulatorTrace::Event::SHARE_WITH) {
    CHECK(share_with_canonical != nullptr);
    event->set_share_with_canonical_id(share_with_canonical->id());
  } else {
    CHECK(share_with_canonical == nullptr);
  }
}

void NoFragmentationStatsHeap::Alloc(const BufferValue* buffer, int64 size) {
  current_heap_size_ += size;
  if (current_heap_size_ > max_heap_size_) {
    max_heap_size_ = current_heap_size_;
  }
}

void NoFragmentationStatsHeap::AccountForSubcomputationMemory(
    const HloInstruction* instruction, int64 alloc_size_by_instruction,
    const absl::flat_hash_map<const HloComputation*, int64>&
        memory_by_computation) {
  // We only count the memory usage of the largest subcomputation, instead of
  // adding them all, because subcomputations won't execute in parallel.
  int64 max_subcomputation_bytes = 0;
  for (const auto* c : instruction->called_computations()) {
    auto it = memory_by_computation.find(c);
    if (it != memory_by_computation.end()) {
      int64 subcomputation_bytes = it->second;
      if (subcomputation_bytes > max_subcomputation_bytes) {
        max_subcomputation_bytes = subcomputation_bytes;
      }
    }
  }
  if (max_subcomputation_bytes > 0 &&
      (instruction->opcode() == HloOpcode::kWhile ||
       instruction->opcode() == HloOpcode::kCall ||
       instruction->opcode() == HloOpcode::kConditional)) {
    // The output buffer of while/call/conditional is always aliased with the
    // output buffer of the root instruction in the body. Don't double count.
    max_subcomputation_bytes -= alloc_size_by_instruction;
  }
  max_heap_size_ =
      std::max(max_heap_size_, current_heap_size_ + max_subcomputation_bytes);
}

void NoFragmentationStatsHeap::Free(const BufferValue* buffer, int64 size) {
  current_heap_size_ -= size;
}

HeapSimulator::Result NoFragmentationStatsHeap::Finish() {
  // The result.chunk_map is empty, since we only collect stats, and don't
  // actually compute chunk assignments.
  Result result;
  result.heap_size = max_heap_size_;
  return result;
}

void GlobalDecreasingSizeBestFitHeap::Alloc(const BufferValue* buffer,
                                            int64 size) {
  // Degenerate case: 0-sized buffers are always allocated at offset 0.
  if (size == 0) {
    result_.chunk_map.emplace(buffer, Chunk{0, 0});
    return;
  }

  auto emplace_result = buffer_intervals_.emplace(
      buffer, BufferInterval{buffer, size, current_time_, -1, {}, true});
  DCHECK(emplace_result.second);
  ++current_time_;
}

void GlobalDecreasingSizeBestFitHeap::ShareWith(const BufferValue* buffer,
                                                const BufferValue* share_with,
                                                int64 size) {
  // Degenerate case: 0-sized buffers are always allocated at offset 0.
  if (size == 0) {
    result_.chunk_map.emplace(buffer, Chunk{0, 0});
    return;
  }
  DCHECK_NE(buffer_intervals_.count(share_with), 0);
  buffer_intervals_[share_with].colocations.push_back(buffer);
  auto emplace_result = buffer_intervals_.emplace(
      buffer, BufferInterval{buffer, size, current_time_, -1, {}, false});
  DCHECK(emplace_result.second);
  ++current_time_;
}

absl::flat_hash_set<const BufferValue*>
GlobalDecreasingSizeBestFitHeap::GetTransitiveColocations(
    const BufferInterval& interval) {
  absl::flat_hash_set<const BufferValue*> result;
  std::vector<const BufferInterval*> worklist = {&interval};
  while (!worklist.empty()) {
    const BufferInterval* item = worklist.back();
    worklist.pop_back();
    for (const BufferValue* buffer_colocated : item->colocations) {
      result.insert(buffer_colocated);
      worklist.push_back(&buffer_intervals_[buffer_colocated]);
    }
  }

  return result;
}

void GlobalDecreasingSizeBestFitHeap::Free(const BufferValue* buffer,
                                           int64 size) {
  // Degenerate case: 0-sized buffers are always allocated at offset 0.
  if (size == 0) {
    return;
  }
  BufferInterval& buffer_interval = FindOrDie(buffer_intervals_, buffer);
  DCHECK_EQ(buffer_interval.buffer, buffer);
  DCHECK_EQ(buffer_interval.size, size);
  DCHECK_EQ(buffer_interval.end, -1);
  if (buffer_interval.end != -1) {
    return;
  }
  buffer_interval.end = current_time_;
  ++current_time_;
}

namespace {

// Node in BufferIntervalTree that stores the alloc and free times of a
// buffer, and the chunk assigned to it.
struct BufferIntervalTreeNode {
  // Alloc time.
  int64 start;
  // Free time.
  int64 end;
  // Maximum free time of all nodes in the subtree where this node is the
  // root.
  int64 subtree_end;
  // Allocated chunk for the buffer.
  HeapSimulator::Chunk chunk;
  // Left child.
  BufferIntervalTreeNode* left;
  // Right child.
  BufferIntervalTreeNode* right;
};

// An interval tree that can query buffers overlapping in time.
class BufferIntervalTree {
 public:
  explicit BufferIntervalTree(int capacity) : node_storage_(capacity) {}

  using Chunk = HeapSimulator::Chunk;

  // Adds a buffer to the interval tree, with the time interval and allocated
  // chunk specified.
  void Add(int64 start, int64 end, const Chunk& chunk) {
    int index = node_count_;
    DCHECK_LT(index, node_storage_.size());
    ++node_count_;

    node_storage_[index] =
        BufferIntervalTreeNode{start, end, end, chunk, nullptr, nullptr};

    if (index == 0) {
      // This is root.
      return;
    }

    BufferIntervalTreeNode* parent = &node_storage_[0];
    while (true) {
      parent->subtree_end = std::max(parent->subtree_end, end);
      if (parent->start > start) {
        if (parent->left == nullptr) {
          parent->left = &node_storage_[index];
          return;
        }
        parent = parent->left;
      } else {
        if (parent->right == nullptr) {
          parent->right = &node_storage_[index];
          return;
        }
        parent = parent->right;
      }
    }
  }

  // Returns vector of allocated chunks that overlap with the given time
  // interval.
  std::vector<Chunk> ChunksOverlappingInTime(int64 start, int64 end) {
    std::vector<Chunk> result;
    if (node_count_ == 0) {
      return result;
    }
    std::vector<BufferIntervalTreeNode*> visiting_stack;
    visiting_stack.push_back(&node_storage_[0]);
    while (!visiting_stack.empty()) {
      BufferIntervalTreeNode* top = visiting_stack.back();
      visiting_stack.pop_back();
      if (start > top->subtree_end) {
        continue;
      }
      if (top->left != nullptr) {
        visiting_stack.push_back(top->left);
      }
      if (top->start <= end && top->end >= start) {
        result.push_back(top->chunk);
      }
      if (end < top->start) {
        continue;
      }
      if (top->right != nullptr) {
        visiting_stack.push_back(top->right);
      }
    }
    return result;
  }

 private:
  int64 node_count_ = 0;
  std::vector<BufferIntervalTreeNode> node_storage_;
};

}  // namespace

HeapSimulator::Result GlobalDecreasingSizeBestFitHeap::Finish() {
  std::vector<BufferInterval> sorted_buffer_intervals;
  for (auto& entry : buffer_intervals_) {
    sorted_buffer_intervals.push_back(entry.second);
  }
  if (type_ == kTemporal) {
    // Sort by live-range. A live range is defined by the range between the
    // start of the first buffer and the end of the last co-located
    // buffer. There could be "holes" in the live ranges of each co-located
    // buffers, but in this heuristics we think they are contiguous.
    absl::c_sort(sorted_buffer_intervals,
                 [&](const BufferInterval& x, const BufferInterval& y) {
                   int64 x_end = x.end;
                   for (auto colocation : GetTransitiveColocations(x)) {
                     x_end = std::max(x_end, buffer_intervals_[colocation].end);
                   }

                   int64 y_end = y.end;
                   for (auto colocation : GetTransitiveColocations(y)) {
                     y_end = std::max(y_end, buffer_intervals_[colocation].end);
                   }

                   if (x_end - x.start != y_end - y.start) {
                     return x_end - x.start > y_end - y.start;
                   }

                   if (x.size != y.size) {
                     return x.size > y.size;
                   }
                   return x.buffer->id() < y.buffer->id();
                 });
  } else {
    // Sort by spatial size. We don't look at co-locates as they should have the
    // same size.
    CHECK(type_ == kSpatial);
    absl::c_sort(sorted_buffer_intervals,
                 [&](const BufferInterval& x, const BufferInterval& y) {
                   if (x.size != y.size) {
                     return x.size > y.size;
                   }
                   if (x.end - x.start != y.end - y.start) {
                     return x.end - x.start > y.end - y.start;
                   }
                   return x.buffer->id() < y.buffer->id();
                 });
  }

  BufferIntervalTree interval_tree(sorted_buffer_intervals.size());
  for (auto& buffer_interval : sorted_buffer_intervals) {
    if (!buffer_interval.need_allocation) {
      continue;
    }
    VLOG(1) << "Finding chunks for buffer: "
            << buffer_interval.buffer->ToString();
    VLOG(1) << "Size " << buffer_interval.size << ", start "
            << buffer_interval.start << ", end " << buffer_interval.end;
    auto chunks_overlapping_in_time = interval_tree.ChunksOverlappingInTime(
        buffer_interval.start, buffer_interval.end);
    // Get all colocated buffers and gather all interferenced chunks.
    //
    // Imagine that we've already allocated three chunks : a, b and c.  And now
    // we want to allocate d. Since e is colocated with d, we have to allocate
    // chunks for them together at the same address. To do this, we first gather
    // all chunks that overlap with d and e on the time dimension, in this case
    // the overlapped chunks are a and b (c doesn't overlap with either of d and
    // e), then find create a new chunk that doesn't overlap with a and b on the
    // space dimension.
    //
    // space
    //   ^
    //   |+--d---+      +---e---+
    //   |
    //   |+---+  +---------------+  +-------+
    //   ||   |  |               |  |       |
    //   ||   |  |               |  |       |
    //   |+-a-+  +-------b-------+  +---c---+
    //   ----------------------------------------> time
    for (auto colocation : GetTransitiveColocations(buffer_interval)) {
      auto colocation_interval = buffer_intervals_[colocation];
      auto colocation_overlapping = interval_tree.ChunksOverlappingInTime(
          colocation_interval.start, colocation_interval.end);
      VLOG(1) << "  Alias size " << colocation_interval.size << ", start "
              << colocation_interval.start << ", end "
              << colocation_interval.end << " "
              << colocation_interval.buffer->ToString();
      chunks_overlapping_in_time.insert(chunks_overlapping_in_time.end(),
                                        colocation_overlapping.begin(),
                                        colocation_overlapping.end());
    }
    absl::c_sort(
        chunks_overlapping_in_time,
        [](const Chunk& x, const Chunk& y) { return x.offset < y.offset; });

    // Find the minimum free chunk that can hold this buffer.
    Chunk min_fit_chunk{-1, INT64_MAX};
    auto use_free_chunk_if_smaller = [&](int64 free_offset, int64 free_size) {
      if (free_size < buffer_interval.size) {
        return;
      }

      if (free_size < min_fit_chunk.size) {
        min_fit_chunk = {free_offset, free_size};
      }
    };

    int64 offset = 0;
    for (auto& chunk : chunks_overlapping_in_time) {
      if (offset < chunk.offset) {
        use_free_chunk_if_smaller(offset, chunk.offset - offset);
      }
      offset =
          std::max(offset, RoundUpToNearest(chunk.chunk_end(), alignment_));
    }
    use_free_chunk_if_smaller(offset, result_.heap_size - offset);

    if (min_fit_chunk.offset == -1) {
      // Increase the heap size to fit in the last free chunk.
      result_.heap_size = offset + buffer_interval.size;
      min_fit_chunk = {offset, buffer_interval.size};
    }

    min_fit_chunk.size = buffer_interval.size;
    const auto emplace_result =
        result_.chunk_map.emplace(buffer_interval.buffer, min_fit_chunk);

    DCHECK(emplace_result.second);

    interval_tree.Add(buffer_interval.start, buffer_interval.end,
                      min_fit_chunk);
    for (auto colocation : GetTransitiveColocations(buffer_interval)) {
      const auto emplace_result =
          result_.chunk_map.emplace(colocation, min_fit_chunk);
      DCHECK(emplace_result.second);
      auto colocation_interval = buffer_intervals_[colocation];
      interval_tree.Add(colocation_interval.start, colocation_interval.end,
                        min_fit_chunk);
    }
  }
  VLOG(1) << "result heap_size: " << result_.heap_size;
  return result_;
}

HeapSimulator::Result ChooseBestHeapAlgorithm::Finish() {
  DCHECK(!algorithms_.empty());
  std::vector<Result> results(algorithms_.size());
  int64 min_size = INT64_MAX;
  int min_size_index = -1;
  for (int i = 0; i < algorithms_.size(); ++i) {
    results[i] = algorithms_[i]->Finish();
    if (results[i].heap_size < min_size) {
      min_size = results[i].heap_size;
      min_size_index = i;
    }
  }

  DCHECK_GE(min_size_index, 0);
  return results[min_size_index];
}

}  // namespace xla
