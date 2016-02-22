/* Copyright 2015 Google Inc. All Rights Reserved.

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

/// <reference path="../graph.ts" />
/// <reference path="scene.ts" />
/// <reference path="annotation.ts" />
/// <reference path="contextmenu.ts" />

module tf.graph.scene.node {

/**
 * Select or Create a "g.nodes" group to a given sceneGroup
 * and builds a number of "g.node" groups inside the group.
 *
 * Structure Pattern:
 *
 * <g class="nodes">
 *   <g class="node">
 *     <g class="in-annotations">
 *       ...
 *     </g>
 *     <g class="out-annotations">
 *       ...
 *     </g>
 *     <g class="nodeshape">
 *      <!--
 *      Content of the node shape should be for the node itself. For example a
 *      Metanode would have a <rect> with rounded edges, an op would have an
 *      <ellipse>. More complex nodes like series may contain multiple elements
 *      which are conditionally visible based on whether the node is expanded.
 *      -->
 *     </g>
 *     <text class="label">node name</text>
 *     <g class="subscene">
 *       <!--
 *       Content of  the subscene (only for metanode and series node).
 *
 *       Subscene is a svg group that contains content of the
 *       metanode's metagraph that is recursively generated by Scene.build().
 *
 *       When the graph is expanded multiple times, a subscene can contain
 *       nested subscenes inside.
 *       -->
 *     </g>
 *   </g>
 *   ...
 * </g>
 *
 *
 * @param sceneGroup selection of the container
 * @param nodeData array of render node information to map
 * @param sceneBehavior parent scene module
 * @return selection of the created nodeGroups
 */
export function buildGroup(sceneGroup,
    nodeData: render.RenderNodeInfo[], sceneBehavior) {
  let container = scene.selectOrCreateChild(sceneGroup, "g",
    Class.Node.CONTAINER);
  // Select all children and join with data.
  // (Note that all children of g.nodes are g.node)
  let nodeGroups = container.selectAll(function() {
    // using d3's selector function
    // See https://github.com/mbostock/d3/releases/tag/v2.0.0
    // (It's not listed in the d3 wiki.)
    return this.childNodes; // this here refers to container.node()
  })
    .data(nodeData, (d) => {
      // make sure that we don't have to swap shape type
      return d.node.name + ":" + d.node.type;
    });

  // ENTER
  nodeGroups.enter()
    .append("g")
    .attr("data-name", d => { return d.node.name; })
    .each(function(d) {
      let nodeGroup = d3.select(this);
      // index node group for quick stylizing
      sceneBehavior.addNodeGroup(d.node.name, nodeGroup);
    });

  // UPDATE
  nodeGroups
    .attr("class", d => {
      return Class.Node.GROUP + " " + nodeClass(d);
    })
    .each(function(d) {
      let nodeGroup = d3.select(this);
      // add g.in-annotations (always add -- to keep layer order consistent.)
      let inAnnotationBox = scene.selectOrCreateChild(nodeGroup, "g",
        Class.Annotation.INBOX);
      annotation.buildGroup(inAnnotationBox, d.inAnnotations, d,
        sceneBehavior);

      // add g.out-annotations  (always add -- to keep layer order consistent.)
      let outAnnotationBox = scene.selectOrCreateChild(nodeGroup, "g",
        Class.Annotation.OUTBOX);
      annotation.buildGroup(outAnnotationBox, d.outAnnotations, d,
        sceneBehavior);

      // label
      let label = labelBuild(nodeGroup, d, sceneBehavior);
      // Do not add interaction to metanode labels as they live inside the
      // metanode shape which already has the same interactions.
      addInteraction(label, d, sceneBehavior, d.node.type === NodeType.META);

      // build .shape below label
      let shape = buildShape(nodeGroup, d, Class.Node.SHAPE, label.node());
      if (d.node.isGroupNode) {
        addButton(shape, d, sceneBehavior);
      }
      addInteraction(shape, d, sceneBehavior);

      // build subscene on the top
      subsceneBuild(nodeGroup, <render.RenderGroupNodeInfo> d,
          sceneBehavior);

      stylize(nodeGroup, d, sceneBehavior);
      position(nodeGroup, d, sceneBehavior);
    });

  // EXIT
  nodeGroups.exit()
    .each(function(d) {
      // remove all indices on remove
      sceneBehavior.removeNodeGroup(d.node.name);

      let nodeGroup = d3.select(this);
      if (d.inAnnotations.list.length > 0) {
        nodeGroup.select("." + Class.Annotation.INBOX)
          .selectAll("." + Class.Annotation.GROUP)
          .each(a => {
            sceneBehavior.removeAnnotationGroup(a, d);
          });
      }
      if (d.outAnnotations.list.length > 0) {
        nodeGroup.select("." + Class.Annotation.OUTBOX)
          .selectAll("." + Class.Annotation.GROUP)
          .each(a => {
            sceneBehavior.removeAnnotationGroup(a, d);
          });
      }
    })
    .remove();
  return nodeGroups;
};

