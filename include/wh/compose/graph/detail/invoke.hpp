#pragma once

#include "wh/compose/graph/detail/graph_class.hpp"

namespace wh::compose::detail {

auto start_scoped_graph(const graph &graph, wh::core::run_context &context, graph_value &input,
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
#include "wh/core/stdexec/detail/receiver_completion.hpp"
#include "wh/core/stdexec/detail/scheduled_drive_loop.hpp"
#include "wh/core/stdexec/detail/shared_operation_state.hpp"
#include "wh/core/stdexec/detail/single_completion_slot.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"

namespace wh::compose {

namespace detail {

[[nodiscard]] inline auto make_graph_run_report(const wh::core::result<graph_value> &status,
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

template <typename receiver_t>
class graph::invoke_operation
    : public std::enable_shared_from_this<invoke_operation<receiver_t>>,
      private wh::core::detail::scheduled_drive_loop<invoke_operation<receiver_t>,
                                                     wh::core::detail::any_resume_scheduler_t> {
public:
  using receiver_env_t =
      std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_t &>()))>;
  using drive_loop_t =
      wh::core::detail::scheduled_drive_loop<invoke_operation<receiver_t>,
                                             wh::core::detail::any_resume_scheduler_t>;
  friend class wh::core::detail::callback_guard<invoke_operation>;
  friend drive_loop_t;

  class child_receiver {
  public:
    using receiver_concept = stdexec::receiver_t;

    invoke_operation *owner{nullptr};
    receiver_env_t env_{};

    auto set_value(wh::core::result<graph_value> status) && noexcept -> void {
      auto scope = owner->callbacks_.enter(owner);
      owner->finish_child(std::move(status));
    }

    template <typename error_t> auto set_error(error_t &&) && noexcept -> void {
      auto scope = owner->callbacks_.enter(owner);
      owner->finish_child(wh::core::result<graph_value>::failure(wh::core::errc::internal_error));
    }

    auto set_stopped() && noexcept -> void {
      auto scope = owner->callbacks_.enter(owner);
      owner->finish_child(wh::core::result<graph_value>::failure(wh::core::errc::canceled));
    }

    [[nodiscard]] auto get_env() const noexcept -> receiver_env_t { return env_; }
  };

  using child_sender_t = graph_sender;
  using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;
  using final_completion_t =
      wh::core::detail::receiver_completion<receiver_t, wh::core::result<graph_invoke_result>>;

  template <typename request_arg_t, typename receiver_arg_t>
    requires std::same_as<std::remove_cvref_t<request_arg_t>, graph_invoke_request> &&
                 std::constructible_from<receiver_t, receiver_arg_t>
  invoke_operation(const graph *owner, request_arg_t &&request, wh::core::run_context &context,
                   const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
                   receiver_arg_t &&receiver)
      : drive_loop_t(graph_scheduler), owner_(owner), context_(context),
        request_(std::forward<request_arg_t>(request)),
        receiver_(std::forward<receiver_arg_t>(receiver)),
        receiver_env_(stdexec::get_env(receiver_)) {}

  invoke_operation(const invoke_operation &) = delete;
  auto operator=(const invoke_operation &) -> invoke_operation & = delete;

  auto start() noexcept -> void { request_drive(); }

private:
  [[nodiscard]] auto finished() const noexcept -> bool {
    return delivered_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto completion_pending() const noexcept -> bool {
    return pending_completion_.has_value();
  }

  [[nodiscard]] auto take_completion() noexcept -> std::optional<final_completion_t> {
    if (!pending_completion_.has_value()) {
      return std::nullopt;
    }
    auto completion = std::move(pending_completion_);
    pending_completion_.reset();
    return completion;
  }

  [[nodiscard]] auto acquire_owner_lifetime_guard() noexcept -> std::shared_ptr<invoke_operation> {
    auto keepalive = this->weak_from_this().lock();
    if (!keepalive) {
      ::wh::core::contract_violation(::wh::core::contract_kind::invariant,
                                     "graph invoke owner lifetime guard expired");
    }
    return keepalive;
  }

  auto on_callback_exit() noexcept -> void {
    if (completion_.ready()) {
      request_drive();
    }
  }

  auto request_drive() noexcept -> void { drive_loop_t::request_drive(); }

  auto drive() noexcept -> void {
    while (!finished()) {
      if (callbacks_.active()) {
        return;
      }

      if (auto current = completion_.take(); current.has_value()) {
        wh_invariant(child_in_flight_);
        child_op_.reset();
        child_in_flight_ = false;
        complete(std::move(*current));
        return;
      }

      if (child_in_flight_) {
        return;
      }

      if (started_) {
        return;
      }

      started_ = true;
      start_runtime();
      if (finished()) {
        return;
      }
      if (completion_.ready()) {
        continue;
      }
      if (child_in_flight_) {
        return;
      }
      return;
    }
  }

  auto drive_error(const wh::core::error_code error) noexcept -> void { finish(error); }

  auto complete(wh::core::result<graph_value> status) noexcept -> void {
    if (delivered_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    graph_invoke_result invoke_result{};
    invoke_result.report = detail::make_graph_run_report(status, std::move(report_outputs_));
    invoke_result.output_status = std::move(status);
    pending_completion_.emplace(final_completion_t::set_value(
        std::move(receiver_), wh::core::result<graph_invoke_result>{std::move(invoke_result)}));
  }

  auto finish(const wh::core::error_code code) noexcept -> void {
    if (delivered_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    child_op_.reset();
    child_in_flight_ = false;
    completion_.reset();
    pending_completion_.emplace(final_completion_t::set_value(
        std::move(receiver_), wh::core::result<graph_invoke_result>::failure(code)));
  }

  auto finish_child(wh::core::result<graph_value> status) noexcept -> void {
    if (finished()) {
      return;
    }
#ifndef NDEBUG
    wh_invariant(completion_.publish(std::move(status)));
#else
    completion_.publish(std::move(status));
#endif
    request_drive();
  }

  auto start_runtime() noexcept -> void {
    try {
      auto controls = std::move(request_.controls);
      auto call_options = std::move(controls.call);
      auto sender = detail::invoke_runtime::start_graph_run(detail::invoke_runtime::run_state{
          owner_,
          std::move(request_.input),
          context_,
          std::move(call_options),
          wh::core::detail::any_resume_scheduler_t{this->scheduler()},
          {},
          nullptr,
          nullptr,
          request_.services,
          std::move(controls),
          std::addressof(report_outputs_),
          nullptr});
      child_op_.emplace_from(stdexec::connect, std::move(sender),
                             child_receiver{this, receiver_env_});
      child_in_flight_ = true;
      stdexec::start(child_op_.get());
    } catch (...) {
      child_op_.reset();
      child_in_flight_ = false;
      finish(wh::core::errc::internal_error);
    }
  }

  const graph *owner_{nullptr};
  wh::core::run_context &context_;
  graph_invoke_request request_{};
  detail::runtime_state::invoke_outputs report_outputs_{};
  receiver_t receiver_;
  receiver_env_t receiver_env_{};
  wh::core::detail::manual_lifetime_box<child_op_t> child_op_{};
  wh::core::detail::single_completion_slot<wh::core::result<graph_value>> completion_{};
  wh::core::detail::callback_guard<invoke_operation> callbacks_{};
  std::optional<final_completion_t> pending_completion_{};
  std::atomic<bool> delivered_{false};
  bool started_{false};
  bool child_in_flight_{false};
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
             wh::core::detail::receiver_with_launch_scheduler<receiver_t>
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self, receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    const auto env = stdexec::get_env(receiver);
    auto graph_scheduler = wh::core::detail::get_launch_scheduler(env);
    using operation_t = invoke_operation<stored_receiver_t>;
    return wh::core::detail::shared_operation_state<operation_t>{std::make_shared<operation_t>(
        self.owner_, std::forward<self_t>(self).request_, *self.context_,
        wh::core::detail::erase_resume_scheduler(std::move(graph_scheduler)), std::move(receiver))};
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

inline auto start_bound_graph(const graph &graph, wh::core::run_context &context,
                              graph_value &input, const graph_call_options *call_options,
                              const node_path *path_prefix,
                              graph_process_state *parent_process_state,
                              detail::runtime_state::invoke_outputs *nested_outputs,
                              const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
                              const detail::invoke_runtime::run_state *parent_state,
                              const graph_runtime_services *services,
                              graph_invoke_controls controls) -> graph_sender {
  auto bound_call_options =
      call_options != nullptr ? graph_call_options{*call_options} : graph_call_options{};
  auto bound_path_prefix = path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  return detail::invoke_runtime::start_graph_run(detail::invoke_runtime::run_state{
      std::addressof(graph), std::move(input), context, std::move(bound_call_options),
      wh::core::detail::any_resume_scheduler_t{graph_scheduler}, std::move(bound_path_prefix),
      parent_process_state, nested_outputs, services, std::move(controls), nullptr, parent_state});
}

inline auto start_scoped_graph(const graph &graph, wh::core::run_context &context,
                               graph_value &input, const graph_call_scope *call_scope,
                               const node_path *path_prefix,
                               graph_process_state *parent_process_state,
                               detail::runtime_state::invoke_outputs *nested_outputs,
                               const wh::core::detail::any_resume_scheduler_t &graph_scheduler,
                               const detail::invoke_runtime::run_state *parent_state,
                               const graph_runtime_services *services,
                               graph_invoke_controls controls) -> graph_sender {
  auto bound_call_scope = call_scope != nullptr ? *call_scope : graph_call_scope{};
  auto bound_path_prefix = path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  return detail::invoke_runtime::start_graph_run(detail::invoke_runtime::run_state{
      std::addressof(graph), std::move(input), context, std::move(bound_call_scope),
      wh::core::detail::any_resume_scheduler_t{graph_scheduler}, std::move(bound_path_prefix),
      parent_process_state, nested_outputs, services, std::move(controls), nullptr, parent_state});
}

inline auto start_nested_graph(const graph &graph, wh::core::run_context &context,
                               graph_value &input, const node_runtime &runtime) -> graph_sender {
  return detail::node_runtime_access::nested_entry(runtime)(
      graph, context, input, runtime.call_options(), runtime.path(), runtime.process_state(),
      detail::node_runtime_access::invoke_outputs(runtime), runtime.trace());
}

} // namespace detail

} // namespace wh::compose
