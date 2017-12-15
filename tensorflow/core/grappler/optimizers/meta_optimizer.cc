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

#include "tensorflow/core/grappler/optimizers/meta_optimizer.h"
#include "tensorflow/core/framework/function.pb.h"
#include "tensorflow/core/framework/versions.pb.h"
#include "tensorflow/core/grappler/optimizers/arithmetic_optimizer.h"
#include "tensorflow/core/grappler/optimizers/auto_parallel.h"
#include "tensorflow/core/grappler/optimizers/constant_folding.h"
#include "tensorflow/core/grappler/optimizers/dependency_optimizer.h"
#include "tensorflow/core/grappler/optimizers/graph_optimizer.h"
#include "tensorflow/core/grappler/optimizers/layout_optimizer.h"
#include "tensorflow/core/grappler/optimizers/memory_optimizer.h"
#include "tensorflow/core/grappler/optimizers/model_pruner.h"
#include "tensorflow/core/grappler/utils/topological_sort.h"
#include "tensorflow/core/lib/core/status.h"

namespace tensorflow {
namespace grappler {

std::unique_ptr<GraphOptimizer> MetaOptimizer::NewOptimizer(
    const string& optimizer) {
  VLOG(1) << "Adding graph optimization pass: " << optimizer;
  std::unique_ptr<GraphOptimizer> graph_optimizer;
  if (optimizer == "pruning") {
    graph_optimizer.reset(new ModelPruner());
  }
  if (optimizer == "constfold") {
    graph_optimizer.reset(new ConstantFolding(cpu_device_));
  }
  if (optimizer == "layout") {
    graph_optimizer.reset(new LayoutOptimizer());
  }
  if (optimizer == "memory") {
    graph_optimizer.reset(new MemoryOptimizer(RewriterConfig::MANUAL));
  }
  if (optimizer == "arithmetic") {
    graph_optimizer.reset(
        new ArithmeticOptimizer(cfg_.arithmetic_optimization()));
  }
  if (optimizer == "autoparallel") {
    graph_optimizer.reset(
        new AutoParallel(cfg_.auto_parallel().num_replicas()));
  }
  if (optimizer == "dependency") {
    graph_optimizer.reset(
        new DependencyOptimizer(cfg_.dependency_optimization()));
  }
  return graph_optimizer;
}

Status MetaOptimizer::Optimize(Cluster* cluster, const GrapplerItem& item,
                               GraphDef* optimized_graph) {
  std::vector<std::unique_ptr<GraphOptimizer>> optimizers;
  if (cfg_.optimizers().empty()) {
    if (!cfg_.disable_model_pruning()) {
      optimizers.push_back(std::unique_ptr<GraphOptimizer>(new ModelPruner()));
    }
    if (cfg_.constant_folding() != RewriterConfig::OFF) {
      optimizers.push_back(std::unique_ptr<GraphOptimizer>(
          new ConstantFolding(cfg_.constant_folding(), cpu_device_)));
    }
    if (cfg_.arithmetic_optimization() != RewriterConfig::OFF) {
      optimizers.push_back(std::unique_ptr<GraphOptimizer>(
          new ArithmeticOptimizer(cfg_.arithmetic_optimization())));
    }
    if (cfg_.dependency_optimization() != RewriterConfig::OFF) {
      optimizers.push_back(std::unique_ptr<GraphOptimizer>(
          new DependencyOptimizer(cfg_.dependency_optimization())));
    }
    if (cfg_.layout_optimizer() == RewriterConfig::ON) {
      optimizers.push_back(
          std::unique_ptr<GraphOptimizer>(new LayoutOptimizer()));
    }
    if (cfg_.memory_optimization() > 1) {
      if (cfg_.memory_optimizer_target_node_name_prefix().empty()) {
        optimizers.push_back(std::unique_ptr<GraphOptimizer>(
            // Use the default target node name prefix "gradients/"
            new MemoryOptimizer(cfg_.memory_optimization())));
      } else {
        optimizers.push_back(
            std::unique_ptr<GraphOptimizer>(new MemoryOptimizer(
                cfg_.memory_optimization(),
                cfg_.memory_optimizer_target_node_name_prefix())));
      }
    }
    if (cfg_.auto_parallel().enable()) {
      optimizers.push_back(std::unique_ptr<GraphOptimizer>(
          new AutoParallel(cfg_.auto_parallel().num_replicas())));
    }
  } else {
    std::set<string> available_optimizers = {
        "pruning",      "constfold",  "layout",    "memory",
        "autoparallel", "arithmetic", "dependency"};
    for (const auto& optimizer : cfg_.optimizers()) {
      if (available_optimizers.find(optimizer) != available_optimizers.end()) {
        optimizers.push_back(NewOptimizer(optimizer));
      }
    }
  }

  if (optimizers.empty()) {
    *optimized_graph = item.graph;
    return Status::OK();
  }

  bool already_optimized = false;
  for (const auto& optimizer : optimizers) {
    if (!already_optimized) {
      auto status = optimizer->Optimize(cluster, item, optimized_graph);
      string result;
      if (!status.ok()) {
        VLOG(1) << "Not able to apply optimizer " << optimizer->name()
                << ". Return status: " << status.ToString();
        result = status.ToString();
      } else {
        already_optimized = true;
        result = strings::StrCat(
            "OK. "
            "Graph size before: ",
            item.graph.node_size(),
            ". Graph size after: ", optimized_graph->node_size());
      }
      result_.push_back(std::make_pair(optimizer->name(), result));
      VLOG(1) << "Optimizer " << optimizer->name()
              << " return status: " << result;
    } else {
      GrapplerItem optimized_item(item, std::move(*optimized_graph));
      auto status =
          optimizer->Optimize(cluster, optimized_item, optimized_graph);
      string result;
      if (!status.ok()) {
        VLOG(1) << "Not able to apply optimizer " << optimizer->name()
                << ". Return status: " << status.ToString();
        optimized_graph->Swap(&optimized_item.graph);
        result = status.ToString();
      } else {
        result = strings::StrCat(
            "OK. "
            "Graph size before: ",
            optimized_item.graph.node_size(),
            ". Graph size after: ", optimized_graph->node_size());
      }
      result_.push_back(std::make_pair(optimizer->name(), result));
      VLOG(1) << "Optimizer " << optimizer->name()
              << " return status: " << result;
    }
  }

  if (already_optimized) {
    TF_RETURN_IF_ERROR(TopologicalSort(optimized_graph));
    // Make sure that the optimizers preserved the graph version and library.
    DCHECK_GE(optimized_graph->library().function_size(),
              item.graph.library().function_size());
    DCHECK_GE(optimized_graph->library().gradient_size(),
              item.graph.library().gradient_size());
    DCHECK_EQ(optimized_graph->versions().producer(),
              item.graph.versions().producer());
  } else {
    *optimized_graph = item.graph;
  }

  return Status::OK();
}

void MetaOptimizer::PrintResult() {
  for (const auto& result : result_) {
    LOG(INFO) << "Return status of optimizer " << result.first << ": "
              << result.second;
  }
}

void MetaOptimizer::Feedback(Cluster* cluster, const GrapplerItem& item,
                             const GraphDef& pruned_graph, double result) {
  // Nothing to do for MetaOptimizer.
}

bool MetaOptimizerEnabled(const RewriterConfig& cfg) {
  return !cfg.disable_model_pruning() ||
         cfg.layout_optimizer() == RewriterConfig::ON ||
         cfg.constant_folding() != RewriterConfig::OFF ||
         cfg.dependency_optimization() != RewriterConfig::OFF ||
         cfg.arithmetic_optimization() != RewriterConfig::OFF ||
         cfg.auto_parallel().enable() || cfg.memory_optimization() > 1 ||
         !cfg.optimizers().empty();
}

Status RunMetaOptimizer(const GrapplerItem& item, const RewriterConfig& cfg,
                        DeviceBase* cpu_device, Cluster* cluster,
                        GraphDef* optimized_graph) {
  MetaOptimizer optimizer(cpu_device, cfg);
  return optimizer.Optimize(cluster, item, optimized_graph);
}

}  // namespace grappler
}  // namespace tensorflow
