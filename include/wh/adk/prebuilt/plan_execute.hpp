// Defines the prebuilt plan-execute scenario wrapper that keeps planning,
// execution, and replanning on top of existing chat-model contracts.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/adk/event_stream.hpp"
#include "wh/adk/types.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/flow/agent/utils.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/serialization/registry.hpp"
#include "wh/schema/stream/reader.hpp"

namespace wh::adk::prebuilt {

/// One materialized plan-execute state snapshot.
struct plan_execute_state {
  /// User input text frozen before the planner runs.
  std::string user_input{};
  /// Current active plan represented as ordered step texts.
  std::vector<std::string> current_plan{};
  /// Ordered executed step texts.
  std::vector<std::string> executed_steps{};
  /// Ordered execution result texts written after each executor turn.
  std::vector<std::string> execution_history{};
  /// Final text produced by the respond branch, when any.
  std::optional<std::string> final_text{};
  /// Named final outputs materialized for callers.
  std::unordered_map<std::string, std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      output_values{};
};

/// Immutable execution report returned by one plan-execute run.
struct plan_execute_report {
  /// Number of planner invocations performed during the run.
  std::size_t planner_calls{0U};
  /// Number of executor invocations performed during the run.
  std::size_t executor_calls{0U};
  /// Number of replanner invocations performed during the run.
  std::size_t replanner_calls{0U};
  /// Number of completed execute-replan iterations.
  std::size_t iterations{0U};
  /// Terminal error when the scenario did not complete successfully.
  std::optional<wh::core::error_code> final_error{};
  /// True once the scenario converged through the respond branch.
  bool completed{false};
};

/// Final result bundle returned by one plan-execute run.
struct plan_execute_result {
  /// Event stream emitted by the wrapped scenario.
  agent_event_stream_reader events{};
  /// Final mutable scenario state.
  plan_execute_state state{};
  /// Immutable execution report.
  plan_execute_report report{};
};

/// Frozen authoring options for one plan-execute wrapper.
struct plan_execute_options {
  /// Maximum execute-replan loop iterations. Zero falls back to 8.
  std::size_t max_iterations{8U};
  /// True forwards planner/executor/replanner messages into the public event stream.
  bool emit_internal_events{true};
  /// True appends one terminal state snapshot event after the main event chain.
  bool emit_state_snapshot{true};
  /// Optional final-output slot name written into `state.output_values`.
  std::string output_key{};
  /// Stable planner phase label used by event metadata.
  std::string planner_name{"planner"};
  /// Stable executor phase label used by event metadata.
  std::string executor_name{"executor"};
  /// Stable replanner phase label used by event metadata.
  std::string replanner_name{"replanner"};
};

namespace detail {

enum class plan_execute_decision_kind {
  plan = 0U,
  respond,
};

struct plan_execute_decision {
  plan_execute_decision_kind kind{plan_execute_decision_kind::plan};
  std::vector<std::string> next_plan{};
  std::string response_text{};
};

[[nodiscard]] inline auto trim_ascii(std::string value) -> std::string {
  auto first = std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) == 0;
  });
  auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) == 0;
  }).base();
  if (first >= last) {
    return {};
  }
  return std::string{first, last};
}

[[nodiscard]] inline auto split_non_empty_lines(const std::string_view text)
    -> std::vector<std::string> {
  std::vector<std::string> lines{};
  std::string current{};
  for (const char ch : text) {
    if (ch == '\n') {
      auto trimmed = trim_ascii(std::move(current));
      if (!trimmed.empty()) {
        lines.push_back(std::move(trimmed));
      }
      current.clear();
      continue;
    }
    if (ch != '\r') {
      current.push_back(ch);
    }
  }
  auto trimmed = trim_ascii(std::move(current));
  if (!trimmed.empty()) {
    lines.push_back(std::move(trimmed));
  }
  return lines;
}

[[nodiscard]] inline auto plan_execute_run_path(
    const std::string_view scenario_name, const std::string_view phase_name)
    -> run_path {
  return run_path{{"agent", scenario_name, phase_name}};
}

inline auto set_plan_execute_sequence_id(event_metadata &metadata,
                                         const std::size_t sequence_id)
    -> void {
  metadata.attributes.insert_or_assign("sequence_id", wh::core::any{sequence_id});
}

