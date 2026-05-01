// Defines reusable ADK event-to-message stream helpers that flatten message
// substreams without rebuilding one intermediate event vector.
#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/adk/event_stream.hpp"
#include "wh/adk/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"
#include "wh/core/stdexec/detail/scheduled_resume_turn.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::adk::detail {

/// Consumes one movable message event and visits each materialized message in
/// order without first collecting a second owned vector.
template <typename visitor_t>
  requires std::invocable<visitor_t &, wh::schema::message>
inline auto consume_message_event_messages(message_event event, visitor_t &&visitor)
    -> wh::core::result<void> {
  if (auto *value = std::get_if<wh::schema::message>(&event.content); value != nullptr) {
    return std::invoke(visitor, std::move(*value));
  }

  auto *stream = std::get_if<agent_message_stream_reader>(&event.content);
  if (stream == nullptr) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }

  while (true) {
    auto next = stream->read();
    if (next.has_error()) {
      return wh::core::result<void>::failure(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.error.failed()) {
      return wh::core::result<void>::failure(chunk.error);
    }
    if (chunk.eof) {
      return {};
    }
    if (!chunk.value.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::protocol_error);
    }

    auto visited = std::invoke(visitor, std::move(*chunk.value));
    if (visited.has_error()) {
      return visited;
    }
  }
}

