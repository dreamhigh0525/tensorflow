#include <tensorflow/core/common_runtime/constant_folding.h>
#include <tensorflow/core/graph/graph_constructor.h>
#include <tensorflow/core/graph/node_builder.h>
#include <tensorflow/core/graph/subgraph.h>
#include <tensorflow/core/platform/init_main.h>
#include <tensorflow/core/public/session.h>
#include <tensorflow/core/util/command_line_flags.h>
#include <tensorflow/tools/graph_transforms/transform_utils.h>

namespace tensorflow {
namespace graph_transforms {

// Remove control depdencies in preparation for inference.
// In the tensorflow graph, control dependencies are represented as extra
// inputs which are referenced with "^tensor_name".
// See node_def.proto for more details.
Status RemoveControlDependencies(const GraphDef& input_graph_def,
                const TransformFuncContext& context,
                GraphDef* output_graph_def) {
    output_graph_def->Clear();
    for (const NodeDef& node : input_graph_def.node()) {
        NodeDef* new_node = output_graph_def->mutable_node()->Add();
        *new_node = node;
        new_node->clear_input();
        for (const auto& input : node.input()) {
            if (input[0] != '^') {
                new_node->add_input(input);
            }
        }
    }
    return Status::OK();
}

REGISTER_GRAPH_TRANSFORM("remove_control_dependencies", RemoveControlDependencies);

}  // namespace graph_transforms
}  // namespace tensorflow
