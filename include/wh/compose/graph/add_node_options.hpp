// Defines per-node metadata and validation flags used when adding graph nodes.
#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/compose/graph/compile_info.hpp"
#include "wh/compose/node/detail/gate.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/core/callback.hpp"

namespace wh::compose {

namespace detail {

template <typename payload_t, typename handler_t>
concept typed_node_state_phase_handler =
    requires(handler_t handler, const graph_state_cause &cause,
             graph_process_state &process_state,
             std::remove_cvref_t<payload_t> &payload,
             wh::core::run_context &context) {
      {
        handler(cause, process_state, payload, context)
      } -> std::same_as<wh::core::result<void>>;
    };

template <typename payload_t, typename handler_t>
[[nodiscard]] inline auto
bind_typed_node_state_phase_handler(handler_t &&handler)
    -> graph_state_pre_handler {
  using stored_payload_t = std::remove_cvref_t<payload_t>;
  using stored_handler_t = std::remove_cvref_t<handler_t>;
  return graph_state_pre_handler{
      [handler = stored_handler_t{std::forward<handler_t>(handler)}](
          const graph_state_cause &cause, graph_process_state &process_state,
          graph_value &payload,
          wh::core::run_context &context) -> wh::core::result<void> {
        if constexpr (std::same_as<stored_payload_t, graph_value>) {
          return handler(cause, process_state, payload, context);
        } else {
          auto *typed = wh::core::any_cast<stored_payload_t>(&payload);
          if (typed == nullptr) {
            return wh::core::result<void>::failure(
                wh::core::errc::type_mismatch);
          }
          return handler(cause, process_state, *typed, context);
        }
      }};
}

} // namespace detail

/// One local callback registration stored on a node definition.
struct graph_node_callback_registration {
  /// Registration metadata such as stage filters and logical name.
  wh::core::callback_config config{};
  /// Stage-bound callbacks registered for this node scope.
  wh::core::stage_callbacks callbacks{};
};

/// Ordered node-local callback plan stored on one node definition.
using graph_node_callback_plan = std::vector<graph_node_callback_registration>;

/// Node-level observation defaults stored in graph definitions.
struct graph_node_observation {
  /// Enables callback emission for this node when runtime supports it.
  bool callbacks_enabled{true};
  /// True means invoke-time observation overrides may patch this node.
  bool allow_invoke_override{true};
  /// Default local callback registrations attached to this node.
  graph_node_callback_plan local_callbacks{};
};

/// One node-local state phase declaration/binding stored on a node definition.
struct graph_node_state_phase_option {
  /// True means this phase must be available at invoke time.
  bool required{false};
  /// Compile-visible payload type used for graph compile validation.
  value_gate payload{};
  /// Bound authored handler for this phase when present.
  graph_state_pre_handler handler{nullptr};

  /// Marks this phase as required even when the handler will be supplied later.
  auto require() noexcept -> graph_node_state_phase_option & {
    required = true;
    return *this;
  }

  template <typename payload_t = graph_value, typename handler_t>
    requires detail::typed_node_state_phase_handler<payload_t, handler_t>
  /// Binds one authored state phase handler on the exact payload type.
  auto bind(handler_t &&value) -> graph_node_state_phase_option & {
    using stored_payload_t = std::remove_cvref_t<payload_t>;
    required = true;
    payload = value_gate::exact<stored_payload_t>();
    handler = detail::bind_typed_node_state_phase_handler<stored_payload_t>(
        std::forward<handler_t>(value));
    return *this;
  }

  /// Returns true when this phase participates in compile/runtime handling.
  [[nodiscard]] auto active() const noexcept -> bool {
    return required || static_cast<bool>(handler);
  }
};

/// Authored node-local state hook plan stored on one node definition.
class graph_node_state_options {
public:
  template <typename payload_t = graph_value, typename handler_t>
    requires detail::typed_node_state_phase_handler<payload_t, handler_t>
  auto bind_pre(handler_t &&handler) -> graph_node_state_options & {
    pre_.template bind<payload_t>(std::forward<handler_t>(handler));
    sync_authored_handlers();
    return *this;
  }

  template <typename payload_t = graph_value, typename handler_t>
    requires detail::typed_node_state_phase_handler<payload_t, handler_t>
  auto bind_post(handler_t &&handler) -> graph_node_state_options & {
    post_.template bind<payload_t>(std::forward<handler_t>(handler));
    sync_authored_handlers();
    return *this;
  }

