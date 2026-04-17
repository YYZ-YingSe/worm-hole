#pragma once

#include "wh/compose/graph/detail/graph_class.hpp"

namespace wh::compose::detail {

auto start_session(const graph &graph,
                   detail::invoke_runtime::invoke_session session)
    -> graph_sender;

template <typename scope_t>
  requires std::same_as<std::remove_cvref_t<scope_t>, graph_call_options> ||
           std::same_as<std::remove_cvref_t<scope_t>, graph_call_scope>
auto start_session(const graph &graph, wh::core::run_context &context,
                   graph_value &&input, scope_t &&call_scope,
                   const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
                   node_path path_prefix = {},
                   graph_process_state *parent_process_state = nullptr,
                   detail::runtime_state::invoke_outputs *nested_outputs = nullptr,
                   const graph_runtime_services *services = nullptr,
                   graph_invoke_controls controls = {},
                   detail::runtime_state::invoke_outputs *published_outputs = nullptr,
                   const detail::invoke_runtime::invoke_session *parent_state = nullptr)
    -> graph_sender;

auto start_request(const graph &graph, wh::core::run_context &context,
                   graph_invoke_request request,
                   const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
                   detail::runtime_state::invoke_outputs *published_outputs = nullptr)
    -> graph_sender;

auto start_bound_graph(const graph &graph, wh::core::run_context &context,
                       graph_value &input,
                       const graph_call_options *call_options,
                       const node_path *path_prefix,
                       graph_process_state *parent_process_state,
                       detail::runtime_state::invoke_outputs *nested_outputs,
                       const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
                       const detail::invoke_runtime::invoke_session *parent_state,
                       const graph_runtime_services *services,
                       graph_invoke_controls controls) -> graph_sender;

auto start_scoped_graph(const graph &graph, wh::core::run_context &context, graph_value &input,
                        const graph_call_scope *call_scope, const node_path *path_prefix,
                        graph_process_state *parent_process_state,
                        detail::runtime_state::invoke_outputs *nested_outputs,
                        const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
                        const detail::invoke_runtime::invoke_session *parent_state = nullptr,
                        const graph_runtime_services *services = nullptr,
                        graph_invoke_controls controls = {}) -> graph_sender;

auto start_nested_graph(const graph &graph, wh::core::run_context &context,
                        graph_value &input, const node_runtime &runtime)
    -> graph_sender;

} // namespace wh::compose::detail

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto make_graph_run_report(const wh::core::result<graph_value> &status,
                                                detail::runtime_state::invoke_outputs &&outputs)
    -> graph_run_report {
  graph_run_report report{};
  report.transition_log = std::move(outputs.transition_log);
  report.completed_node_keys = std::move(outputs.completed_node_keys);
  report.debug_events = std::move(outputs.debug_events);
  report.state_snapshot_events = std::move(outputs.state_snapshot_events);
  report.state_delta_events = std::move(outputs.state_delta_events);
  report.runtime_message_events = std::move(outputs.runtime_message_events);
  report.custom_events = std::move(outputs.custom_events);
  report.remaining_forwarded_checkpoint_keys =
      std::move(outputs.remaining_forwarded_checkpoint_keys);
  report.step_limit_error = std::move(outputs.step_limit_error);
  report.node_timeout_error = std::move(outputs.node_timeout_error);
  report.node_run_error = std::move(outputs.node_run_error);
  report.graph_run_error = std::move(outputs.graph_run_error);
  report.stream_read_error = std::move(outputs.stream_read_error);
  report.interrupt_resolution = std::move(outputs.external_interrupt_resolution);
  report.checkpoint_error = std::move(outputs.checkpoint_error);
  if (!report.graph_run_error.has_value() && status.has_error()) {
    if (report.node_run_error.has_value()) {
      report.graph_run_error = graph_run_error_detail{
          .phase = compose_error_phase::execute,
          .path = report.node_run_error->path,
          .node = report.node_run_error->node,
          .code = report.node_run_error->code,
          .raw_error = report.node_run_error->raw_error,
          .message = report.node_run_error->message,
      };
    } else if (report.stream_read_error.has_value()) {
      report.graph_run_error = graph_run_error_detail{
          .phase = compose_error_phase::execute,
          .path = report.stream_read_error->path,
          .node = report.stream_read_error->node,
          .code = report.stream_read_error->code,
          .raw_error = report.stream_read_error->raw_error,
          .message = report.stream_read_error->message,
      };
    }
  }
  return report;
}

} // namespace detail

template <typename receiver_t> class graph::invoke_operation {
public:
  using operation_state_concept = stdexec::operation_state_t;
  using receiver_env_t =
      std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_t &>()))>;
  using graph_scheduler_t = wh::core::detail::any_resume_scheduler_t;

  class child_receiver {
  public:
    using receiver_concept = stdexec::receiver_t;

    invoke_operation *owner{nullptr};
    receiver_env_t env_{};

    auto set_value(wh::core::result<graph_value> status) && noexcept -> void {
      owner->finish_child(std::move(status));
    }

    template <typename error_t>
    auto set_error(error_t &&error) && noexcept -> void {
      owner->finish_child(wh::core::result<graph_value>::failure(
          invoke_operation::map_child_error(std::forward<error_t>(error))));
    }

    auto set_stopped() && noexcept -> void {
      owner->finish_child(wh::core::result<graph_value>::failure(wh::core::errc::canceled));
    }

    [[nodiscard]] auto get_env() const noexcept -> receiver_env_t { return env_; }
  };

  using child_sender_t =
      decltype(stdexec::starts_on(std::declval<const graph_scheduler_t &>(),
                                  std::declval<graph_sender>()));
  using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

  template <typename request_arg_t, typename receiver_arg_t>
    requires std::same_as<std::remove_cvref_t<request_arg_t>, graph_invoke_request> &&
                 std::constructible_from<receiver_t, receiver_arg_t>
  invoke_operation(const graph *owner, request_arg_t &&request, wh::core::run_context &context,
                   const graph_scheduler_t &graph_scheduler,
                   receiver_arg_t &&receiver)
      : owner_(owner), context_(context), request_(std::forward<request_arg_t>(request)),
        graph_scheduler_(graph_scheduler),
        receiver_(std::forward<receiver_arg_t>(receiver)),
        receiver_env_(stdexec::get_env(receiver_)) {}

  invoke_operation(const invoke_operation &) = delete;
  auto operator=(const invoke_operation &) -> invoke_operation & = delete;
  invoke_operation(invoke_operation &&) = delete;
  auto operator=(invoke_operation &&) -> invoke_operation & = delete;

  ~invoke_operation() { destroy_child(); }

  auto start() & noexcept -> void { start_runtime(); }

