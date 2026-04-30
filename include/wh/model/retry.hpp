// Defines the retry wrapper for chat models while preserving the base
// chat-model contract and tool-binding surface.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>

#include "wh/core/callback.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/function.hpp"
#include "wh/core/intrusive_ptr.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/reader.hpp"

#include <stdexec/execution.hpp>

namespace wh::model {

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
using retry_predicate = wh::core::callback_function<bool(wh::core::error_code) const>;

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

[[nodiscard]] inline auto default_retry_predicate(const wh::core::error_code) noexcept -> bool {
  return true;
}

[[nodiscard]] inline auto default_retry_backoff(const std::size_t attempt)
    -> std::chrono::milliseconds {
  using namespace std::chrono_literals;
  const auto capped_attempt = std::min<std::size_t>(attempt, 8U);
  const auto base = std::chrono::duration_cast<std::chrono::milliseconds>(
      100ms * (1ULL << (capped_attempt - 1U)));
  const auto jitter = std::chrono::milliseconds{static_cast<std::int64_t>((attempt * 17U) % 97U)};
  return std::min(base + jitter, std::chrono::duration_cast<std::chrono::milliseconds>(10s));
}

[[nodiscard]] inline auto make_retry_run_info(const wh::core::component_descriptor &descriptor)
    -> wh::core::callback_run_info {
  wh::core::callback_run_info info{};
  info.name = descriptor.type_name;
  info.type = descriptor.type_name;
  info.component = wh::core::component_kind::model;
  return info;
}

inline auto emit_will_retry(wh::core::run_context &context,
                            const wh::core::component_descriptor &descriptor,
                            const std::size_t attempt, const wh::core::error_code error,
                            const std::chrono::milliseconds backoff) -> void {
  wh::core::inject_callback_event(context, wh::core::callback_stage::error,
                                  will_retry_error{
                                      .attempt = attempt,
                                      .error = error,
                                      .error_text = error.message(),
                                      .backoff = backoff,
                                  },
                                  make_retry_run_info(descriptor));
}

inline auto emit_retry_exhausted(wh::core::run_context &context,
                                 const wh::core::component_descriptor &descriptor,
                                 const std::size_t attempts, const wh::core::error_code last_error)
    -> void {
  wh::core::inject_callback_event(context, wh::core::callback_stage::error,
                                  retry_exhausted_error{
                                      .attempts = attempts,
                                      .last_error = last_error,
                                  },
                                  make_retry_run_info(descriptor));
}

[[nodiscard]] inline auto should_retry(const retry_chat_model_options &options,
                                       const wh::core::error_code error) -> bool {
  if (static_cast<bool>(options.should_retry)) {
    return options.should_retry(error);
  }
  return default_retry_predicate(error);
}

[[nodiscard]] inline auto compute_backoff(const retry_chat_model_options &options,
                                          const std::size_t attempt) -> std::chrono::milliseconds {
  if (static_cast<bool>(options.backoff)) {
    return options.backoff(attempt);
  }
  return default_retry_backoff(attempt);
}

[[nodiscard]] inline auto max_retry_attempts(const retry_chat_model_options &options)
    -> std::size_t {
  return std::max<std::size_t>(options.max_attempts, 1U);
}

template <typename model_t>
concept retry_sync_invoke_const_handler =
    requires(const model_t &model, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      { model.invoke(request, context) } -> std::same_as<wh::model::chat_invoke_result>;
    };

template <typename model_t>
concept retry_sync_invoke_move_handler =
    requires(const model_t &model, wh::model::chat_request &&request,
             wh::core::run_context &context) {
      { model.invoke(std::move(request), context) } -> std::same_as<wh::model::chat_invoke_result>;
    };

template <typename model_t>
concept retry_sync_invoke_handler =
    retry_sync_invoke_const_handler<model_t> || retry_sync_invoke_move_handler<model_t>;

template <typename model_t>
concept retry_sync_stream_const_handler =
    requires(const model_t &model, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      { model.stream(request, context) } -> std::same_as<wh::model::chat_message_stream_result>;
    };

template <typename model_t>
concept retry_sync_stream_move_handler =
    requires(const model_t &model, wh::model::chat_request &&request,
             wh::core::run_context &context) {
      { model.stream(std::move(request), context) } -> std::same_as<wh::model::chat_message_stream_result>;
    };

template <typename model_t>
concept retry_sync_stream_handler =
    retry_sync_stream_const_handler<model_t> || retry_sync_stream_move_handler<model_t>;

template <typename model_t>
concept retry_async_invoke_const_handler =
    requires(const model_t &model, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      { model.async_invoke(request, context) } -> stdexec::sender;
    };

template <typename model_t>
concept retry_async_invoke_move_handler =
    requires(const model_t &model, wh::model::chat_request &&request,
             wh::core::run_context &context) {
      { model.async_invoke(std::move(request), context) } -> stdexec::sender;
    };

template <typename model_t>
concept retry_async_invoke_handler =
    retry_async_invoke_const_handler<model_t> || retry_async_invoke_move_handler<model_t>;

template <typename model_t>
concept retry_async_stream_const_handler =
    requires(const model_t &model, const wh::model::chat_request &request,
             wh::core::run_context &context) {
      { model.async_stream(request, context) } -> stdexec::sender;
    };

template <typename model_t>
concept retry_async_stream_move_handler =
    requires(const model_t &model, wh::model::chat_request &&request,
             wh::core::run_context &context) {
      { model.async_stream(std::move(request), context) } -> stdexec::sender;
    };

template <typename model_t>
concept retry_async_stream_handler =
    retry_async_stream_const_handler<model_t> || retry_async_stream_move_handler<model_t>;

template <typename model_t>
concept retry_model_like =
    wh::core::component_descriptor_provider<model_t> &&
    requires(const model_t &model, std::span<const wh::schema::tool_schema_definition> tools) {
      { model.bind_tools(tools) } -> std::same_as<std::remove_cvref_t<model_t>>;
    } &&
    (retry_sync_invoke_handler<model_t> || retry_async_invoke_handler<model_t>) &&
    (retry_sync_stream_handler<model_t> || retry_async_stream_handler<model_t>);

template <typename model_t>
  requires retry_sync_invoke_handler<model_t>
[[nodiscard]] inline auto run_retry_sync_invoke(const model_t &model,
                                                const wh::model::chat_request &request,
                                                wh::core::run_context &context)
    -> wh::model::chat_invoke_result {
  if constexpr (retry_sync_invoke_const_handler<model_t>) {
    return model.invoke(request, context);
  } else {
    return model.invoke(wh::model::chat_request{request}, context);
  }
}

template <typename model_t>
  requires retry_sync_stream_handler<model_t>
[[nodiscard]] inline auto run_retry_sync_stream(const model_t &model,
                                                const wh::model::chat_request &request,
                                                wh::core::run_context &context)
    -> wh::model::chat_message_stream_result {
  if constexpr (retry_sync_stream_const_handler<model_t>) {
    return model.stream(request, context);
  } else {
    return model.stream(wh::model::chat_request{request}, context);
  }
}

template <typename model_t>
  requires retry_async_invoke_handler<model_t>
[[nodiscard]] inline auto make_retry_async_invoke_sender(const model_t &model,
                                                         const wh::model::chat_request &request,
                                                         wh::core::run_context &context) {
  if constexpr (retry_async_invoke_const_handler<model_t>) {
    return model.async_invoke(request, context);
  } else {
    return wh::core::detail::request_result_sender<wh::model::chat_invoke_result>(
        wh::model::chat_request{request},
        [&model, &context](auto &&owned_request) -> decltype(auto) {
          return model.async_invoke(std::forward<decltype(owned_request)>(owned_request), context);
        });
  }
}

template <typename model_t>
  requires retry_async_stream_handler<model_t>
[[nodiscard]] inline auto make_retry_async_stream_sender(const model_t &model,
                                                         const wh::model::chat_request &request,
                                                         wh::core::run_context &context) {
  if constexpr (retry_async_stream_const_handler<model_t>) {
    return model.async_stream(request, context);
  } else {
    return wh::core::detail::request_result_sender<wh::model::chat_message_stream_result>(
        wh::model::chat_request{request},
        [&model, &context](auto &&owned_request) -> decltype(auto) {
          return model.async_stream(std::forward<decltype(owned_request)>(owned_request), context);
        });
  }
}

template <typename result_t, stdexec::sender sender_t>
[[nodiscard]] inline auto wait_retry_sender(sender_t &&sender) -> result_t {
  auto waited = stdexec::sync_wait(
      wh::core::detail::normalize_result_sender<result_t>(std::forward<sender_t>(sender)));
  if (!waited.has_value()) {
    return result_t::failure(wh::core::errc::canceled);
  }
  return std::move(std::get<0>(*waited));
}

template <typename model_t>
  requires retry_async_stream_handler<model_t>
[[nodiscard]] inline auto run_retry_blocking_async_stream(const model_t &model,
                                                          const wh::model::chat_request &request,
                                                          wh::core::run_context &context)
    -> wh::model::chat_message_stream_result {
  return wait_retry_sender<wh::model::chat_message_stream_result>(
      make_retry_async_stream_sender(model, request, context));
}

// Returns final failure when retry should stop; returns nullopt when caller
// should open the next attempt instead.
[[nodiscard]] inline auto resolve_retry_failure(wh::core::run_context &context,
                                                const wh::core::component_descriptor &descriptor,
                                                const retry_chat_model_options &options,
                                                const std::size_t attempt,
                                                const std::size_t max_attempts,
                                                const wh::core::error_code error)
    -> std::optional<wh::core::error_code> {
  const auto retryable = should_retry(options, error);
  if (!retryable) {
    return error;
  }

  if (attempt >= max_attempts) {
    emit_retry_exhausted(context, descriptor, attempt, error);
    return wh::core::make_error(wh::core::errc::retry_exhausted);
  }

  const auto backoff = compute_backoff(options, attempt);
  emit_will_retry(context, descriptor, attempt, error, backoff);
  return std::nullopt;
}

template <typename model_t>
class retry_stream_reader final
    : public wh::schema::stream::stream_base<retry_stream_reader<model_t>, wh::schema::message> {
private:
  using chunk_type = wh::schema::stream::stream_chunk<wh::schema::message>;
  using result_t = wh::schema::stream::stream_result<chunk_type>;
  using try_result_t = wh::schema::stream::stream_try_result<chunk_type>;
  using async_result_sender = wh::core::detail::result_sender<result_t>;
  using open_sender = wh::core::detail::result_sender<wh::core::result<void>>;

public:
  retry_stream_reader() = default;
  retry_stream_reader(const retry_stream_reader &) = delete;
  auto operator=(const retry_stream_reader &) -> retry_stream_reader & = delete;
  retry_stream_reader(retry_stream_reader &&) noexcept = default;
  auto operator=(retry_stream_reader &&) noexcept -> retry_stream_reader & = default;
  ~retry_stream_reader() = default;

  // The returned reader owns the retry state so stream() can return
  // immediately and reopen the upstream model later when a terminal read fails.
  retry_stream_reader(std::shared_ptr<const model_t> model, retry_chat_model_options options,
                      wh::core::component_descriptor descriptor, wh::model::chat_request request,
                      wh::core::run_context context,
                      wh::model::chat_message_stream_reader reader,
                      const std::size_t attempt, const std::size_t max_attempts)
      : state_(wh::core::detail::make_intrusive<state>(
            std::move(model), std::move(options), std::move(descriptor), std::move(request),
            std::move(context), std::move(reader), attempt, max_attempts)) {}

  [[nodiscard]] auto read_impl() -> result_t {
    try {
      return read_next(state_);
    } catch (...) {
      state_->terminal = true;
      [[maybe_unused]] const auto closed = close_current_reader(state_, true);
      return result_t::failure(wh::core::map_current_exception());
    }
  }

  [[nodiscard]] auto try_read_impl() -> try_result_t {
    try {
      return try_read_next(state_);
    } catch (...) {
      state_->terminal = true;
      [[maybe_unused]] const auto closed = close_current_reader(state_, true);
      return result_t::failure(wh::core::map_current_exception());
    }
  }

  [[nodiscard]] auto read_async() const {
    if constexpr (retry_async_stream_handler<model_t>) {
      return read_async_impl(state_);
    } else {
      return async_result_sender{stdexec::just(read_next(state_))};
    }
  }

  auto close_impl() -> wh::core::result<void> {
    state_->terminal = true;
    return close_current_reader(state_, true);
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool { return state_->terminal; }

  auto set_automatic_close(const wh::schema::stream::auto_close_options &options) -> void {
    state_->automatic_close = options.enabled;
    if (state_->reader.has_value()) {
      wh::schema::stream::detail::set_automatic_close_if_supported(*state_->reader, options);
    }
  }

private:
  struct state : wh::core::detail::intrusive_enable_from_this<state> {
    std::shared_ptr<const model_t> model{};
    retry_chat_model_options options{};
    wh::core::component_descriptor descriptor{};
    wh::model::chat_request request{};
    wh::core::run_context context{};
    std::optional<wh::model::chat_message_stream_reader> reader{};
    std::size_t attempt{0U};
    std::size_t max_attempts{1U};
    bool automatic_close{true};
    bool terminal{false};

    state() = default;

    state(std::shared_ptr<const model_t> model_value, retry_chat_model_options options_value,
          wh::core::component_descriptor descriptor_value, wh::model::chat_request request_value,
          wh::core::run_context context_value, wh::model::chat_message_stream_reader reader_value,
          const std::size_t attempt_value, const std::size_t max_attempts_value)
        : model(std::move(model_value)), options(std::move(options_value)),
          descriptor(std::move(descriptor_value)), request(std::move(request_value)),
          context(std::move(context_value)), reader(std::move(reader_value)), attempt(attempt_value),
          max_attempts(max_attempts_value) {}
  };

  [[nodiscard]] static auto make_error_chunk(std::string source,
                                             const wh::core::error_code error) -> result_t {
    auto chunk = chunk_type{};
    chunk.source = std::move(source);
    chunk.error = error;
    return result_t{std::move(chunk)};
  }

  [[nodiscard]] static auto close_current_reader(const wh::core::detail::intrusive_ptr<state> &state,
                                                 const bool force) -> wh::core::result<void> {
    if (!state->reader.has_value()) {
      return {};
    }
    if (!force && !state->automatic_close) {
      return {};
    }

    auto status = state->reader->close();
    state->reader.reset();
    if (status.has_error() && status.error() != wh::core::errc::channel_closed) {
      return status;
    }
    return {};
  }

  static auto attach_reader(const wh::core::detail::intrusive_ptr<state> &state,
                            wh::model::chat_message_stream_reader reader) -> void {
    wh::schema::stream::detail::set_automatic_close_if_supported(
        reader, wh::schema::stream::auto_close_options{.enabled = state->automatic_close});
    state->reader.emplace(std::move(reader));
  }

  [[nodiscard]] static auto open_next_attempt(const wh::core::detail::intrusive_ptr<state> &state)
      -> wh::core::result<void> {
    while (!state->terminal && !state->reader.has_value()) {
      const auto next_attempt = state->attempt + 1U;
      wh::model::chat_message_stream_result status{};
      if constexpr (retry_sync_stream_handler<model_t>) {
        status = run_retry_sync_stream(*state->model, state->request, state->context);
      } else {
        status = run_retry_blocking_async_stream(*state->model, state->request, state->context);
      }
      state->attempt = next_attempt;
      if (status.has_value()) {
        attach_reader(state, std::move(status).value());
        return {};
      }

      auto final_error = resolve_retry_failure(state->context, state->descriptor, state->options,
                                               state->attempt, state->max_attempts,
                                               status.error());
      if (final_error.has_value()) {
        state->terminal = true;
        return wh::core::result<void>::failure(*final_error);
      }
    }
    return {};
  }

  [[nodiscard]] static auto open_next_attempt_async(
      const wh::core::detail::intrusive_ptr<state> &state) -> open_sender
    requires retry_async_stream_handler<model_t>
  {
    if (!state || !state->model) {
      return open_sender{stdexec::just(wh::core::result<void>::failure(wh::core::errc::not_found))};
    }
    if (state->terminal || state->reader.has_value()) {
      return open_sender{stdexec::just(wh::core::result<void>{})};
    }

    const auto next_attempt = state->attempt + 1U;
    state->attempt = next_attempt;
    return open_sender{
        wh::core::detail::normalize_result_sender<wh::model::chat_message_stream_result>(
            make_retry_async_stream_sender(*state->model, state->request, state->context)) |
        stdexec::let_value(
            [state, next_attempt](wh::model::chat_message_stream_result &status) mutable
                -> open_sender {
              if (status.has_value()) {
                attach_reader(state, std::move(status).value());
                return open_sender{stdexec::just(wh::core::result<void>{})};
              }

              auto final_error = resolve_retry_failure(state->context, state->descriptor,
                                                       state->options, next_attempt,
                                                       state->max_attempts, status.error());
              if (final_error.has_value()) {
                state->terminal = true;
                return open_sender{
                    stdexec::just(wh::core::result<void>::failure(*final_error))};
              }
              return open_next_attempt_async(std::move(state));
            })};
  }

  [[nodiscard]] static auto retry_after_error(const wh::core::detail::intrusive_ptr<state> &state,
                                              const wh::core::error_code error)
      -> std::optional<wh::core::error_code> {
    [[maybe_unused]] const auto closed = close_current_reader(state, true);
    auto final_error = resolve_retry_failure(state->context, state->descriptor, state->options,
                                             state->attempt, state->max_attempts, error);
    if (final_error.has_value()) {
      state->terminal = true;
    }
    return final_error;
  }

  [[nodiscard]] static auto read_next(const wh::core::detail::intrusive_ptr<state> &state)
      -> result_t {
    while (true) {
      if (!state || !state->model) {
        return result_t::failure(wh::core::errc::not_found);
      }
      if (state->terminal) {
        return result_t{chunk_type::make_eof()};
      }

      auto ready = open_next_attempt(state);
      if (ready.has_error()) {
        return result_t::failure(ready.error());
      }

      auto next = state->reader->read();
      if (next.has_error()) {
        auto final_error = retry_after_error(state, next.error());
        if (final_error.has_value()) {
          return result_t::failure(*final_error);
        }
        continue;
      }

      auto chunk = std::move(next).value();
      if (chunk.error.failed()) {
        auto final_error = retry_after_error(state, chunk.error);
        if (final_error.has_value()) {
          return make_error_chunk(std::move(chunk.source), *final_error);
        }
        continue;
      }

      if (chunk.eof) {
        state->terminal = true;
        auto close_status = close_current_reader(state, false);
        if (close_status.has_error()) {
          return result_t::failure(close_status.error());
        }
      }
      return result_t{std::move(chunk)};
    }
  }

  [[nodiscard]] static auto try_read_next(const wh::core::detail::intrusive_ptr<state> &state)
      -> try_result_t {
    while (true) {
      if (!state || !state->model) {
        return result_t::failure(wh::core::errc::not_found);
      }
      if (state->terminal) {
        return result_t{chunk_type::make_eof()};
      }

      auto ready = open_next_attempt(state);
      if (ready.has_error()) {
        return result_t::failure(ready.error());
      }

      auto next = state->reader->try_read();
      if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
        return wh::schema::stream::stream_pending;
      }

      auto result = std::get<result_t>(std::move(next));
      if (result.has_error()) {
        auto final_error = retry_after_error(state, result.error());
        if (final_error.has_value()) {
          return result_t::failure(*final_error);
        }
        continue;
      }

      auto chunk = std::move(result).value();
      if (chunk.error.failed()) {
        auto final_error = retry_after_error(state, chunk.error);
        if (final_error.has_value()) {
          return make_error_chunk(std::move(chunk.source), *final_error);
        }
        continue;
      }

      if (chunk.eof) {
        state->terminal = true;
        auto close_status = close_current_reader(state, false);
        if (close_status.has_error()) {
          return result_t::failure(close_status.error());
        }
      }
      return result_t{std::move(chunk)};
    }
  }

  [[nodiscard]] static auto read_async_impl(const wh::core::detail::intrusive_ptr<state> &state)
      -> async_result_sender
    requires retry_async_stream_handler<model_t>
  {
    using input_result_t = result_t;
    if (!state || !state->model) {
      return async_result_sender{stdexec::just(result_t::failure(wh::core::errc::not_found))};
    }
    if (state->terminal) {
      return async_result_sender{stdexec::just(result_t{chunk_type::make_eof()})};
    }
    if (!state->reader.has_value()) {
      return async_result_sender{
          open_next_attempt_async(state) |
          stdexec::let_value([state](wh::core::result<void> &ready) mutable -> async_result_sender {
            if (ready.has_error()) {
              return async_result_sender{stdexec::just(result_t::failure(ready.error()))};
            }
            return read_async_impl(std::move(state));
          })};
    }

    auto sender = state->reader->read_async();
    return async_result_sender{
        std::move(sender) |
        stdexec::then([](auto status) { return input_result_t{std::move(status)}; }) |
        stdexec::upon_error([](auto &&) noexcept {
          return input_result_t::failure(wh::core::errc::internal_error);
        }) |
        stdexec::let_value([state](input_result_t &next) mutable -> async_result_sender {
          if (next.has_error()) {
            auto final_error = retry_after_error(state, next.error());
            if (final_error.has_value()) {
              return async_result_sender{stdexec::just(result_t::failure(*final_error))};
            }
            return read_async_impl(std::move(state));
          }

          auto chunk = std::move(next).value();
          if (chunk.error.failed()) {
            auto final_error = retry_after_error(state, chunk.error);
            if (final_error.has_value()) {
              return async_result_sender{
                  stdexec::just(make_error_chunk(std::move(chunk.source), *final_error))};
            }
            return read_async_impl(std::move(state));
          }

          if (chunk.eof) {
            state->terminal = true;
            auto close_status = close_current_reader(state, false);
            if (close_status.has_error()) {
              return async_result_sender{
                  stdexec::just(result_t::failure(close_status.error()))};
            }
          }
          return async_result_sender{stdexec::just(result_t{std::move(chunk)})};
        })};
  }

  wh::core::detail::intrusive_ptr<state> state_{wh::core::detail::make_intrusive<state>()};
};

template <typename model_t>
struct retry_async_state : wh::core::detail::intrusive_enable_from_this<retry_async_state<model_t>> {
  std::shared_ptr<const model_t> model{};
  retry_chat_model_options options{};
  wh::core::component_descriptor descriptor{};
  wh::model::chat_request request{};
  wh::core::run_context context{};
  std::size_t next_attempt{0U};
  std::size_t max_attempts{1U};

