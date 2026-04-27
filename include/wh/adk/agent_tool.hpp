// Defines the public authored agent-to-tool bridge surface.
#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/adk/runner.hpp"
#include "wh/agent/agent.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/tool.hpp"
#include "wh/tool/call_scope.hpp"

namespace wh::adk {

/// Supported authored input shapes for one future agent-tool bridge.
enum class agent_tool_input_mode {
  /// Use the default `{request:string}` tool input contract.
  request = 0U,
  /// Rehydrate one full-history request from the current ReAct state.
  message_history,
  /// Pass the raw JSON payload through as one user message.
  custom_schema,
};

/// Final bridge report returned after one agent-tool run.
struct agent_tool_result {
  /// Boundary-visible event stream after prefix projection and filtering.
  agent_event_stream_reader events{};
  /// Ordered text chunks extracted from the child path for tool streaming.
  std::vector<std::string> output_chunks{};
  /// Flattened text returned to the tool caller.
  std::string output_text{};
  /// Final message observed on the child-agent path, when any.
  std::optional<wh::schema::message> final_message{};
  /// Terminal error observed inside the bridge, when any.
  std::optional<wh::core::error_code> final_error{};
  /// True when the child path emitted one interrupt action.
  bool interrupted{false};
};

namespace detail {

using agent_tool_sync_runner = wh::core::callback_function<agent_run_result(
    const wh::adk::run_request &, wh::core::run_context &) const>;

using agent_tool_async_runner =
    wh::core::callback_function<wh::core::detail::result_sender<agent_run_result>(
        wh::adk::run_request, wh::core::run_context &) const>;

struct agent_tool_runner_binding {
  agent_tool_sync_runner sync{nullptr};
  agent_tool_async_runner async{nullptr};

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(sync) || static_cast<bool>(async);
  }
};

/// Frozen bridge runtime captured once from the public authored shell.
struct agent_tool_runtime {
  /// Stable public tool name.
  std::string tool_name{};
  /// Stable bound child-agent name.
  std::string agent_name{};
  /// Frozen input mapping mode.
  agent_tool_input_mode input_mode{agent_tool_input_mode::request};
  /// True forwards child internal events after boundary filtering.
  bool forward_internal_events{false};
  /// Frozen child-agent execution entrypoints preserved at authored capability.
  agent_tool_runner_binding runner{};
};

template <typename value_t> struct agent_tool_runner_box {
  template <typename value_u>
    requires std::constructible_from<value_t, value_u>
  explicit agent_tool_runner_box(value_u &&stored) : value(std::forward<value_u>(stored)) {}

  mutable value_t value;
};

template <typename runner_t>
concept agent_tool_sync_runner_object_const = requires(
    runner_t &runner, const wh::adk::run_request &request, wh::core::run_context &context) {
  { runner.run(request, context) } -> std::same_as<agent_run_result>;
};

template <typename runner_t>
concept agent_tool_sync_runner_object_move =
    requires(runner_t &runner, wh::adk::run_request request, wh::core::run_context &context) {
      { runner.run(std::move(request), context) } -> std::same_as<agent_run_result>;
    };

template <typename runner_t>
concept agent_tool_sync_runner_object =
    agent_tool_sync_runner_object_const<runner_t> || agent_tool_sync_runner_object_move<runner_t>;

template <typename runner_t>
concept agent_tool_sync_runner_callable_const = requires(
    runner_t &runner, const wh::adk::run_request &request, wh::core::run_context &context) {
  { std::invoke(runner, request, context) } -> std::same_as<agent_run_result>;
};

template <typename runner_t>
concept agent_tool_sync_runner_callable_move =
    requires(runner_t &runner, wh::adk::run_request request, wh::core::run_context &context) {
      { std::invoke(runner, std::move(request), context) } -> std::same_as<agent_run_result>;
    };

template <typename runner_t>
concept agent_tool_sync_runner_callable = agent_tool_sync_runner_callable_const<runner_t> ||
                                          agent_tool_sync_runner_callable_move<runner_t>;

template <typename runner_t>
concept agent_tool_async_runner_object_const = requires(
    runner_t &runner, const wh::adk::run_request &request, wh::core::run_context &context) {
  requires wh::core::detail::sender_exact_value<decltype(runner.run_async(request, context)),
                                                agent_run_result>;
};

template <typename runner_t>
concept agent_tool_async_runner_object_move =
    requires(runner_t &runner, wh::adk::run_request request, wh::core::run_context &context) {
      requires wh::core::detail::sender_exact_value<
          decltype(runner.run_async(std::move(request), context)), agent_run_result>;
    };

template <typename runner_t>
concept agent_tool_async_runner_object =
    agent_tool_async_runner_object_const<runner_t> || agent_tool_async_runner_object_move<runner_t>;

template <typename runner_t>
concept agent_tool_async_runner_callable_const = requires(
    runner_t &runner, const wh::adk::run_request &request, wh::core::run_context &context) {
  requires wh::core::detail::sender_exact_value<decltype(std::invoke(runner, request, context)),
                                                agent_run_result>;
};

template <typename runner_t>
concept agent_tool_async_runner_callable_move =
    requires(runner_t &runner, wh::adk::run_request request, wh::core::run_context &context) {
      requires wh::core::detail::sender_exact_value<
          decltype(std::invoke(runner, std::move(request), context)), agent_run_result>;
    };

template <typename runner_t>
concept agent_tool_async_runner_callable = agent_tool_async_runner_callable_const<runner_t> ||
                                           agent_tool_async_runner_callable_move<runner_t>;

template <typename runner_t>
concept bindable_agent_tool_runner =
    agent_tool_sync_runner_object<std::remove_cvref_t<runner_t>> ||
    agent_tool_sync_runner_callable<std::remove_cvref_t<runner_t>> ||
    agent_tool_async_runner_object<std::remove_cvref_t<runner_t>> ||
    agent_tool_async_runner_callable<std::remove_cvref_t<runner_t>>;

template <typename runner_t>
[[nodiscard]] inline auto dispatch_agent_tool_sync_runner(runner_t &runner,
                                                          const wh::adk::run_request &request,
                                                          wh::core::run_context &context)
    -> agent_run_result {
  if constexpr (agent_tool_sync_runner_object_const<runner_t>) {
    return runner.run(request, context);
  } else if constexpr (agent_tool_sync_runner_object_move<runner_t>) {
    return runner.run(wh::adk::run_request{request}, context);
  } else if constexpr (agent_tool_sync_runner_callable_const<runner_t>) {
    return std::invoke(runner, request, context);
  } else {
    return std::invoke(runner, wh::adk::run_request{request}, context);
  }
}

template <typename runner_t>
[[nodiscard]] inline auto dispatch_agent_tool_async_runner(runner_t &runner,
                                                           wh::adk::run_request request,
                                                           wh::core::run_context &context)
    -> wh::core::detail::result_sender<agent_run_result> {
  if constexpr (agent_tool_async_runner_object<runner_t>) {
    return wh::core::detail::request_result_sender<agent_run_result>(
        std::move(request), [&runner, &context](auto &&forwarded_request) mutable {
          return runner.run_async(std::forward<decltype(forwarded_request)>(forwarded_request),
                                  context);
        });
  } else {
    return wh::core::detail::request_result_sender<agent_run_result>(
        std::move(request), [&runner, &context](auto &&forwarded_request) mutable {
          return std::invoke(runner, std::forward<decltype(forwarded_request)>(forwarded_request),
                             context);
        });
  }
}

template <typename runner_t>
[[nodiscard]] inline auto dispatch_agent_tool_runner(runner_t &runner,
                                                     const wh::adk::run_request &request,
                                                     wh::core::run_context &context)
    -> agent_run_result {
  return dispatch_agent_tool_sync_runner(runner, request, context);
}

struct agent_tool_access;

} // namespace detail

