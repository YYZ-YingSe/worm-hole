// Defines compile-time compose contract checks that run after lowering.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "wh/compose/graph/graph.hpp"

namespace wh::compose {

namespace detail {

struct node_gate_state {
  input_gate input{};
  output_gate declared_output{};
  output_gate resolved_output{};
};

[[nodiscard]] inline auto adapter_name(const edge_adapter_kind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case edge_adapter_kind::none:
    return "none";
  case edge_adapter_kind::value_to_stream:
    return "value_to_stream";
  case edge_adapter_kind::stream_to_value:
    return "stream_to_value";
  case edge_adapter_kind::custom:
    return "custom";
  }
  return "none";
}

[[nodiscard]] inline auto gate_text(const input_gate &gate) -> std::string {
  std::string text{gate_name(gate.kind)};
  if (gate.kind == input_gate_kind::value_exact && !gate.value.empty()) {
    text += "<";
    text += gate.value.name();
    text += ">";
  }
  return text;
}

[[nodiscard]] inline auto gate_text(const output_gate &gate) -> std::string {
  std::string text{gate_name(gate.kind)};
  if (gate.kind == output_gate_kind::value_exact && !gate.value.empty()) {
    text += "<";
    text += gate.value.name();
    text += ">";
  }
  return text;
}

[[nodiscard]] inline auto same_value_gate(const value_gate &lhs,
                                          const value_gate &rhs) noexcept
    -> bool {
  return lhs.key() == rhs.key();
}

[[nodiscard]] inline auto lifted_value_gate(const auto &edge,
                                            const output_gate &source) noexcept
    -> output_gate {
  switch (edge.adapter.kind) {
  case edge_adapter_kind::none:
    if (source.kind == output_gate_kind::value_exact) {
      return source;
    }
    return output_gate::dynamic();
  case edge_adapter_kind::stream_to_value:
    if (source.kind == output_gate_kind::reader) {
      return output_gate::exact<std::vector<graph_value>>();
    }
    return output_gate::dynamic();
  case edge_adapter_kind::custom:
    return output_gate::dynamic();
  case edge_adapter_kind::value_to_stream:
    return output_gate::dynamic();
  }
  return output_gate::dynamic();
}

[[nodiscard]] inline auto compatible_value_edge(const auto &edge,
                                                const output_gate &source,
                                                const input_gate &target) noexcept
    -> bool {
  if (target.kind == input_gate_kind::value_open) {
    return true;
  }
  if (target.kind == input_gate_kind::reader) {
    return source.kind == output_gate_kind::reader ||
           source.kind == output_gate_kind::value_exact ||
           source.kind == output_gate_kind::value_dynamic;
  }

  if (source.kind == output_gate_kind::reader) {
    if (edge.adapter.kind == edge_adapter_kind::stream_to_value) {
      return target.value.key() ==
             wh::core::any_type_key_v<std::vector<graph_value>>;
    }
    return edge.adapter.kind == edge_adapter_kind::custom;
  }

  if (source.kind == output_gate_kind::value_dynamic) {
    return true;
  }

  if (source.kind == output_gate_kind::value_exact) {
    return same_value_gate(source.value, target.value);
  }

  return true;
}

[[nodiscard]] inline auto incompatible_edge_message(
    const std::string_view source_key, const std::string_view target_key,
    const auto &edge, const output_gate &source, const input_gate &target)
    -> std::string {
  std::string message{"edge contract impossible: "};
  message += std::string{source_key};
  message += " -> ";
  message += std::string{target_key};
  message += " adapter=";
  message += adapter_name(edge.adapter.kind);
  message += " source=";
  message += gate_text(source);
  message += " target=";
  message += gate_text(target);
  return message;
}

} // namespace detail

inline auto graph::validate_contracts() -> wh::core::result<void> {
  auto &index = compiled_execution_index_.index;
  const auto node_count = index.id_to_key.size();
  std::vector<detail::node_gate_state> gates(node_count);

  for (std::uint32_t node_id = 0U; node_id < static_cast<std::uint32_t>(node_count);
       ++node_id) {
    const auto &node = nodes_.at(index.id_to_key[node_id]);
    gates[node_id].input = detail::authored_input_gate(node);
    gates[node_id].declared_output = detail::authored_output_gate(node);
    if (gates[node_id].declared_output.kind ==
        output_gate_kind::value_passthrough) {
      gates[node_id].resolved_output = output_gate::dynamic();
    } else {
      gates[node_id].resolved_output = gates[node_id].declared_output;
    }
  }

  for (const auto &key : compile_order_) {
    const auto node_id_iter = index.key_to_id.find(key);
    if (node_id_iter == index.key_to_id.end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    const auto node_id = node_id_iter->second;
    if (gates[node_id].declared_output.kind !=
        output_gate_kind::value_passthrough) {
      continue;
    }

    const auto &value_edges = compiled_execution_index_.plan.inputs[node_id].value_edges;
    if (value_edges.size() != 1U) {
      gates[node_id].resolved_output = output_gate::dynamic();
      continue;
    }

    const auto edge_id = value_edges.front();
    const auto &edge = index.indexed_edges[edge_id];
    gates[node_id].resolved_output = detail::lifted_value_gate(
        edge, gates[edge.from].resolved_output);
  }

  for (const auto &edge : index.indexed_edges) {
    if (edge.no_data) {
      continue;
    }
    const auto &source_gate = gates[edge.from].resolved_output;
    const auto &target_gate = gates[edge.to].input;
    if (detail::compatible_value_edge(edge, source_gate, target_gate)) {
      continue;
    }
    return fail_fast(
        wh::core::errc::contract_violation,
        detail::incompatible_edge_message(index.id_to_key[edge.from],
                                          index.id_to_key[edge.to], edge,
                                          source_gate, target_gate));
  }

  return {};
}

} // namespace wh::compose