  retry_async_state(std::shared_ptr<const model_t> model_value,
                    retry_chat_model_options options_value,
                    wh::core::component_descriptor descriptor_value,
                    wh::model::chat_request request_value,
                    wh::core::run_context context_value, const std::size_t max_attempts_value)
      : model(std::move(model_value)), options(std::move(options_value)),
        descriptor(std::move(descriptor_value)), request(std::move(request_value)),
        context(std::move(context_value)), max_attempts(max_attempts_value) {}
};

template <typename model_t>
[[nodiscard]] inline auto make_retry_stream_result(
    const std::shared_ptr<model_t> &model, const retry_chat_model_options &options,
    const wh::core::component_descriptor &descriptor, const wh::model::chat_request &request,
    const wh::core::run_context &context, wh::model::chat_message_stream_reader reader,
    const std::size_t attempt, const std::size_t max_attempts)
    -> wh::model::chat_message_stream_result {
  auto owned_context = wh::core::clone_run_context(context);
  if (owned_context.has_error()) {
    [[maybe_unused]] const auto closed = reader.close();
    return wh::core::result<wh::model::chat_message_stream_reader>::failure(owned_context.error());
  }

  return wh::model::chat_message_stream_reader{
      detail::retry_stream_reader<model_t>{model, options, descriptor, request,
                                           std::move(owned_context).value(), std::move(reader),
                                           attempt, max_attempts}};
}

template <typename model_t>
[[nodiscard]] inline auto make_retry_async_invoke_sender(
    wh::core::detail::intrusive_ptr<retry_async_state<model_t>> state)
    -> wh::core::detail::result_sender<wh::model::chat_invoke_result>
  requires retry_async_invoke_handler<model_t>
{
  using sender_t = wh::core::detail::result_sender<wh::model::chat_invoke_result>;
  if (!state || !state->model) {
    return sender_t{
        stdexec::just(wh::model::chat_invoke_result::failure(wh::core::errc::not_found))};
  }

  const auto attempt = state->next_attempt + 1U;
  state->next_attempt = attempt;
  return sender_t{
      wh::core::detail::normalize_result_sender<wh::model::chat_invoke_result>(
          detail::make_retry_async_invoke_sender(*state->model, state->request, state->context)) |
      stdexec::let_value([state = std::move(state),
                          attempt](wh::model::chat_invoke_result &status) mutable -> sender_t {
        if (status.has_value()) {
          return sender_t{stdexec::just(std::move(status))};
        }

        auto final_error = resolve_retry_failure(state->context, state->descriptor, state->options,
                                                 attempt, state->max_attempts, status.error());
        if (final_error.has_value()) {
          return sender_t{
              stdexec::just(wh::model::chat_invoke_result::failure(*final_error))};
        }
        return make_retry_async_invoke_sender(std::move(state));
      })};
}

template <typename model_t>
[[nodiscard]] inline auto make_retry_async_stream_sender(
    wh::core::detail::intrusive_ptr<retry_async_state<model_t>> state)
    -> wh::core::detail::result_sender<wh::model::chat_message_stream_result>
  requires retry_async_stream_handler<model_t>
{
  using sender_t = wh::core::detail::result_sender<wh::model::chat_message_stream_result>;
  if (!state || !state->model) {
    return sender_t{stdexec::just(
        wh::model::chat_message_stream_result::failure(wh::core::errc::not_found))};
  }

  const auto attempt = state->next_attempt + 1U;
  state->next_attempt = attempt;
  return sender_t{
      wh::core::detail::normalize_result_sender<wh::model::chat_message_stream_result>(
          detail::make_retry_async_stream_sender(*state->model, state->request, state->context)) |
      stdexec::let_value([state = std::move(state),
                          attempt](wh::model::chat_message_stream_result &status) mutable
                         -> sender_t {
        if (status.has_error()) {
          auto final_error = resolve_retry_failure(state->context, state->descriptor,
                                                   state->options, attempt, state->max_attempts,
                                                   status.error());
          if (final_error.has_value()) {
            return sender_t{stdexec::just(
                wh::model::chat_message_stream_result::failure(*final_error))};
          }
          return make_retry_async_stream_sender(std::move(state));
        }

        return sender_t{stdexec::just(detail::make_retry_stream_result(
            state->model, state->options, state->descriptor, state->request, state->context,
            std::move(status).value(), attempt, state->max_attempts))};
      })};
}

} // namespace detail