/**
 * Update or remove the subscene of a render group node depending on whether it
 * is a expanded. If the node is not a group node, this method has no effect.
 *
 * @param nodeGroup selection of the container
 * @param renderNodeInfo the render information for the node.
 * @param sceneBehavior parent scene module
 * @return Selection of the subscene group, or null if node group does not have
 *        a subscene. Op nodes, bridge nodes and unexpanded group nodes will
 *        not have a subscene.
 */
function subsceneBuild(nodeGroup,
    renderNodeInfo: render.RenderGroupNodeInfo, sceneBehavior) {
  if (renderNodeInfo.node.isGroupNode) {
    if (renderNodeInfo.expanded) {
      // Recursively build the subscene.
      return scene.buildGroup(nodeGroup, renderNodeInfo, sceneBehavior,
        Class.Subscene.GROUP);
    }
    // Clean out existing subscene if the node is not expanded.
    scene.selectChild(nodeGroup, "g", Class.Subscene.GROUP).remove();
  }
  return null;
};

/**
 * Translate the subscene of the given node group
 */
function subscenePosition(nodeGroup, d: render.RenderNodeInfo) {
  let x0 = d.x - d.width / 2.0 + d.paddingLeft;
  let y0 = d.y - d.height / 2.0 + d.paddingTop;

  let subscene = scene.selectChild(nodeGroup, "g", Class.Subscene.GROUP);
  scene.translate(subscene, x0, y0);
};

/**
 * Add an expand/collapse button to a group node
 *
 * @param selection The group node selection.
 * @param d Info about the node being rendered.
 * @param sceneBehavior parent scene module.
 */
function addButton(selection, d: render.RenderNodeInfo, sceneBehavior) {
  let group = scene.selectOrCreateChild(
    selection, "g", Class.Node.BUTTON_CONTAINER);
  scene.selectOrCreateChild(group, "circle", Class.Node.BUTTON_CIRCLE);
  scene.selectOrCreateChild(group, "path", Class.Node.EXPAND_BUTTON).attr(
    "d", "M0,-2.2 V2.2 M-2.2,0 H2.2");
  scene.selectOrCreateChild(group, "path", Class.Node.COLLAPSE_BUTTON).attr(
    "d", "M-2.2,0 H2.2");
  group.on("click", d => {
    // Stop this event's propagation so that it isn't also considered a
    // node-select.
    (<Event>d3.event).stopPropagation();
    sceneBehavior.fire("node-toggle-expand", { name: d.node.name });
  });
  scene.positionButton(group, d);
};

/**
 * Fire node-* events when the selection is interacted.
 *
 * @param disableInteraction When true, have the provided selection
 * ignore all pointer events. Used for text labels inside of metanodes, which
 * don't need interaction as their surrounding shape has interaction, and if
 * given interaction would cause conflicts with the expand/collapse button.
 */
function addInteraction(selection, d: render.RenderNodeInfo,
    sceneBehavior, disableInteraction?: boolean) {
  if (disableInteraction) {
    selection.attr("pointer-events", "none");
    return;
  }

  let contextMenuFunction = tf.graph.scene.contextmenu.getMenu(
    getContextMenu(d.node, sceneBehavior));
  selection.on("dblclick", d => {
    sceneBehavior.fire("node-toggle-expand", { name: d.node.name });
  })
    .on("mouseover", d => {
      // don't send mouseover over expanded group,
      // otherwise it is causing too much glitches
      if (sceneBehavior.isNodeExpanded(d)) { return; }

      sceneBehavior.fire("node-highlight", { name: d.node.name });
    })
    .on("mouseout", d => {
      // don't send mouseover over expanded group,
      // otherwise it is causing too much glitches
      if (sceneBehavior.isNodeExpanded(d)) { return; }

      sceneBehavior.fire("node-unhighlight", { name: d.node.name });
    })
    .on("click", d => {
      // Stop this event's propagation so that it isn't also considered
      // a graph-select.
      (<Event>d3.event).stopPropagation();
      sceneBehavior.fire("node-select", { name: d.node.name });
    })
    .on("contextmenu", (d, i) => {
      sceneBehavior.fire("node-select", { name: d.node.name });
      contextMenuFunction.call(d, i);
    });
};