private:
  template <typename error_t>
  [[nodiscard]] static auto map_child_error(error_t &&error) noexcept
      -> wh::core::error_code {
    if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                               wh::core::error_code>) {
      return std::forward<error_t>(error);
    } else if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                      std::exception_ptr>) {
      try {
        std::rethrow_exception(std::forward<error_t>(error));
      } catch (...) {
        return wh::core::map_current_exception();
      }
    } else {
      return wh::core::make_error(wh::core::errc::internal_error);
    }
  }

  [[nodiscard]] auto delivered() const noexcept -> bool {
    return delivered_.load(std::memory_order_acquire);
  }

  auto complete_status(wh::core::result<graph_value> status) noexcept -> void {
    if (delivered_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    graph_invoke_result invoke_result{};
    invoke_result.report = detail::make_graph_run_report(status, std::move(report_outputs_));
    invoke_result.output_status = std::move(status);
    stdexec::set_value(std::move(receiver_),
                       wh::core::result<graph_invoke_result>{std::move(invoke_result)});
  }

  auto complete_error(const wh::core::error_code code) noexcept -> void {
    if (delivered_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    stdexec::set_value(std::move(receiver_),
                       wh::core::result<graph_invoke_result>::failure(code));
  }

  auto finish_child(wh::core::result<graph_value> status) noexcept -> void {
    if (delivered()) {
      return;
    }
    complete_status(std::move(status));
  }

  auto start_runtime() noexcept -> void {
    try {
      auto sender = detail::start_request(
          *owner_, context_, std::move(request_),
          graph_scheduler_t{graph_scheduler_}, std::addressof(report_outputs_));
      [[maybe_unused]] auto &child_op = child_op_.construct_with([&]() {
        return stdexec::connect(
            stdexec::starts_on(graph_scheduler_t{graph_scheduler_},
                               std::move(sender)),
            child_receiver{this, receiver_env_});
      });
      child_op_engaged_ = true;
      stdexec::start(child_op_.get());
    } catch (...) {
      destroy_child();
      complete_error(wh::core::errc::internal_error);
    }
  }

  auto destroy_child() noexcept -> void {
    if (child_op_engaged_) {
      child_op_.destruct();
      child_op_engaged_ = false;
    }
  }

  const graph *owner_{nullptr};
  wh::core::run_context &context_;
  graph_invoke_request request_{};
  detail::runtime_state::invoke_outputs report_outputs_{};
  graph_scheduler_t graph_scheduler_;
  receiver_t receiver_;
  receiver_env_t receiver_env_{};
  wh::core::detail::manual_lifetime<child_op_t> child_op_{};
  std::atomic<bool> delivered_{false};
  bool child_op_engaged_{false};
};

template <typename request_t> class graph::invoke_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = graph::invoke_completion_signatures;

  template <typename owner_t, typename request_arg_t>
    requires std::constructible_from<request_t, request_arg_t>
  invoke_sender(owner_t *owner, wh::core::run_context &context, request_arg_t &&request)
      : owner_(owner), context_(&context), request_(std::forward<request_arg_t>(request)) {}

  template <typename self_t, stdexec::receiver_of<completion_signatures> receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, invoke_sender> &&
             wh::core::detail::receiver_with_launch_scheduler<receiver_t> &&
             (!std::is_const_v<std::remove_reference_t<self_t>> ||
              std::copy_constructible<request_t>)
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self, receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    const auto env = stdexec::get_env(receiver);
    auto graph_scheduler = wh::core::detail::get_launch_scheduler(env);
    using operation_t = invoke_operation<stored_receiver_t>;
    if constexpr (std::is_const_v<std::remove_reference_t<self_t>>) {
      return operation_t{
          self.owner_,
          self.request_,
          *self.context_,
          wh::core::detail::erase_resume_scheduler(std::move(graph_scheduler)),
          std::move(receiver)};
    } else {
      return operation_t{
          self.owner_,
          std::forward<self_t>(self).request_,
          *self.context_,
          wh::core::detail::erase_resume_scheduler(std::move(graph_scheduler)),
          std::move(receiver)};
    }
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

private:
  const graph *owner_{nullptr};
  wh::core::run_context *context_{nullptr};
  wh_no_unique_address request_t request_{};
};

template <typename request_t>
  requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
inline auto graph::invoke(wh::core::run_context &context, request_t &&request) const -> auto {
  return make_invoke_sender(graph_invoke_request{std::forward<request_t>(request)}, context);
}

template <typename request_t>
  requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
inline auto graph::make_invoke_sender(request_t &&request, wh::core::run_context &context) const
    -> invoke_sender<graph_invoke_request> {
  return invoke_sender<graph_invoke_request>{
      this, context, graph_invoke_request{std::forward<request_t>(request)}};
}

namespace detail {

} // namespace detail

} // namespace wh::compose
