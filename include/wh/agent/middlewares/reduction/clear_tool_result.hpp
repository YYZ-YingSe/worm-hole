// Defines request-level history reduction helpers that shrink old tool message
// payloads before the next model turn.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "wh/agent/middlewares/surface.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"

namespace wh::agent::middlewares::reduction {

/// Token estimator used by tool-result reduction.
using token_estimator = wh::core::callback_function<std::size_t(const wh::schema::message &) const>;

/// Public configuration for the clear-tool-result request transform.
struct clear_tool_result_options {
  /// Total-token threshold above which old tool messages may be reduced.
  std::size_t max_history_tokens{4096U};
  /// Recent suffix window that must remain untouched.
  std::size_t protected_recent_tokens{1024U};
  /// Placeholder text written into reduced tool messages.
  std::string placeholder{"[tool result omitted]"};
  /// Tool names that must never be reduced.
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      excluded_tool_names{};
  /// Optional replacement token estimator.
  token_estimator estimate_tokens{nullptr};
};

namespace detail {

[[nodiscard]] inline auto count_part_tokens(const wh::schema::message_part &part) -> std::size_t {
  if (const auto *text = std::get_if<wh::schema::text_part>(&part); text != nullptr) {
    return (text->text.size() + 3U) / 4U;
  }
  if (const auto *tool = std::get_if<wh::schema::tool_call_part>(&part); tool != nullptr) {
    return (tool->arguments.size() + 3U) / 4U;
  }
  return 0U;
}

[[nodiscard]] inline auto default_estimate_tokens(const wh::schema::message &message)
    -> std::size_t {
  std::size_t total = 0U;
  for (const auto &part : message.parts) {
    total += count_part_tokens(part);
  }
  return total;
}

[[nodiscard]] inline auto should_reduce_message(const wh::schema::message &message,
                                                const clear_tool_result_options &options) -> bool {
  return message.role == wh::schema::message_role::tool &&
         !options.excluded_tool_names.contains(message.tool_name);
}

} // namespace detail

/// Creates one request transform that clears old tool-message content once the
/// configured total-token threshold is exceeded.
[[nodiscard]] inline auto make_clear_tool_result_transform(clear_tool_result_options options = {})
    -> wh::agent::middlewares::request_transform_binding {
  if (options.placeholder.empty()) {
    options.placeholder = "[tool result omitted]";
  }

  return wh::agent::middlewares::request_transform_binding{
      .sync = [options = std::move(options)](
                  wh::model::chat_request request,
                  wh::core::run_context &) -> wh::agent::middlewares::request_transform_result {
        const auto &estimate = static_cast<bool>(options.estimate_tokens)
                                   ? options.estimate_tokens
                                   : token_estimator{detail::default_estimate_tokens};

        std::size_t total_tokens = 0U;
        for (const auto &message : request.messages) {
          total_tokens += estimate(message);
        }
        if (total_tokens <= options.max_history_tokens) {
          return request;
        }

        std::size_t protected_tokens = 0U;
        std::size_t protected_begin = request.messages.size();
        while (protected_begin > 0U && protected_tokens < options.protected_recent_tokens) {
          --protected_begin;
          protected_tokens += estimate(request.messages[protected_begin]);
        }

        for (std::size_t index = 0U; index < protected_begin; ++index) {
          auto &message = request.messages[index];
          if (!detail::should_reduce_message(message, options)) {
            continue;
          }
          message.parts.clear();
          message.parts.emplace_back(wh::schema::text_part{options.placeholder});
        }
        return request;
      }};
}

/// Builds one reduction middleware surface that exports the clear-tool-result
/// transform and no tools.
[[nodiscard]] inline auto make_clear_tool_result_surface(clear_tool_result_options options = {})
    -> wh::agent::middlewares::middleware_surface {
  wh::agent::middlewares::middleware_surface surface{};
  surface.request_transforms.push_back(make_clear_tool_result_transform(std::move(options)));
  return surface;
}

} // namespace wh::agent::middlewares::reduction
