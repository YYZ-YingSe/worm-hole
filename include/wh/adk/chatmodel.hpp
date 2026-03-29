// Defines the chat-model-agent wrapper that closes the loop between authored
// instructions, model turns, tool routing, ReAct state, and ADK events.
#pragma once

#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/adk/instruction.hpp"
#include "wh/adk/event_stream.hpp"
#include "wh/adk/react.hpp"
#include "wh/adk/types.hpp"
#include "wh/agent/react.hpp"
#include "wh/agent/toolset.hpp"
#include "wh/compose/graph.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/stream/reader.hpp"

namespace wh::adk {

/// Middleware hooks applied around one chat-model turn.
struct chat_model_agent_middleware {
  /// Optional hook that may rewrite the next model request before execution.
  wh::core::callback_function<wh::core::result<void>(wh::model::chat_request &) const>
      before_model{nullptr};
  /// Optional hook that may rewrite the model response after execution.
  wh::core::callback_function<wh::core::result<void>(
      const wh::model::chat_request &, wh::model::chat_response &) const>
      after_model{nullptr};
};

/// Runtime knobs for one chat-model-agent loop.
struct chat_model_agent_options {
  /// Maximum model iterations before the loop emits one terminal error.
  std::size_t max_iterations{20U};
  /// True forwards intermediate assistant/tool events to the public event stream.
  bool emit_internal_events{false};
  /// Optional output slot written into `react_state::output_values`.
  std::string output_key{};
  /// Final-output materialization mode used for `output_key`.
  wh::agent::react_output_mode output_mode{wh::agent::react_output_mode::value};
};

/// Result bundle returned after one chat-model-agent run finishes emitting.
struct chat_model_agent_result {
  /// Event stream containing the user-visible ADK events of this run.
  agent_event_stream_reader events{};
  /// Final mutable ReAct state after the run stopped.
  wh::agent::react_state state{};
  /// Immutable execution report including rounds and terminal outcome.
  wh::agent::react_report report{};
};

namespace detail {

template <typename value_t>
[[nodiscard]] inline auto read_graph_value(wh::compose::graph_value &&value)
    -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto make_agent_run_path(const std::string_view agent_name)
    -> run_path {
  return run_path{{"agent", agent_name}};
}

[[nodiscard]] inline auto make_tool_run_path(const std::string_view agent_name,
                                             const std::string_view tool_name,
                                             const std::string_view call_id)
    -> run_path {
  return run_path{{"agent", agent_name, "tool", tool_name, call_id}};
}

[[nodiscard]] inline auto make_agent_metadata(const std::string_view agent_name)
    -> event_metadata {
  return event_metadata{
      .run_path = make_agent_run_path(agent_name),
      .agent_name = std::string{agent_name},
  };
}

[[nodiscard]] inline auto make_tool_metadata(const std::string_view agent_name,
                                             const std::string_view tool_name,
                                             const std::string_view call_id)
    -> event_metadata {
  return event_metadata{
      .run_path = make_tool_run_path(agent_name, tool_name, call_id),
      .agent_name = std::string{agent_name},
      .tool_name = std::string{tool_name},
  };
}

[[nodiscard]] inline auto make_event_reader(std::vector<agent_event> events)
    -> agent_event_stream_reader {
  return agent_event_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(events))};
}

/// One model-turn artifact containing the executed request and its response.
struct model_turn_artifact {
  /// Request after instruction and middleware rewriting.
  wh::model::chat_request request{};
  /// Response emitted by the model for this turn.
  wh::model::chat_response response{};
};

/// Owned runtime state retained by the sender-first chat-model-agent loop.
struct chat_model_runtime_state {
  /// Base request carrying options and authored tool declarations.
  wh::model::chat_request base_request{};
  /// Mutable ReAct state advanced turn by turn.
  wh::agent::react_state state{};
  /// Immutable round report accumulated during the run.
  wh::agent::react_report report{};
  /// Buffered ADK events emitted by the loop.
  std::vector<agent_event> events{};
};

/// Final result boundary returned by one chat-model-agent sender.
using chat_model_run_result = wh::core::result<chat_model_agent_result>;

