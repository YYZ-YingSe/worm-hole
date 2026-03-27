#pragma once

#include "wh/compose/graph/graph.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::compose {

template <typename receiver_t>
class graph::invoke_operation {
public:
  template <typename input_t, typename call_options_t, typename receiver_arg_t>
    requires std::same_as<std::remove_cvref_t<input_t>, graph_value> &&
             std::same_as<std::remove_cvref_t<call_options_t>, graph_call_options> &&
             std::constructible_from<receiver_t, receiver_arg_t>
  invoke_operation(const graph *owner, input_t &&input,
                   wh::core::run_context &context,
                   call_options_t &&call_options, receiver_arg_t &&receiver)
      : owner_(owner), context_(context), input_(std::forward<input_t>(input)),
        call_options_(std::forward<call_options_t>(call_options)),
        receiver_(std::forward<receiver_arg_t>(receiver)) {}

  invoke_operation(const invoke_operation &) = delete;
  auto operator=(const invoke_operation &) -> invoke_operation & = delete;

  auto start() & noexcept -> void { start_runtime(); }

private:
  auto finish(const wh::core::error_code code) noexcept -> void {
    stdexec::set_value(std::move(receiver_),
                       wh::core::result<graph_value>::failure(code));
  }

  auto start_runtime() noexcept -> void {
    try {
      auto lane_scheduler =
          stdexec::get_scheduler(stdexec::get_env(receiver_));
      detail::invoke_runtime::run_state::launch(
          detail::invoke_runtime::run_state{
              owner_, std::move(input_), context_, std::move(call_options_),
              wh::core::detail::erase_resume_scheduler(lane_scheduler), {},
              nullptr, nullptr},
          std::move(receiver_), std::move(lane_scheduler));
    } catch (...) {
      finish(wh::core::errc::internal_error);
    }
  }

  const graph *owner_{nullptr};
  wh::core::run_context &context_;
  graph_value input_{};
  graph_call_options call_options_{};
  receiver_t receiver_;
};

template <typename input_t, typename call_options_t>
class graph::invoke_sender {
public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = graph::invoke_completion_signatures;

  template <typename owner_t, typename input_arg_t, typename call_options_arg_t>
    requires std::constructible_from<input_t, input_arg_t> &&
             std::constructible_from<call_options_t, call_options_arg_t>
  invoke_sender(owner_t *owner, wh::core::run_context &context,
                input_arg_t &&input, call_options_arg_t &&call_options)
      : owner_(owner), context_(&context), input_(std::forward<input_arg_t>(input)),
        call_options_(std::forward<call_options_arg_t>(call_options)) {}

  template <typename self_t,
            stdexec::receiver_of<completion_signatures> receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, invoke_sender> &&
             wh::core::detail::env_with_scheduler<stdexec::env_of_t<receiver_t>>
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self,
                                            receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    return invoke_operation<stored_receiver_t>{
        self.owner_, std::forward<self_t>(self).input_, *self.context_,
        std::forward<self_t>(self).call_options_, std::move(receiver)};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

private:
  const graph *owner_{nullptr};
  wh::core::run_context *context_{nullptr};
  [[no_unique_address]] input_t input_{};
  [[no_unique_address]] call_options_t call_options_{};
};

template <typename input_t>
  requires std::same_as<std::remove_cvref_t<input_t>, graph_value>
inline auto graph::invoke(wh::core::run_context &context, input_t &&input) const
    -> auto {
  return make_invoke_sender(graph_value(std::forward<input_t>(input)), context,
                            graph_call_options{});
}

template <typename input_t, typename call_options_t>
  requires std::same_as<std::remove_cvref_t<input_t>, graph_value> &&
           std::same_as<std::remove_cvref_t<call_options_t>, graph_call_options>
inline auto graph::invoke(wh::core::run_context &context, input_t &&input,
                          call_options_t &&call_options) const -> auto {
  return make_invoke_sender(
      graph_value(std::forward<input_t>(input)), context,
      graph_call_options{std::forward<call_options_t>(call_options)});
}

template <typename input_t, typename call_options_t>
  requires std::same_as<std::remove_cvref_t<input_t>, graph_value> &&
           std::same_as<std::remove_cvref_t<call_options_t>, graph_call_options>
inline auto graph::make_invoke_sender(input_t &&input,
                                      wh::core::run_context &context,
                                      call_options_t &&call_options) const
    -> invoke_sender<graph_value, graph_call_options> {
  return invoke_sender<graph_value, graph_call_options>{
      this, context, graph_value{std::forward<input_t>(input)},
      graph_call_options{std::forward<call_options_t>(call_options)}};
}

namespace detail {

inline auto start_bound_graph(
    const graph &graph, wh::core::run_context &context,
    graph_value &input, const graph_call_options *call_options,
    const node_path *path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const wh::core::detail::any_resume_scheduler_t *resume_scheduler)
    -> graph_sender {
  auto bound_call_options =
      call_options != nullptr ? graph_call_options{*call_options}
                              : graph_call_options{};
  auto bound_path_prefix =
      path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  auto scheduler =
      resume_scheduler != nullptr
          ? wh::core::detail::any_resume_scheduler_t{*resume_scheduler}
          : wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
  return detail::invoke_runtime::run_state::start(
      detail::invoke_runtime::run_state{
          std::addressof(graph), std::move(input), context,
          std::move(bound_call_options), std::move(scheduler),
          std::move(bound_path_prefix), parent_process_state, nested_outputs});
}

inline auto start_scoped_graph(
    const graph &graph, wh::core::run_context &context,
    graph_value &input, const graph_call_scope *call_scope,
    const node_path *path_prefix, graph_process_state *parent_process_state,
    detail::runtime_state::invoke_outputs *nested_outputs,
    const wh::core::detail::any_resume_scheduler_t *resume_scheduler)
    -> graph_sender {
  auto bound_call_scope =
      call_scope != nullptr ? *call_scope : graph_call_scope{};
  auto bound_path_prefix =
      path_prefix != nullptr ? node_path{*path_prefix} : node_path{};
  auto scheduler =
      resume_scheduler != nullptr
          ? wh::core::detail::any_resume_scheduler_t{*resume_scheduler}
          : wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
  return detail::invoke_runtime::run_state::start(
      detail::invoke_runtime::run_state{
          std::addressof(graph), std::move(input), context,
          std::move(bound_call_scope), std::move(scheduler),
          std::move(bound_path_prefix), parent_process_state, nested_outputs});
}

inline auto start_nested_graph(const graph &graph,
                               wh::core::run_context &context,
                               graph_value &input,
                               const node_runtime &runtime) -> graph_sender {
  return runtime.nested_entry(graph, context, input, runtime.call_options,
                              runtime.path, runtime.local_process_state,
                              runtime.invoke_outputs, runtime.trace);
}

} // namespace detail

} // namespace wh::compose
