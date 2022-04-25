/*******************************************************************************
 * Copyright (C) 2021-2022 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

#include <iomanip>

#include "absl/synchronization/mutex.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/framework/node_def.pb.h"
#include "tensorflow/core/grappler/clusters/cluster.h"
#include "tensorflow/core/grappler/grappler_item.h"
#include "tensorflow/core/grappler/op_types.h"
#include "tensorflow/core/grappler/optimizers/custom_graph_optimizer_registry.h"
#include "tensorflow/core/grappler/utils.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/grappler/costs/analytical_cost_estimator.h"
#include "tensorflow/core/grappler/clusters/virtual_cluster.h"

#include "openvino_tensorflow/api.h"
#include "openvino_tensorflow/backend_manager.h"
#include "openvino_tensorflow/cluster_manager.h"
#include "openvino_tensorflow/grappler/ovtf_optimizer.h"

#include "ocm/include/ocm_nodes_checker.h"

#include <iostream>

using namespace std;

namespace tensorflow {
namespace openvino_tensorflow {

Status OVTFOptimizer::Init(
    const tensorflow::RewriterConfig_CustomGraphOptimizer* config) {
  const auto params = config->parameter_map();
  for (auto i : params) {
    m_config_map["_ovtf_" + i.first] = i.second.s();
    OVTF_VLOG(3) << "Attribute: " << i.first
                 << " Value: " << m_config_map["_ovtf_" + i.first];
  }
  return Status::OK();
}

Status OVTFOptimizer::Optimize(tensorflow::grappler::Cluster* cluster,
                               const tensorflow::grappler::GrapplerItem& item,
                               GraphDef* output) {
  OVTF_VLOG(5) << "OVTF_OPTIMIZER: grappler item id " << item.id;

  // Convert the GraphDef to Graph
  GraphConstructorOptions opts;
  opts.allow_internal_ops = true;
  opts.expect_device_spec = false;

  grappler::GrapplerItem grappler_item(item);
  GraphDef& graph_def = grappler_item.graph;  
  FunctionLibraryDefinition flib(OpRegistry::Global(), graph_def.library());
  Graph graph(flib);
  TF_RETURN_IF_ERROR(ConvertGraphDefToGraph(opts, graph_def, &graph));
  OVTF_VLOG(5) << "OVTF_OPTIMIZER: Successfully converted GraphDef to Graph";

  std::unique_ptr<grappler::OpLevelCostEstimator> node_estimator = absl::make_unique<grappler::OpLevelCostEstimator>();
  std::unique_ptr<grappler::ReadyNodeManager> node_manager = grappler::ReadyNodeManagerFactory("FirstReady");
  std::unique_ptr<grappler::AnalyticalCostEstimator> estimator = absl::make_unique<grappler::AnalyticalCostEstimator>(
      cluster, std::move(node_estimator), std::move(node_manager),
      /*use_static_shapes=*/true, /*use_aggressive_shape_inference=*/true);
  OVTF_VLOG(0) << "openvino-tensorflow is using analytical cost estimator.";
  tensorflow::Status init_status = estimator->Initialize(grappler_item);
  OVTF_VLOG(0) << "openvino-tensorflow is initializing estimator.";
  if (!init_status.ok()) return init_status;

  tensorflow::RunMetadata run_metadata;
  grappler::Costs costs;
  estimator->PredictCosts(grappler_item.graph, &run_metadata, &costs);
  OVTF_VLOG(0) << "openvino-tensorflow is predicting costs.";

  // For filename generation purposes, grab a fresh index. This is just an
  // arbitrary integer to avoid filename collisions resulting from subsequent
  // runs of this pass.
  int idx = FreshIndex();

  // If openvino-tensorflow is disabled via python disable api or
  // OPENVINO_TF_DISABLE is
  // set
  // we will not do anything; all subsequent passes become a no-op.
  bool ovtf_not_enabled = false;
  const char* openvino_tf_disable_env = std::getenv("OPENVINO_TF_DISABLE");
  if (!(openvino_tf_disable_env == nullptr)) {
    // // disable openvino-tensorflow if env variable is "1"
    char env_value = openvino_tf_disable_env[0];
    if (env_value == '1') {
      ovtf_not_enabled = true;
    }
  }
  ovtf_not_enabled = (!api::IsEnabled() || ovtf_not_enabled);
  bool already_processed = util::IsAlreadyProcessed(&graph);
  if (!already_processed && ovtf_not_enabled) {
    OVTF_VLOG(0) << "openvino_tensorflow is available but disabled.";
  }
  if (ovtf_not_enabled || already_processed) {
    OVTF_VLOG(1) << std::string("Rewrite pass will not run because ") +
                        (already_processed ? "graph is already preprocessed"
                                           : "openvino_tensorflow is disabled");
    NGraphClusterManager::EvictAllClusters();
    NGraphClusterManager::EvictMRUClusters();
    graph.ToGraphDef(output);
    return Status::OK();
  }

  // TODO: Find out a better way to preserve feed nodes, init_ops and
  // keep_ops instead of just skipping those from clustering.
  // Get nodes to be preserved/skipped
  std::set<string> nodes_to_preserve;

  // Feed Nodes
  for (size_t i = 0; i < item.feed.size(); i++) {
    nodes_to_preserve.insert(item.feed[i].first);
  }

  // Keep Ops
  nodes_to_preserve.insert(item.keep_ops.begin(), item.keep_ops.end());

  // Init Ops
  nodes_to_preserve.insert(item.init_ops.begin(), item.init_ops.end());

  // Find a list of nodes that are of the types that are disabled
  std::set<string> disabled_nodes;
  std::set<string> disabled_ops_set = api::GetDisabledOps();
  for (auto itr : graph.nodes()) {
    if (disabled_ops_set.find(itr->type_string()) != disabled_ops_set.end()) {
      disabled_nodes.insert(itr->name());
    }
  }

  // Fetch Nodes
  std::set<string> fetch_nodes;
  for (const string& f : item.fetch) {
    int pos = f.find(":");
    fetch_nodes.insert(f.substr(0, pos));
  }

  // nodes_to_add_identity_to = fetch_nodes - disabled_nodes
  std::set<string> nodes_to_add_identity_to;
  std::set_difference(fetch_nodes.begin(), fetch_nodes.end(),
                      disabled_nodes.begin(), disabled_nodes.end(),
                      std::inserter(nodes_to_add_identity_to,
                                    nodes_to_add_identity_to.begin()));

  // Rewrite graph to add IdentityN node so the fetch node can be encapsulated
  // as well
  // If the fetch node in question has 0 outputs or any of the outputs
  // has ref type as a data type then don't add IdentityN node, but the fetch
  // node will be skipped from capturing and marking for clustering.
  TF_RETURN_IF_ERROR(AddIdentityN(&graph, nodes_to_add_identity_to));

  nodes_to_preserve.insert(nodes_to_add_identity_to.begin(),
                           nodes_to_add_identity_to.end());

  //
  // Encapsulation: Part that rewrites the graph for nGraph operation.
  //
  // The part has several phases, each executed in sequence:
  //
  //   1. Marking [mark_for_clustering.cc]
  //   2. Cluster Assignment [assign_clusters.cc]
  //   3. Cluster Deassignment [deassign_clusters.cc]
  //   4. Cluster Encapsulation [encapsulate_clusters.cc] - currently
  //      part of the rewrite_pass.cc to be executed after POST_REWRITE
  //

  // If requested, dump unmarked graphs.
  util::DumpTFGraph(&graph, idx, "unmarked");

  // 1. Mark for clustering then, if requested, dump the graphs.
  // OCM call for marking supported nodes
  std::string device;
  Status exec_status = BackendManager::GetBackendName(device);
  if (exec_status != Status::OK()) {
    throw runtime_error(exec_status.error_message());
  }
  const char* device_id(device.c_str());
  std::string ov_version;