/**
 * Returns the d3 context menu specification for the provided node.
 */
export function getContextMenu(node: Node, sceneBehavior) {
  let menu = [{
    title: d => {
      return tf.graph.getIncludeNodeButtonString(node.include);
    },
    action: (elm, d, i) => {
      sceneBehavior.fire("node-toggle-extract", { name: node.name });
    }
  }];
  if (canBeInSeries(node)) {
    menu.push({
      title: d => {
        return getGroupSettingLabel(node);
      },
      action: (elm, d, i) => {
        sceneBehavior.fire("node-toggle-seriesgroup",
          { name: getSeriesName(node) });
      }
    });
  }
  return menu;
}

/** Returns if a node can be part of a grouped series */
export function canBeInSeries(node: Node) {
  return getSeriesName(node) !== null;
}

/**
 * Returns the name of the possible grouped series containing this node.
 * Returns null if the node cannot be part of a grouped series of nodes.
 */
export function getSeriesName(node: Node) {
  if (!node) {
    return null;
  }
  if (node.type === NodeType.SERIES) {
    return node.name;
  }
  if (node.type === NodeType.OP) {
    let op = <OpNode>node;
    return op.owningSeries;
  }
  return null;
}

/**
 * Returns the SeriesNode that represents the series that the provided node
 * is contained in (or itself if the provided node is itself a SeriesNode).
 * Returns null if the node is not rendered as part of a series.
 */
function getContainingSeries(node: Node) {
  let s: SeriesNode = null;
  if (!node) {
    return null;
  } else if (node.type === NodeType.SERIES) {
    s = <SeriesNode>node;
  } else if (node.parentNode && node.parentNode.type === NodeType.SERIES) {
    s = <SeriesNode>node.parentNode;
  }
  return s;
}

/**
 * Returns the label for a button to toggle the group setting of the provided
 * node.
 */
export function getGroupSettingLabel(node: Node) {
  return tf.graph.getGroupSeriesNodeButtonString(
    getContainingSeries(node) !== null ? tf.graph.SeriesGroupingType.GROUP :
     tf.graph.SeriesGroupingType.UNGROUP);
}

/**
 * Append svg text for label and assign data.
 * @param nodeGroup
 * @param renderNodeInfo The render node information for the label.
 * @param sceneBehavior parent scene module.
 */
function labelBuild(nodeGroup, renderNodeInfo: render.RenderNodeInfo,
    sceneBehavior) {
  let namePath = renderNodeInfo.node.name.split("/");
  let text = namePath[namePath.length - 1];

  // Truncate long labels for unexpanded Metanodes.
  let useFontScale = renderNodeInfo.node.type === NodeType.META &&
    !renderNodeInfo.expanded;

  let label = scene.selectOrCreateChild(nodeGroup, "text", Class.Node.LABEL);
  label.attr("dy", ".35em")
    .attr("text-anchor", "middle");
  if (useFontScale) {
    if (text.length > sceneBehavior.maxMetanodeLabelLength) {
      text = text.substr(0, sceneBehavior.maxMetanodeLabelLength - 2) + "...";
    }
    let scale = getLabelFontScale(sceneBehavior);
    label.attr("font-size", scale(text.length) + "px");
  }
  label.text(text);
  return label;
};

/**
 * d3 scale used for sizing font of labels, used by labelBuild,
 * initialized once by getLabelFontScale.
 */
let fontScale = null;
function getLabelFontScale(sceneBehavior) {
  if (!fontScale) {
    fontScale = d3.scale.linear()
      .domain([sceneBehavior.maxMetanodeLabelLengthLargeFont,
        sceneBehavior.maxMetanodeLabelLength])
      .range([sceneBehavior.maxMetanodeLabelLengthFontSize,
        sceneBehavior.minMetanodeLabelLengthFontSize]).clamp(true);
  }
  return fontScale;
}

/**
 * Set label position of a given node group
 */
function labelPosition(nodeGroup, cx: number, cy: number,
    yOffset: number) {
  scene.selectChild(nodeGroup, "text", Class.Node.LABEL).transition()
    .attr("x", cx)
    .attr("y", cy + yOffset);
};

/**
 * Select or append/insert shape for a node and assign renderNode
 * as the shape's data.
 *
 * @param nodeGroup
 * @param d Render node information.
 * @param nodeClass class for the element.
 * @param before Reference DOM node for insertion.
 * @return Selection of the shape.
 */