/// Frozen bridge from one authored agent into one tool boundary.
class agent_tool {
public:
  /// Creates one bridge from one public tool name, description, and bound
  /// agent.
  agent_tool(std::string name, std::string description, wh::agent::agent agent) noexcept
      : name_(std::move(name)), description_(std::move(description)),
        bound_agent_(std::move(agent)) {}

  agent_tool(const agent_tool &) = delete;
  auto operator=(const agent_tool &) -> agent_tool & = delete;
  agent_tool(agent_tool &&) noexcept = default;
  auto operator=(agent_tool &&) noexcept -> agent_tool & = default;
  ~agent_tool() = default;

  /// Returns the stable tool name exposed to model/tool routing.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the human-readable tool description.
  [[nodiscard]] auto description() const noexcept -> std::string_view { return description_; }

  /// Returns the authored input mode selected for this bridge.
  [[nodiscard]] auto input_mode() const noexcept -> agent_tool_input_mode { return input_mode_; }

  /// Returns true after metadata, schema mode, and agent binding freeze.
  [[nodiscard]] auto frozen() const noexcept -> bool { return runtime_.has_value(); }

  /// Returns true when internal events should be forwarded across the bridge.
  [[nodiscard]] auto forward_internal_events() const noexcept -> bool {
    return forward_internal_events_;
  }

  /// Returns the bound authored agent that will later be lowered behind the
  /// tool boundary.
  [[nodiscard]] auto bound_agent() const noexcept -> const wh::agent::agent & {
    return bound_agent_;
  }

  /// Sets the authored input mode before freeze.
  auto set_input_mode(const agent_tool_input_mode input_mode) -> wh::core::result<void> {
    auto mutable_result = ensure_mutable();
    if (mutable_result.has_error()) {
      return mutable_result;
    }
    input_mode_ = input_mode;
    return {};
  }