inline auto append_plan_execute_message_event(
    std::vector<agent_event> &events, const std::string_view scenario_name,
    const std::string_view phase_name, wh::schema::message message) -> void {
  event_metadata metadata{};
  metadata.run_path = plan_execute_run_path(scenario_name, phase_name);
  metadata.agent_name = std::string{scenario_name};
  set_plan_execute_sequence_id(metadata, events.size());
  events.push_back(make_message_event(std::move(message), std::move(metadata)));
}

inline auto append_plan_execute_error_event(
    std::vector<agent_event> &events, const std::string_view scenario_name,
    const std::string_view phase_name, const wh::core::error_code error,
    std::string message) -> void {
  event_metadata metadata{};
  metadata.run_path = plan_execute_run_path(scenario_name, phase_name);
  metadata.agent_name = std::string{scenario_name};
  set_plan_execute_sequence_id(metadata, events.size());
  events.push_back(
      make_error_event(error, std::move(message), {}, std::move(metadata)));
}

inline auto append_plan_execute_snapshot_event(
    std::vector<agent_event> &events, const std::string_view scenario_name,
    const plan_execute_state &state) -> void {
  event_metadata metadata{};
  metadata.run_path = plan_execute_run_path(scenario_name, "snapshot");
  metadata.agent_name = std::string{scenario_name};
  set_plan_execute_sequence_id(metadata, events.size());
  events.push_back(make_custom_event("plan_execute_state_snapshot",
                                     wh::core::any{state}, std::move(metadata)));
}

[[nodiscard]] inline auto
make_plan_execute_event_reader(std::vector<agent_event> events)
    -> agent_event_stream_reader {
  return agent_event_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(events))};
}

[[nodiscard]] inline auto
capture_plan_execute_user_input(const wh::model::chat_request &request)
    -> std::string {
  std::string text{};
  for (const auto &message : request.messages) {
    if (message.role != wh::schema::message_role::user) {
      continue;
    }
    const auto rendered = wh::flow::agent::render_message_text(message);
    if (rendered.empty()) {
      continue;
    }
    if (!text.empty()) {
      text.push_back('\n');
    }
    text.append(rendered);
  }
  return text;
}

[[nodiscard]] inline auto
parse_plan_execute_message(const wh::schema::message &message)
    -> wh::core::result<std::vector<std::string>> {
  for (const auto &part : message.parts) {
    const auto *tool = std::get_if<wh::schema::tool_call_part>(&part);
    if (tool == nullptr) {
      continue;
    }
    if (tool->name != "plan") {
      return wh::core::result<std::vector<std::string>>::failure(
          wh::core::errc::invalid_argument);
    }
    auto lines = split_non_empty_lines(tool->arguments);
    if (lines.empty()) {
      return wh::core::result<std::vector<std::string>>::failure(
          wh::core::errc::invalid_argument);
    }
    return lines;
  }

  auto lines =
      split_non_empty_lines(wh::flow::agent::render_message_text(message));
  if (lines.empty()) {
    return wh::core::result<std::vector<std::string>>::failure(
        wh::core::errc::invalid_argument);
  }
  return lines;
}

[[nodiscard]] inline auto
parse_plan_execute_replanner_message(const wh::schema::message &message)
    -> wh::core::result<plan_execute_decision> {
  for (const auto &part : message.parts) {
    const auto *tool = std::get_if<wh::schema::tool_call_part>(&part);
    if (tool == nullptr) {
      continue;
    }
    if (tool->name == "plan") {
      auto next_plan = split_non_empty_lines(tool->arguments);
      if (next_plan.empty()) {
        return wh::core::result<plan_execute_decision>::failure(
            wh::core::errc::invalid_argument);
      }
      return plan_execute_decision{
          .kind = plan_execute_decision_kind::plan,
          .next_plan = std::move(next_plan),
      };
    }
    if (tool->name == "respond") {
      auto response_text = trim_ascii(tool->arguments);
      if (response_text.empty()) {
        return wh::core::result<plan_execute_decision>::failure(
            wh::core::errc::invalid_argument);
      }
      return plan_execute_decision{
          .kind = plan_execute_decision_kind::respond,
          .response_text = std::move(response_text),
      };
    }
    return wh::core::result<plan_execute_decision>::failure(
        wh::core::errc::invalid_argument);
  }

  return wh::core::result<plan_execute_decision>::failure(
      wh::core::errc::invalid_argument);
}

