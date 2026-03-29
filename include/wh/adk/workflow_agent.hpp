// Defines authored workflow-agent metadata plus thin runtime lowering onto one
// compose node, reusing child agent runners instead of introducing a second
// workflow runtime.
#pragma once

#include <cstddef>
#include <concepts>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/adk/event_stream.hpp"
#include "wh/adk/utils.hpp"
#include "wh/adk/workflow.hpp"
#include "wh/agent/agent.hpp"
#include "wh/compose/flow/chain.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/model/chat_model.hpp"

namespace wh::adk {

/// Final runtime state emitted by one workflow-agent execution.
struct workflow_agent_state {
  /// Conversation/request state carried into the next workflow step.
  wh::model::chat_request request{};
  /// Ordered completed step keys used for diagnostics and resume bookkeeping.
  std::vector<std::string> completed_steps{};
  /// Number of loop iterations fully consumed by this run.
  std::size_t consumed_iterations{0U};
  /// True when one break-loop control action terminated loop execution.
  bool break_requested{false};
  /// Optional final message observed on the emitted event sequence.
  std::optional<wh::schema::message> final_message{};
  /// Optional terminal error captured while executing one child step.
  std::optional<wh::core::error_code> final_error{};
};

/// Final result returned by workflow-agent runs.
struct workflow_agent_result {
  /// Event stream visible to the workflow caller.
  agent_event_stream_reader events{};
  /// Final runtime state after this workflow execution.
  workflow_agent_state state{};
};

namespace detail {

using workflow_run_status = wh::core::result<workflow_agent_result>;
using workflow_iteration_status = wh::core::result<void>;

template <typename value_t>
[[nodiscard]] inline auto read_graph_ref(wh::compose::graph_value &value)
    -> wh::core::result<std::reference_wrapper<value_t>> {
  if (auto *typed = wh::core::any_cast<std::reference_wrapper<value_t>>(&value);
      typed != nullptr) {
    return typed->get();
  }
  return wh::core::result<std::reference_wrapper<value_t>>::failure(
      wh::core::errc::type_mismatch);
}

using workflow_step_runner = wh::core::callback_function<
    agent_run_result(const wh::model::chat_request &, wh::core::run_context &) const>;

/// Frozen workflow step with its already-resolved child runner.
struct bound_workflow_step {
  /// Stable authored step metadata.
  workflow_step step{};
  /// Bound child execution entrypoint.
  workflow_step_runner runner{nullptr};
};

/// One isolated branch result produced by parallel workflow lowering.
struct workflow_parallel_branch_result {
  /// Branch-local workflow state after executing one child step.
  workflow_agent_state state{};
  /// Branch-local event list after run-path projection.
  std::vector<agent_event> events{};
};

/// Runtime-only workflow execution state that keeps move-only events out of
/// the graph value boundary.
struct workflow_runtime_state {
  /// Copyable report/state returned to the workflow caller.
  workflow_agent_state state{};
  /// Ordered events emitted by child steps after workflow run-path projection.
  std::vector<agent_event> events{};
  /// Parallel branch results written by branch nodes before merge runs.
  std::vector<std::optional<workflow_parallel_branch_result>> parallel_results{};
};

template <typename value_t> struct mutable_box {
  mutable value_t value;
};

template <typename value_t>
concept workflow_step_output_value =
    std::same_as<std::remove_cvref_t<value_t>, agent_run_output> ||
    std::same_as<std::remove_cvref_t<value_t>, agent_event_stream_reader> ||
    requires(value_t value) {
      { std::move(value.events) } -> std::same_as<agent_event_stream_reader &&>;
    };

template <typename result_t>
concept workflow_step_result =
    requires(result_t result) {
      result.has_value();
      result.has_error();
      result.error();
      requires workflow_step_output_value<
          typename std::remove_cvref_t<result_t>::value_type>;
    };

template <workflow_step_output_value value_t>
[[nodiscard]] inline auto
to_agent_run_output(value_t value) -> agent_run_output {
  if constexpr (std::same_as<std::remove_cvref_t<value_t>, agent_run_output>) {
    return value;
  } else if constexpr (std::same_as<std::remove_cvref_t<value_t>,
                                    agent_event_stream_reader>) {
    return agent_run_output{.events = std::move(value)};
  } else {
    agent_run_output output{};
    output.events = std::move(value.events);
    if constexpr (requires { value.final_message; }) {
      output.final_message = std::move(value.final_message);
    } else if constexpr (requires { value.report.final_message; }) {
      output.final_message = std::move(value.report.final_message);
    }
    if constexpr (requires { value.output_values; }) {
      output.output_values = std::move(value.output_values);
    } else if constexpr (requires { value.state.output_values; }) {
      output.output_values = std::move(value.state.output_values);
    }
    return output;
  }
}

template <workflow_step_result result_t>
[[nodiscard]] inline auto
to_agent_run_result(result_t result) -> agent_run_result {
  if (result.has_error()) {
    return agent_run_result::failure(result.error());
  }
  return to_agent_run_output(std::move(result).value());
}

template <typename runner_t>
concept workflow_runner_object =
    requires(runner_t &runner, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      requires workflow_step_result<decltype(runner.run(request, context))>;
    };

template <typename runner_t>
concept workflow_runner_callable =
    requires(runner_t &runner, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      requires workflow_step_result<decltype(std::invoke(runner, request, context))>;
    };

template <typename runner_t>
concept workflow_bindable_runner =
    std::copy_constructible<std::remove_cvref_t<runner_t>> &&
    (workflow_runner_object<std::remove_cvref_t<runner_t>> ||
     workflow_runner_callable<std::remove_cvref_t<runner_t>>);

template <typename runner_t>
[[nodiscard]] inline auto dispatch_workflow_runner(
    runner_t &runner, const wh::model::chat_request &request,
    wh::core::run_context &context) {
  if constexpr (workflow_runner_object<runner_t>) {
    return runner.run(request, context);
  } else {
    return std::invoke(runner, request, context);
  }
}

[[nodiscard]] inline auto workflow_step_prefix(const std::string_view workflow_name,
                                               const std::string_view step_key,
                                               const std::string_view agent_name)
    -> run_path {
  return run_path{{"agent", workflow_name, "agent", agent_name, step_key}};
}

inline auto append_workflow_error(workflow_runtime_state &runtime_state,
                                  const std::string_view workflow_name,
                                  const std::string_view step_key,
                                  const std::string_view agent_name,
                                  const wh::core::error_code error,
                                  const std::string_view message) -> void {
  runtime_state.state.final_error = error;
  runtime_state.events.push_back(make_error_event(
      error, std::string{message}, {},
      event_metadata{
          .run_path = workflow_step_prefix(workflow_name, step_key, agent_name),
          .agent_name = std::string{agent_name},
      }));
}

inline auto absorb_normalized_events(workflow_runtime_state &runtime_state,
                                     std::vector<agent_event> events)
    -> wh::core::result<void> {
  for (auto &event : events) {
    if (auto *message = std::get_if<message_event>(&event.payload);
        message != nullptr) {
      auto snapshots = snapshot_message_event(std::move(*message));
      if (snapshots.has_error()) {
        return wh::core::result<void>::failure(snapshots.error());
      }
      for (auto &entry : snapshots.value()) {
        runtime_state.state.request.messages.push_back(entry);
        runtime_state.state.final_message = entry;
        runtime_state.events.push_back(
            make_message_event(std::move(entry), std::move(event.metadata)));
      }
      continue;
    }

    if (const auto *control = std::get_if<control_action>(&event.payload);
        control != nullptr &&
        control->kind == control_action_kind::break_loop) {
      runtime_state.state.break_requested = true;
    }
    if (const auto *error = std::get_if<error_event>(&event.payload);
        error != nullptr) {
      runtime_state.state.final_error = error->code;
    }
    runtime_state.events.push_back(std::move(event));
  }
  return {};
}

inline auto execute_step(workflow_runtime_state &runtime_state,
                         const bound_workflow_step &step,
                         const std::string_view workflow_name,
                         wh::core::run_context &context) -> wh::core::result<void> {
  auto run_result = step.runner(runtime_state.state.request, context);
  if (run_result.has_error()) {
    append_workflow_error(runtime_state, workflow_name, step.step.key,
                          step.step.agent_name, run_result.error(),
                          "workflow step failed");
    return {};
  }

  auto events = collect_agent_events(std::move(run_result).value().events);
  if (events.has_error()) {
    append_workflow_error(runtime_state, workflow_name, step.step.key,
                          step.step.agent_name, events.error(),
                          "workflow step event collection failed");
    return {};
  }

  auto prefix =
      workflow_step_prefix(workflow_name, step.step.key, step.step.agent_name);
  std::vector<agent_event> normalized{};
  normalized.reserve(events.value().size());
  for (auto &event : events.value()) {
    normalized.push_back(prefix_agent_event(std::move(event), prefix));
  }
  auto absorbed = absorb_normalized_events(runtime_state, std::move(normalized));
  if (absorbed.has_error()) {
    append_workflow_error(runtime_state, workflow_name, step.step.key,
                          step.step.agent_name, absorbed.error(),
                          "workflow step message snapshot failed");
    return {};
  }

  runtime_state.state.completed_steps.push_back(step.step.key);
  return {};
}

inline auto merge_parallel_branch_result(
    workflow_runtime_state &runtime_state,
    workflow_parallel_branch_result branch_result,
    const std::size_t base_message_count) -> void;

inline auto merge_parallel_branch_result(
    workflow_runtime_state &runtime_state,
    workflow_parallel_branch_result branch_result,
    const std::size_t base_message_count) -> void {
  if (branch_result.state.request.messages.size() > base_message_count) {
    auto delta_begin =
        branch_result.state.request.messages.begin() +
        static_cast<std::ptrdiff_t>(base_message_count);
    runtime_state.state.request.messages.insert(
        runtime_state.state.request.messages.end(),
        std::make_move_iterator(delta_begin),
        std::make_move_iterator(branch_result.state.request.messages.end()));
  }
  runtime_state.events.insert(runtime_state.events.end(),
                              std::make_move_iterator(branch_result.events.begin()),
                              std::make_move_iterator(branch_result.events.end()));
  runtime_state.state.completed_steps.insert(
      runtime_state.state.completed_steps.end(),
      std::make_move_iterator(branch_result.state.completed_steps.begin()),
      std::make_move_iterator(branch_result.state.completed_steps.end()));
  if (branch_result.state.final_message.has_value()) {
    runtime_state.state.final_message = std::move(branch_result.state.final_message);
  }
  runtime_state.state.break_requested =
      runtime_state.state.break_requested || branch_result.state.break_requested;
  if (branch_result.state.final_error.has_value()) {
    runtime_state.state.final_error = branch_result.state.final_error;
  }
}

[[nodiscard]] inline auto
make_graph_output_sender(const wh::compose::graph &graph,
                         wh::core::run_context &context,
                         wh::compose::graph_value input) {
  wh::compose::graph_invoke_request request{};
  request.input = std::move(input);
  return wh::core::detail::normalize_result_sender<wh::core::result<wh::compose::graph_value>>(
      wh::core::detail::map_result_sender<wh::core::result<wh::compose::graph_value>>(
          graph.invoke(context, std::move(request)),
          [](wh::compose::graph_invoke_result invoke_result) {
            return std::move(invoke_result.output_status);
          }));
}

[[nodiscard]] inline auto
make_workflow_result(std::unique_ptr<workflow_runtime_state> runtime_state)
    -> workflow_agent_result {
  return workflow_agent_result{
      .events = agent_event_stream_reader{
          wh::schema::stream::make_values_stream_reader(
              std::move(runtime_state->events))},
      .state = std::move(runtime_state->state),
  };
}

[[nodiscard]] inline auto
make_workflow_single_sender(const wh::compose::graph &graph,
                            wh::core::run_context &context,
                            std::unique_ptr<workflow_runtime_state> runtime_state) {
  return wh::core::detail::normalize_result_sender<workflow_run_status>(
      wh::core::detail::map_result_sender<workflow_run_status>(
          make_graph_output_sender(
              graph, context,
              wh::compose::graph_value{wh::core::any(std::ref(*runtime_state))}),
          [runtime_state = std::move(runtime_state)](
              wh::compose::graph_value &&) mutable {
            return make_workflow_result(std::move(runtime_state));
          }));
}

[[nodiscard]] inline auto
make_workflow_iteration_sender(const wh::compose::graph &graph,
                               wh::core::run_context &context,
                               workflow_runtime_state &runtime_state) {
  return wh::core::detail::normalize_result_sender<workflow_iteration_status>(
      wh::core::detail::map_result_sender<workflow_iteration_status>(
          make_graph_output_sender(
              graph, context,
              wh::compose::graph_value{wh::core::any(std::ref(runtime_state))}),
          [](wh::compose::graph_value &&) -> void {}));
}

struct workflow_loop_state {
  const wh::compose::graph *graph{nullptr};
  wh::core::run_context *context{nullptr};
  std::unique_ptr<workflow_runtime_state> runtime_state{};
  std::optional<std::size_t> max_iterations{};
};

class workflow_loop_sender {
  template <typename receiver_t> class operation {
    using receiver_env_t =
        decltype(stdexec::get_env(std::declval<const receiver_t &>()));

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;
      operation *op{nullptr};
      receiver_env_t env_{};

      auto set_value(workflow_iteration_status status) && noexcept -> void {
        op->on_child_value(std::move(status));
      }

      template <typename error_t>
      auto set_error(error_t &&) && noexcept -> void {
        op->complete_failure(wh::core::errc::internal_error);
      }

      auto set_stopped() && noexcept -> void {
        op->complete_failure(wh::core::errc::canceled);
      }

      [[nodiscard]] auto get_env() const noexcept { return env_; }
    };