export function buildShape(nodeGroup, d, nodeClass: string, before?) {
  // Create a group to house the underlying visual elements.
  let shapeGroup = scene.selectOrCreateChild(nodeGroup, "g", nodeClass,
    before);
  // TODO(jimbo): DOM structure should be templated in HTML somewhere, not JS.
  switch (d.node.type) {
    case NodeType.OP:
      scene.selectOrCreateChild(shapeGroup, "ellipse",
        Class.Node.COLOR_TARGET);
      break;
    case NodeType.SERIES:
      // Choose the correct stamp to use to represent this series.
      let stampType = "annotation";
      let groupNodeInfo = <render.RenderGroupNodeInfo>d;
      if (groupNodeInfo.coreGraph) {
        stampType = groupNodeInfo.node.hasNonControlEdges
          ? "vertical" : "horizontal";
      }
      scene.selectOrCreateChild(shapeGroup, "use", Class.Node.COLOR_TARGET)
        .attr("xlink:href", "#op-series-" + stampType + "-stamp");
      scene.selectOrCreateChild(shapeGroup, "rect", Class.Node.COLOR_TARGET)
        .attr({ rx: d.radius, ry: d.radius });
      break;
    case NodeType.BRIDGE:
      scene.selectOrCreateChild(shapeGroup, "rect", Class.Node.COLOR_TARGET)
        .attr({ rx: d.radius, ry: d.radius });
      break;
    case NodeType.META:
      scene.selectOrCreateChild(shapeGroup, "rect", Class.Node.COLOR_TARGET)
        .attr({ rx: d.radius, ry: d.radius });
      break;
    default:
      throw Error("Unrecognized node type: " + d.node.type);
  }
  return shapeGroup;
};

export function nodeClass(d: render.RenderNodeInfo) {
  switch (d.node.type) {
    case NodeType.OP:
      return Class.OPNODE;
    case NodeType.META:
      return Class.METANODE;
    case NodeType.SERIES:
      return Class.SERIESNODE;
    case NodeType.BRIDGE:
      return Class.BRIDGENODE;
    case NodeType.ELLIPSIS:
      return Class.ELLIPSISNODE;
  };
  throw Error("Unrecognized node type: " + d.node.type);
};

/** Modify node and its subscene and its label's positional attributes */
function position(nodeGroup, d: render.RenderNodeInfo, sceneBehavior) {
  let shapeGroup = scene.selectChild(nodeGroup, "g", Class.Node.SHAPE);
  let cx = layout.computeCXPositionOfNodeShape(d);
  switch (d.node.type) {
    case NodeType.OP: {
      // position shape
      let shape = scene.selectChild(shapeGroup, "ellipse");
      scene.positionEllipse(shape, cx, d.y, d.coreBox.width, d.coreBox.height);
      labelPosition(nodeGroup, cx, d.y, d.labelOffset);
      break;
    }
    case NodeType.META: {
      // position shape
      let shape = scene.selectChild(shapeGroup, "rect");
      if (d.expanded) {
        scene.positionRect(shape, d.x, d.y, d.width, d.height);
        subscenePosition(nodeGroup, d);
        // put label on top
        labelPosition(nodeGroup, cx, d.y,
          - d.height / 2 + d.labelHeight / 2);
      } else {
        scene.positionRect(shape, cx, d.y, d.coreBox.width, d.coreBox.height);
        labelPosition(nodeGroup, cx, d.y, 0);
      }
      break;
    }
    case NodeType.SERIES: {
      let shape = scene.selectChild(shapeGroup, "use");
      if (d.expanded) {
        scene.positionRect(shape, d.x, d.y, d.width, d.height);
        subscenePosition(nodeGroup, d);
        // put label on top
        labelPosition(nodeGroup, cx, d.y,
          - d.height / 2 + d.labelHeight / 2);
      } else {
        scene.positionRect(shape, cx, d.y, d.coreBox.width, d.coreBox.height);
        labelPosition(nodeGroup, cx, d.y, d.labelOffset);
      }
    }
    case NodeType.BRIDGE: {
      // position shape
      // NOTE: In reality, these will not be visible, but it helps to put them
      // in the correct position for debugging purposes.
      let shape = scene.selectChild(shapeGroup, "rect");
      scene.positionRect(shape, d.x, d.y, d.width, d.height);
      break;
    }
    default: {
      throw Error("Unrecognized node type: " + d.node.type);
    }
  }
};

/** Enum specifying the options to color nodes by */
export enum ColorBy { STRUCTURE, DEVICE, COMPUTE_TIME, MEMORY };

/**
 * Returns the fill color for the node given its state and the "color by"
 * option.
 */