/// Current graph phase being executed by the loop.
enum class chat_model_phase : std::uint8_t { model = 0U, tools };

inline auto append_error_event(std::vector<agent_event> &events,
                               const std::string_view agent_name,
                               const wh::core::error_code error,
                               std::string message) -> void {
  events.push_back(make_error_event(error, std::move(message), {},
                                    make_agent_metadata(agent_name)));
}

inline auto append_exit_event(std::vector<agent_event> &events,
                              const std::string_view agent_name) -> void {
  events.push_back(make_control_event(
      control_action{.kind = control_action_kind::exit},
      make_agent_metadata(agent_name)));
}

inline auto merge_tools(wh::model::chat_request &request,
                        const wh::agent::toolset &tools) -> void {
  for (const auto &schema : tools.schemas()) {
    bool exists = false;
    for (const auto &current : request.tools) {
      if (current.name == schema.name) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      request.tools.push_back(schema);
    }
  }
}

[[nodiscard]] inline auto
make_chat_model_result(std::unique_ptr<chat_model_runtime_state> runtime_state)
    -> chat_model_agent_result {
  return chat_model_agent_result{
      .events = make_event_reader(std::move(runtime_state->events)),
      .state = std::move(runtime_state->state),
      .report = std::move(runtime_state->report),
  };
}

/// Resolves the tools-node execution mode from the registered value-tool
/// capabilities and rejects mixed sync/async-only registries up front.
[[nodiscard]] inline auto resolve_tools_exec_mode(
    const wh::compose::tool_registry &registry)
    -> wh::core::result<wh::compose::node_exec_mode> {
  bool all_sync_invoke = true;
  bool all_async_invoke = true;

  for (const auto &[tool_name, entry] : registry) {
    static_cast<void>(tool_name);
    const bool has_sync_invoke = static_cast<bool>(entry.invoke);
    const bool has_async_invoke = static_cast<bool>(entry.async_invoke);

    if (!has_sync_invoke && !has_async_invoke) {
      return wh::core::result<wh::compose::node_exec_mode>::failure(
          wh::core::errc::not_supported);
    }

    all_sync_invoke = all_sync_invoke && has_sync_invoke;
    all_async_invoke = all_async_invoke && has_async_invoke;
  }

  if (all_sync_invoke) {
    return wh::compose::node_exec_mode::sync;
  }
  if (all_async_invoke) {
    return wh::compose::node_exec_mode::async;
  }

  return wh::core::result<wh::compose::node_exec_mode>::failure(
      wh::core::errc::not_supported);
}

} // namespace detail

/// Thin chat-model-agent wrapper that reuses compose model/tools nodes instead
/// of introducing a parallel execution runtime.
template <wh::model::chat_model_like model_t>
class chat_model_agent {
public:
  /// Creates one authored chat-model agent from the required name,
  /// description, and model.
  chat_model_agent(std::string name, std::string description,
                   model_t model) noexcept
      : name_(std::move(name)), description_(std::move(description)),
        model_(std::move(model)) {}

  chat_model_agent(const chat_model_agent &) = delete;
  auto operator=(const chat_model_agent &) -> chat_model_agent & = delete;
  chat_model_agent(chat_model_agent &&) noexcept = default;
  auto operator=(chat_model_agent &&) noexcept -> chat_model_agent & = default;
  ~chat_model_agent() = default;

  /// Returns the stable authored agent name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the human-readable agent description.
  [[nodiscard]] auto description() const noexcept -> std::string_view {
    return description_;
  }

  /// Returns true after compose nodes and loop metadata have been frozen.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Returns the authored toolset visible to this agent.
  [[nodiscard]] auto tools() const noexcept -> const wh::agent::toolset & {
    return tools_;
  }