    using child_sender_t = decltype(make_workflow_iteration_sender(
        std::declval<const wh::compose::graph &>(),
        std::declval<wh::core::run_context &>(),
        std::declval<workflow_runtime_state &>()));
    using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

  public:
    explicit operation(workflow_loop_state state, receiver_t receiver)
        : receiver_(std::move(receiver)), state_(std::move(state)) {}

    auto start() & noexcept -> void { pump(); }

  private:
    [[nodiscard]] auto reached_iteration_limit() const noexcept -> bool {
      return state_.max_iterations.has_value() && *state_.max_iterations != 0U &&
             state_.runtime_state->state.consumed_iterations >= *state_.max_iterations;
    }

    auto pump() noexcept -> void {
      if (failure_.has_value()) {
        complete();
        return;
      }
      while (!terminal_ready_) {
        start_next_iteration();
        if (iteration_pending_) {
          return;
        }
      }
      complete();
    }

    auto start_next_iteration() noexcept -> void {
      if (state_.graph == nullptr || state_.context == nullptr ||
          state_.runtime_state == nullptr) {
        complete_failure(wh::core::errc::contract_violation);
        return;
      }
      if (reached_iteration_limit()) {
        terminal_ready_ = true;
        return;
      }

      iteration_pending_ = true;
      inline_completion_ = false;
      starting_child_ = true;

      try {
        child_op_.emplace_from(
            stdexec::connect,
            make_workflow_iteration_sender(*state_.graph, *state_.context,
                                           *state_.runtime_state),
            child_receiver{this, stdexec::get_env(receiver_)});
      } catch (...) {
        iteration_pending_ = false;
        inline_completion_ = false;
        starting_child_ = false;
        complete_failure(wh::core::errc::internal_error);
        return;
      }

      stdexec::start(child_op_.get());
      starting_child_ = false;

      if (!inline_completion_) {
        return;
      }

      iteration_pending_ = false;
      child_op_.reset();
    }