[[nodiscard]] inline auto build_plan_execute_executor_request(
    const wh::model::chat_request &base, const plan_execute_state &state,
    const std::string_view current_step) -> wh::model::chat_request {
  wh::model::chat_request request = base;
  wh::schema::message message{};
  message.role = wh::schema::message_role::system;
  std::string text{};
  text.append("Goal:\n");
  text.append(state.user_input);
  text.append("\nCurrent step:\n");
  text.append(current_step);
  if (!state.execution_history.empty()) {
    text.append("\nExecution history:\n");
    for (const auto &entry : state.execution_history) {
      text.append(entry);
      text.push_back('\n');
    }
  }
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  request.messages.push_back(std::move(message));
  return request;
}

[[nodiscard]] inline auto build_plan_execute_replanner_request(
    const wh::model::chat_request &base, const plan_execute_state &state,
    const std::string_view latest_result) -> wh::model::chat_request {
  wh::model::chat_request request = base;
  wh::schema::message message{};
  message.role = wh::schema::message_role::system;
  std::string text{};
  text.append("Goal:\n");
  text.append(state.user_input);
  text.append("\nLatest execution result:\n");
  text.append(latest_result);
  if (!state.current_plan.empty()) {
    text.append("\nCurrent plan:\n");
    for (const auto &step : state.current_plan) {
      text.append(step);
      text.push_back('\n');
    }
  }
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  request.messages.push_back(std::move(message));
  return request;
}

} // namespace detail

/// Prebuilt plan-execute scenario wrapper that reuses existing chat-model
/// contracts and emits ADK events without introducing a second runtime.
template <wh::model::chat_model_like planner_model_t,
          wh::model::chat_model_like executor_model_t,
          wh::model::chat_model_like replanner_model_t = planner_model_t>
class plan_execute {
public:
  /// Creates one prebuilt plan-execute scenario from the required name and
  /// staged models.
  plan_execute(std::string name, planner_model_t planner,
               executor_model_t executor, replanner_model_t replanner) noexcept
      : name_(std::move(name)), planner_(std::move(planner)),
        executor_(std::move(executor)), replanner_(std::move(replanner)) {}

  plan_execute(const plan_execute &) = delete;
  auto operator=(const plan_execute &) -> plan_execute & = delete;
  plan_execute(plan_execute &&) noexcept = default;
  auto operator=(plan_execute &&) noexcept -> plan_execute & = default;
  ~plan_execute() = default;

  /// Returns the stable scenario name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the frozen authoring options.
  [[nodiscard]] auto options() const noexcept -> const plan_execute_options & {
    return options_;
  }

  /// Replaces the frozen authoring options.
  auto set_options(plan_execute_options options) -> wh::core::result<void> {
    options_ = std::move(options);
    return {};
  }

  /// Replaces the loop iteration cap. Zero falls back to 8.
  auto set_max_iterations(const std::size_t max_iterations)
      -> wh::core::result<void> {
    options_.max_iterations = max_iterations;
    return {};
  }

  /// Replaces the optional final-output slot name.
  auto set_output_key(std::string output_key) -> wh::core::result<void> {
    options_.output_key = std::move(output_key);
    return {};
  }

  /// Registers the scenario state types into the supplied serialization registry.
  static auto register_state_types(wh::schema::serialization_registry &registry)
      -> wh::core::result<void> {
    auto state = registry.register_type<plan_execute_state>(
        "wh.adk.prebuilt.plan_execute.state");
    if (state.has_error() && state.error() != wh::core::errc::already_exists) {
      return state;
    }
    auto report = registry.register_type<plan_execute_report>(
        "wh.adk.prebuilt.plan_execute.report");
    if (report.has_error() && report.error() != wh::core::errc::already_exists) {
      return report;
    }
    return {};
  }

