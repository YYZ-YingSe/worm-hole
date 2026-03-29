// Defines authoring options for the multi-agent host flow.
#pragma once

#include <string>

namespace wh::flow::agent::multiagent::host {

/// Frozen authoring options for one host flow shell.
struct host_options {
  /// Stable exported graph name.
  std::string graph_name{"multiagent_host_graph"};
  /// Stable host route-node name.
  std::string host_node_name{"host_route"};
  /// Stable summarize-node name.
  std::string summarize_node_name{"host_summarize"};
};

} // namespace wh::flow::agent::multiagent::host
