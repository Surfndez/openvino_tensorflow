/*****************************************************************************
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
*****************************************************************************/

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <ie_core.hpp>
#include "ngraph/ngraph.hpp"
#include <tensorflow_frontend/graph_iterator.hpp>
#include <tensorflow_frontend/decoder.hpp>
#include "openvino_tensorflow/ovtf_decoder.h"

using namespace std;

namespace tensorflow {
namespace openvino_tensorflow {

// A Inference Engine executable object produced by compiling an nGraph
// function.
class OVTFGraphIterator : public ov::frontend::GraphIterator {
public:
    OVTFGraphIterator(const ::tensorflow::GraphDef *graph_def) : m_graph_def(graph_def) {
        m_nodes.resize(m_graph_def->node_size());
        for (size_t i = 0; i < m_nodes.size(); ++i)
            m_nodes[i] = &m_graph_def->node(i);
    }

    /// Set iterator to the start position
    void reset() override {
      node_index = 0;
    }

    size_t size() const override {
      return m_nodes.size();
    }

    /// Moves to the next node in the graph
    void next() override {
      node_index++;
    }

    bool is_end() const override {
      return node_index >= m_nodes.size();
    }

    /// Return NodeContext for the current node that iterator points to
    std::shared_ptr<ov::frontend::DecoderBase> get_decoder() const override {
        return std::make_shared<OVTFDecoder>(m_nodes[node_index]);
    }

private:
    std::vector<const ::tensorflow::NodeDef*> m_nodes;
    size_t node_index = 0;
    const ::tensorflow::GraphDef *m_graph_def;
};
}  // namespace openvino_tensorflow
}  // namespace tensorflow
