// Defines the retry wrapper for chat models while preserving the base
// chat-model contract and tool-binding surface.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "wh/core/callback.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/stream/reader.hpp"

namespace wh::adk {

/// Retry event emitted before the wrapper starts the next attempt.
struct will_retry_error {
  /// One-based failed attempt index.
  std::size_t attempt{0U};
  /// Error observed on the failed attempt.
  wh::core::error_code error{};
  /// Stable string form of the failed attempt error.
  std::string error_text{};
  /// Computed backoff duration associated with the next retry.
  std::chrono::milliseconds backoff{0};
};

/// Retry event emitted when the wrapper exhausts the retry budget.
struct retry_exhausted_error {
  /// Total attempts consumed by the wrapper.
  std::size_t attempts{0U};
  /// Last observed attempt error.
  wh::core::error_code last_error{};
};

/// Predicate used to decide whether the wrapper should retry the last error.
using retry_predicate =
    wh::core::callback_function<bool(wh::core::error_code) const>;

/// Function used to compute the next backoff duration from the one-based
/// failed attempt index.
using retry_backoff_function =
    wh::core::callback_function<std::chrono::milliseconds(std::size_t) const>;

/// Retry policy parameters applied by `retry_chat_model`.
struct retry_chat_model_options {
  /// Maximum total attempts, including the first attempt.
  std::size_t max_attempts{3U};
  /// Predicate deciding whether the failed attempt may retry.
  retry_predicate should_retry{nullptr};
  /// Function computing the advisory backoff duration before the next attempt.
  retry_backoff_function backoff{nullptr};
};

namespace detail {

[[nodiscard]] inline auto default_retry_predicate(
    const wh::core::error_code) noexcept -> bool {
  return true;
}

[[nodiscard]] inline auto default_retry_backoff(const std::size_t attempt)
    -> std::chrono::milliseconds {
  using namespace std::chrono_literals;
  const auto capped_attempt = std::min<std::size_t>(attempt, 8U);
  const auto base = std::chrono::duration_cast<std::chrono::milliseconds>(
      100ms * (1ULL << (capped_attempt - 1U)));
  const auto jitter = std::chrono::milliseconds{
      static_cast<std::int64_t>((attempt * 17U) % 97U)};
  return std::min(base + jitter,
                  std::chrono::duration_cast<std::chrono::milliseconds>(10s));
}

[[nodiscard]] inline auto make_run_info(
    const wh::core::component_descriptor &descriptor)
    -> wh::core::callback_run_info {
  wh::core::callback_run_info info{};
  info.name = descriptor.type_name;
  info.type = descriptor.type_name;
  info.component = wh::core::component_kind::model;
  return info;
}

inline auto emit_will_retry(wh::core::run_context &context,
                            const wh::core::component_descriptor &descriptor,
                            const std::size_t attempt,
                            const wh::core::error_code error,
                            const std::chrono::milliseconds backoff) -> void {
  wh::core::inject_callback_event(
      context, wh::core::callback_stage::error,
      will_retry_error{
          .attempt = attempt,
          .error = error,
          .error_text = error.message(),
          .backoff = backoff,
      },
      make_run_info(descriptor));
}

inline auto emit_retry_exhausted(
    wh::core::run_context &context,
    const wh::core::component_descriptor &descriptor, const std::size_t attempts,
    const wh::core::error_code last_error) -> void {
  wh::core::inject_callback_event(
      context, wh::core::callback_stage::error,
      retry_exhausted_error{
          .attempts = attempts,
          .last_error = last_error,
      },
      make_run_info(descriptor));
}

[[nodiscard]] inline auto should_retry(
    const retry_chat_model_options &options,
    const wh::core::error_code error) -> bool {
  if (static_cast<bool>(options.should_retry)) {
    return options.should_retry(error);
  }
  return default_retry_predicate(error);
}

[[nodiscard]] inline auto compute_backoff(
    const retry_chat_model_options &options, const std::size_t attempt)
    -> std::chrono::milliseconds {
  if (static_cast<bool>(options.backoff)) {
    return options.backoff(attempt);
  }
  return default_retry_backoff(attempt);
}

[[nodiscard]] inline auto consume_stream_probe(
    wh::model::chat_message_stream_reader &reader) -> wh::core::result<void> {
  while (true) {
    auto next = reader.read();
    if (next.has_error()) {
      return wh::core::result<void>::failure(next.error());
    }
    if (next.value().error.failed()) {
      return wh::core::result<void>::failure(next.value().error);
    }
    if (next.value().eof) {
      return {};
    }
  }
}

template <typename success_t>
[[nodiscard]] inline auto finish_retry_failure(
    wh::core::run_context &context,
    const wh::core::component_descriptor &descriptor,
    const std::size_t attempt, const bool retryable,
    const wh::core::error_code error) -> wh::core::result<success_t> {
  if (retryable) {
    emit_retry_exhausted(context, descriptor, attempt, error);
    return wh::core::result<success_t>::failure(wh::core::errc::retry_exhausted);
  }
  return wh::core::result<success_t>::failure(error);
}

} // namespace detail

/// Retry wrapper that preserves the chat-model contract while retrying invoke
/// and stream startup/consumption failures.
template <wh::model::chat_model_like model_t>
class retry_chat_model {
public:
  retry_chat_model() = default;

