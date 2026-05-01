// Defines compile-time compose contract checks that run after lowering.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/graph_class.hpp"

namespace wh::compose {

namespace detail {

struct node_gate_state {
  input_gate input{};
  output_gate declared_output{};
  output_gate resolved_output{};
};

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

[[nodiscard]] inline auto same_value_gate(const value_gate &lhs, const value_gate &rhs) noexcept
    -> bool {
  return lhs.key() == rhs.key();
}

[[nodiscard]] inline auto lifted_value_gate(const auto &edge, const output_gate &source) noexcept
    -> output_gate {
  switch (edge.lowering_kind) {
  case edge_lowering_kind::none:
    if (source.kind == output_gate_kind::value_exact) {
      return source;
    }
    return output_gate::dynamic();
  case edge_lowering_kind::stream_to_value:
    if (source.kind == output_gate_kind::reader) {
      return output_gate::exact<std::vector<graph_value>>();
    }
    return output_gate::dynamic();
  case edge_lowering_kind::custom:
    return output_gate::dynamic();
  case edge_lowering_kind::value_to_stream:
    return output_gate::dynamic();
  }
  return output_gate::dynamic();
}

[[nodiscard]] inline auto compatible_value_edge(const auto &edge, const output_gate &source,
                                                const input_gate &target) noexcept -> bool {
  if (target.kind == input_gate_kind::value_open) {
    return true;
  }
  if (target.kind == input_gate_kind::reader) {
    return source.kind == output_gate_kind::reader ||
           source.kind == output_gate_kind::value_exact ||
           source.kind == output_gate_kind::value_dynamic;
  }

  if (source.kind == output_gate_kind::reader) {
    if (edge.lowering_kind == edge_lowering_kind::stream_to_value) {
      return target.value.key() == wh::core::any_type_key_v<std::vector<graph_value>>;
    }
    return edge.lowering_kind == edge_lowering_kind::custom;
  }

  if (source.kind == output_gate_kind::value_dynamic) {
    return true;
  }

  if (source.kind == output_gate_kind::value_exact) {
    return same_value_gate(source.value, target.value);
  }

  return true;
}

[[nodiscard]] inline auto incompatible_edge_message(const std::string_view source_key,
                                                    const std::string_view target_key,
                                                    const auto &edge, const output_gate &source,
                                                    const input_gate &target) -> std::string {
  std::string message{"edge contract impossible: "};
  message += std::string{source_key};
  message += " -> ";
  message += std::string{target_key};
  message += " lowering=";
  message += to_string(edge.lowering_kind);
  message += " source=";
  message += gate_text(source);
  message += " target=";
  message += gate_text(target);
  return message;
}

[[nodiscard]] inline auto compiled_value_fan_in_gate(const auto &index,
                                                     const std::vector<node_gate_state> &gates,
                                                     const std::uint32_t node_id) noexcept
    -> std::optional<output_gate> {
  const auto incoming = index.incoming_data(node_id);
  if (incoming.empty()) {
    return std::nullopt;
  }
  if (incoming.size() != 1U) {
    return output_gate::exact<graph_value_map>();
  }

  const auto edge_id = incoming.front();
  const auto &edge = index.indexed_edges[edge_id];
  return lifted_value_gate(edge, gates[edge.from].resolved_output);
}

[[nodiscard]] inline auto
incompatible_value_fan_in_message(const std::string_view node_key, const output_gate &actual,
                                  const input_gate &target, const std::size_t incoming_count)
    -> std::string {
  std::string message{"value fan-in impossible: node="};
  message += std::string{node_key};
  message += " incoming=";
  message += std::to_string(incoming_count);
  message += " actual=";
  message += gate_text(actual);
  message += " target=";
  message += gate_text(target);
  return message;
}

[[nodiscard]] inline auto accepts_fan_in_value_map(const auto &index, const input_gate &target,
                                                   const std::uint32_t node_id) noexcept -> bool {
  if (target.kind != input_gate_kind::value_exact ||
      target.value.key() != wh::core::any_type_key_v<graph_value_map>) {
    return false;
  }
  return index.incoming_data(node_id).size() > 1U;
}

} // namespace detail

inline auto graph::validate_contracts() -> wh::core::result<void> {
  auto &index = core().compiled_execution_index_.index;
  auto &boundary_input_gate = core().boundary_input_gate_;
  auto &boundary_output_gate = core().boundary_output_gate_;
  const auto &compile_order = core().compile_order_;
  const auto node_count = index.id_to_key.size();
  std::vector<detail::node_gate_state> gates(node_count);
  boundary_input_gate = core().options_.boundary.input == node_contract::stream
                            ? input_gate::reader()
                            : input_gate::open();
  boundary_output_gate = core().options_.boundary.output == node_contract::stream
                             ? output_gate::reader()
                             : output_gate::dynamic();

  for (std::uint32_t node_id = 0U; node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    const auto &node = core().nodes_.at(index.id_to_key[node_id]);
    gates[node_id].input = detail::authored_input_gate(node);
    gates[node_id].declared_output = detail::authored_output_gate(node);
    if (node_id < core().compiled_nodes_.size()) {
      const auto &meta = core().compiled_nodes_[node_id].meta;
      if (meta.compiled_input_gate.has_value()) {
        gates[node_id].input = *meta.compiled_input_gate;
      }
      if (meta.compiled_output_gate.has_value()) {
        gates[node_id].declared_output = *meta.compiled_output_gate;
      }
    }
    if (gates[node_id].declared_output.kind == output_gate_kind::value_passthrough) {
      gates[node_id].resolved_output = output_gate::dynamic();
    } else {
      gates[node_id].resolved_output = gates[node_id].declared_output;
    }
  }

  for (const auto &key : compile_order) {
    const auto node_id_iter = index.key_to_id.find(key);
    if (node_id_iter == index.key_to_id.end()) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    const auto node_id = node_id_iter->second;
    if (gates[node_id].declared_output.kind != output_gate_kind::value_passthrough) {
      continue;
    }

    const auto &value_edges = core().compiled_execution_index_.plan.inputs[node_id].value_edges;
    if (value_edges.size() != 1U) {
      gates[node_id].resolved_output = output_gate::dynamic();
      continue;
    }

    const auto edge_id = value_edges.front();
    const auto &edge = index.indexed_edges[edge_id];
    gates[node_id].resolved_output =
        detail::lifted_value_gate(edge, gates[edge.from].resolved_output);
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
    if (detail::accepts_fan_in_value_map(index, target_gate, edge.to)) {
      continue;
    }
    return fail_fast(wh::core::errc::contract_violation,
                     detail::incompatible_edge_message(index.id_to_key[edge.from],
                                                       index.id_to_key[edge.to], edge, source_gate,
                                                       target_gate));
  }

  for (std::uint32_t node_id = 0U; node_id < static_cast<std::uint32_t>(node_count); ++node_id) {
    const auto &target_gate = gates[node_id].input;
    if (target_gate.kind != input_gate_kind::value_exact) {
      continue;
    }

    auto actual_gate = detail::compiled_value_fan_in_gate(index, gates, node_id);
    if (!actual_gate.has_value()) {
      continue;
    }
    if (actual_gate->kind != output_gate_kind::value_exact) {
      continue;
    }
    if (detail::same_value_gate(actual_gate->value, target_gate.value)) {
      continue;
    }
    return fail_fast(wh::core::errc::contract_violation,
                     detail::incompatible_value_fan_in_message(
                         index.id_to_key[node_id], *actual_gate, target_gate,
                         index.incoming_data(node_id).size()));
  }

  if (core().options_.boundary.input == node_contract::value) {
    const auto start_edges = index.outgoing_data(index.start_id);
    if (start_edges.size() == 1U) {
      const auto &edge = index.indexed_edges[start_edges.front()];
      if (edge.lowering_kind == edge_lowering_kind::none &&
          gates[edge.to].input.kind == input_gate_kind::value_exact) {
        boundary_input_gate = gates[edge.to].input;
      }
    }
  }

  if (core().options_.boundary.output == node_contract::value) {
    auto end_gate = detail::compiled_value_fan_in_gate(index, gates, index.end_id);
    if (end_gate.has_value()) {
      boundary_output_gate = *end_gate;
    }
  }

  return {};
}

} // namespace wh::compose
