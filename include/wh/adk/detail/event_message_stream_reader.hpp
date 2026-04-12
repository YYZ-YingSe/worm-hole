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
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/detail/inline_drive_loop.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"
#include "wh/core/stdexec/detail/single_completion_slot.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::adk::detail {

/// Consumes one movable message event and visits each materialized message in
/// order without first collecting a second owned vector.
template <typename visitor_t>
  requires std::invocable<visitor_t &, wh::schema::message>
inline auto consume_message_event_messages(message_event event,
                                           visitor_t &&visitor)
    -> wh::core::result<void> {
  if (auto *value = std::get_if<wh::schema::message>(&event.content);
      value != nullptr) {
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
    : public wh::schema::stream::stream_base<event_message_stream_reader,
                                             wh::schema::message> {
public:
  using value_type = wh::schema::message;
  using chunk_type = wh::schema::stream::stream_chunk<value_type>;
  using result_type = wh::schema::stream::stream_result<chunk_type>;
  using try_result_type = wh::schema::stream::stream_try_result<chunk_type>;
  using message_result_t = wh::schema::stream::stream_result<
      wh::schema::stream::stream_chunk<wh::schema::message>>;
  using event_result_t = wh::adk::agent_event_stream_result;
  using active_try_result =
      std::variant<std::monostate, wh::schema::stream::stream_signal,
                   result_type>;

  event_message_stream_reader() = default;

  explicit event_message_stream_reader(
      wh::adk::agent_event_stream_reader reader) noexcept
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

      auto mapped = process_event_result(
          std::move(std::get<wh::adk::agent_event_stream_result>(next)));
      if (mapped.has_value()) {
        return std::move(*mapped);
      }
    }
  }

  class read_sender {
  public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(result_type),
        stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

    explicit read_sender(event_message_stream_reader &owner) noexcept
        : owner_(&owner) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    class operation
        : public wh::core::detail::inline_drive_loop<operation<receiver_t>> {
      using drive_loop_t =
          wh::core::detail::inline_drive_loop<operation<receiver_t>>;
      friend drive_loop_t;
      friend class wh::core::detail::callback_guard<operation>;
      using receiver_env_t = std::remove_cvref_t<
          decltype(stdexec::get_env(std::declval<const receiver_t &>()))>;
      using final_completion_t =
          wh::core::detail::receiver_completion<receiver_t, result_type>;

      struct stopped_tag {};
      using child_completion_t =
          std::variant<event_result_t, message_result_t, std::exception_ptr,
                       stopped_tag>;

      struct child_receiver {
        using receiver_concept = stdexec::receiver_t;

        operation *self{nullptr};
        receiver_env_t env_{};

        auto set_value(event_result_t status) && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          self->finish_child(child_completion_t{std::move(status)});
        }

        auto set_value(message_result_t status) && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          self->finish_child(child_completion_t{std::move(status)});
        }

        template <typename error_t>
        auto set_error(error_t &&error) && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                     std::exception_ptr>) {
            self->finish_child(
                child_completion_t{std::forward<error_t>(error)});
          } else {
            try {
              throw std::forward<error_t>(error);
            } catch (...) {
              self->finish_child(child_completion_t{std::current_exception()});
            }
          }
        }

        auto set_stopped() && noexcept -> void {
          auto scope = self->callbacks_.enter(self);
          self->finish_child(child_completion_t{stopped_tag{}});
        }

        [[nodiscard]] auto get_env() const noexcept -> receiver_env_t {
          return env_;
        }
      };

      using event_child_sender_t = decltype(
          std::declval<wh::adk::agent_event_stream_reader &>().read_async());
      using message_child_sender_t = decltype(
          std::declval<wh::adk::agent_message_stream_reader &>().read_async());
      using event_child_op_t =
          stdexec::connect_result_t<event_child_sender_t, child_receiver>;
      using message_child_op_t =
          stdexec::connect_result_t<message_child_sender_t, child_receiver>;

      enum class child_kind : std::uint8_t { none = 0U, event, message };

    public:
      using operation_state_concept = stdexec::operation_state_t;

      operation(event_message_stream_reader *owner, receiver_t receiver)
          : owner_(owner), receiver_(std::move(receiver)),
            env_(stdexec::get_env(receiver_)) {}

      auto start() & noexcept -> void { request_drive(); }

    private:
      [[nodiscard]] auto finished() const noexcept -> bool {
        return delivered_.load(std::memory_order_acquire);
      }

      [[nodiscard]] auto completion_pending() const noexcept -> bool {
        return pending_completion_.has_value();
      }

      [[nodiscard]] auto take_completion() noexcept
          -> std::optional<final_completion_t> {
        if (!pending_completion_.has_value()) {
          return std::nullopt;
        }
        auto completion = std::move(pending_completion_);
        pending_completion_.reset();
        return completion;
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
            reset_child();
            if (auto *event = std::get_if<event_result_t>(&*current);
                event != nullptr) {
              auto mapped = owner_->process_event_result(std::move(*event));
              if (mapped.has_value()) {
                complete_value(std::move(*mapped));
                return;
              }
              continue;
            }

            if (auto *message = std::get_if<message_result_t>(&*current);
                message != nullptr) {
              auto mapped =
                  owner_->process_active_message_result(std::move(*message));
              if (mapped.has_value()) {
                complete_value(std::move(*mapped));
                return;
              }
              continue;
            }

            if (auto *error = std::get_if<std::exception_ptr>(&*current);
                error != nullptr) {
              complete_error(std::move(*error));
              return;
            }

            complete_stopped();
            return;
          }

          if (owner_->active_message_reader_.has_value()) {
            start_message_child();
            return;
          }

          start_event_child();
          return;
        }
      }

      auto reset_child() noexcept -> void {
        switch (child_kind_) {
        case child_kind::none:
          return;
        case child_kind::event:
          event_child_op_.reset();
          break;
        case child_kind::message:
          message_child_op_.reset();
          break;
        }
        child_kind_ = child_kind::none;
      }

      auto start_event_child() noexcept -> void {
        try {
          event_child_op_.emplace_from(stdexec::connect,
                                       owner_->reader_.read_async(),
                                       child_receiver{this, env_});
          child_kind_ = child_kind::event;
          stdexec::start(event_child_op_.get());
        } catch (...) {
          complete_error(std::current_exception());
        }
      }

      auto start_message_child() noexcept -> void {
        if (!owner_->active_message_reader_.has_value()) {
          child_kind_ = child_kind::none;
          request_drive();
          return;
        }

        try {
          message_child_op_.emplace_from(
              stdexec::connect, owner_->active_message_reader_->read_async(),
              child_receiver{this, env_});
          child_kind_ = child_kind::message;
          stdexec::start(message_child_op_.get());
        } catch (...) {
          complete_error(std::current_exception());
        }
      }

      auto finish_child(child_completion_t completion) noexcept -> void {
        if (finished()) {
          return;
        }
        if (!completion_.publish(std::move(completion))) {
          std::terminate();
        }
        request_drive();
      }

      auto complete_value(result_type status) noexcept -> void {
        if (delivered_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        pending_completion_.emplace(
            final_completion_t::set_value(std::move(receiver_),
                                          std::move(status)));
      }

      auto complete_error(std::exception_ptr error) noexcept -> void {
        if (delivered_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        pending_completion_.emplace(
            final_completion_t::set_error(std::move(receiver_),
                                          std::move(error)));
      }

      auto complete_stopped() noexcept -> void {
        if (delivered_.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        pending_completion_.emplace(
            final_completion_t::set_stopped(std::move(receiver_)));
      }

      event_message_stream_reader *owner_{nullptr};
      receiver_t receiver_;
      receiver_env_t env_{};
      wh::core::detail::manual_lifetime_box<event_child_op_t> event_child_op_{};
      wh::core::detail::manual_lifetime_box<message_child_op_t>
          message_child_op_{};
      wh::core::detail::single_completion_slot<child_completion_t> completion_{};
      wh::core::detail::callback_guard<operation> callbacks_{};
      std::optional<final_completion_t> pending_completion_{};
      std::atomic<bool> delivered_{false};
      child_kind child_kind_{child_kind::none};
    };

    template <stdexec::receiver_of<completion_signatures> receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) &&
        -> operation<receiver_t> {
      return operation<receiver_t>{owner_, std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept
        -> wh::core::detail::async_completion_env {
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
  [[nodiscard]] auto process_event_result(event_result_t next)
      -> std::optional<result_type> {
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
    if (auto *message = std::get_if<wh::adk::message_event>(&event.payload);
        message != nullptr) {
      if (auto *value = std::get_if<wh::schema::message>(&message->content);
          value != nullptr) {
        return result_type{chunk_type::make_value(std::move(*value))};
      }
      if (auto *reader =
              std::get_if<wh::adk::agent_message_stream_reader>(&message->content);
          reader != nullptr) {
        active_message_reader_.emplace(std::move(*reader));
      }
      return std::nullopt;
    }

    if (auto *error = std::get_if<wh::adk::error_event>(&event.payload);
        error != nullptr) {
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

      auto mapped = process_active_message_result(
          std::move(std::get<message_result_t>(next)));
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