    auto on_child_value(workflow_iteration_status status) noexcept -> void {
      if (status.has_error()) {
        complete_failure(status.error());
        return;
      }

      ++state_.runtime_state->state.consumed_iterations;
      terminal_ready_ = state_.runtime_state->state.break_requested ||
                        state_.runtime_state->state.final_error.has_value() ||
                        reached_iteration_limit();

      if (starting_child_) {
        inline_completion_ = true;
        return;
      }

      iteration_pending_ = false;
      child_op_.reset();
      pump();
    }

    auto complete_failure(const wh::core::error_code error) noexcept -> void {
      failure_ = error;
      terminal_ready_ = true;
      if (starting_child_) {
        inline_completion_ = true;
        return;
      }
      iteration_pending_ = false;
      child_op_.reset();
      pump();
    }

    auto complete() noexcept -> void {
      if (failure_.has_value()) {
        stdexec::set_value(std::move(receiver_),
                           workflow_run_status::failure(*failure_));
        return;
      }
      stdexec::set_value(
          std::move(receiver_),
          workflow_run_status{make_workflow_result(std::move(state_.runtime_state))});
    }

    receiver_t receiver_;
    workflow_loop_state state_{};
    wh::core::detail::manual_lifetime_box<child_op_t> child_op_{};
    std::optional<wh::core::error_code> failure_{};
    bool iteration_pending_{false};
    bool starting_child_{false};
    bool inline_completion_{false};
    bool terminal_ready_{false};
  };

public:
  using sender_concept = stdexec::sender_t;

