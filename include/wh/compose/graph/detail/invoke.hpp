#pragma once

#include "wh/compose/graph/detail/graph_class.hpp"

namespace wh::compose::detail {

auto start_scoped_graph(
    const graph &graph, wh::core::run_context &context, graph_value &input,
    const graph_call_scope *call_scope, const node_path *path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
    const detail::invoke_runtime::run_state *parent_state = nullptr,
    const graph_runtime_services *services = nullptr,
    graph_invoke_controls controls = {}) -> graph_sender;

} // namespace wh::compose::detail

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/node/detail/runtime_access.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto
make_graph_run_report(const wh::core::result<graph_value> &status,
                      detail::runtime_state::invoke_outputs &&outputs)
    -> graph_run_report {
  graph_run_report report{};
  report.transition_log = std::move(outputs.transition_log);
  report.last_completed_nodes = std::move(outputs.last_completed_nodes);
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
  report.interrupt_resolution =
      std::move(outputs.external_interrupt_resolution);
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
  using receiver_env_t =
      decltype(stdexec::get_env(std::declval<const receiver_t &>()));
  friend class wh::core::detail::callback_guard<invoke_operation>;

  class child_receiver {
  public:
    using receiver_concept = stdexec::receiver_t;

    invoke_operation *owner{nullptr};
    receiver_env_t env_{};

    auto set_value(wh::core::result<graph_value> status) && noexcept -> void {
      auto scope = owner->callbacks_.enter(owner);
      owner->on_child_value(std::move(status));
    }

    template <typename error_t> auto set_error(error_t &&) && noexcept -> void {
      auto scope = owner->callbacks_.enter(owner);
      owner->on_child_value(wh::core::result<graph_value>::failure(
          wh::core::errc::internal_error));
    }

    auto set_stopped() && noexcept -> void {
      auto scope = owner->callbacks_.enter(owner);
      owner->on_child_value(
          wh::core::result<graph_value>::failure(wh::core::errc::canceled));
    }

    [[nodiscard]] auto get_env() const noexcept -> receiver_env_t {
      return env_;
    }
  };

  using child_sender_t = graph_sender;
  using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

  template <typename request_arg_t, typename receiver_arg_t>
    requires std::same_as<std::remove_cvref_t<request_arg_t>,
                          graph_invoke_request> &&
                 std::constructible_from<receiver_t, receiver_arg_t>
  invoke_operation(const graph *owner, request_arg_t &&request,
                   wh::core::run_context &context, receiver_arg_t &&receiver)
      : owner_(owner), context_(context),
        request_(std::forward<request_arg_t>(request)),
        receiver_(std::forward<receiver_arg_t>(receiver)) {}

  invoke_operation(const invoke_operation &) = delete;
  auto operator=(const invoke_operation &) -> invoke_operation & = delete;

  auto start() & noexcept -> void { start_runtime(); }

private:
  auto complete(wh::core::result<graph_value> status) noexcept -> void {
    graph_invoke_result invoke_result{};
    invoke_result.report =
        detail::make_graph_run_report(status, std::move(report_outputs_));
    invoke_result.output_status = std::move(status);
    stdexec::set_value(
        std::move(receiver_),
        wh::core::result<graph_invoke_result>{std::move(invoke_result)});
  }

  auto finish(const wh::core::error_code code) noexcept -> void {
    stdexec::set_value(std::move(receiver_),
                       wh::core::result<graph_invoke_result>::failure(code));
  }

  auto on_child_value(wh::core::result<graph_value> status) noexcept -> void {
    wh_invariant(!pending_status_.has_value());
    pending_status_.emplace(std::move(status));
    flush_pending_completion();
  }

  auto on_callback_exit() noexcept -> void { flush_pending_completion(); }

  auto flush_pending_completion() noexcept -> void {
    if (starting_child_ || callbacks_.active() ||
        !pending_status_.has_value()) {
      return;
    }
    auto status = std::move(*pending_status_);
    pending_status_.reset();
    complete(std::move(status));
  }

  auto start_runtime() noexcept -> void {
    try {
      const auto env = stdexec::get_env(receiver_);
      auto graph_scheduler = wh::core::detail::get_launch_scheduler(env);
      auto controls = std::move(request_.controls);
      auto call_options = std::move(controls.call);
      auto sender = detail::invoke_runtime::start_graph_run(
          detail::invoke_runtime::run_state{
              owner_,
              std::move(request_.input),
              context_,
              std::move(call_options),
              wh::core::detail::erase_resume_scheduler(
                  std::move(graph_scheduler)),
              {},
              nullptr,
              nullptr,
              request_.services,
              std::move(controls),
              std::addressof(report_outputs_),
              nullptr});
      starting_child_ = true;
      child_op_.emplace_from(stdexec::connect, std::move(sender),
                             child_receiver{this, env});
      stdexec::start(child_op_.get());
      starting_child_ = false;
      flush_pending_completion();
    } catch (...) {
      starting_child_ = false;
      pending_status_.reset();
      child_op_.reset();
      finish(wh::core::errc::internal_error);
    }
  }

  const graph *owner_{nullptr};
  wh::core::run_context &context_;
  graph_invoke_request request_{};
  detail::runtime_state::invoke_outputs report_outputs_{};
  receiver_t receiver_;
  wh::core::detail::manual_lifetime_box<child_op_t> child_op_{};
  std::optional<wh::core::result<graph_value>> pending_status_{};
  wh::core::detail::callback_guard<invoke_operation> callbacks_{};
  bool starting_child_{false};
};