  template <typename payload_t = graph_value, typename handler_t>
    requires(!std::same_as<std::remove_cvref_t<payload_t>,
                           graph_stream_reader>) &&
            detail::typed_node_state_phase_handler<payload_t, handler_t>
  auto bind_stream_pre(handler_t &&handler) -> graph_node_state_options & {
    stream_pre_.template bind<payload_t>(std::forward<handler_t>(handler));
    sync_authored_handlers();
    return *this;
  }

  template <typename payload_t = graph_value, typename handler_t>
    requires(!std::same_as<std::remove_cvref_t<payload_t>,
                           graph_stream_reader>) &&
            detail::typed_node_state_phase_handler<payload_t, handler_t>
  auto bind_stream_post(handler_t &&handler) -> graph_node_state_options & {
    stream_post_.template bind<payload_t>(std::forward<handler_t>(handler));
    sync_authored_handlers();
    return *this;
  }

  auto require_pre() noexcept -> graph_node_state_options & {
    pre_.require();
    return *this;
  }

  auto require_post() noexcept -> graph_node_state_options & {
    post_.require();
    return *this;
  }

  auto require_stream_pre() noexcept -> graph_node_state_options & {
    stream_pre_.require();
    return *this;
  }

  auto require_stream_post() noexcept -> graph_node_state_options & {
    stream_post_.require();
    return *this;
  }

  [[nodiscard]] auto any() const noexcept -> bool {
    return pre_.active() || post_.active() || stream_pre_.active() ||
           stream_post_.active();
  }

  [[nodiscard]] auto metadata() const noexcept
      -> graph_compile_state_handler_metadata {
    return graph_compile_state_handler_metadata{
        .pre = pre_.active(),
        .post = post_.active(),
        .stream_pre = stream_pre_.active(),
        .stream_post = stream_post_.active(),
    };
  }

  [[nodiscard]] auto pre() const noexcept
      -> const graph_node_state_phase_option & {
    return pre_;
  }

  [[nodiscard]] auto post() const noexcept
      -> const graph_node_state_phase_option & {
    return post_;
  }

  [[nodiscard]] auto stream_pre() const noexcept
      -> const graph_node_state_phase_option & {
    return stream_pre_;
  }

  [[nodiscard]] auto stream_post() const noexcept
      -> const graph_node_state_phase_option & {
    return stream_post_;
  }

  [[nodiscard]] auto authored_handlers() const noexcept
      -> const graph_node_state_handlers * {
    if (!has_authored_handler()) {
      return nullptr;
    }
    return std::addressof(authored_handlers_);
  }

private:
  [[nodiscard]] auto has_authored_handler() const noexcept -> bool {
    return static_cast<bool>(authored_handlers_.pre) ||
           static_cast<bool>(authored_handlers_.post) ||
           static_cast<bool>(authored_handlers_.stream_pre) ||
           static_cast<bool>(authored_handlers_.stream_post);
  }

  auto sync_authored_handlers() noexcept -> void {
    authored_handlers_.pre = pre_.handler;
    authored_handlers_.post = post_.handler;
    authored_handlers_.stream_pre = stream_pre_.handler;
    authored_handlers_.stream_post = stream_post_.handler;
  }

  graph_node_state_phase_option pre_{};
  graph_node_state_phase_option post_{};
  graph_node_state_phase_option stream_pre_{};
  graph_node_state_phase_option stream_post_{};
  graph_node_state_handlers authored_handlers_{};
};

/// Node-level metadata stored in graph definitions.
struct graph_add_node_options {
  /// Optional display name for diagnostics.
  std::string name{};
  /// Stable logical node type used by diagnostics and introspection.
  std::string type{};
  /// Logical input key consumed by keyed payload adapters.
  std::string input_key{};
  /// Logical output key produced by keyed payload adapters.
  std::string output_key{};
  /// Node-level observation defaults stored at registration time.
  graph_node_observation observation{};
  /// Optional display label emitted in introspection events.
  std::string label{};
  /// True means node may execute with no control predecessor.
  bool allow_no_control{false};
  /// True means node may execute with no data predecessor.
  bool allow_no_data{false};
  /// Optional node-level retry budget override (falls back to graph default).
  std::optional<std::size_t> retry_budget_override{};
  /// Optional node-level timeout override (`nullopt` falls back to graph
  /// default).
  std::optional<std::chrono::milliseconds> timeout_override{};
  /// Optional node-level retry window override used for policy composability
  /// checks.
  std::optional<std::chrono::milliseconds> retry_window_override{};
  /// Optional node-level parallel gate override (`>=1`).
  std::optional<std::size_t> max_parallel_override{};
  /// Declares node-level authored/runtime state hooks.
  graph_node_state_options state{};
  /// Optional subgraph compile snapshot forwarded to parent compile callback.
  std::optional<graph_compile_info> subgraph_compile_info{};
};

} // namespace wh::compose