  explicit workflow_loop_sender(workflow_loop_state state) noexcept
      : state_(std::move(state)) {}

  template <typename self_t, stdexec::receiver receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, workflow_loop_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>)
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver) {
    return operation<receiver_t>{std::forward<self_t>(self).state_,
                                 std::move(receiver)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, workflow_loop_sender> &&
             (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    return stdexec::completion_signatures<
        stdexec::set_value_t(workflow_run_status)>{};
  }

private:
  template <typename> friend class operation;

  workflow_loop_state state_{};
};

} // namespace detail

/// Authored workflow-agent shell that binds one root agent name to one frozen
/// workflow description and a named child-agent set.
class workflow_agent {
public:
  /// Creates one workflow-agent shell from authored metadata and workflow.
  workflow_agent(std::string name, std::string description, workflow source) noexcept
      : name_(std::move(name)), description_(std::move(description)),
        root_(name_), source_(std::move(source)) {}

  workflow_agent(const workflow_agent &) = delete;
  auto operator=(const workflow_agent &) -> workflow_agent & = delete;
  workflow_agent(workflow_agent &&) noexcept = default;
  auto operator=(workflow_agent &&) noexcept -> workflow_agent & = default;
  ~workflow_agent() = default;

  /// Returns the authored workflow-agent name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the authored workflow-agent description.
  [[nodiscard]] auto description() const noexcept -> std::string_view {
    return description_;
  }

