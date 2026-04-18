// Defines tools-node call execution helpers.
#pragma once

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

#include "wh/compose/node/detail/tools/state.hpp"
#include "wh/compose/node/detail/tools/tool_event_stream_reader.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/detail/receiver_stop_bridge.hpp"
#include "wh/schema/stream.hpp"

namespace wh::compose {
namespace detail {

using call_completion_sender =
    wh::core::detail::result_sender<wh::core::result<call_completion>>;
using stream_completion_sender =
    wh::core::detail::result_sender<wh::core::result<stream_completion>>;

template <typename sender_t>
  requires stdexec::sender<sender_t> &&
           (!std::same_as<std::remove_cvref_t<sender_t>, tools_invoke_sender>)
[[nodiscard]] inline auto erase_tools_invoke(sender_t &&sender)
    -> tools_invoke_sender {
  return tools_invoke_sender{std::forward<sender_t>(sender)};
}

[[nodiscard]] inline auto erase_tools_invoke(tools_invoke_sender sender)
    -> tools_invoke_sender {
  return sender;
}

template <typename sender_t>
  requires stdexec::sender<sender_t> &&
           (!std::same_as<std::remove_cvref_t<sender_t>, tools_stream_sender>)
[[nodiscard]] inline auto erase_tools_stream(sender_t &&sender)
    -> tools_stream_sender {
  return tools_stream_sender{std::forward<sender_t>(sender)};
}

[[nodiscard]] inline auto erase_tools_stream(tools_stream_sender sender)
    -> tools_stream_sender {
  return sender;
}

template <typename sender_t, typename result_t>
[[nodiscard]] inline auto ready_sender(result_t status) -> sender_t {
  return sender_t{wh::core::detail::ready_sender(std::move(status))};
}

template <typename sender_t, typename result_t>
[[nodiscard]] inline auto failure_sender(const wh::core::error_code error)
    -> sender_t {
  return sender_t{wh::core::detail::failure_result_sender<result_t>(error)};
}

template <typename sender_t>
  requires stdexec::sender<sender_t> &&
           (!std::same_as<std::remove_cvref_t<sender_t>, call_completion_sender>)
[[nodiscard]] inline auto erase_call_completion(sender_t &&sender)
    -> call_completion_sender {
  return call_completion_sender{std::forward<sender_t>(sender)};
}

[[nodiscard]] inline auto erase_call_completion(call_completion_sender sender)
    -> call_completion_sender {
  return sender;
}

template <typename sender_t>
  requires stdexec::sender<sender_t> &&
           (!std::same_as<std::remove_cvref_t<sender_t>, stream_completion_sender>)
[[nodiscard]] inline auto erase_stream_completion(sender_t &&sender)
    -> stream_completion_sender {
  return stream_completion_sender{std::forward<sender_t>(sender)};
}

[[nodiscard]] inline auto
erase_stream_completion(stream_completion_sender sender)
    -> stream_completion_sender {
  return sender;
}

[[nodiscard]] inline auto replay_stream(const graph_value &cached)
    -> wh::core::result<graph_stream_reader> {
  const auto *values = wh::core::any_cast<std::vector<graph_value>>(&cached);
  if (values == nullptr) {
    return wh::core::result<graph_stream_reader>::failure(
        wh::core::errc::type_mismatch);
  }
  return make_values_stream_reader(*values);
}

[[nodiscard]] inline auto
clone_call_context(const wh::core::run_context &context)
    -> wh::core::result<wh::core::run_context> {
  return wh::core::clone_run_context(context);
}

[[nodiscard]] inline auto
make_stream_completion(const std::size_t index, tool_call call,
                       graph_stream_reader stream, graph_value rerun_extra,
                       std::optional<wh::core::run_context> after_context = std::nullopt)
    -> stream_completion {
  return stream_completion{
      .index = index,
      .call = std::move(call),
      .stream = std::move(stream),
      .after_context = std::move(after_context),
      .rerun_extra = std::move(rerun_extra),
  };
}

[[nodiscard]] inline auto call_value(const tools_state &state,
                                     const tool_call &call,
                                     wh::tool::call_scope scope)
    -> wh::core::result<graph_value> {
  const auto *tool = find_tool(state, call.tool_name);
  if (tool == nullptr || !static_cast<bool>(tool->invoke)) {
    return wh::core::result<graph_value>::failure(
        tool == nullptr ? wh::core::errc::not_found
                        : wh::core::errc::not_supported);
  }
  return tool->invoke(call, std::move(scope));
}

[[nodiscard]] inline auto call_stream(const tools_state &state,
                                      const tool_call &call,
                                      wh::tool::call_scope scope)
    -> wh::core::result<graph_stream_reader> {
  const auto *tool = find_tool(state, call.tool_name);
  if (tool == nullptr || !static_cast<bool>(tool->stream)) {
    return wh::core::result<graph_stream_reader>::failure(
        tool == nullptr ? wh::core::errc::not_found
                        : wh::core::errc::not_supported);
  }
  return tool->stream(call, std::move(scope));
}

[[nodiscard]] inline auto start_value(const tools_state &state, tool_call call,
                                      wh::tool::call_scope scope)
    -> tools_invoke_sender {
  const auto *tool = find_tool(state, call.tool_name);
  if (tool == nullptr || !static_cast<bool>(tool->async_invoke)) {
    return failure_sender<tools_invoke_sender, wh::core::result<graph_value>>(
        tool == nullptr ? wh::core::errc::not_found
                        : wh::core::errc::not_supported);
  }
  return erase_tools_invoke(
      tool->async_invoke(std::move(call), std::move(scope)));
}

[[nodiscard]] inline auto start_stream(const tools_state &state, tool_call call,
                                       wh::tool::call_scope scope)
    -> tools_stream_sender {
  const auto *tool = find_tool(state, call.tool_name);
  if (tool == nullptr || !static_cast<bool>(tool->async_stream)) {
    return failure_sender<tools_stream_sender,
                          wh::core::result<graph_stream_reader>>(
        tool == nullptr ? wh::core::errc::not_found
                        : wh::core::errc::not_supported);
  }
  return erase_tools_stream(
      tool->async_stream(std::move(call), std::move(scope)));
}

[[nodiscard]] inline auto run_before(const tools_options &options,
                                     tool_call &call,
                                     const wh::tool::call_scope &scope)
    -> wh::core::result<void> {
  for (const auto &middleware : options.middleware) {
    if (!static_cast<bool>(middleware.before)) {
      continue;
    }
    auto status = middleware.before(call, scope);
    if (status.has_error()) {
      return wh::core::result<void>::failure(status.error());
    }
  }
  return {};
}

[[nodiscard]] inline auto run_call(const tools_state &state,
                                   const std::size_t index,
                                   wh::core::run_context &context)
    -> wh::core::result<call_completion> {
  const auto &plan = state.plans[index];
  auto call = make_call(plan);
  auto &rerun = state.rerun();
  if (!rerun.ids.empty() && !rerun.ids.contains(call.call_id)) {
    const auto cached = rerun.outputs.find(call.call_id);
    if (cached == rerun.outputs.end()) {
      return wh::core::result<call_completion>::failure(
          wh::core::errc::not_found);
    }
    return call_completion{
        .index = index,
        .call = std::move(call),
        .value = cached->second,
        .rerun_extra = wh::core::any(std::string{plan.call->arguments}),
    };
  }

  auto call_context = clone_call_context(context);
  if (call_context.has_error()) {
    return wh::core::result<call_completion>::failure(call_context.error());
  }
  auto scope = make_scope(call, call_context.value());
  auto before = run_before(*state.options, call, scope);
  if (before.has_error()) {
    auto merged = merge_call_context(context, call_context.value());
    if (merged.has_error()) {
      return wh::core::result<call_completion>::failure(merged.error());
    }
    return wh::core::result<call_completion>::failure(before.error());
  }

  auto invoked = call_value(state, call, scope);
  if (invoked.has_error()) {
    auto merged = merge_call_context(context, call_context.value());
    if (merged.has_error()) {
      return wh::core::result<call_completion>::failure(merged.error());
    }
    return wh::core::result<call_completion>::failure(invoked.error());
  }
  auto value = std::move(invoked).value();
  if (has_tool_afters(state.afters)) {
    auto after = run_after(state.afters, call, value, scope);
    if (after.has_error()) {
      auto merged = merge_call_context(context, call_context.value());
      if (merged.has_error()) {
        return wh::core::result<call_completion>::failure(merged.error());
      }
      return wh::core::result<call_completion>::failure(after.error());
    }
  }

  auto merged = merge_call_context(context, call_context.value());
  if (merged.has_error()) {
    return wh::core::result<call_completion>::failure(merged.error());
  }

  return call_completion{
      .index = index,
      .call = std::move(call),
      .value = std::move(value),
      .rerun_extra = wh::core::any(std::string{plan.call->arguments}),
  };
}

[[nodiscard]] inline auto run_stream_call(const tools_state &state,
                                          const std::size_t index,
                                          wh::core::run_context &context)
    -> wh::core::result<stream_completion> {
  const auto &plan = state.plans[index];
  auto call = make_call(plan);
  auto &rerun = state.rerun();
  if (!rerun.ids.empty() && !rerun.ids.contains(call.call_id)) {
    const auto cached = rerun.outputs.find(call.call_id);
    if (cached == rerun.outputs.end()) {
      return wh::core::result<stream_completion>::failure(
          wh::core::errc::not_found);
    }
    auto replayed = replay_stream(cached->second);
    if (replayed.has_error()) {
      return wh::core::result<stream_completion>::failure(replayed.error());
    }
    if (!has_tool_afters(state.afters)) {
      return make_stream_completion(
          index, std::move(call), std::move(replayed).value(),
          wh::core::any(std::string{plan.call->arguments}));
    }
    auto call_context = clone_call_context(context);
    if (call_context.has_error()) {
      return wh::core::result<stream_completion>::failure(call_context.error());
    }
    return make_stream_completion(
        index, std::move(call), std::move(replayed).value(),
        wh::core::any(std::string{plan.call->arguments}),
        std::move(call_context).value());
  }

  auto call_context = clone_call_context(context);
  if (call_context.has_error()) {
    return wh::core::result<stream_completion>::failure(call_context.error());
  }
  auto scope = make_scope(call, call_context.value());
  auto before = run_before(*state.options, call, scope);
  if (before.has_error()) {
    auto merged = merge_call_context(context, call_context.value());
    if (merged.has_error()) {
      return wh::core::result<stream_completion>::failure(merged.error());
    }
    return wh::core::result<stream_completion>::failure(before.error());
  }

  auto streamed = call_stream(state, call, scope);
  if (streamed.has_error()) {
    auto merged = merge_call_context(context, call_context.value());
    if (merged.has_error()) {
      return wh::core::result<stream_completion>::failure(merged.error());
    }
    return wh::core::result<stream_completion>::failure(streamed.error());
  }

  auto merged = merge_call_context(context, call_context.value());
  if (merged.has_error()) {
    return wh::core::result<stream_completion>::failure(merged.error());
  }

  if (!has_tool_afters(state.afters)) {
    return make_stream_completion(
        index, std::move(call), std::move(streamed).value(),
        wh::core::any(std::string{plan.call->arguments}));
  }

  return make_stream_completion(
      index, std::move(call), std::move(streamed).value(),
      wh::core::any(std::string{plan.call->arguments}),
      std::move(call_context).value());
}

[[nodiscard]] inline auto start_call(const tools_state &state,
                                     const std::size_t index,
                                     wh::core::run_context &base_context)
    -> call_completion_sender {
  const auto &plan = state.plans[index];
  auto call = make_call(plan);
  auto &rerun = state.rerun();
  if (!rerun.ids.empty() && !rerun.ids.contains(call.call_id)) {
    const auto cached = rerun.outputs.find(call.call_id);
    if (cached == rerun.outputs.end()) {
      return failure_sender<call_completion_sender,
                            wh::core::result<call_completion>>(
          wh::core::errc::not_found);
    }
    return ready_sender<call_completion_sender>(
        wh::core::result<call_completion>{call_completion{
            .index = index,
            .call = std::move(call),
            .value = cached->second,
            .rerun_extra = wh::core::any(std::string{plan.call->arguments})}});
  }

  auto cloned_context = clone_call_context(base_context);
  if (cloned_context.has_error()) {
    return failure_sender<call_completion_sender,
                          wh::core::result<call_completion>>(cloned_context.error());
  }
  auto call_context = std::move(cloned_context).value();
  auto scope = make_scope(call, call_context);
  auto before = run_before(*state.options, call, scope);
  if (before.has_error()) {
    return failure_sender<call_completion_sender,
                          wh::core::result<call_completion>>(before.error());
  }

  auto invoke = start_value(state, tool_call{call}, scope);
  if (!has_tool_afters(state.afters)) {
    return erase_call_completion(
        std::move(invoke) |
        stdexec::then([call = std::move(call), index,
                       rerun_extra = wh::core::any(
                           std::string{plan.call->arguments})](
                          wh::core::result<graph_value> status) mutable
                          -> wh::core::result<call_completion> {
          if (status.has_error()) {
            return wh::core::result<call_completion>::failure(status.error());
          }
          return call_completion{.index = index,
                                 .call = std::move(call),
                                 .value = std::move(status).value(),
                                 .rerun_extra = std::move(rerun_extra)};
        }));
  }

  auto call_context_ptr =
      std::make_unique<wh::core::run_context>(std::move(call_context));
  return erase_call_completion(
      std::move(invoke) |
      stdexec::then(
          [afters = state.afters, call = std::move(call), index,
           call_context = std::move(call_context_ptr),
           rerun_extra = wh::core::any(std::string{plan.call->arguments})](
              wh::core::result<graph_value> status) mutable
              -> wh::core::result<call_completion> {
            if (status.has_error()) {
              return wh::core::result<call_completion>::failure(status.error());
            }
            auto value = std::move(status).value();
            auto scope = make_scope(call, *call_context);
            auto after = run_after(afters, call, value, scope);
            if (after.has_error()) {
              return wh::core::result<call_completion>::failure(after.error());
            }
            return call_completion{.index = index,
                                   .call = std::move(call),
                                   .value = std::move(value),
                                   .rerun_extra = std::move(rerun_extra)};
          }));
}

[[nodiscard]] inline auto start_stream_call(const tools_state &state,
                                            const std::size_t index,
                                            wh::core::run_context &base_context)
    -> stream_completion_sender {
  const auto &plan = state.plans[index];
  auto call = make_call(plan);
  auto &rerun = state.rerun();
  if (!rerun.ids.empty() && !rerun.ids.contains(call.call_id)) {
    const auto cached = rerun.outputs.find(call.call_id);
    if (cached == rerun.outputs.end()) {
      return failure_sender<stream_completion_sender,
                            wh::core::result<stream_completion>>(
          wh::core::errc::not_found);
    }
    auto replayed = replay_stream(cached->second);
    if (replayed.has_error()) {
      return failure_sender<stream_completion_sender,
                            wh::core::result<stream_completion>>(
          replayed.error());
    }
    if (!has_tool_afters(state.afters)) {
      return ready_sender<stream_completion_sender>(
          wh::core::result<stream_completion>{make_stream_completion(
              index, std::move(call), std::move(replayed).value(),
              wh::core::any(std::string{plan.call->arguments}))});
    }
    auto cloned_context = clone_call_context(base_context);
    if (cloned_context.has_error()) {
      return failure_sender<stream_completion_sender,
                            wh::core::result<stream_completion>>(
          cloned_context.error());
    }
    return ready_sender<stream_completion_sender>(
        wh::core::result<stream_completion>{make_stream_completion(
            index, std::move(call), std::move(replayed).value(),
            wh::core::any(std::string{plan.call->arguments}),
            std::move(cloned_context).value())});
  }

  auto cloned_context = clone_call_context(base_context);
  if (cloned_context.has_error()) {
    return failure_sender<stream_completion_sender,
                          wh::core::result<stream_completion>>(cloned_context.error());
  }
  auto call_context = std::move(cloned_context).value();
  auto scope = make_scope(call, call_context);
  auto before = run_before(*state.options, call, scope);
  if (before.has_error()) {
    return failure_sender<stream_completion_sender,
                          wh::core::result<stream_completion>>(before.error());
  }

  auto stream = start_stream(state, tool_call{call}, scope);
  if (!has_tool_afters(state.afters)) {
    return erase_stream_completion(
        std::move(stream) |
        stdexec::then([call = std::move(call), index,
                       rerun_extra = wh::core::any(
                           std::string{plan.call->arguments})](
                          wh::core::result<graph_stream_reader> status) mutable
                          -> wh::core::result<stream_completion> {
          if (status.has_error()) {
            return wh::core::result<stream_completion>::failure(status.error());
          }
          return make_stream_completion(index, std::move(call),
                                        std::move(status).value(),
                                        std::move(rerun_extra));
        }));
  }

  auto call_context_ptr =
      std::make_unique<wh::core::run_context>(std::move(call_context));
  return erase_stream_completion(
      std::move(stream) |
      stdexec::then([call = std::move(call), index,
                     call_context = std::move(call_context_ptr),
                     rerun_extra =
                         wh::core::any(std::string{plan.call->arguments})](
                        wh::core::result<graph_stream_reader> status) mutable
                        -> wh::core::result<stream_completion> {
        if (status.has_error()) {
          return wh::core::result<stream_completion>::failure(status.error());
        }
        return make_stream_completion(index, std::move(call),
                                      std::move(status).value(),
                                      std::move(rerun_extra),
                                      std::move(*call_context));
      }));
}

} // namespace detail
} // namespace wh::compose
