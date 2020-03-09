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

#include "tensorflow/core/profiler/utils/group_events.h"

#include <stack>

#include "absl/strings/str_join.h"
#include "absl/types/optional.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/profiler/utils/tf_xplane_visitor.h"
#include "tensorflow/core/profiler/utils/xplane_schema.h"
#include "tensorflow/core/profiler/utils/xplane_utils.h"

namespace tensorflow {
namespace profiler {
namespace {

// Creates stat metadata for the stats which may be added by grouping.
void CreateStatMetadata(XPlane* plane) {
  XPlaneBuilder builder(plane);
  builder.GetOrCreateStatMetadata(GetStatTypeStr(StatType::kGroupId));
  builder.GetOrCreateStatMetadata(GetStatTypeStr(StatType::kStepName));
}

// Returns event type if it is a KernelLaunch or KernelExecute event.
absl::optional<int64> GetKernelEventType(const XPlaneVisitor& visitor,
                                         const XEvent& event) {
  for (const auto& stat : event.stats()) {
    if (visitor.GetStatType(stat) == StatType::kCorrelationId) {
      // TODO(b/149095099): avoid string comparison.
      return visitor.Name() == kHostThreads ? HostEventType::kKernelLaunch
                                            : HostEventType::kKernelExecute;
    }
  }
  return absl::nullopt;
}

int64 GetEventType(const XPlaneVisitor& visitor, const XEvent& event) {
  if (absl::optional<int64> event_type = visitor.GetEventType(event)) {
    return *event_type;
  } else if (absl::optional<int64> kernel_event_type =
                 GetKernelEventType(visitor, event)) {
    // KernelLaunch and KernelExecute event types are not supported by
    // XPlaneVisitor and should be checked separately.
    // TODO(148346217): Make XPlaneVisitor support KernelLaunch and
    // KernelExecute event types.
    return *kernel_event_type;
  } else {
    return HostEventType::kUnknownHostEventType;
  }
}

const XStat* GetStat(const XPlaneVisitor& visitor, const XEvent& event,
                     int64 stat_type) {
  for (const auto& stat : event.stats()) {
    if (visitor.GetStatType(stat) == stat_type) {
      return &stat;
    }
  }
  return nullptr;
}

void SetGroupId(const XPlaneVisitor& visitor, int64 group_id, XEvent* event) {
  AddOrUpdateIntStat(*visitor.GetStatMetadataId(StatType::kGroupId), group_id,
                     event);
}

using VirtualEventNodeMap =
    absl::flat_hash_map<int64 /*step_id*/,
                        absl::flat_hash_map<int64 /*iter_num*/, EventNode*>>;

std::unique_ptr<XEvent> CreateVirtualEvent(const XStat& step_id_stat,
                                           const XStat& iter_num_stat) {
  auto virtual_event = absl::make_unique<XEvent>();
  *virtual_event->add_stats() = step_id_stat;
  *virtual_event->add_stats() = iter_num_stat;
  return virtual_event;
}

bool NeedsVirtualEventsForHostTrainingLoop(
    const std::vector<int64 /*EventType*/>& root_event_types) {
  return std::find(root_event_types.begin(), root_event_types.end(),
                   HostEventType::kHostTrainingLoopIteration) !=
         root_event_types.end();
}

bool NeedsVirtualEventsForAsyncExecutor(
    const std::vector<int64 /*EventType*/>& root_event_types) {
  return std::find(root_event_types.begin(), root_event_types.end(),
                   HostEventType::kAsyncExecutorTraceContext) !=
         root_event_types.end();
}

bool HasFunctionRun(EventNode* event_node) {
  for (EventNode* child : event_node->GetChildren()) {
    if (child->GetPlaneVisitor().GetEventType(child->GetEvent()) ==
        HostEventType::kFunctionRun) {
      return true;
    }
  }
  return false;
}

}  // namespace

const XStat* EventNode::GetContextStat(int64 stat_type) const {
  if (const XStat* stat = GetStat(*visitor_, *event_, stat_type)) {
    return stat;
  } else if (parent_) {
    return parent_->GetContextStat(stat_type);
  }
  return nullptr;
}

std::string EventNode::GetGroupName() const {
  std::vector<std::string> name_parts;
  if (const XStat* graph_type_stat = GetContextStat(StatType::kGraphType)) {
    name_parts.push_back(graph_type_stat->str_value());
  }
  int64 step_num = group_id_.value_or(0);
  if (const XStat* step_num_stat = GetContextStat(StatType::kStepNum)) {
    step_num = step_num_stat->int64_value();
  }
  if (const XStat* iter_num_stat = GetContextStat(StatType::kIterNum)) {
    step_num = iter_num_stat->int64_value();
  }
  name_parts.push_back(absl::StrCat(step_num));
  return absl::StrJoin(name_parts, " ");
}

void EventNode::PropagateGroupId(int64 group_id) {
  group_id_ = group_id;
  SetGroupId(*visitor_, group_id, event_);
  for (const auto& child : children_) {
    child->PropagateGroupId(*group_id_);
  }
}

void EventNode::AddStepName(absl::string_view step_name) {
  AddOrUpdateStrStat(*visitor_->GetStatMetadataId(StatType::kStepName),
                     step_name, event_);
}

bool EventNode::IsNestedIn(EventNode* parent) {
  return parent && IsNested(GetEvent(), parent->GetEvent());
}

void EventForest::ConnectIntraThread(const XPlaneVisitor& visitor,
                                     XPlane* plane) {
  for (auto& line : *plane->mutable_lines()) {
    std::vector<EventNode*> parent_nodes;
    for (auto& event : *line.mutable_events()) {
      auto cur_node = absl::make_unique<EventNode>(&visitor, &event);
      while (!parent_nodes.empty()) {
        EventNode* parent_node = parent_nodes.back();
        if (cur_node->IsNestedIn(parent_node)) {
          parent_node->AddChild(cur_node.get());
          break;
        } else {
          parent_nodes.pop_back();
        }
      }
      parent_nodes.push_back(cur_node.get());
      // event_node_map_ keeps cur_node alive.
      event_node_map_[GetEventType(visitor, event)].push_back(
          std::move(cur_node));
    }
  }
}

void EventForest::ConnectInterThread(
    const std::vector<InterThreadConnectInfo>& connect_info_list) {
  for (const auto& connect_info : connect_info_list) {
    absl::flat_hash_map<std::vector<int64>, EventNode*> connect_map;
    const std::vector<int64>& stat_types = connect_info.stat_types;
    if (auto parent_event_node_list =
            gtl::FindOrNull(event_node_map_, connect_info.parent_event_type)) {
      for (const auto& parent_event_node : *parent_event_node_list) {
        std::vector<int64> stats;
        for (auto stat_type : stat_types) {
          const XStat* stat = parent_event_node->GetContextStat(stat_type);
          if (!stat) break;
          stats.push_back(stat->value_case() == stat->kInt64Value
                              ? stat->int64_value()
                              : stat->uint64_value());
        }
        if (stats.size() == stat_types.size()) {
          connect_map[stats] = parent_event_node.get();
        }
      }
    }
    if (auto child_event_node_list =
            gtl::FindOrNull(event_node_map_, connect_info.child_event_type)) {
      for (const auto& child_event_node : *child_event_node_list) {
        std::vector<int64> stats;
        for (auto stat_type : stat_types) {
          const XStat* stat = child_event_node->GetContextStat(stat_type);
          if (!stat) break;
          stats.push_back(stat->value_case() == stat->kInt64Value
                              ? stat->int64_value()
                              : stat->uint64_value());
        }
        if (stats.size() == stat_types.size()) {
          if (auto parent_event_node = gtl::FindPtrOrNull(connect_map, stats)) {
            parent_event_node->AddChild(child_event_node.get());
          }
        }
      }
    }
  }
}

void EventForest::CreateEventGroup(
    const std::vector<int64 /*EventType*/>& root_event_types) {
  int64 next_group_id = 0;
  for (int64 root_event_type : root_event_types) {
    if (auto root_event_node_list =
            gtl::FindOrNull(event_node_map_, root_event_type)) {
      for (const auto& root_event_node : *root_event_node_list) {
        // Skip if it already belongs to a group.
        if (root_event_node->GetGroupId()) continue;
        int64 group_id = next_group_id++;
        root_event_node->PropagateGroupId(group_id);
        std::string group_name = root_event_node->GetGroupName();
        // TODO(jihochoi): change event name instead.
        root_event_node->AddStepName(group_name);
        event_group_name_map_[group_id] = std::move(group_name);
      }
    }
  }
}

void EventForest::CreateVirtualEventsForHostTrainingLoop() {
  VirtualEventNodeMap virtual_event_node_map;
  auto executor_event_node_list =
      gtl::FindOrNull(event_node_map_, HostEventType::kExecutorStateProcess);
  if (!executor_event_node_list) return;
  for (auto& executor_event_node : *executor_event_node_list) {
    const XStat* step_id_stat =
        executor_event_node->GetContextStat(StatType::kStepId);
    const XStat* iter_num_stat =
        executor_event_node->GetContextStat(StatType::kIterNum);
    if (!step_id_stat || !iter_num_stat) continue;
    int64 step_id = step_id_stat->int64_value();
    int64 iter_num = iter_num_stat->int64_value();
    // Process the event with nonzero iter_num only to filter out the events
    // related to tf.data.
    // TODO(jihochoi): Filter out tf.data events more reliably.
    if (!iter_num) continue;
    EventNode*& virtual_event_node = virtual_event_node_map[step_id][iter_num];
    if (!virtual_event_node) {
      std::unique_ptr<XEvent> new_virtual_event =
          CreateVirtualEvent(*step_id_stat, *iter_num_stat);
      auto new_virtual_event_node = absl::make_unique<EventNode>(
          &executor_event_node->GetPlaneVisitor(), new_virtual_event.get());
      // virtual_event_container_ keeps new_virtual_event alive.
      virtual_event_container_.push_back(std::move(new_virtual_event));
      virtual_event_node = new_virtual_event_node.get();
      // event_node_map_ keeps new_virtual_event_node alive.
      event_node_map_[HostEventType::kHostTrainingLoopIteration].push_back(
          std::move(new_virtual_event_node));
    }
    virtual_event_node->AddChild(executor_event_node.get());
  }
}

void EventForest::CreateVirtualEventsForAsyncExecutor() {
  auto eager_kernel_execute_event_node_list =
      gtl::FindOrNull(event_node_map_, HostEventType::kEagerKernelExecute);
  if (!eager_kernel_execute_event_node_list) return;
  EventNode* virtual_event_node = nullptr;
  for (auto& eager_kernel_execute_event_node :
       *eager_kernel_execute_event_node_list) {
    if (HasFunctionRun(eager_kernel_execute_event_node.get())) {
      auto new_virtual_event = absl::make_unique<XEvent>();
      auto new_virtual_event_node = absl::make_unique<EventNode>(
          &eager_kernel_execute_event_node->GetPlaneVisitor(),
          new_virtual_event.get());
      // virtual_event_container_ keeps new_virtual_event alive.
      virtual_event_container_.push_back(std::move(new_virtual_event));
      virtual_event_node = new_virtual_event_node.get();
      // event_node_map_ keeps new_virtual_event_node alive.
      event_node_map_[HostEventType::kAsyncExecutorTraceContext].push_back(
          std::move(new_virtual_event_node));
    }
    if (virtual_event_node) {
      virtual_event_node->AddChild(eager_kernel_execute_event_node.get());
    }
  }
}

EventForest::EventForest(
    const std::vector<InterThreadConnectInfo>& connect_info_list,
    const std::vector<int64>& root_event_types,
    const std::function<XPlaneVisitor(const XPlane*)> visitor_factory,
    XSpace* space) {
  visitors_.reserve(space->planes_size());
  for (auto& plane : *space->mutable_planes()) {
    CreateStatMetadata(&plane);
    visitors_.push_back(visitor_factory(&plane));
    ConnectIntraThread(visitors_.back(), &plane);
  }
  ConnectInterThread(connect_info_list);
  if (NeedsVirtualEventsForHostTrainingLoop(root_event_types)) {
    CreateVirtualEventsForHostTrainingLoop();
  }
  if (NeedsVirtualEventsForAsyncExecutor(root_event_types)) {
    CreateVirtualEventsForAsyncExecutor();
  }
  CreateEventGroup(root_event_types);
}

void GroupTfEvents(XSpace* space, EventGroupNameMap* event_group_name_map) {
  if (!space) return;
  std::vector<InterThreadConnectInfo> connect_info_list(
      {{HostEventType::kFunctionRun,
        HostEventType::kExecutorStateProcess,
        {StatType::kStepId}},
       {HostEventType::kSessionRun,
        HostEventType::kExecutorStateProcess,
        {StatType::kStepId}},
       {HostEventType::kExecutorStateProcess,
        HostEventType::kIteratorGetNextOp,
        {StatType::kStepId, StatType::kIterNum}},
       {HostEventType::kKernelLaunch,
        HostEventType::kKernelExecute,
        {StatType::kCorrelationId}}});
  const std::vector<int64 /*EventType*/> root_event_types(
      {HostEventType::kHostTrainingLoopIteration, HostEventType::kTraceContext,
       HostEventType::kFunctionRun, HostEventType::kSessionRun});
  EventForest event_forest(connect_info_list, root_event_types,
                           CreateTfXPlaneVisitor, space);
  if (event_group_name_map) {
    *event_group_name_map = event_forest.GetEventGroupNameMap();
  }
}

}  // namespace profiler
}  // namespace tensorflow