  /// Returns true after metadata, child bindings, and workflow shape freeze.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Adds one named child agent before freeze.
  auto add_agent(wh::agent::agent &&child) -> wh::core::result<void> {
    return root_.add_child(std::move(child));
  }

  /// Binds one executable child runner by authored agent name.
  auto bind_runner(std::string name, detail::workflow_step_runner runner)
      -> wh::core::result<void> {
    if (runtime_graph_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (name.empty() || !static_cast<bool>(runner)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    step_runners_.insert_or_assign(std::move(name), std::move(runner));
    return {};
  }

  /// Binds one executable child runner by authored agent name.
  template <detail::workflow_bindable_runner runner_t>
  auto bind_runner(const std::string_view name, runner_t &&runner)
      -> wh::core::result<void> {
    using stored_runner_t = std::remove_cvref_t<runner_t>;
    return bind_runner(
        std::string{name},
        detail::workflow_step_runner{
            [runner_box = detail::mutable_box<stored_runner_t>{
                 stored_runner_t{std::forward<runner_t>(runner)}}](
                const wh::model::chat_request &request,
                wh::core::run_context &context) -> agent_run_result {
              return detail::to_agent_run_result(
                  detail::dispatch_workflow_runner(runner_box.value, request,
                                                   context));
            }});
  }

  /// Returns the authored root agent shell.
  [[nodiscard]] auto root_agent() const noexcept -> const wh::agent::agent & {
    return root_;
  }

  /// Returns the frozen workflow description used by this agent.
  [[nodiscard]] auto workflow_definition() const noexcept -> const workflow & {
    return source_;
  }

  /// Validates authored metadata, step bindings, and child topology.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || description_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    auto workflow_frozen = source_.freeze();
    if (workflow_frozen.has_error()) {
      return workflow_frozen;
    }
    for (const auto &step : source_.steps()) {
      auto child = root_.child(step.agent_name);
      if (child.has_error()) {
        return wh::core::result<void>::failure(child.error());
      }
    }
    auto root_frozen = root_.freeze();
    if (root_frozen.has_error()) {
      return root_frozen;
    }
    frozen_ = true;
    return {};
  }

private:
  /// Runs the frozen workflow body with one owned request payload.
  [[nodiscard]] auto execute(wh::model::chat_request request,
                             wh::core::run_context &context) {
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<detail::workflow_run_status>(
            wh::core::errc::internal_error));
    using loop_sender_t = detail::workflow_loop_sender;
    using single_sender_t = decltype(detail::make_workflow_single_sender(
        std::declval<const wh::compose::graph &>(),
        std::declval<wh::core::run_context &>(),
        std::declval<std::unique_ptr<detail::workflow_runtime_state>>()));
    using dispatch_sender_t = wh::core::detail::variant_sender<
        failure_sender_t, loop_sender_t, single_sender_t>;

    return wh::core::detail::defer_result_sender<detail::workflow_run_status>(
            [this, request = std::move(request), &context]() mutable
                -> dispatch_sender_t {
              auto ready = ensure_runtime_ready();
              if (ready.has_error()) {
                return dispatch_sender_t{
                    wh::core::detail::failure_result_sender<
                        detail::workflow_run_status>(ready.error())};
              }
              if (!runtime_graph_.has_value()) {
                return dispatch_sender_t{
                    wh::core::detail::failure_result_sender<
                        detail::workflow_run_status>(
                        wh::core::errc::contract_violation)};
              }

              auto runtime_state = std::make_unique<detail::workflow_runtime_state>();
              runtime_state->state.request = std::move(request);
              if (source_.mode() == workflow_mode::parallel) {
                runtime_state->parallel_results.resize(source_.steps().size());
              }

              if (source_.mode() == workflow_mode::loop) {
                return dispatch_sender_t{detail::workflow_loop_sender{
                    detail::workflow_loop_state{
                        .graph = std::addressof(*runtime_graph_),
                        .context = std::addressof(context),
                        .runtime_state = std::move(runtime_state),
                        .max_iterations = source_.max_iterations(),
                    }}};
              }

              return dispatch_sender_t{detail::make_workflow_single_sender(
                  *runtime_graph_, context, std::move(runtime_state))};
            });
  }

public:
  /// Runs the lowered workflow body through one compiled compose node.
  [[nodiscard]] auto run(const wh::model::chat_request &request,
                         wh::core::run_context &context) {
    return execute(request, context);
  }

