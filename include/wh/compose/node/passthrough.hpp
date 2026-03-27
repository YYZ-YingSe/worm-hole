// Defines passthrough graph-node helpers that forward payload unchanged.
#pragma once

#include <concepts>
#include <string>
#include <utility>

#include "wh/compose/node/authored.hpp"

namespace wh::compose {

inline auto passthrough_node::compile() const & -> compiled_node {
  auto copied = *this;
  return std::move(copied).compile();
}

inline auto passthrough_node::compile() && -> compiled_node {
  return make_compiled_sync_node(
      node_kind::passthrough, default_exec_origin(node_kind::passthrough),
      descriptor_.input_contract, descriptor_.output_contract,
      std::move(descriptor_.key),
      [](graph_value &input, wh::core::run_context &,
         const node_runtime &) -> wh::core::result<graph_value> {
        return std::move(input);
      },
      std::move(options_));
}

template <node_contract Contract = node_contract::value, typename key_t>
  requires std::constructible_from<std::string, key_t &&>
/// Creates one passthrough node by key.
[[nodiscard]] inline auto make_passthrough_node(key_t &&key) -> passthrough_node {
  auto descriptor = node_descriptor{
      .key = std::string{std::forward<key_t>(key)},
      .kind = node_kind::passthrough,
      .exec_mode = node_exec_mode::sync,
      .exec_origin = default_exec_origin(node_kind::passthrough),
      .input_contract = Contract,
      .output_contract = Contract,
      .input_gate_info = Contract == node_contract::value
                             ? input_gate::open()
                             : input_gate::reader(),
      .output_gate_info = Contract == node_contract::value
                              ? output_gate::passthrough()
                              : output_gate::reader(),
  };
  auto options = graph_add_node_options{};
  options.name = descriptor.key;
  return passthrough_node{std::move(descriptor), std::move(options)};
}

} // namespace wh::compose