  /// Appends one instruction fragment before the first run freezes the build.
  auto append_instruction(std::string text, const std::int32_t priority = 0)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    instruction_.append(std::move(text), priority);
    return {};
  }

  /// Replaces the current base instruction before the first run freezes the build.
  auto replace_instruction(std::string text, const std::int32_t priority = 0)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    instruction_.replace(std::move(text), priority);
    return {};
  }

  /// Registers one executable tool component before freeze.
  template <wh::agent::detail::registered_tool_component tool_t>
  auto add_tool(const tool_t &tool,
                const wh::agent::tool_registration registration = {})
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    return tools_.add_tool(tool, registration);
  }

  /// Registers one raw compose tool entry before freeze.
  auto add_tool_entry(wh::schema::tool_schema_definition schema,
                      wh::compose::tool_entry entry,
                      const wh::agent::tool_registration registration = {})
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    return tools_.add_entry(std::move(schema), std::move(entry), registration);
  }

  /// Appends one tool middleware layer before freeze.
  auto add_tool_middleware(wh::compose::tool_middleware middleware)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    return tools_.add_middleware(std::move(middleware));
  }

  /// Appends one model-turn middleware layer before freeze.
  auto add_middleware(chat_model_agent_middleware middleware)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    middlewares_.push_back(std::move(middleware));
    return {};
  }

  /// Replaces the maximum iteration budget. `0` falls back to the safe default.
  auto set_max_iterations(const std::size_t max_iterations)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    options_.max_iterations = max_iterations;
    return {};
  }

  /// Enables or disables forwarding of intermediate assistant/tool events.
  auto set_emit_internal_events(const bool enabled) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    options_.emit_internal_events = enabled;
    return {};
  }

  /// Sets the optional output slot name written into `react_state::output_values`.
  auto set_output_key(std::string output_key) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    options_.output_key = std::move(output_key);
    return {};
  }

  /// Sets whether the configured output key stores the final message or the
  /// final rendered text.
  auto set_output_mode(const wh::agent::react_output_mode mode)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    options_.output_mode = mode;
    return {};
  }

  /// Validates required metadata and compiles the compose model/tools nodes
  /// once so all future runs reuse the same lowered shape.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || description_.empty() || !model_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    wh::compose::graph model_graph{};
    auto model_added = model_graph.add_lambda(
        "chat_model_agent_model",
        [model = std::move(*model_)](wh::compose::graph_value &input,
                                     wh::core::run_context &context,
                                     const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto *request = wh::core::any_cast<wh::model::chat_request>(&input);
          if (request == nullptr) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::type_mismatch);
          }

          if constexpr (requires {
                          {
                            model.bind_tools(std::span<const wh::schema::tool_schema_definition>{})
                          } -> std::same_as<model_t>;
                        }) {
            if (!request->tools.empty()) {
              auto bound =
                  model.bind_tools(std::span<const wh::schema::tool_schema_definition>{
                      request->tools.data(), request->tools.size()});
              auto result = bound.invoke(*request, context);
              if (result.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    result.error());
              }
              return wh::compose::graph_value{detail::model_turn_artifact{
                  .request = std::move(*request),
                  .response = std::move(result).value(),
              }};
            }
          }

          auto result = model.invoke(*request, context);
          if (result.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                result.error());
          }
          return wh::compose::graph_value{detail::model_turn_artifact{
              .request = std::move(*request),
              .response = std::move(result).value(),
          }};
        });
    if (model_added.has_error()) {
      return wh::core::result<void>::failure(model_added.error());
    }
    auto model_entry = model_graph.add_entry_edge("chat_model_agent_model");
    if (model_entry.has_error()) {
      return wh::core::result<void>::failure(model_entry.error());
    }
    auto model_exit = model_graph.add_exit_edge("chat_model_agent_model");
    if (model_exit.has_error()) {
      return wh::core::result<void>::failure(model_exit.error());
    }
    auto model_compiled = model_graph.compile();
    if (model_compiled.has_error()) {
      return wh::core::result<void>::failure(model_compiled.error());
    }
    model_graph_ = std::move(model_graph);
    model_.reset();

    if (!tools_.empty()) {
      wh::compose::graph tools_graph{};
      auto exec_mode = detail::resolve_tools_exec_mode(tools_.registry());
      if (exec_mode.has_error()) {
        return wh::core::result<void>::failure(exec_mode.error());
      }

      wh::core::result<void> tools_added{};
      if (exec_mode.value() == wh::compose::node_exec_mode::sync) {
        tools_added =
            tools_graph.add_tools<wh::compose::node_contract::value,
                                  wh::compose::node_contract::value,
                                  wh::compose::node_exec_mode::sync>(
                "chat_model_agent_tools", tools_.registry(), {},
                tools_.runtime_options());
      } else {
        tools_added =
            tools_graph.add_tools<wh::compose::node_contract::value,
                                  wh::compose::node_contract::value,
                                  wh::compose::node_exec_mode::async>(
                "chat_model_agent_tools", tools_.registry(), {},
                tools_.runtime_options());
      }
      if (tools_added.has_error()) {
        return wh::core::result<void>::failure(tools_added.error());
      }
      auto tools_entry = tools_graph.add_entry_edge("chat_model_agent_tools");
      if (tools_entry.has_error()) {
        return wh::core::result<void>::failure(tools_entry.error());
      }
      auto tools_exit = tools_graph.add_exit_edge("chat_model_agent_tools");
      if (tools_exit.has_error()) {
        return wh::core::result<void>::failure(tools_exit.error());
      }
      auto tools_compiled = tools_graph.compile();
      if (tools_compiled.has_error()) {
        return wh::core::result<void>::failure(tools_compiled.error());
      }
      tools_graph_ = std::move(tools_graph);
    }

    frozen_ = true;
    return {};
  }

  /// Runs one full model/tool loop and returns the produced ADK events plus the
  /// final ReAct state/report.
  auto run(const wh::model::chat_request &request, wh::core::run_context &context)
      -> auto {
    return execute(request, context);
  }

  /// Runs one full model/tool loop from a movable request payload.
  auto run(wh::model::chat_request &&request, wh::core::run_context &context)
      -> auto {
    return execute(std::move(request), context);
  }