/// Shared reader that flattens `agent_event_stream_reader` into one pure
/// message stream while preserving event-stream EOF and error semantics.
class event_message_stream_reader final
    : public wh::schema::stream::stream_base<event_message_stream_reader, wh::schema::message> {
public:
  using value_type = wh::schema::message;
  using chunk_type = wh::schema::stream::stream_chunk<value_type>;
  using result_type = wh::schema::stream::stream_result<chunk_type>;
  using try_result_type = wh::schema::stream::stream_try_result<chunk_type>;
  using message_result_t =
      wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<wh::schema::message>>;
  using event_result_t = wh::adk::agent_event_stream_result;
  using active_try_result =
      std::variant<std::monostate, wh::schema::stream::stream_signal, result_type>;

  event_message_stream_reader() = default;

  explicit event_message_stream_reader(wh::adk::agent_event_stream_reader reader) noexcept
      : reader_(std::move(reader)) {}

  [[nodiscard]] auto read_impl() -> result_type {
    while (true) {
      if (active_message_reader_.has_value()) {
        auto active = read_active_message();
        if (active.has_value()) {
          return std::move(*active);
        }
        continue;
      }

      auto next = wh::adk::read_agent_event_stream(reader_);
      auto mapped = process_event_result(std::move(next));
      if (mapped.has_value()) {
        return std::move(*mapped);
      }
    }
  }

  [[nodiscard]] auto try_read_impl() -> try_result_type {
    while (true) {
      if (active_message_reader_.has_value()) {
        auto active = try_read_active_message();
        if (std::holds_alternative<wh::schema::stream::stream_signal>(active)) {
          return wh::schema::stream::stream_pending;
        }
        if (auto *result = std::get_if<result_type>(&active); result != nullptr) {
          return std::move(*result);
        }
        continue;
      }

      auto next = reader_.try_read();
      if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
        return wh::schema::stream::stream_pending;
      }

      auto mapped =
          process_event_result(std::move(std::get<wh::adk::agent_event_stream_result>(next)));
      if (mapped.has_value()) {
        return std::move(*mapped);
      }
    }
  }

  class read_sender {
  public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(result_type),
                                       stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>;

    explicit read_sender(event_message_stream_reader &owner) noexcept : owner_(&owner) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    class operation {
      using self_t = operation;
      using receiver_env_t =
          std::remove_cvref_t<decltype(stdexec::get_env(std::declval<const receiver_t &>()))>;
      using resume_scheduler_t = wh::core::detail::resume_scheduler_t<receiver_env_t>;
      friend class wh::core::detail::scheduled_resume_turn<self_t, resume_scheduler_t>;

      struct stopped_tag {};
      using child_completion_t =
          std::variant<event_result_t, message_result_t, wh::core::error_code, stopped_tag>;

      struct final_completion {
        std::optional<result_type> value{};
        bool stopped{false};
      };

      struct child_receiver {
        using receiver_concept = stdexec::receiver_t;

        operation *self{nullptr};
        receiver_env_t env_{};

        auto set_value(event_result_t status) && noexcept -> void {
          complete(child_completion_t{std::move(status)});
        }

        auto set_value(message_result_t status) && noexcept -> void {
          complete(child_completion_t{std::move(status)});
        }

        template <typename error_t> auto set_error(error_t &&error) && noexcept -> void {
          complete(map_async_error(std::forward<error_t>(error)));
        }

        auto set_stopped() && noexcept -> void { complete(child_completion_t{stopped_tag{}}); }

        [[nodiscard]] auto get_env() const noexcept -> receiver_env_t { return env_; }

      private:
        template <typename error_t>
        [[nodiscard]] static auto map_async_error(error_t &&error) noexcept -> child_completion_t {
          if constexpr (std::same_as<std::remove_cvref_t<error_t>, wh::core::error_code>) {
            return child_completion_t{std::forward<error_t>(error)};
          } else {
            if constexpr (std::same_as<std::remove_cvref_t<error_t>, std::exception_ptr>) {
              try {
                std::rethrow_exception(std::forward<error_t>(error));
              } catch (...) {
                return child_completion_t{wh::core::map_current_exception()};
              }
            } else {
              return child_completion_t{wh::core::make_error(wh::core::errc::internal_error)};
            }
          }
        }

        auto complete(child_completion_t completion) noexcept -> void {
          self->publish_child_completion(std::move(completion));
          self->request_resume();
          self->arrive();
        }
      };

      using event_child_sender_t =
          decltype(std::declval<wh::adk::agent_event_stream_reader &>().read_async());
      using message_child_sender_t =
          decltype(std::declval<wh::adk::agent_message_stream_reader &>().read_async());
      using event_child_op_t = stdexec::connect_result_t<event_child_sender_t, child_receiver>;
      using message_child_op_t = stdexec::connect_result_t<message_child_sender_t, child_receiver>;

      enum class child_kind : std::uint8_t { none = 0U, event, message };

    public:
      using operation_state_concept = stdexec::operation_state_t;

      operation(event_message_stream_reader *owner, receiver_t receiver)
          : owner_(owner), receiver_(std::move(receiver)), env_(stdexec::get_env(receiver_)),
            scheduler_(wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env_)),
            resume_turn_(scheduler_) {}

      operation(const operation &) = delete;
      auto operator=(const operation &) -> operation & = delete;
      operation(operation &&) = delete;
      auto operator=(operation &&) -> operation & = delete;

      ~operation() {
        resume_turn_.destroy();
        destroy_child();
      }

      auto start() & noexcept -> void {
        request_resume();
        arrive();
      }

    private:
      [[nodiscard]] auto completed() const noexcept -> bool {
        return completed_.load(std::memory_order_acquire);
      }

      [[nodiscard]] auto terminal_pending() const noexcept -> bool { return terminal_.has_value(); }

      [[nodiscard]] auto child_active() const noexcept -> bool {
        return child_kind_ != child_kind::none;
      }

      [[nodiscard]] auto child_completion_ready() const noexcept -> bool {
        return child_completion_ready_.load(std::memory_order_acquire);
      }

      [[nodiscard]] auto resume_turn_completed() const noexcept -> bool { return completed(); }

      auto request_resume() noexcept -> void { resume_turn_.request(this); }

      auto arrive() noexcept -> void {
        if (count_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
          maybe_complete();
        }
      }

      auto resume_turn_arrive() noexcept -> void { arrive(); }

      auto resume_turn_add_ref() noexcept -> void {
        count_.fetch_add(1U, std::memory_order_relaxed);
      }

      auto resume_turn_schedule_error(const wh::core::error_code error) noexcept -> void {
        set_terminal_failure(error);
      }

      auto resume_turn_run() noexcept -> void {
        resume();
        maybe_complete();
      }

      auto resume_turn_idle() noexcept -> void { maybe_complete(); }

      auto maybe_complete() noexcept -> void {
        if (completed()) {
          return;
        }
        if (count_.load(std::memory_order_acquire) != 0U || !should_complete()) {
          return;
        }
        complete();
      }

      [[nodiscard]] auto should_complete() const noexcept -> bool {
        return terminal_pending() && !child_active() && !child_completion_ready() &&
               !resume_turn_.running();
      }

      auto destroy_child() noexcept -> void {
        switch (child_kind_) {
        case child_kind::none:
          return;
        case child_kind::event:
          event_child_op_.template destruct<event_child_op_t>();
          break;
        case child_kind::message:
          message_child_op_.template destruct<message_child_op_t>();
          break;
        }
        child_kind_ = child_kind::none;
      }

      auto publish_child_completion(child_completion_t completion) noexcept -> void {
        if (completed()) {
          return;
        }
        wh_invariant(!child_completion_ready());
        child_completion_.emplace(std::move(completion));
        child_completion_ready_.store(true, std::memory_order_release);
      }

      [[nodiscard]] auto drain_child_completion() noexcept -> bool {
        if (!child_completion_ready_.exchange(false, std::memory_order_acq_rel)) {
          return false;
        }
        wh_invariant(child_completion_.has_value());
        auto current = std::move(*child_completion_);
        child_completion_.reset();
        destroy_child();

        if (auto *event = std::get_if<event_result_t>(&current); event != nullptr) {
          auto mapped = owner_->process_event_result(std::move(*event));
          if (mapped.has_value()) {
            set_terminal_value(std::move(*mapped));
          }
          return true;
        }
        if (auto *message = std::get_if<message_result_t>(&current); message != nullptr) {
          auto mapped = owner_->process_active_message_result(std::move(*message));
          if (mapped.has_value()) {
            set_terminal_value(std::move(*mapped));
          }
          return true;
        }
        if (auto *error = std::get_if<wh::core::error_code>(&current); error != nullptr) {
          set_terminal_failure(*error);
          return true;
        }
        set_terminal_stopped();
        return true;
      }

      [[nodiscard]] auto start_event_child() noexcept -> bool {
        try {
          [[maybe_unused]] auto &child_op =
              event_child_op_.template construct_with<event_child_op_t>([&]() -> event_child_op_t {
                return stdexec::connect(owner_->reader_.read_async(), child_receiver{this, env_});
              });
          child_kind_ = child_kind::event;
          count_.fetch_add(1U, std::memory_order_relaxed);
          stdexec::start(event_child_op_.template get<event_child_op_t>());
          return false;
        } catch (...) {
          destroy_child();
          set_terminal_failure(wh::core::map_current_exception());
          return true;
        }
      }

      [[nodiscard]] auto start_message_child() noexcept -> bool {
        try {
          [[maybe_unused]] auto &child_op =
              message_child_op_.template construct_with<message_child_op_t>(
                  [&]() -> message_child_op_t {
                    return stdexec::connect(owner_->active_message_reader_->read_async(),
                                            child_receiver{this, env_});
                  });
          child_kind_ = child_kind::message;
          count_.fetch_add(1U, std::memory_order_relaxed);
          stdexec::start(message_child_op_.template get<message_child_op_t>());
          return false;
        } catch (...) {
          destroy_child();
          set_terminal_failure(wh::core::map_current_exception());
          return true;
        }
      }

      auto set_terminal(final_completion completion) noexcept -> void {
        if (terminal_pending()) {
          return;
        }
        terminal_.emplace(std::move(completion));
        maybe_complete();
      }

      auto set_terminal_value(result_type status) noexcept -> void {
        set_terminal(final_completion{.value = std::move(status)});
      }

      auto set_terminal_failure(const wh::core::error_code error) noexcept -> void {
        set_terminal_value(result_type::failure(error));
      }

      auto set_terminal_stopped() noexcept -> void {
        set_terminal(final_completion{.stopped = true});
      }

      auto complete() noexcept -> void {
        if (!terminal_pending() || completed_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }

        auto completion = std::move(*terminal_);
        terminal_.reset();
        if (completion.stopped) {
          stdexec::set_stopped(std::move(receiver_));
          return;
        }
        wh_invariant(completion.value.has_value());
        stdexec::set_value(std::move(receiver_), std::move(*completion.value));
      }

      auto resume() noexcept -> void {
        while (!completed()) {
          if (drain_child_completion()) {
            continue;
          }

          if (terminal_pending()) {
            return;
          }

          if (child_active()) {
            return;
          }

          if (owner_->active_message_reader_.has_value()) {
            if (start_message_child()) {
              continue;
            }
            return;
          }

          if (start_event_child()) {
            continue;
          }
          return;
        }
      }

      event_message_stream_reader *owner_{nullptr};
      receiver_t receiver_;
      receiver_env_t env_{};
      resume_scheduler_t scheduler_{};
      wh::core::detail::manual_storage<sizeof(event_child_op_t), alignof(event_child_op_t)>
          event_child_op_{};
      wh::core::detail::manual_storage<sizeof(message_child_op_t), alignof(message_child_op_t)>
          message_child_op_{};
      std::optional<child_completion_t> child_completion_{};
      std::optional<final_completion> terminal_{};
      std::atomic<std::size_t> count_{1U};
      std::atomic<bool> child_completion_ready_{false};
      std::atomic<bool> completed_{false};
      wh::core::detail::scheduled_resume_turn<self_t, resume_scheduler_t> resume_turn_;
      child_kind child_kind_{child_kind::none};
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) && -> operation<receiver_t> {
      return operation<receiver_t>{owner_, std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> wh::core::detail::async_completion_env {
      return {};
    }

  private:
    event_message_stream_reader *owner_{nullptr};
  };

  [[nodiscard]] auto read_async() & -> read_sender { return read_sender{*this}; }

  auto close_impl() -> wh::core::result<void> {
    closed_ = true;
    if (active_message_reader_.has_value()) {
      auto closed = active_message_reader_->close();
      active_message_reader_.reset();
      if (closed.has_error() && closed.error() != wh::core::errc::not_found) {
        return closed;
      }
    }
    return wh::adk::close_agent_event_stream(reader_);
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return closed_ || reader_.is_closed();
  }

private:
  [[nodiscard]] auto process_event_result(event_result_t next) -> std::optional<result_type> {
    if (next.has_error() && next.error() == wh::core::errc::not_found) {
      closed_ = true;
      return result_type{chunk_type::make_eof()};
    }
    if (next.has_error()) {
      return result_type::failure(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.eof) {
      closed_ = true;
      return result_type{chunk_type::make_eof()};
    }
    if (chunk.error.failed()) {
      return result_type{chunk_type{.error = chunk.error}};
    }
    if (!chunk.value.has_value()) {
      return std::nullopt;
    }

    return map_event(std::move(*chunk.value));
  }

  [[nodiscard]] auto process_active_message_result(message_result_t next)
      -> std::optional<result_type> {
    if (next.has_error() && next.error() == wh::core::errc::not_found) {
      active_message_reader_.reset();
      return std::nullopt;
    }
    if (next.has_error()) {
      active_message_reader_.reset();
      return result_type{chunk_type{.error = next.error()}};
    }

    auto chunk = std::move(next).value();
    if (chunk.error.failed()) {
      active_message_reader_.reset();
      return result_type{chunk_type{.error = chunk.error}};
    }
    if (chunk.eof) {
      active_message_reader_.reset();
      return std::nullopt;
    }
    if (chunk.value.has_value()) {
      return result_type{chunk_type::make_value(std::move(*chunk.value))};
    }
    return std::nullopt;
  }

  auto map_event(wh::adk::agent_event event) -> std::optional<result_type> {
    if (auto *message = std::get_if<wh::adk::message_event>(&event.payload); message != nullptr) {
      if (auto *value = std::get_if<wh::schema::message>(&message->content); value != nullptr) {
        return result_type{chunk_type::make_value(std::move(*value))};
      }
      if (auto *reader = std::get_if<wh::adk::agent_message_stream_reader>(&message->content);
          reader != nullptr) {
        active_message_reader_.emplace(std::move(*reader));
      }
      return std::nullopt;
    }

    if (auto *error = std::get_if<wh::adk::error_event>(&event.payload); error != nullptr) {
      return result_type{chunk_type{.error = error->code}};
    }

    return std::nullopt;
  }

  auto read_active_message() -> std::optional<result_type> {
    while (active_message_reader_.has_value()) {
      auto next = active_message_reader_->read();
      auto mapped = process_active_message_result(std::move(next));
      if (mapped.has_value()) {
        return std::move(*mapped);
      }
    }
    return std::nullopt;
  }

  auto try_read_active_message() -> active_try_result {
    while (active_message_reader_.has_value()) {
      auto next = active_message_reader_->try_read();
      if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
        return wh::schema::stream::stream_pending;
      }

      auto mapped = process_active_message_result(std::move(std::get<message_result_t>(next)));
      if (mapped.has_value()) {
        return std::move(*mapped);
      }
    }
    return std::monostate{};
  }

  wh::adk::agent_event_stream_reader reader_{};
  std::optional<wh::adk::agent_message_stream_reader> active_message_reader_{};
  bool closed_{false};
};

} // namespace wh::adk::detail