  /// Runs the lowered workflow body from a movable request payload.
  [[nodiscard]] auto run(wh::model::chat_request &&request,
                         wh::core::run_context &context) {
    return execute(std::move(request), context);
  }
  /// Builds the runtime compose node once child runner bindings are ready.
  auto ensure_runtime_ready() -> wh::core::result<void> {
    auto frozen = freeze();
    if (frozen.has_error()) {
      return frozen;
    }
    if (runtime_graph_.has_value()) {
      return {};
    }
    for (const auto &step : source_.steps()) {
      if (!step_runners_.contains(step.agent_name)) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
    }

    std::vector<detail::bound_workflow_step> steps{};
    steps.reserve(source_.steps().size());
    for (const auto &step : source_.steps()) {
      auto iter = step_runners_.find(step.agent_name);
      if (iter == step_runners_.end()) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
      steps.push_back(detail::bound_workflow_step{
          .step = step,
          .runner = iter->second,
      });
    }
    auto mode = source_.mode();
    if (mode == workflow_mode::sequential || mode == workflow_mode::loop) {
      wh::compose::chain chain{};
      for (const auto &step : steps) {
        auto added = chain.append(wh::compose::make_lambda_node(
            step.step.key,
            [step, workflow_name = name_](
                wh::compose::graph_value &input, wh::core::run_context &context,
                const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_value> {
              auto runtime_state =
                  detail::read_graph_ref<detail::workflow_runtime_state>(input);
              if (runtime_state.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    runtime_state.error());
              }
              auto &owned_state = runtime_state.value().get();
              auto executed =
                  detail::execute_step(owned_state, step, workflow_name, context);
              if (executed.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    executed.error());
              }
              return wh::compose::graph_value{wh::core::any(std::ref(owned_state))};
            }));
        if (added.has_error()) {
          return wh::core::result<void>::failure(added.error());
        }
      }
      auto compiled = chain.compile();
      if (compiled.has_error()) {
        return wh::core::result<void>::failure(compiled.error());
      }
      runtime_graph_ = std::move(chain).release_graph();
      step_runners_.clear();
      return {};
    }