  /// Replaces the custom schema payload before freeze.
  auto set_custom_schema(wh::schema::tool_schema_definition schema) -> wh::core::result<void> {
    auto mutable_result = ensure_mutable();
    if (mutable_result.has_error()) {
      return mutable_result;
    }
    custom_schema_ = std::move(schema);
    return {};
  }

  /// Enables or disables authored internal-event forwarding before freeze.
  auto set_forward_internal_events(const bool enabled) -> wh::core::result<void> {
    auto mutable_result = ensure_mutable();
    if (mutable_result.has_error()) {
      return mutable_result;
    }
    forward_internal_events_ = enabled;
    return {};
  }

  /// Binds one executable child runner by the authored bound-agent name.
  auto bind_runner(detail::agent_tool_runner_binding runner) -> wh::core::result<void> {
    auto mutable_result = ensure_mutable();
    if (mutable_result.has_error()) {
      return mutable_result;
    }
    if (!static_cast<bool>(runner)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    runner_ = std::move(runner);
    return {};
  }

  /// Binds one executable child runner from a callable or `.run(...)` object.
  template <detail::bindable_agent_tool_runner runner_t>
  auto bind_runner(runner_t &&runner) -> wh::core::result<void> {
    using stored_runner_t = std::remove_cvref_t<runner_t>;
    auto runner_box = std::make_shared<detail::agent_tool_runner_box<stored_runner_t>>(
        std::forward<runner_t>(runner));
    detail::agent_tool_runner_binding binding{};
    if constexpr (detail::agent_tool_sync_runner_object<stored_runner_t> ||
                  detail::agent_tool_sync_runner_callable<stored_runner_t>) {
      binding.sync = detail::agent_tool_sync_runner{
          [runner_box](const wh::adk::run_request &request,
                       wh::core::run_context &context) -> agent_run_result {
            return detail::dispatch_agent_tool_sync_runner(runner_box->value, request, context);
          }};
    }
    if constexpr (detail::agent_tool_async_runner_object<stored_runner_t> ||
                  detail::agent_tool_async_runner_callable<stored_runner_t>) {
      binding.async = detail::agent_tool_async_runner{
          [runner_box](wh::adk::run_request request, wh::core::run_context &context)
              -> wh::core::detail::result_sender<agent_run_result> {
            return detail::dispatch_agent_tool_async_runner(runner_box->value, std::move(request),
                                                            context);
          }};
    }
    return bind_runner(std::move(binding));
  }

  /// Materializes the authored tool schema visible to runtime/tool routing.
  [[nodiscard]] auto tool_schema() const -> wh::schema::tool_schema_definition;

  /// Runs one concrete tool call through the frozen child-agent runner.
  auto run(const wh::compose::tool_call &call, const wh::tool::call_scope &scope) const
      -> wh::core::result<agent_tool_result>;

  /// Runs one concrete tool call on the frozen bridge and exposes the text
  /// stream view projected from the boundary-visible event stream.
  auto stream(const wh::compose::tool_call &call, const wh::tool::call_scope &scope) const
      -> wh::core::result<wh::compose::graph_stream_reader>;

  /// Lowers the frozen bridge into one compose tool entry.
  [[nodiscard]] auto compose_entry() const -> wh::core::result<wh::compose::tool_entry>;

  /// Freezes schema selection, bound agent, and bridge runner.
  auto freeze() -> wh::core::result<void> {
    if (runtime_.has_value()) {
      return {};
    }
    if (name_.empty() || description_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (input_mode_ == agent_tool_input_mode::custom_schema && !custom_schema_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto agent_frozen = bound_agent_.freeze();
    if (agent_frozen.has_error()) {
      return agent_frozen;
    }
    if (!static_cast<bool>(runner_)) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    runtime_.emplace(detail::agent_tool_runtime{
        .tool_name = name_,
        .agent_name = std::string{bound_agent_.name()},
        .input_mode = input_mode_,
        .forward_internal_events = forward_internal_events_,
        .runner = runner_,
    });
    return {};
  }

private:
  /// Rejects bridge mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (runtime_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Stable tool name exposed to model/tool routing.
  std::string name_{};
  /// Human-readable bridge description.
  std::string description_{};
  /// Authored input mode selected for this bridge.
  agent_tool_input_mode input_mode_{agent_tool_input_mode::request};
  /// Optional caller-provided custom schema.
  std::optional<wh::schema::tool_schema_definition> custom_schema_{};
  /// True when internal events should later be forwarded across the bridge.
  bool forward_internal_events_{false};
  /// Bound authored agent that will later be lowered behind the tool boundary.
  wh::agent::agent bound_agent_{""};
  /// Frozen child-agent execution entrypoints preserved at authored capability.
  detail::agent_tool_runner_binding runner_{};
  /// Cached runtime bundle materialized exactly once at freeze.
  std::optional<detail::agent_tool_runtime> runtime_{};

  friend struct detail::agent_tool_access;
};

} // namespace wh::adk

#include "wh/adk/detail/agent_tool_impl.hpp"