export function getFillForNode(templateIndex, colorBy,
    renderInfo: render.RenderNodeInfo, isExpanded: boolean): string {
  let colorParams = tf.graph.render.MetanodeColors;
  switch (colorBy) {
    case ColorBy.STRUCTURE:
      if (renderInfo.node.type === tf.graph.NodeType.META) {
        let tid = (<Metanode>renderInfo.node).templateId;
        return tid === null ?
          colorParams.UNKNOWN :
          colorParams.STRUCTURE_PALETTE(templateIndex(tid), isExpanded);
      } else if (renderInfo.node.type === tf.graph.NodeType.SERIES) {
        // If expanded, we're showing the background rect, which we want to
        // appear gray. Otherwise we're showing a stack of ellipses which we
        // want to show white.
        return isExpanded ? colorParams.EXPANDED_COLOR : "white";
      } else if (renderInfo.node.type === NodeType.BRIDGE) {
        return renderInfo.structural ? "#f0e" :
          (<BridgeNode>renderInfo.node).inbound ? "#0ef" : "#fe0";
      } else {
        // Op nodes are white.
        return "white";
      }
    case ColorBy.DEVICE:
      if (renderInfo.deviceColors == null) {
        // Return the hue for unknown device.
        return colorParams.UNKNOWN;
      }
      let id = renderInfo.node.name;
      let escapedId = tf.escapeQuerySelector(id);
      let gradientDefs = d3.select("svg#svg defs #linearGradients");
      let linearGradient =
        gradientDefs.select("linearGradient#" + escapedId);
      // If the linear gradient is not there yet, create it.
      if (linearGradient.size() === 0) {
        linearGradient = gradientDefs.append("linearGradient").attr("id", id);
        // Re-create the stops of the linear gradient.
        linearGradient.selectAll("*").remove();
        let cumulativeProportion = 0;
        // For each device, create a stop using the proportion of that device.
        _.each(renderInfo.deviceColors, d => {
          let color = d.color;
          linearGradient.append("stop")
            .attr("offset", cumulativeProportion)
            .attr("stop-color", color);
          linearGradient.append("stop")
            .attr("offset", cumulativeProportion + d.proportion)
            .attr("stop-color", color);
          cumulativeProportion += d.proportion;
        });
      }
      return isExpanded ? colorParams.EXPANDED_COLOR : `url(#${escapedId})`;
    case ColorBy.COMPUTE_TIME:
      return isExpanded ?
        colorParams.EXPANDED_COLOR : renderInfo.computeTimeColor ||
        colorParams.UNKNOWN;
    case ColorBy.MEMORY:
      return isExpanded ?
        colorParams.EXPANDED_COLOR : renderInfo.memoryColor ||
        colorParams.UNKNOWN;
    default:
      throw new Error("Unknown case to color nodes by");
  }
}

/**
 * Modify node style by toggling class and assign attributes (only for things
 * that can't be done in css).
 */
export function stylize(nodeGroup, renderInfo: render.RenderNodeInfo,
    sceneBehavior, nodeClass?) {
  nodeClass = nodeClass || Class.Node.SHAPE;
  let isHighlighted = sceneBehavior.isNodeHighlighted(renderInfo.node.name);
  let isSelected = sceneBehavior.isNodeSelected(renderInfo.node.name);
  let isExtract = renderInfo.isInExtract || renderInfo.isOutExtract;
  let isExpanded = renderInfo.expanded;
  nodeGroup.classed("highlighted", isHighlighted);
  nodeGroup.classed("selected", isSelected);
  nodeGroup.classed("extract", isExtract);
  nodeGroup.classed("expanded", isExpanded);

  // Main node always exists here and it will be reached before subscene,
  // so d3 selection is fine here.
  let node = nodeGroup.select("." + nodeClass + " ." + Class.Node.COLOR_TARGET);
  let fillColor = getFillForNode(sceneBehavior.templateIndex,
    ColorBy[sceneBehavior.colorBy.toUpperCase()],
    renderInfo, isExpanded);
  node.style("fill", fillColor);

  // Choose outline to be darker version of node color if the node is a single
  // color and is not selected.
  node.style("stroke", isSelected ? null : getStrokeForFill(fillColor));
};

/**
 * Given a node's fill color/gradient, determine the stroke for the node.
 */
export function getStrokeForFill(fill: string) {
  // If node is colored by a gradient, then use a dark gray outline.
  return fill.substring(0, 3) === "url" ?
    tf.graph.render.MetanodeColors.GRADIENT_OUTLINE :
    d3.rgb(fill).darker().toString();
}

} // close module