    if (mode == workflow_mode::parallel) {
      constexpr std::string_view parallel_input_key =
          "workflow_agent_parallel_input";
      wh::compose::chain chain{};
      auto root_added = chain.append(wh::compose::make_lambda_node(
          std::string{parallel_input_key},
          [](wh::compose::graph_value &input, wh::core::run_context &,
             const wh::compose::graph_call_scope &)
              -> wh::core::result<wh::compose::graph_value> { return input; }));
      if (root_added.has_error()) {
        return wh::core::result<void>::failure(root_added.error());
      }

      wh::compose::parallel fanout{};
      for (std::size_t step_index = 0U; step_index < steps.size(); ++step_index) {
        const auto &step = steps[step_index];
        auto added = fanout.add_lambda(wh::compose::make_lambda_node(
            step.step.key,
            [step, step_index, workflow_name = name_](
                wh::compose::graph_value &input,
                wh::core::run_context &context,
                const wh::compose::graph_call_scope &)
                -> wh::core::result<wh::compose::graph_value> {
              auto runtime_state =
                  detail::read_graph_ref<detail::workflow_runtime_state>(input);
              if (runtime_state.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    runtime_state.error());
              }
              auto &shared_state = runtime_state.value().get();
              if (step_index >= shared_state.parallel_results.size()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    wh::core::errc::not_found);
              }
              detail::workflow_runtime_state branch{};
              branch.state.request = shared_state.state.request;
              auto executed =
                  detail::execute_step(branch, step, workflow_name, context);
              if (executed.has_error()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    executed.error());
              }
              shared_state.parallel_results[step_index] =
                  detail::workflow_parallel_branch_result{
                      .state = std::move(branch.state),
                      .events = std::move(branch.events),
                  };
              return wh::compose::graph_value{
                  wh::core::any(std::ref(shared_state))};
            }));
        if (added.has_error()) {
          return wh::core::result<void>::failure(added.error());
        }
      }
      auto fanout_added = chain.append_parallel(std::move(fanout));
      if (fanout_added.has_error()) {
        return wh::core::result<void>::failure(fanout_added.error());
      }

      auto merge_added = chain.append(wh::compose::make_lambda_node(
          "workflow_agent_parallel_merge",
          [steps = steps](
              wh::compose::graph_value &input, wh::core::run_context &,
              const wh::compose::graph_call_scope &)
              -> wh::core::result<wh::compose::graph_value> {
            auto *map_input = wh::core::any_cast<wh::compose::graph_value_map>(&input);
            if (map_input == nullptr) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::type_mismatch);
            }
            if (steps.empty()) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::contract_violation);
            }
            const auto runtime_iter = map_input->find(steps.front().step.key);
            if (runtime_iter == map_input->end()) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::not_found);
            }
            auto runtime_state =
                detail::read_graph_ref<detail::workflow_runtime_state>(
                    runtime_iter->second);
            if (runtime_state.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  runtime_state.error());
            }
            auto &owned_state = runtime_state.value().get();
            const auto base_message_count = owned_state.state.request.messages.size();
            for (std::size_t step_index = 0U; step_index < steps.size(); ++step_index) {
              const auto branch_iter = map_input->find(steps[step_index].step.key);
              if (branch_iter == map_input->end() ||
                  step_index >= owned_state.parallel_results.size() ||
                  !owned_state.parallel_results[step_index].has_value()) {
                return wh::core::result<wh::compose::graph_value>::failure(
                    wh::core::errc::not_found);
              }
              detail::merge_parallel_branch_result(
                  owned_state,
                  std::move(*owned_state.parallel_results[step_index]),
                  base_message_count);
              owned_state.parallel_results[step_index].reset();
              if (owned_state.state.final_error.has_value()) {
                break;
              }
            }
            return wh::compose::graph_value{wh::core::any(std::ref(owned_state))};
          }));
      if (merge_added.has_error()) {
        return wh::core::result<void>::failure(merge_added.error());
      }
      auto compiled = chain.compile();
      if (compiled.has_error()) {
        return wh::core::result<void>::failure(compiled.error());
      }
      runtime_graph_ = std::move(chain).release_graph();
      step_runners_.clear();
      return {};
    }

    return wh::core::result<void>::failure(wh::core::errc::not_supported);
  }

  /// Stable workflow-agent name used by future lowering and diagnostics.
  std::string name_{};
  /// Human-readable workflow-agent description.
  std::string description_{};
  /// Root authored agent shell that owns the named child-agent set.
  wh::agent::agent root_{""};
  /// Frozen workflow description bound to the root child-agent set.
  workflow source_{};
  /// Bound runtime runners keyed by authored child agent name.
  std::unordered_map<std::string, detail::workflow_step_runner,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      step_runners_{};
  /// Compiled compose graph used to run lowered workflow bodies.
  std::optional<wh::compose::graph> runtime_graph_{};
  /// True after authored metadata has been frozen successfully.
  bool frozen_{false};
};

} // namespace wh::adk
