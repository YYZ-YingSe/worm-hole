// Provides declarations and utilities for `wh/tool/utils/error_handler.hpp`.
#pragma once

#include <string>
#include <utility>

#include "wh/core/error.hpp"
#include "wh/schema/stream.hpp"
#include "wh/tool/tool.hpp"

namespace wh::tool::utils {

/// Preserves control-flow errors, wraps others as internal error.
[[nodiscard]] inline auto pass_through_or_wrap(const wh::core::error_code error)
    -> wh::core::error_code {
  if (error.code() == wh::core::errc::canceled ||
      error.code() == wh::core::errc::contract_violation) {
    return error;
  }
  return wh::core::make_error(wh::core::errc::internal_error);
}

/// Returns true for interrupt/resume-related control-flow errors.
[[nodiscard]] inline auto
is_interrupt_or_resume_error(const wh::core::error_code error) -> bool {
  return error.code() == wh::core::errc::canceled ||
         error.code() == wh::core::errc::contract_violation;
}

/// Applies tool invoke error policy for one-shot invocation path.
[[nodiscard]] inline auto
wrap_invoke_error(wh::core::result<std::string> invoked)
    -> wh::core::result<std::string> {
  if (invoked.has_value()) {
    return invoked;
  }
  if (is_interrupt_or_resume_error(invoked.error())) {
    return wh::core::result<std::string>::failure(invoked.error());
  }
  return wh::core::result<std::string>::failure(
      pass_through_or_wrap(invoked.error()));
}

/// Converts stream-start errors into fallback error stream when needed.
[[nodiscard]] inline auto
wrap_stream_error(wh::core::result<tool_output_stream_reader> streamed,
                  const std::string_view tool_name)
    -> wh::core::result<tool_output_stream_reader> {
  if (streamed.has_value()) {
    return streamed;
  }
  if (is_interrupt_or_resume_error(streamed.error())) {
    return wh::core::result<tool_output_stream_reader>::failure(
        streamed.error());
  }

  std::string error_text = "[";
  error_text += std::string{tool_name};
  error_text += "] ";
  error_text += streamed.error().to_string();
  return tool_output_stream_reader{
      wh::schema::stream::make_single_value_stream_reader<std::string>(
          std::move(error_text))};
}

namespace detail {

template <typename tool_t> struct error_policy_tool_impl {
  tool_t input_tool{};

  [[nodiscard]] auto invoke(const wh::tool::tool_request &request) const
      -> wh::core::result<std::string>
    requires requires(const tool_t &tool, const wh::tool::tool_request &value,
                      wh::core::run_context &context) {
      {
        tool.invoke(value, context)
      } -> std::same_as<wh::tool::tool_invoke_result>;
    }
  {
    wh::core::run_context callback_context{};
    return wrap_invoke_error(input_tool.invoke(request, callback_context));
  }

  [[nodiscard]] auto stream(const wh::tool::tool_request &request) const
      -> wh::core::result<tool_output_stream_reader>
    requires requires(const tool_t &tool, const wh::tool::tool_request &value,
                      wh::core::run_context &context) {
      {
        tool.stream(value, context)
      } -> std::same_as<wh::tool::tool_output_stream_result>;
    }
  {
    wh::core::run_context callback_context{};
    return wrap_stream_error(input_tool.stream(request, callback_context),
                             input_tool.schema().name);
  }
};

} // namespace detail

/// Returns a tool handle that applies the standard error policy.
template <typename tool_t>
[[nodiscard]] inline auto apply_error_policy(const tool_t &input_tool) {
  if constexpr (
      !requires(const tool_t &tool, const wh::tool::tool_request &request,
                wh::core::run_context &context) {
        {
          tool.invoke(request, context)
        } -> std::same_as<wh::tool::tool_invoke_result>;
      } &&
      !requires(const tool_t &tool, const wh::tool::tool_request &request,
                wh::core::run_context &context) {
        {
          tool.stream(request, context)
        } -> std::same_as<wh::tool::tool_output_stream_result>;
      }) {
    return input_tool;
  }

  return wh::tool::tool{input_tool.schema(),
                        detail::error_policy_tool_impl<tool_t>{input_tool},
                        input_tool.default_options()};
}

} // namespace wh::tool::utils
