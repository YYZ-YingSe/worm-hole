// Defines state-handler runtime helpers extracted from graph execution core.
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::state_runtime {

enum class state_phase : std::uint8_t {
  pre = 0U,
  post,
};

using state_handler = wh::core::callback_function<wh::core::result<void>(
    const graph_state_cause &, graph_process_state &, graph_value &, wh::core::run_context &)
                                                      const>;

[[nodiscard]] inline auto has_any_state_handler(const graph_node_state_handlers &handlers) noexcept
    -> bool {
  return static_cast<bool>(handlers.pre) || static_cast<bool>(handlers.post) ||
         static_cast<bool>(handlers.stream_pre) || static_cast<bool>(handlers.stream_post);
}

[[nodiscard]] inline auto resolve_node_state_handlers(const graph_state_handler_registry *registry,
                                                      const std::string_view node_key,
                                                      const graph_add_node_options &node_options)
    -> wh::core::result<const graph_node_state_handlers *> {
  const auto metadata = node_options.state.metadata();
  const auto *authored = node_options.state.authored_handlers();
  const graph_node_state_handlers *external = nullptr;
  bool external_entry_present = false;

  if (registry != nullptr) {
    const auto iter = registry->find(node_key);
    if (iter != registry->end()) {
      external_entry_present = true;
      if (has_any_state_handler(iter->second)) {
        external = std::addressof(iter->second);
      }
    }
  }

  if (authored != nullptr && external != nullptr) {
    return wh::core::result<const graph_node_state_handlers *>::failure(
        wh::core::errc::contract_violation);
  }
  if (authored != nullptr) {
    return authored;
  }
  if (registry == nullptr) {
    if (metadata.any()) {
      return wh::core::result<const graph_node_state_handlers *>::failure(
          wh::core::errc::not_found);
    }
    return static_cast<const graph_node_state_handlers *>(nullptr);
  }
  if (external == nullptr) {
    if (metadata.any()) {
      if (external_entry_present) {
        return wh::core::result<const graph_node_state_handlers *>::failure(
            wh::core::errc::contract_violation);
      }
      return wh::core::result<const graph_node_state_handlers *>::failure(
          wh::core::errc::not_found);
    }
    return static_cast<const graph_node_state_handlers *>(nullptr);
  }
  if ((metadata.pre && !external->pre) || (metadata.post && !external->post) ||
      (metadata.stream_pre && !external->stream_pre) ||
      (metadata.stream_post && !external->stream_post)) {
    return wh::core::result<const graph_node_state_handlers *>::failure(
        wh::core::errc::contract_violation);
  }
  return external;
}

[[nodiscard]] inline auto value_handler_for(const graph_node_state_handlers *handlers,
                                            const state_phase phase) noexcept -> state_handler {
  if (handlers == nullptr) {
    return nullptr;
  }
  return phase == state_phase::pre ? handlers->pre : handlers->post;
}

[[nodiscard]] inline auto stream_handler_for(const graph_node_state_handlers *handlers,
                                             const state_phase phase) noexcept -> state_handler {
  if (handlers == nullptr) {
    return nullptr;
  }
  return phase == state_phase::pre ? handlers->stream_pre : handlers->stream_post;
}

[[nodiscard]] inline auto has_stream_phase(const graph_node_state_handlers *handlers,
                                           const state_phase phase) noexcept -> bool {
  return static_cast<bool>(stream_handler_for(handlers, phase));
}

[[nodiscard]] inline auto needs_async_phase(const graph_node_state_handlers *handlers,
                                            const graph_value &payload,
                                            const state_phase phase) noexcept -> bool {
  if (handlers == nullptr || wh::core::any_cast<graph_stream_reader>(&payload) == nullptr) {
    return false;
  }
  return static_cast<bool>(value_handler_for(handlers, phase)) ||
         static_cast<bool>(stream_handler_for(handlers, phase));
}

inline auto apply_phase(wh::core::run_context &context, const graph_node_state_handlers *handlers,
                        const graph_state_cause &cause, graph_process_state &process_state,
                        graph_value &payload, const state_phase phase) -> wh::core::result<void> {
  const auto value_handler = value_handler_for(handlers, phase);
  if (value_handler) {
    auto status = value_handler(cause, process_state, payload, context);
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }

  const auto stream_handler = stream_handler_for(handlers, phase);
  if (stream_handler) {
    auto status = stream_handler(cause, process_state, payload, context);
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }
  return {};
}

template <typename on_read_error_t>
auto apply_pre_handlers(wh::core::run_context &context, const graph_node_state_handlers *handlers,
                        [[maybe_unused]] const std::string_view node_key,
                        const graph_state_cause &cause, graph_process_state &process_state,
                        graph_value &input, [[maybe_unused]] on_read_error_t &&on_read_error)
    -> wh::core::result<void> {
  if (handlers == nullptr) {
    return {};
  }
  return apply_phase(context, handlers, cause, process_state, input, state_phase::pre);
}

template <typename on_read_error_t>
auto apply_post_handlers(wh::core::run_context &context, const graph_node_state_handlers *handlers,
                         [[maybe_unused]] const std::string_view node_key,
                         const graph_state_cause &cause, graph_process_state &process_state,
                         graph_value &output, [[maybe_unused]] on_read_error_t &&on_read_error)
    -> wh::core::result<void> {
  if (handlers == nullptr) {
    return {};
  }
  return apply_phase(context, handlers, cause, process_state, output, state_phase::post);
}

} // namespace wh::compose::detail::state_runtime