private:
  /// Rejects authored mutation after the first successful freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Returns the effective iteration cap, falling back to the safe default.
  [[nodiscard]] auto effective_max_iterations() const noexcept -> std::size_t {
    return options_.max_iterations == 0U ? 20U : options_.max_iterations;
  }

  /// Builds the next model request from agent metadata, accumulated history,
  /// and the bound tool schema set.
  [[nodiscard]] auto build_model_request(
      const wh::model::chat_request &base_request,
      const wh::agent::react_state &state) const -> wh::model::chat_request {
    wh::model::chat_request request = base_request;
    request.messages = state.messages;
    detail::merge_tools(request, tools_);
    if (auto instruction_message = make_instruction_message(
            description_, instruction_.render()); instruction_message.has_value()) {
      request.messages.insert(request.messages.begin(),
                              std::move(*instruction_message));
    }
    return request;
  }

  struct loop_state {
    /// Stable agent instance reused by all loop iterations.
    chat_model_agent *owner{nullptr};
    /// Borrowed run context forwarded into compose graph execution.
    wh::core::run_context *context{nullptr};
    /// Owned runtime state kept alive until the final sender completion.
    std::unique_ptr<detail::chat_model_runtime_state> runtime_state{};
  };

  class loop_sender {
    template <typename receiver_t> class operation {
      using receiver_env_t =
          decltype(stdexec::get_env(std::declval<const receiver_t &>()));

      struct child_receiver {
        using receiver_concept = stdexec::receiver_t;
        operation *op{nullptr};
        receiver_env_t env_{};

        auto set_value(wh::core::result<wh::compose::graph_invoke_result> result) && noexcept
            -> void {
          op->on_child_value(std::move(result));
        }

        template <typename error_t>
        auto set_error(error_t &&) && noexcept -> void {
          op->on_child_error(wh::core::errc::internal_error);
        }

        auto set_stopped() && noexcept -> void {
          op->on_child_error(wh::core::errc::canceled);
        }

        [[nodiscard]] auto get_env() const noexcept { return env_; }
      };

      using child_sender_t = decltype(std::declval<const wh::compose::graph &>().invoke(
          std::declval<wh::core::run_context &>(),
          std::declval<wh::compose::graph_invoke_request>()));
      using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

    public:
      explicit operation(loop_state state, receiver_t receiver)
          : receiver_(std::move(receiver)), state_(std::move(state)) {}

      auto start() & noexcept -> void { pump(); }

    private:
      auto pump() noexcept -> void {
        while (!terminal_ready_) {
          start_next_phase();
          if (child_pending_) {
            return;
          }
        }
        complete();
      }

      auto start_next_phase() noexcept -> void {
        if (state_.owner == nullptr || state_.context == nullptr ||
            state_.runtime_state == nullptr) {
          finish_with_error(wh::core::make_error(wh::core::errc::contract_violation),
                            "chat model agent runtime unavailable");
          return;
        }

        if (phase_ == detail::chat_model_phase::model) {
          start_model_phase();
          return;
        }
        start_tools_phase();
      }

      auto start_model_phase() noexcept -> void {
        auto &runtime_state = *state_.runtime_state;
        if (runtime_state.state.remaining_iterations == 0U) {
          finish_with_error(
              wh::core::make_error(wh::core::errc::resource_exhausted),
              "chat model agent exhausted max iterations");
          return;
        }

        --runtime_state.state.remaining_iterations;
        auto request = state_.owner->build_model_request(runtime_state.base_request,
                                                         runtime_state.state);
        for (auto &middleware : state_.owner->middlewares_) {
          if (!static_cast<bool>(middleware.before_model)) {
            continue;
          }
          auto result = middleware.before_model(request);
          if (result.has_error()) {
            finish_with_error(result.error(),
                              "chat model agent before_model failed");
            return;
          }
        }

        wh::compose::graph_invoke_request invoke_request{};
        invoke_request.input = wh::core::any(std::move(request));
        start_graph(*state_.owner->model_graph_, std::move(invoke_request));
      }

      auto start_tools_phase() noexcept -> void {
        if (!state_.owner->tools_graph_.has_value()) {
          finish_with_error(wh::core::make_error(wh::core::errc::protocol_error),
                            "chat model agent emitted tool calls without bound tools");
          return;
        }

        auto history_request = make_history_request(state_.runtime_state->state);
        if (history_request.has_error()) {
          finish_with_error(history_request.error(),
                            "chat model agent failed to materialize tool history");
          return;
        }

        wh::compose::graph_invoke_request invoke_request{};
        invoke_request.input = wh::core::any(make_tool_batch(
            state_.runtime_state->state.pending_tool_actions, history_request.value()));
        start_graph(*state_.owner->tools_graph_, std::move(invoke_request));
      }

      auto start_graph(const wh::compose::graph &graph,
                       wh::compose::graph_invoke_request request) noexcept -> void {
        child_pending_ = true;
        inline_completion_ = false;
        starting_child_ = true;

        try {
          child_op_.emplace_from(stdexec::connect,
                                 graph.invoke(*state_.context, std::move(request)),
                                 child_receiver{this, stdexec::get_env(receiver_)});
        } catch (...) {
          child_pending_ = false;
          inline_completion_ = false;
          starting_child_ = false;
          finish_with_error(wh::core::make_error(wh::core::errc::internal_error),
                            "chat model agent failed to start compose invoke");
          return;
        }

        stdexec::start(child_op_.get());
        starting_child_ = false;

        if (!inline_completion_) {
          return;
        }

        child_pending_ = false;
        child_op_.reset();
      }

      auto on_child_value(wh::core::result<wh::compose::graph_invoke_result> result) noexcept
          -> void {
        if (result.has_error()) {
          on_child_error(result.error());
          return;
        }

        auto invoke_result = std::move(result).value();
        if (invoke_result.output_status.has_error()) {
          on_child_error(invoke_result.output_status.error());
          return;
        }

        if (phase_ == detail::chat_model_phase::model) {
          consume_model_output(std::move(invoke_result.output_status).value());
        } else {
          consume_tool_output(std::move(invoke_result.output_status).value());
        }

        if (starting_child_) {
          inline_completion_ = true;
          return;
        }

        child_pending_ = false;
        child_op_.reset();
        pump();
      }

      auto on_child_error(const wh::core::error_code error) noexcept -> void {
        finish_with_error(
            error,
            phase_ == detail::chat_model_phase::model
                ? "chat model agent model invoke failed"
                : "chat model agent tools invoke failed");

        if (starting_child_) {
          inline_completion_ = true;
          return;
        }

        child_pending_ = false;
        child_op_.reset();
        pump();
      }

      auto consume_model_output(wh::compose::graph_value value) noexcept -> void {
        auto artifact =
            detail::read_graph_value<detail::model_turn_artifact>(std::move(value));
        if (artifact.has_error()) {
          finish_with_error(artifact.error(),
                            "chat model agent model output type mismatch");
          return;
        }

        auto owned_artifact = std::move(artifact).value();
        for (auto &middleware : state_.owner->middlewares_) {
          if (!static_cast<bool>(middleware.after_model)) {
            continue;
          }
          auto result = middleware.after_model(owned_artifact.request,
                                               owned_artifact.response);
          if (result.has_error()) {
            finish_with_error(result.error(),
                              "chat model agent after_model failed");
            return;
          }
        }

        auto &runtime_state = *state_.runtime_state;
        auto round = wh::agent::react_round_record{
            .assistant_message = std::move(owned_artifact.response.message),
        };

        runtime_state.state.messages.push_back(round.assistant_message);
        round.tool_actions =
            collect_tool_actions(round.assistant_message, state_.owner->tools_);
        runtime_state.report.rounds.push_back(std::move(round));

        auto &current_round = runtime_state.report.rounds.back();
        if (current_round.tool_actions.empty()) {
          finish_with_message(current_round.assistant_message,
                              detail::make_agent_metadata(state_.owner->name_));
          detail::append_exit_event(runtime_state.events, state_.owner->name_);
          return;
        }

        if (state_.owner->options_.emit_internal_events) {
          runtime_state.events.push_back(make_message_event(
              current_round.assistant_message,
              detail::make_agent_metadata(state_.owner->name_)));
        }

        if (!state_.owner->tools_graph_.has_value()) {
          finish_with_error(wh::core::make_error(wh::core::errc::protocol_error),
                            "chat model agent emitted tool calls without bound tools");
          return;
        }

        runtime_state.state.pending_tool_actions = current_round.tool_actions;
        phase_ = detail::chat_model_phase::tools;
      }

      auto consume_tool_output(wh::compose::graph_value value) noexcept -> void {
        auto tool_results =
            detail::read_graph_value<std::vector<wh::compose::tool_result>>(
                std::move(value));
        if (tool_results.has_error()) {
          finish_with_error(tool_results.error(),
                            "chat model agent tools output type mismatch");
          return;
        }

        auto &runtime_state = *state_.runtime_state;
        auto &current_round = runtime_state.report.rounds.back();
        current_round.tool_results = std::move(tool_results).value();
        runtime_state.state.pending_tool_actions.clear();

        for (const auto &result : current_round.tool_results) {
          auto tool_message = tool_result_to_message(result);
          if (tool_message.has_error()) {
            finish_with_error(tool_message.error(),
                              "chat model agent tool result type mismatch");
            return;
          }

          auto materialized = std::move(tool_message).value();
          runtime_state.state.messages.push_back(materialized);
          if (state_.owner->tools_.is_return_direct_tool(result.tool_name)) {
            runtime_state.state.return_direct_call_id = result.call_id;
            runtime_state.report.return_direct_call_id = result.call_id;
            finish_with_message(
                materialized,
                detail::make_tool_metadata(state_.owner->name_, result.tool_name,
                                           result.call_id));
            detail::append_exit_event(runtime_state.events, state_.owner->name_);
            return;
          }

          if (state_.owner->options_.emit_internal_events) {
            runtime_state.events.push_back(make_message_event(
                materialized,
                detail::make_tool_metadata(state_.owner->name_, result.tool_name,
                                           result.call_id)));
          }
        }

        if (!runtime_state.report.final_error.has_value()) {
          phase_ = detail::chat_model_phase::model;
        }
      }

      auto finish_with_message(const wh::schema::message &message,
                               event_metadata metadata) noexcept -> void {
        auto &runtime_state = *state_.runtime_state;
        runtime_state.report.final_message = message;
        runtime_state.report.final_text = render_message_text(message);
        runtime_state.report.completed = true;
        write_output_value(runtime_state.state, state_.owner->options_.output_key,
                           state_.owner->options_.output_mode, message);
        runtime_state.events.push_back(
            make_message_event(message, std::move(metadata)));
        terminal_ready_ = true;
      }

      auto finish_with_error(const wh::core::error_code error,
                             std::string message) noexcept -> void {
        auto &runtime_state = *state_.runtime_state;
        runtime_state.report.final_error = error;
        detail::append_error_event(runtime_state.events, state_.owner->name_, error,
                                   std::move(message));
        terminal_ready_ = true;
      }

      auto complete() noexcept -> void {
        stdexec::set_value(
            std::move(receiver_),
            detail::chat_model_run_result{
                detail::make_chat_model_result(std::move(state_.runtime_state))});
      }

      receiver_t receiver_;
      loop_state state_{};
      wh::core::detail::manual_lifetime_box<child_op_t> child_op_{};
      detail::chat_model_phase phase_{detail::chat_model_phase::model};
      bool child_pending_{false};
      bool starting_child_{false};
      bool inline_completion_{false};
      bool terminal_ready_{false};
    };

  public:
    using sender_concept = stdexec::sender_t;

    explicit loop_sender(loop_state state) noexcept : state_(std::move(state)) {}

    template <typename self_t, stdexec::receiver receiver_t>
      requires std::same_as<std::remove_cvref_t<self_t>, loop_sender> &&
               (!std::is_const_v<std::remove_reference_t<self_t>>)
    STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                              receiver_t receiver) {
      return operation<receiver_t>{std::forward<self_t>(self).state_,
                                   std::move(receiver)};
    }
    STDEXEC_EXPLICIT_THIS_END(connect)

    template <typename self_t, typename... env_t>
      requires std::same_as<std::remove_cvref_t<self_t>, loop_sender> &&
               (sizeof...(env_t) >= 1U)
    static consteval auto get_completion_signatures() {
      return stdexec::completion_signatures<
          stdexec::set_value_t(detail::chat_model_run_result)>{};
    }

  private:
    template <typename> friend class operation;

    loop_state state_{};
  };

  auto ensure_runtime_ready() -> wh::core::result<void> {
    auto frozen = freeze();
    if (frozen.has_error()) {
      return frozen;
    }
    if (!model_graph_.has_value()) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Executes the frozen model/tools loop through compose graph invoke senders.
  [[nodiscard]] auto execute(wh::model::chat_request request,
                             wh::core::run_context &context) {
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<detail::chat_model_run_result>(
            wh::core::errc::internal_error));
    using dispatch_sender_t =
        wh::core::detail::variant_sender<failure_sender_t, loop_sender>;

    return wh::core::detail::defer_result_sender<detail::chat_model_run_result>(
        [this, request = std::move(request), &context]() mutable -> dispatch_sender_t {
          auto ready = ensure_runtime_ready();
          if (ready.has_error()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<
                    detail::chat_model_run_result>(ready.error())};
          }

          auto runtime_state =
              std::make_unique<detail::chat_model_runtime_state>();
          runtime_state->base_request = std::move(request);
          runtime_state->state.messages =
              std::move(runtime_state->base_request.messages);
          runtime_state->state.remaining_iterations = effective_max_iterations();
          runtime_state->base_request.messages.clear();

          return dispatch_sender_t{loop_sender{loop_state{
              .owner = this,
              .context = std::addressof(context),
              .runtime_state = std::move(runtime_state),
          }}};
        });
  }

  /// Stable authored agent name.
  std::string name_{};
  /// Human-readable agent description.
  std::string description_{};
  /// Deferred model object moved into the frozen model graph on first freeze.
  std::optional<model_t> model_{};
  /// Authored instruction fragments rendered into the system input message.
  wh::adk::instruction instruction_{};
  /// Authored tool registry and schema set bound to the model.
  wh::agent::toolset tools_{};
  /// Around-model middlewares applied in declaration order.
  std::vector<chat_model_agent_middleware> middlewares_{};
  /// Runtime knobs frozen with the lowered graph shape.
  chat_model_agent_options options_{};
  /// Frozen compose model graph reused by all subsequent runs.
  std::optional<wh::compose::graph> model_graph_{};
  /// Optional frozen compose tools graph reused by tool-enabled runs.
  std::optional<wh::compose::graph> tools_graph_{};
  /// True after the agent shape has been frozen once.
  bool frozen_{false};
};

} // namespace wh::adk
