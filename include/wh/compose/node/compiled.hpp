// Defines compiled compose runtime node metadata and execution programs.
#pragma once

#include <concepts>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/graph/restore_shape.hpp"
#include "wh/compose/graph/snapshot.hpp"
#include "wh/compose/node/detail/gate.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/stdexec/result_sender.hpp"

namespace wh::compose {

struct compiled_node_meta {
  std::string key{};
  node_kind kind{node_kind::component};
  node_exec_mode exec_mode{node_exec_mode::sync};
  node_exec_origin exec_origin{default_exec_origin(node_kind::component)};
  node_contract input_contract{node_contract::value};
  node_contract output_contract{node_contract::value};
  std::optional<input_gate> compiled_input_gate{};
  std::optional<output_gate> compiled_output_gate{};
  graph_add_node_options options{};
  std::optional<graph_snapshot> subgraph_snapshot{};
  std::optional<graph_restore_shape> subgraph_restore_shape{};
};

struct compiled_sync_program {
  node_sync_factory run{nullptr};

  [[nodiscard]] auto operator()(graph_value &input, wh::core::run_context &context,
                                const node_runtime &runtime) const
      -> wh::core::result<graph_value> {
    if (!static_cast<bool>(run)) {
      return wh::core::result<graph_value>::failure(wh::core::errc::not_supported);
    }
    return run(input, context, runtime);
  }
};

struct compiled_async_program {
  node_async_factory run{nullptr};

  [[nodiscard]] auto operator()(graph_value &input, wh::core::run_context &context,
                                const node_runtime &runtime) const -> graph_sender {
    if (!static_cast<bool>(run)) {
      return detail::failure_graph_sender(wh::core::errc::not_supported);
    }
    return run(input, context, runtime);
  }
};

using compiled_node_program = std::variant<compiled_sync_program, compiled_async_program>;

struct compiled_node {
  compiled_node_meta meta{};
  compiled_node_program program{};
};

[[nodiscard]] inline auto compiled_node_is_sync(const compiled_node &node) noexcept -> bool {
  return std::holds_alternative<compiled_sync_program>(node.program);
}

[[nodiscard]] inline auto compiled_node_is_async(const compiled_node &node) noexcept -> bool {
  return std::holds_alternative<compiled_async_program>(node.program);
}

[[nodiscard]] inline auto run_compiled_sync_node(const compiled_node &node, graph_value &input,
                                                 wh::core::run_context &context,
                                                 const node_runtime &runtime)
    -> wh::core::result<graph_value> {
  if (const auto *program = std::get_if<compiled_sync_program>(&node.program)) {
    return (*program)(input, context, runtime);
  }
  return wh::core::result<graph_value>::failure(wh::core::errc::contract_violation);
}

[[nodiscard]] inline auto run_compiled_async_node(const compiled_node &node, graph_value &input,
                                                  wh::core::run_context &context,
                                                  const node_runtime &runtime) -> graph_sender {
  if (const auto *program = std::get_if<compiled_async_program>(&node.program)) {
    return (*program)(input, context, runtime);
  }
  return detail::failure_graph_sender(wh::core::errc::contract_violation);
}

template <typename key_t, typename run_t, typename options_t = graph_add_node_options>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_add_node_options, options_t &&> &&
           node_sync_run<std::remove_cvref_t<run_t>>
[[nodiscard]] inline auto
make_compiled_sync_node(const node_kind kind, const node_exec_origin exec_origin,
                        const node_contract input_contract, const node_contract output_contract,
                        key_t &&key, run_t &&run, options_t &&options = {}) -> compiled_node {
  return compiled_node{
      .meta =
          compiled_node_meta{
              .key = std::string{std::forward<key_t>(key)},
              .kind = kind,
              .exec_mode = node_exec_mode::sync,
              .exec_origin = exec_origin,
              .input_contract = input_contract,
              .output_contract = output_contract,
              .options = graph_add_node_options{std::forward<options_t>(options)},
          },
      .program = compiled_sync_program{detail::bind_node_sync_factory(std::forward<run_t>(run))},
  };
}

template <typename key_t, typename run_t, typename options_t = graph_add_node_options>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_add_node_options, options_t &&> &&
           (node_async_run<std::remove_cvref_t<run_t>> || graph_result_sender<run_t>)
[[nodiscard]] inline auto
make_compiled_async_node(const node_kind kind, const node_exec_origin exec_origin,
                         const node_contract input_contract, const node_contract output_contract,
                         key_t &&key, run_t &&run, options_t &&options = {}) -> compiled_node {
  return compiled_node{
      .meta =
          compiled_node_meta{
              .key = std::string{std::forward<key_t>(key)},
              .kind = kind,
              .exec_mode = node_exec_mode::async,
              .exec_origin = exec_origin,
              .input_contract = input_contract,
              .output_contract = output_contract,
              .options = graph_add_node_options{std::forward<options_t>(options)},
          },
      .program = compiled_async_program{detail::bind_node_async_factory(std::forward<run_t>(run))},
  };
}

} // namespace wh::compose
