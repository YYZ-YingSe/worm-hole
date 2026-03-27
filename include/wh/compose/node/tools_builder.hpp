// Defines the public tools-node builder surface.
#pragma once

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>

#include "wh/compose/node/authored.hpp"
#include "wh/compose/node/detail/contract.hpp"
#include "wh/compose/node/detail/tools/runtime.hpp"
#include "wh/compose/node/tools_contract.hpp"

namespace wh::compose {

inline auto tools_node::compile() const & -> compiled_node {
  auto copied = *this;
  return std::move(copied).compile();
}

inline auto tools_node::compile() && -> compiled_node {
  if (!static_cast<bool>(payload_.lower)) {
    return {};
  }
  return payload_.lower(std::move(descriptor_.key), std::move(options_));
}

template <node_contract From = node_contract::value,
          node_contract To = node_contract::value,
          node_exec_mode Exec = node_exec_mode::sync, typename key_t,
          typename registry_t, typename options_t = graph_add_node_options,
          typename tools_options_t = tools_options>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<tool_registry, registry_t &&> &&
           std::constructible_from<graph_add_node_options, options_t &&> &&
           std::constructible_from<tools_options, tools_options_t &&> &&
           detail::typed_request<From, tool_batch> &&
           detail::typed_response<
               To, std::conditional_t<To == node_contract::value,
                                      std::vector<tool_result>,
                                      graph_stream_reader>> &&
           (To == node_contract::value || To == node_contract::stream)
/// Builds one graph node that dispatches to one of many named tools.
[[nodiscard]] inline auto make_tools_node(key_t &&key, registry_t &&registry,
                                          options_t &&options = {},
                                          tools_options_t &&runtime_options = {})
    -> tools_node {
  auto stored_key = std::string{std::forward<key_t>(key)};
  auto tools = tool_registry{std::forward<registry_t>(registry)};
  auto tool_options =
      tools_options{std::forward<tools_options_t>(runtime_options)};
  auto node_options = detail::decorate_node_options(
      std::forward<options_t>(options), "tools_node", "tools_node");
  return tools_node{
      node_descriptor{
          .key = std::move(stored_key),
          .kind = node_kind::tools,
          .exec_mode = Exec,
          .exec_origin = default_exec_origin(node_kind::tools),
          .input_contract = From,
          .output_contract = To,
          .input_gate_info = detail::typed_input_gate<From, tool_batch>(),
          .output_gate_info =
              detail::typed_output_gate<To, std::vector<tool_result>>(),
      },
      tools_payload{
          .lower =
              [tools = std::move(tools), tool_options = std::move(tool_options)](
                  std::string key, graph_add_node_options options) mutable
              -> compiled_node {
            if constexpr (Exec == node_exec_mode::sync) {
              return make_compiled_sync_node(
                  node_kind::tools, default_exec_origin(node_kind::tools), From,
                  To, std::move(key),
                  [tools = std::move(tools), tool_options = std::move(tool_options)](
                      const graph_value &input, wh::core::run_context &context,
                      const node_runtime &runtime)
                      -> wh::core::result<graph_value> {
                    return detail::run_tools_sync<To>(input, context, runtime,
                                                      tools, tool_options);
                  },
                  std::move(options));
            } else {
              return make_compiled_async_node(
                  node_kind::tools, default_exec_origin(node_kind::tools), From,
                  To, std::move(key),
                  [tools = std::move(tools), tool_options = std::move(tool_options)](
                      const graph_value &input, wh::core::run_context &context,
                      const node_runtime &runtime) -> graph_sender {
                    return detail::run_tools_async<To>(input, context, runtime,
                                                       tools, tool_options);
                  },
                  std::move(options));
            }
          }},
      std::move(node_options)};
}

} // namespace wh::compose
