#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/compose/graph/detail/dag.hpp"
#include "wh/compose/graph/detail/pregel.hpp"

namespace wh::compose {

inline detail::invoke_runtime::run_state::run_state(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_options &&call_options,
    wh::core::detail::any_resume_scheduler_t graph_scheduler,
    node_path path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const graph_runtime_services *services, graph_invoke_controls controls,
    detail::runtime_state::invoke_outputs *published_outputs,
    const run_state *parent_state)
    : owner_(owner), context_(context),
      invoke_controls_(std::move(controls)), services_(services),
      parent_state_(parent_state), path_prefix_(std::move(path_prefix)),
      parent_process_state_(parent_process_state),
      nested_outputs_(nested_outputs), published_outputs_(published_outputs),
      graph_scheduler_(std::move(graph_scheduler)) {
  owned_call_options_ =
      std::make_unique<graph_call_options>(std::move(call_options));
  initialize(std::move(input), graph_call_scope{*owned_call_options_});
}

inline detail::invoke_runtime::run_state::run_state(
    const graph *owner, graph_value &&input, wh::core::run_context &context,
    graph_call_scope call_scope,
    wh::core::detail::any_resume_scheduler_t graph_scheduler,
    node_path path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const graph_runtime_services *services, graph_invoke_controls controls,
    detail::runtime_state::invoke_outputs *published_outputs,
    const run_state *parent_state)
    : owner_(owner), context_(context),
      invoke_controls_(std::move(controls)), services_(services),
      parent_state_(parent_state), path_prefix_(std::move(path_prefix)),
      parent_process_state_(parent_process_state),
      nested_outputs_(nested_outputs), published_outputs_(published_outputs),
      graph_scheduler_(std::move(graph_scheduler)) {
  initialize(std::move(input), std::move(call_scope));
}

inline auto detail::invoke_runtime::run_state::rebind_moved_runtime_storage()
    noexcept -> void {
  if (parent_state_ == nullptr) {
    forwarded_checkpoints_ = std::addressof(owned_forwarded_checkpoints_);
  }
  if (owned_call_options_ == nullptr) {
    return;
  }
  auto rebound_scope =
      graph_call_scope{*owned_call_options_, bound_call_scope_.prefix()};
  if (bound_call_scope_.trace().has_value()) {
    rebound_scope = rebound_scope.with_trace(*bound_call_scope_.trace());
  }
  bound_call_scope_ = std::move(rebound_scope);
}

namespace detail::invoke_runtime {

template <typename receiver_t>
class runtime_operation {
  using graph_scheduler_t = wh::core::detail::any_resume_scheduler_t;
  using dag_run_t = run_state::dag_run<receiver_t, graph_scheduler_t>;
  using pregel_run_t = run_state::pregel_run<receiver_t, graph_scheduler_t>;

public:
  template <typename receiver_arg_t>
    requires std::constructible_from<receiver_t, receiver_arg_t>
  runtime_operation(run_state &&state, receiver_arg_t &&receiver) {
    auto graph_scheduler = graph_scheduler_t{*state.graph_scheduler_};
    if (state.is_dag()) {
      runtime_.template emplace<1U>(std::move(state),
                                    std::forward<receiver_arg_t>(receiver),
                                    std::move(graph_scheduler));
    } else {
      runtime_.template emplace<2U>(
          std::move(state), std::forward<receiver_arg_t>(receiver),
          std::move(graph_scheduler));
    }
  }

  runtime_operation(const runtime_operation &) = delete;
  auto operator=(const runtime_operation &) -> runtime_operation & = delete;

  auto start() & noexcept -> void {
    std::visit([](auto &runtime) {
      using runtime_t = std::remove_cvref_t<decltype(runtime)>;
      if constexpr (!std::same_as<runtime_t, std::monostate>) {
        runtime.bind_outer_stop();
        runtime.request_pump();
      }
    }, runtime_);
  }

private:
  std::variant<std::monostate, dag_run_t, pregel_run_t> runtime_{};
};

class runtime_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(wh::core::result<graph_value>)>;

  explicit runtime_sender(run_state state) {
    state_.emplace(std::move(state));
    state_->rebind_moved_runtime_storage();
  }

  template <typename self_t,
            stdexec::receiver_of<completion_signatures> receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, runtime_sender>
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    return runtime_operation<stored_receiver_t>{std::move(*self.state_),
                                                std::move(receiver)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

private:
  mutable std::optional<run_state> state_{};
};

} // namespace detail::invoke_runtime

inline auto detail::invoke_runtime::run_state::start(
    run_state &&state) -> graph_sender {
  if (state.init_error_.has_value()) {
    return state.immediate_failure(*state.init_error_);
  }
  return graph_sender{detail::invoke_runtime::runtime_sender{std::move(state)}};
}

} // namespace wh::compose