/// Retry wrapper that preserves the chat-model contract while retrying invoke
/// and stream startup or consumption failures.
template <detail::retry_model_like model_t> class retry_chat_model {
public:
  retry_chat_model()
    requires std::default_initializable<model_t>
      : model_(std::make_shared<model_t>()) {}

  /// Stores the wrapped model plus one retry policy.
  explicit retry_chat_model(model_t model, retry_chat_model_options options = {}) noexcept
      : model_(std::make_shared<model_t>(std::move(model))), options_(std::move(options)) {}

  retry_chat_model(const retry_chat_model &) = default;
  retry_chat_model(retry_chat_model &&) noexcept = default;
  auto operator=(const retry_chat_model &) -> retry_chat_model & = default;
  auto operator=(retry_chat_model &&) noexcept -> retry_chat_model & = default;
  ~retry_chat_model() = default;

  /// Returns stable descriptor metadata for the retry wrapper.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"RetryChatModel", wh::core::component_kind::model};
  }

  /// Exposes the wrapped model for diagnostics and tests.
  [[nodiscard]] auto wrapped_model() const noexcept -> const model_t & { return *model_; }

  /// Exposes the retry policy for diagnostics and tests.
  [[nodiscard]] auto options() const noexcept -> const retry_chat_model_options & {
    return options_;
  }

  /// Retries invoke failures according to the configured policy.
  [[nodiscard]] auto invoke(const wh::model::chat_request &request,
                            wh::core::run_context &context) const
      -> wh::core::result<wh::model::chat_response>
    requires detail::retry_sync_invoke_handler<model_t>
  {
    const auto descriptor = this->descriptor();
    const auto max_attempts = detail::max_retry_attempts(options_);
    for (std::size_t attempt = 1U; attempt <= max_attempts; ++attempt) {
      auto status = detail::run_retry_sync_invoke(*model_, request, context);
      if (status.has_value()) {
        return status;
      }

      auto final_error =
          detail::resolve_retry_failure(context, descriptor, options_, attempt, max_attempts,
                                        status.error());
      if (final_error.has_value()) {
        return wh::core::result<wh::model::chat_response>::failure(*final_error);
      }
    }
    return wh::core::result<wh::model::chat_response>::failure(wh::core::errc::retry_exhausted);
  }

  /// Retries stream startup failures before returning, then hands back a reader
  /// that will reopen the upstream model on later terminal read failures.
  [[nodiscard]] auto stream(const wh::model::chat_request &request,
                            wh::core::run_context &context) const
      -> wh::core::result<wh::model::chat_message_stream_reader>
    requires detail::retry_sync_stream_handler<model_t>
  {
    const auto descriptor = this->descriptor();
    const auto max_attempts = detail::max_retry_attempts(options_);
    for (std::size_t attempt = 1U; attempt <= max_attempts; ++attempt) {
      auto status = detail::run_retry_sync_stream(*model_, request, context);
      if (status.has_error()) {
        auto final_error =
            detail::resolve_retry_failure(context, descriptor, options_, attempt, max_attempts,
                                          status.error());
        if (final_error.has_value()) {
          return wh::core::result<wh::model::chat_message_stream_reader>::failure(*final_error);
        }
        continue;
      }

      return detail::make_retry_stream_result(model_, options_, descriptor, request, context,
                                              std::move(status).value(), attempt, max_attempts);
    }

    return wh::core::result<wh::model::chat_message_stream_reader>::failure(
        wh::core::errc::retry_exhausted);
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, wh::model::chat_request> &&
             detail::retry_async_invoke_handler<model_t>
  [[nodiscard]] auto async_invoke(request_t &&request, wh::core::run_context &context) const {
    auto owned_context = wh::core::clone_run_context(context);
    if (owned_context.has_error()) {
      return wh::core::detail::result_sender<wh::model::chat_invoke_result>{
          stdexec::just(wh::model::chat_invoke_result::failure(owned_context.error()))};
    }

    return detail::make_retry_async_invoke_sender<model_t>(
        wh::core::detail::make_intrusive<detail::retry_async_state<model_t>>(
            model_, options_, this->descriptor(),
            wh::model::chat_request{std::forward<request_t>(request)},
            std::move(owned_context).value(), detail::max_retry_attempts(options_)));
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, wh::model::chat_request> &&
             detail::retry_async_stream_handler<model_t>
  [[nodiscard]] auto async_stream(request_t &&request, wh::core::run_context &context) const {
    auto owned_context = wh::core::clone_run_context(context);
    if (owned_context.has_error()) {
      return wh::core::detail::result_sender<wh::model::chat_message_stream_result>{
          stdexec::just(wh::model::chat_message_stream_result::failure(owned_context.error()))};
    }

    return detail::make_retry_async_stream_sender<model_t>(
        wh::core::detail::make_intrusive<detail::retry_async_state<model_t>>(
            model_, options_, this->descriptor(),
            wh::model::chat_request{std::forward<request_t>(request)},
            std::move(owned_context).value(), detail::max_retry_attempts(options_)));
  }

  /// Binds tools on the wrapped model first, then reapplies the retry wrapper.
  [[nodiscard]] auto
  bind_tools(const std::span<const wh::schema::tool_schema_definition> tools) const
      -> retry_chat_model<model_t> {
    return retry_chat_model<model_t>{model_->bind_tools(tools), options_};
  }

private:
  /// Wrapped model component.
  std::shared_ptr<model_t> model_{};
  /// Retry policy applied to invoke and stream.
  retry_chat_model_options options_{};
};

} // namespace wh::model
