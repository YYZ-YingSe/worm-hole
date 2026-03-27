// Defines compose graph invoke/runtime frame and action data types.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "wh/compose/graph/detail/runtime/input.hpp"
#include "wh/compose/graph/detail/runtime/process.hpp"
#include "wh/compose/graph/detail/runtime/state.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/error.hpp"

namespace wh::compose {

struct compiled_node;
struct graph_node_state_handlers;

namespace detail::invoke_runtime {

enum class stage : std::uint8_t {
  input = 0U,
  pre_state,
  prepare,
  node,
  post_state,
  freeze,
};

struct node_frame {
  stage stage{stage::input};
  std::uint32_t node_id{0U};
  graph_state_cause cause{};
  const compiled_node *node{nullptr};
  const graph_node_state_handlers *state_handlers{nullptr};
  std::optional<std::string> cache_key{};
  std::size_t retry_budget{0U};
  std::size_t attempt{0U};
  std::optional<std::chrono::milliseconds> timeout_budget{};
  std::optional<input_runtime::reader_lowering> input_lowering{};
  std::optional<graph_value> node_input{};
  node_runtime node_runtime{};
  runtime_state::node_scope node_scope{};
  process_runtime::scoped_node_local_process_state node_local_scope{};
};

struct state_step {
  node_frame frame{};
  graph_value payload{};
  std::optional<graph_sender> sender{};
};

enum class ready_action_kind : std::uint8_t {
  no_ready,
  continue_scan,
  launch,
  terminal_error,
};

struct ready_action {
  ready_action_kind kind{ready_action_kind::continue_scan};
  std::optional<node_frame> frame{};
  wh::core::error_code error{wh::core::errc::ok};

  [[nodiscard]] static auto no_ready() noexcept -> ready_action {
    return ready_action{.kind = ready_action_kind::no_ready};
  }

  [[nodiscard]] static auto continue_scan() noexcept -> ready_action {
    return ready_action{.kind = ready_action_kind::continue_scan};
  }

  [[nodiscard]] static auto terminal_error(
      const wh::core::error_code code) noexcept -> ready_action {
    return ready_action{
        .kind = ready_action_kind::terminal_error,
        .error = code,
    };
  }

  [[nodiscard]] static auto launch(node_frame &&frame_value) -> ready_action {
    return ready_action{
        .kind = ready_action_kind::launch,
        .frame = std::move(frame_value),
    };
  }
};

struct pregel_action {
  enum class kind : std::uint8_t {
    waiting = 0U,
    skip,
    launch,
    terminal_error,
  };

  kind action{kind::waiting};
  std::uint32_t node_id{0U};
  graph_state_cause cause{};
  std::optional<node_frame> frame{};
  wh::core::error_code error{wh::core::errc::ok};

  [[nodiscard]] static auto waiting(const std::uint32_t node_id_value) noexcept
      -> pregel_action {
    return pregel_action{
        .action = kind::waiting,
        .node_id = node_id_value,
    };
  }

  [[nodiscard]] static auto skip(const std::uint32_t node_id_value,
                                 graph_state_cause cause_value)
      -> pregel_action {
    return pregel_action{
        .action = kind::skip,
        .node_id = node_id_value,
        .cause = std::move(cause_value),
    };
  }

  [[nodiscard]] static auto launch(node_frame &&frame_value) -> pregel_action {
    return pregel_action{
        .action = kind::launch,
        .node_id = frame_value.node_id,
        .cause = frame_value.cause,
        .frame = std::move(frame_value),
    };
  }

  [[nodiscard]] static auto terminal_error(
      const std::uint32_t node_id_value, graph_state_cause cause_value,
      const wh::core::error_code code) noexcept -> pregel_action {
    return pregel_action{
        .action = kind::terminal_error,
        .node_id = node_id_value,
        .cause = std::move(cause_value),
        .error = code,
    };
  }
};

} // namespace detail::invoke_runtime

} // namespace wh::compose