  /// Runs the full planner-executor-replanner loop once.
  auto run(const wh::model::chat_request &request, wh::core::run_context &context) const
      -> wh::core::result<plan_execute_result> {
    if (name_.empty()) {
      return wh::core::result<plan_execute_result>::failure(
          wh::core::errc::invalid_argument);
    }

    plan_execute_state state{};
    plan_execute_report report{};
    std::vector<agent_event> events{};
    state.user_input = detail::capture_plan_execute_user_input(request);

    auto planner_status = planner_.invoke(request, context);
    ++report.planner_calls;
    if (planner_status.has_error()) {
      report.final_error = planner_status.error();
      detail::append_plan_execute_error_event(events, name_,
                                              options_.planner_name,
                                              planner_status.error(),
                                              "planner failed");
      return plan_execute_result{
          .events = detail::make_plan_execute_event_reader(std::move(events)),
          .state = std::move(state),
          .report = std::move(report),
      };
    }
    if (options_.emit_internal_events) {
      detail::append_plan_execute_message_event(events, name_,
                                                options_.planner_name,
                                                planner_status.value().message);
    }
    auto initial_plan =
        detail::parse_plan_execute_message(planner_status.value().message);
    if (initial_plan.has_error()) {
      report.final_error = initial_plan.error();
      detail::append_plan_execute_error_event(events, name_,
                                              options_.planner_name,
                                              initial_plan.error(),
                                              "planner output invalid");
      return plan_execute_result{
          .events = detail::make_plan_execute_event_reader(std::move(events)),
          .state = std::move(state),
          .report = std::move(report),
      };
    }
    state.current_plan = std::move(initial_plan).value();

    const auto max_iterations =
        options_.max_iterations == 0U ? 8U : options_.max_iterations;
    while (report.iterations < max_iterations) {
      if (state.current_plan.empty()) {
        report.final_error = wh::core::make_error(wh::core::errc::invalid_argument);
        detail::append_plan_execute_error_event(
            events, name_, options_.replanner_name, *report.final_error,
            "empty plan");
        break;
      }

      const auto current_step = state.current_plan.front();
      auto executor_request = detail::build_plan_execute_executor_request(
          request, state, current_step);
      auto executor_status = executor_.invoke(executor_request, context);
      ++report.executor_calls;
      if (executor_status.has_error()) {
        report.final_error = executor_status.error();
        detail::append_plan_execute_error_event(
            events, name_, options_.executor_name, executor_status.error(),
            "executor failed");
        break;
      }
      if (options_.emit_internal_events) {
        detail::append_plan_execute_message_event(
            events, name_, options_.executor_name,
            executor_status.value().message);
      }

      const auto executed_text =
          wh::flow::agent::render_message_text(executor_status.value().message);
      state.executed_steps.push_back(current_step);
      state.execution_history.push_back(executed_text);

      auto replanner_request = detail::build_plan_execute_replanner_request(
          request, state, executed_text);
      auto replanner_status = replanner_.invoke(replanner_request, context);
      ++report.replanner_calls;
      if (replanner_status.has_error()) {
        report.final_error = replanner_status.error();
        detail::append_plan_execute_error_event(
            events, name_, options_.replanner_name,
            replanner_status.error(), "replanner failed");
        break;
      }
      if (options_.emit_internal_events) {
        detail::append_plan_execute_message_event(
            events, name_, options_.replanner_name,
            replanner_status.value().message);
      }

      auto decision =
          detail::parse_plan_execute_replanner_message(
              replanner_status.value().message);
      if (decision.has_error()) {
        report.final_error = decision.error();
        detail::append_plan_execute_error_event(
            events, name_, options_.replanner_name, decision.error(),
            "replanner decision invalid");
        break;
      }

      ++report.iterations;
      if (decision.value().kind == detail::plan_execute_decision_kind::respond) {
        report.completed = true;
        state.final_text = std::move(decision).value().response_text;
        if (!options_.output_key.empty()) {
          state.output_values.insert_or_assign(options_.output_key,
                                               *state.final_text);
        }
        detail::append_plan_execute_message_event(
            events, name_, "final",
            wh::schema::message{
                .role = wh::schema::message_role::assistant,
                .parts = {wh::schema::text_part{*state.final_text}},
            });
        break;
      }

      state.current_plan = std::move(decision).value().next_plan;
    }

    if (!report.completed && !report.final_error.has_value() &&
        report.iterations >= max_iterations) {
      report.final_error =
          wh::core::make_error(wh::core::errc::resource_exhausted);
      detail::append_plan_execute_error_event(
          events, name_, options_.replanner_name, *report.final_error,
          "iteration budget exhausted");
    }

    if (options_.emit_state_snapshot) {
      detail::append_plan_execute_snapshot_event(events, name_, state);
    }

    return plan_execute_result{
        .events = detail::make_plan_execute_event_reader(std::move(events)),
        .state = std::move(state),
        .report = std::move(report),
    };
  }

private:
  /// Stable scenario name.
  std::string name_{};
  /// Planner model executed before the loop starts.
  planner_model_t planner_;
  /// Executor model executed inside the main loop.
  executor_model_t executor_;
  /// Replanner model executed after each executor step.
  replanner_model_t replanner_;
  /// Frozen authoring options.
  plan_execute_options options_{};
};

} // namespace wh::adk::prebuilt