#if defined(OPENVINO_2022_1)
  ov_version = "2022.1.0";
#endif

  ocm::Framework_Names fName = ocm::Framework_Names::TF;
  ocm::FrameworkNodesChecker FC(fName, device_id, ov_version, &graph);
  FC.SetDisabledOps(api::GetDisabledOps());
  std::vector<void*> nodes_list = FC.MarkSupportedNodes();

  // cast back the nodes in the TF format and mark the nodes for clustering
  // (moved out from MarkForClustering function)
  const std::map<std::string, SetAttributesFunction>& set_attributes_map =
      GetAttributeSetters();
  for (auto void_node : nodes_list) {
    // TODO(amprocte): move attr name to a constant
    tensorflow::Node* node = (tensorflow::Node*)void_node;
    node->AddAttr("_ovtf_marked_for_clustering", true);
    auto it = set_attributes_map.find(node->type_string());
    if (it != set_attributes_map.end()) {
      it->second(node);
    }
  }
  util::DumpTFGraph(&graph, idx, "marked");

  // 2. Assign clusters then, if requested, dump the graphs.
  TF_RETURN_IF_ERROR(AssignClusters(&graph));
  util::DumpTFGraph(&graph, idx, "clustered");

  // 3. Deassign trivial clusters then, if requested, dump the graphs.
  TF_RETURN_IF_ERROR(DeassignClusters(&graph));
  util::DumpTFGraph(&graph, idx, "declustered");

  // 4. Encapsulate clusters then, if requested, dump the graphs.
  auto status = EncapsulateClusters(&graph, idx, m_config_map);
  if (status != Status::OK()) {
    return status;
  }
  util::DumpTFGraph(&graph, idx, "encapsulated");

  // Convert the graph back to Graphdef
  graph.ToGraphDef(output);
  return Status::OK();
}

int OVTFOptimizer::FreshIndex() {
  mutex_lock l(s_serial_counter_mutex);
  return s_serial_counter++;
}
  
class VerboseCustomGraphOptimizerRegistrar
    : public grappler::CustomGraphOptimizerRegistrar {
 public:
  VerboseCustomGraphOptimizerRegistrar(
      const grappler::CustomGraphOptimizerRegistry::Creator& cr,
      const string& name)
      : grappler::CustomGraphOptimizerRegistrar(cr, name) {
    VLOG(1) << "Constructing a CustomOptimizationPass registration object for "
            << name;
  }
};

static VerboseCustomGraphOptimizerRegistrar OVTFOptimizationPass_Registrar(
    []() {
      VLOG(1)
          << "Instantiating CustomOptimizationPass object ovtf-optimizer";
      return new OVTFOptimizer;
    },
    ("ovtf-optimizer"));

}  // end namespace openvino_tensorflow
}  // end namespace tensorflow