  /// Stores the wrapped model plus one retry policy.
  explicit retry_chat_model(model_t model,
                            retry_chat_model_options options = {}) noexcept
      : model_(std::move(model)), options_(std::move(options)) {}

  retry_chat_model(const retry_chat_model &) = default;
  retry_chat_model(retry_chat_model &&) noexcept = default;
  auto operator=(const retry_chat_model &) -> retry_chat_model & = default;
  auto operator=(retry_chat_model &&) noexcept -> retry_chat_model & = default;
  ~retry_chat_model() = default;

  /// Returns stable descriptor metadata for the retry wrapper.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"RetryChatModel",
                                          wh::core::component_kind::model};
  }

  /// Exposes the wrapped model for tests and diagnostics.
  [[nodiscard]] auto wrapped_model() const noexcept -> const model_t & {
    return model_;
  }

  /// Exposes the retry policy for tests and diagnostics.
  [[nodiscard]] auto options() const noexcept
      -> const retry_chat_model_options & {
    return options_;
  }

  /// Retries invoke failures according to the configured policy.
  [[nodiscard]] auto invoke(const wh::model::chat_request &request,
                            wh::core::run_context &context) const
      -> wh::core::result<wh::model::chat_response> {
    const auto descriptor = this->descriptor();
    const auto max_attempts = std::max<std::size_t>(options_.max_attempts, 1U);
    for (std::size_t attempt = 1U; attempt <= max_attempts; ++attempt) {
      auto status = model_.invoke(request, context);
      if (status.has_value()) {
        return status;
      }

      const auto retryable = detail::should_retry(options_, status.error());
      if (!retryable || attempt >= max_attempts) {
        return detail::finish_retry_failure<wh::model::chat_response>(
            context, descriptor, attempt, retryable, status.error());
      }

      const auto backoff = detail::compute_backoff(options_, attempt);
      detail::emit_will_retry(context, descriptor, attempt, status.error(),
                              backoff);
    }
    return wh::core::result<wh::model::chat_response>::failure(
        wh::core::errc::retry_exhausted);
  }

  /// Retries stream startup failures and mid-stream terminal errors by probing
  /// a retained duplicate stream before handing the successful copy to the caller.
  [[nodiscard]] auto stream(const wh::model::chat_request &request,
                            wh::core::run_context &context) const
      -> wh::core::result<wh::model::chat_message_stream_reader> {
    const auto descriptor = this->descriptor();
    const auto max_attempts = std::max<std::size_t>(options_.max_attempts, 1U);
    for (std::size_t attempt = 1U; attempt <= max_attempts; ++attempt) {
      auto status = model_.stream(request, context);
      if (status.has_error()) {
        const auto retryable = detail::should_retry(options_, status.error());
        if (!retryable || attempt >= max_attempts) {
          return detail::finish_retry_failure<wh::model::chat_message_stream_reader>(
              context, descriptor, attempt, retryable, status.error());
        }
        const auto backoff = detail::compute_backoff(options_, attempt);
        detail::emit_will_retry(context, descriptor, attempt, status.error(),
                                backoff);
        continue;
      }

      auto copies =
          wh::schema::stream::make_copy_stream_readers(std::move(status).value(), 2U);
      if (copies.size() != 2U) {
        return wh::core::result<wh::model::chat_message_stream_reader>::failure(
            wh::core::errc::internal_error);
      }

      wh::model::chat_message_stream_reader consumer{std::move(copies[0])};
      wh::model::chat_message_stream_reader probe{std::move(copies[1])};
      auto probe_status = detail::consume_stream_probe(probe);
      probe.close();
      if (probe_status.has_value()) {
        return consumer;
      }

      consumer.close();
      const auto retryable =
          detail::should_retry(options_, probe_status.error());
      if (!retryable || attempt >= max_attempts) {
        return detail::finish_retry_failure<wh::model::chat_message_stream_reader>(
            context, descriptor, attempt, retryable, probe_status.error());
      }

      const auto backoff = detail::compute_backoff(options_, attempt);
      detail::emit_will_retry(context, descriptor, attempt, probe_status.error(),
                              backoff);
    }
    return wh::core::result<wh::model::chat_message_stream_reader>::failure(
        wh::core::errc::retry_exhausted);
  }

  /// Returns a retry wrapper that preserves retry semantics after tool binding.
  [[nodiscard]] auto bind_tools(
      const std::span<const wh::schema::tool_schema_definition> tools) const
      -> retry_chat_model {
    return retry_chat_model{model_.bind_tools(tools), options_};
  }

private:
  /// Wrapped chat model invoked on each attempt.
  model_t model_{};
  /// Retry policy used by invoke and stream paths.
  retry_chat_model_options options_{};
};

} // namespace wh::adk