template <typename request_t> class graph::invoke_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = graph::invoke_completion_signatures;

  template <typename owner_t, typename request_arg_t>
    requires std::constructible_from<request_t, request_arg_t>
  invoke_sender(owner_t *owner, wh::core::run_context &context,
                request_arg_t &&request)
      : owner_(owner), context_(&context),
        request_(std::forward<request_arg_t>(request)) {}

  template <typename self_t,
            stdexec::receiver_of<completion_signatures> receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, invoke_sender> &&
             wh::core::detail::receiver_with_launch_scheduler<receiver_t>
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    return invoke_operation<stored_receiver_t>{
        self.owner_, std::forward<self_t>(self).request_, *self.context_,
        std::move(receiver)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

private:
  const graph *owner_{nullptr};
  wh::core::run_context *context_{nullptr};
  wh_no_unique_address request_t request_{};
};

template <typename request_t>
  requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
inline auto graph::invoke(wh::core::run_context &context,
                          request_t &&request) const -> auto {
  return make_invoke_sender(
      graph_invoke_request{std::forward<request_t>(request)}, context);
}

template <typename request_t>
  requires std::same_as<std::remove_cvref_t<request_t>, graph_invoke_request>
inline auto graph::make_invoke_sender(request_t &&request,
                                      wh::core::run_context &context) const
    -> invoke_sender<graph_invoke_request> {
  return invoke_sender<graph_invoke_request>{
      this, context, graph_invoke_request{std::forward<request_t>(request)}};
}

namespace detail {

inline auto start_bound_graph(
    const graph &graph, wh::core::run_context &context, graph_value &input,
    const graph_call_options *call_options, const node_path *path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
    const detail::invoke_runtime::run_state *parent_state,
    const graph_runtime_services *services, graph_invoke_controls controls)
    -> graph_sender {
  auto bound_call_options = call_options != nullptr
                                ? graph_call_options{*call_options}
                                : graph_call_options{};
  auto bound_path_prefix =
      path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  return detail::invoke_runtime::start_graph_run(
      detail::invoke_runtime::run_state{
          std::addressof(graph), std::move(input), context,
          std::move(bound_call_options),
          wh::core::detail::any_resume_scheduler_t{graph_scheduler},
          std::move(bound_path_prefix), parent_process_state, nested_outputs,
          services, std::move(controls), nullptr, parent_state});
}

inline auto start_scoped_graph(
    const graph &graph, wh::core::run_context &context, graph_value &input,
    const graph_call_scope *call_scope, const node_path *path_prefix,
    graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
    const detail::invoke_runtime::run_state *parent_state,
    const graph_runtime_services *services, graph_invoke_controls controls)
    -> graph_sender {
  auto bound_call_scope =
      call_scope != nullptr ? *call_scope : graph_call_scope{};
  auto bound_path_prefix =
      path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  return detail::invoke_runtime::start_graph_run(
      detail::invoke_runtime::run_state{
          std::addressof(graph), std::move(input), context,
          std::move(bound_call_scope),
          wh::core::detail::any_resume_scheduler_t{graph_scheduler},
          std::move(bound_path_prefix), parent_process_state, nested_outputs,
          services, std::move(controls), nullptr, parent_state});
}

inline auto start_nested_graph(const graph &graph,
                               wh::core::run_context &context,
                               graph_value &input, const node_runtime &runtime)
    -> graph_sender {
  return detail::node_runtime_access::nested_entry(runtime)(
      graph, context, input, runtime.call_options(), runtime.path(),
      runtime.process_state(),
      detail::node_runtime_access::invoke_outputs(runtime), runtime.trace());
}

} // namespace detail

} // namespace wh::compose
