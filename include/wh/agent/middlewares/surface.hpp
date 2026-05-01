// Defines the shared middleware surface used to export instruction fragments,
// tool bindings, and request transforms without introducing a second runtime.
#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/agent/tool_binding.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec/ready_result_sender.hpp"
#include "wh/core/stdexec/result_sender.hpp"
#include "wh/model/chat_model.hpp"

namespace wh::agent::middlewares {

template <typename status_t> using operation_sender = wh::core::detail::result_sender<status_t>;

template <typename status_t, typename... args_t>
using sync_operation = wh::core::callback_function<status_t(args_t...) const>;

template <typename status_t, typename... args_t>
using async_operation = wh::core::callback_function<operation_sender<status_t>(args_t...) const>;

/// One generic sync/async middleware capability bundle.
template <typename status_t, typename... args_t> struct operation_binding {
  /// Optional sync endpoint.
  sync_operation<status_t, args_t...> sync{nullptr};
  /// Optional async endpoint.
  async_operation<status_t, args_t...> async{nullptr};

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(sync) || static_cast<bool>(async);
  }
};

/// Canonical request-transform status used by middleware request pipelines.
using request_transform_result = wh::core::result<wh::model::chat_request>;

/// Type-erased async request-transform boundary.
using request_transform_sender = operation_sender<request_transform_result>;

/// One request-transform capability bundle.
using request_transform_binding =
    operation_binding<request_transform_result, wh::model::chat_request, wh::core::run_context &>;

/// Unified middleware export surface consumed by authored shells.
struct middleware_surface {
  /// Instruction fragments appended in declaration order.
  std::vector<std::string> instruction_fragments{};
  /// Model-visible tools exported by the middleware.
  std::vector<wh::agent::tool_binding_pair> tool_bindings{};
  /// Request transforms applied before each model turn.
  std::vector<request_transform_binding> request_transforms{};
};

namespace detail {

template <typename status_t>
[[nodiscard]] inline auto make_operation_ready_sender(status_t status)
    -> operation_sender<status_t> {
  return operation_sender<status_t>{stdexec::just(std::move(status))};
}

template <typename status_t>
[[nodiscard]] inline auto make_operation_failure_sender(const wh::core::error_code error)
    -> operation_sender<status_t> {
  return operation_sender<status_t>{wh::core::detail::failure_result_sender<status_t>(error)};
}

template <typename status_t, typename... args_t>
[[nodiscard]] inline auto
open_operation_sender(const operation_binding<status_t, args_t...> &binding, auto &&...args)
    -> operation_sender<status_t> {
  if (binding.async) {
    return binding.async(std::forward<decltype(args)>(args)...);
  }
  if (binding.sync) {
    return make_operation_ready_sender<status_t>(
        binding.sync(std::forward<decltype(args)>(args)...));
  }
  return make_operation_failure_sender<status_t>(wh::core::errc::invalid_argument);
}

} // namespace detail

/// Sequentially applies one request-transform pipeline to one chat request.
[[nodiscard]] inline auto
apply_request_transforms(const std::span<const request_transform_binding> transforms,
                         wh::model::chat_request request, wh::core::run_context &context)
    -> request_transform_sender {
  if (transforms.empty()) {
    return detail::make_operation_ready_sender<request_transform_result>(
        request_transform_result{std::move(request)});
  }

  auto sender = detail::open_operation_sender(transforms.front(), std::move(request), context);
  for (const auto &binding_ref : transforms.subspan(1U)) {
    auto binding = binding_ref;
    sender = request_transform_sender{
        std::move(sender) | stdexec::let_value([binding = std::move(binding),
                                                &context](request_transform_result status) mutable
                                                   -> request_transform_sender {
          if (status.has_error()) {
            return detail::make_operation_failure_sender<request_transform_result>(status.error());
          }
          return detail::open_operation_sender(binding, std::move(status).value(), context);
        })};
  }
  return sender;
}

/// Appends one middleware surface into another while preserving declaration
/// order.
inline auto merge_middleware_surface(middleware_surface &target, middleware_surface source)
    -> void {
  for (auto &fragment : source.instruction_fragments) {
    target.instruction_fragments.push_back(std::move(fragment));
  }
  for (auto &binding : source.tool_bindings) {
    target.tool_bindings.push_back(std::move(binding));
  }
  for (auto &transform : source.request_transforms) {
    target.request_transforms.push_back(std::move(transform));
  }
}

} // namespace wh::agent::middlewares
