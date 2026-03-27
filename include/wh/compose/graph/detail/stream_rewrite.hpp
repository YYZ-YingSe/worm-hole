// Defines graph stream rewrite senders used by eager state-stream rewriting.
#pragma once

#include "wh/compose/graph/graph.hpp"

namespace wh::compose::detail {

template <typename handler_t> class rewrite_stream_sender {
  template <typename receiver_t> class operation {
    using receiver_type = std::remove_cvref_t<receiver_t>;
    using receiver_env_t =
        decltype(stdexec::get_env(std::declval<const receiver_type &>()));
    using chunk_result_t = typename graph_stream_reader::chunk_result_type;
    using child_sender_t = decltype(std::declval<graph_stream_reader &>().read_async());

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      operation *self{nullptr};
      std::uint8_t slot{0U};
      receiver_env_t env_{};

      auto set_value(chunk_result_t next) && noexcept -> void {
        self->finish_read(slot, std::move(next));
      }

      template <typename error_t>
      auto set_error(error_t &&error) && noexcept -> void {
        if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                   wh::core::error_code>) {
          self->complete(
              wh::core::result<graph_value>::failure(std::forward<error_t>(error)));
        } else if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                          std::exception_ptr>) {
          try {
            std::rethrow_exception(std::forward<error_t>(error));
          } catch (...) {
            self->complete(wh::core::result<graph_value>::failure(
                wh::core::map_current_exception()));
          }
        } else {
          self->complete(
              wh::core::result<graph_value>::failure(wh::core::errc::internal_error));
        }
      }

      auto set_stopped() && noexcept -> void {
        self->complete(
            wh::core::result<graph_value>::failure(wh::core::errc::canceled));
      }

      [[nodiscard]] auto get_env() const noexcept { return env_; }
    };

    using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

  public:
    using operation_state_concept = stdexec::operation_state_t;

    template <typename stored_receiver_t, typename stored_handler_t>
      requires std::constructible_from<receiver_type, stored_receiver_t &&> &&
               std::constructible_from<handler_t, stored_handler_t &&>
    operation(graph_stream_reader source, graph_stream_writer writer,
              stored_handler_t &&handler, stored_receiver_t &&receiver)
        : receiver_(std::forward<stored_receiver_t>(receiver)),
          env_(stdexec::get_env(receiver_)),
          source_(std::move(source)),
          writer_(std::move(writer)),
          handler_(std::forward<stored_handler_t>(handler)) {}

    auto start() & noexcept -> void { drive(0U); }

  private:
    auto finish_read(const std::uint8_t slot, chunk_result_t next) noexcept
        -> void {
      pending_slot_ = slot;
      pending_.emplace(std::move(next));
      if (driving_) {
        return;
      }
      blocked_slot_.emplace(slot);
      drive();
      blocked_slot_.reset();
    }

    auto drive() noexcept -> void {
      if (driving_ || done_) {
        return;
      }
      driving_ = true;
      while (!done_) {
        if (!pending_.has_value()) {
          start_read(next_slot_);
          if (!pending_.has_value()) {
            break;
          }
        }

        auto next = std::move(*pending_);
        pending_.reset();
        if (!handle_chunk(std::move(next), pending_slot_)) {
          break;
        }
      }
      driving_ = false;
    }

    auto drive(const std::uint8_t slot) noexcept -> void {
      next_slot_ = slot;
      drive();
    }

    auto start_read(const std::uint8_t slot) noexcept -> void {
      try {
        child_ops_[slot].reset();
        child_sender_t sender = source_.read_async();
        child_ops_[slot].emplace_from(stdexec::connect, std::move(sender),
                                      child_receiver{this, slot, env_});
        stdexec::start(child_ops_[slot].get());
      } catch (...) {
        complete(wh::core::result<graph_value>::failure(
            wh::core::map_current_exception()));
      }
    }

    [[nodiscard]] auto handle_chunk(chunk_result_t next,
                                    const std::uint8_t slot) noexcept -> bool {
      if (next.has_error()) {
        complete(wh::core::result<graph_value>::failure(next.error()));
        return false;
      }

      auto chunk = std::move(next).value();
      if (chunk.is_terminal_eof()) {
        auto closed = writer_.close();
        if (closed.has_error()) {
          complete(wh::core::result<graph_value>::failure(closed.error()));
          return false;
        }
        complete(
            wh::core::result<graph_value>{wh::core::any(std::monostate{})});
        return false;
      }
      if (chunk.is_source_eof()) {
        next_slot_ = resume_slot(slot);
        return true;
      }

      if (chunk.error != wh::core::errc::ok) {
        complete(wh::core::result<graph_value>::failure(chunk.error));
        return false;
      }

      if (chunk.value.has_value()) {
        auto chunk_payload = std::move(*chunk.value);
        auto handled = handler_(chunk_payload);
        if (handled.has_error()) {
          complete(wh::core::result<graph_value>::failure(handled.error()));
          return false;
        }
        auto wrote = writer_.try_write(std::move(chunk_payload));
        if (wrote.has_error()) {
          complete(wh::core::result<graph_value>::failure(wrote.error()));
          return false;
        }
      }

      next_slot_ = resume_slot(slot);
      return true;
    }

    [[nodiscard]] auto resume_slot(const std::uint8_t completed_slot) const
        noexcept -> std::uint8_t {
      if (blocked_slot_.has_value() && *blocked_slot_ == completed_slot) {
        return static_cast<std::uint8_t>(completed_slot ^ 1U);
      }
      return completed_slot;
    }

    auto complete(wh::core::result<graph_value> status) noexcept -> void {
      if (done_) {
        return;
      }
      done_ = true;
      pending_.reset();
      stdexec::set_value(std::move(receiver_), std::move(status));
    }

    receiver_type receiver_;
    receiver_env_t env_;
    graph_stream_reader source_{};
    graph_stream_writer writer_{};
    [[no_unique_address]] handler_t handler_;
    std::array<wh::core::detail::manual_lifetime_box<child_op_t>, 2U>
        child_ops_{};
    std::optional<chunk_result_t> pending_{};
    std::optional<std::uint8_t> blocked_slot_{};
    std::uint8_t next_slot_{0U};
    std::uint8_t pending_slot_{0U};
    bool driving_{false};
    bool done_{false};
  };

public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(wh::core::result<graph_value>)>;

  rewrite_stream_sender(graph_stream_reader source, graph_stream_writer writer,
                        handler_t handler)
      : source_(std::move(source)),
        writer_(std::move(writer)),
        handler_(std::move(handler)) {}

  rewrite_stream_sender(const rewrite_stream_sender &) = delete;
  auto operator=(const rewrite_stream_sender &) -> rewrite_stream_sender & = delete;
  rewrite_stream_sender(rewrite_stream_sender &&) noexcept = default;
  auto operator=(rewrite_stream_sender &&) noexcept -> rewrite_stream_sender & =
      default;

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) &&
      -> operation<receiver_t> {
    return operation<receiver_t>{std::move(source_), std::move(writer_),
                                 std::move(handler_), std::move(receiver)};
  }

private:
  graph_stream_reader source_{};
  graph_stream_writer writer_{};
  [[no_unique_address]] handler_t handler_;
};

template <typename handler_t>
[[nodiscard]] inline auto make_rewrite_stream_sender(
    graph_stream_reader source, graph_stream_writer writer, handler_t &&handler) {
  return rewrite_stream_sender<std::remove_cvref_t<handler_t>>{
      std::move(source), std::move(writer), std::forward<handler_t>(handler)};
}

} // namespace wh::compose::detail
