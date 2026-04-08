// Defines stream merge helpers that interleave multiple reader lanes into one
// merged stream.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/detail/child_completion_mailbox.hpp"
#include "wh/core/stdexec/detail/child_set.hpp"
#include "wh/core/stdexec/detail/scheduled_drive_loop.hpp"
#include "wh/core/stdexec/detail/shared_operation_state.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::schema::stream {

/// One named input lane for merge-stream polling.
template <stream_reader reader_t> struct named_stream_reader {
  using value_type = typename reader_t::value_type;

  /// Source label copied into emitted chunks.
  std::string source{};
  /// Underlying reader endpoint.
  reader_t reader{};
  /// Marks reader as finished after EOF or explicit close.
  bool finished{false};
};

template <typename source_t, typename reader_t>
  requires stream_reader<std::remove_cvref_t<reader_t>> &&
           std::constructible_from<std::string, source_t>
named_stream_reader(source_t, reader_t, bool = false)
    -> named_stream_reader<std::remove_cvref_t<reader_t>>;

#include "wh/schema/stream/reader/detail/dynamic_topology_registry.hpp"

namespace detail {

template <typename error_t>
[[nodiscard]] inline auto map_merge_async_error(error_t &&error) noexcept
    -> wh::core::error_code {
  using error_type = std::remove_cvref_t<error_t>;

  if constexpr (std::same_as<error_type, wh::core::error_code>) {
    return std::forward<error_t>(error);
  } else if constexpr (std::same_as<error_type, std::exception_ptr>) {
    try {
      std::rethrow_exception(std::forward<error_t>(error));
    } catch (const std::exception &exception) {
      return wh::core::map_exception(exception);
    } catch (...) {
      return wh::core::errc::internal_error;
    }
  } else {
    return wh::core::errc::internal_error;
  }
}

template <stream_reader reader_t>
inline auto
sort_named_stream_readers(std::vector<named_stream_reader<reader_t>> &readers)
    -> void {
  std::ranges::sort(readers, [](const named_stream_reader<reader_t> &left,
                                const named_stream_reader<reader_t> &right) {
    return left.source < right.source;
  });
}

template <stream_reader reader_t>
[[nodiscard]] inline auto
make_named_stream_readers(std::vector<reader_t> &&readers)
    -> std::vector<named_stream_reader<reader_t>> {
  std::vector<named_stream_reader<reader_t>> named_readers{};
  named_readers.reserve(readers.size());
  for (std::size_t index = 0U; index < readers.size(); ++index) {
    named_readers.push_back(named_stream_reader<reader_t>{
        std::to_string(index), std::move(readers[index]), false});
  }
  return named_readers;
}

} // namespace detail

/// Merges multiple named readers into one interleaved output stream.
template <stream_reader reader_t>
class merge_stream_reader final
    : public stream_base<merge_stream_reader<reader_t>,
                         typename reader_t::value_type> {
public:
  using value_type = typename reader_t::value_type;
  using lane_type = named_stream_reader<reader_t>;
  using chunk_type = stream_chunk<value_type>;
  using lane_source_type = std::string;
  using status_type = stream_result<chunk_type>;
  using try_status_type = stream_try_result<chunk_type>;

  /// Reader polling strategy.
  enum class poll_mode {
    /// Poll readers in fixed registration order.
    fixed,
    /// Poll readers adaptively based on runtime readiness.
    dynamic,
  };

private:
  using topology_registry_t = detail::dynamic_topology_registry<reader_t>;
  using topology_sync_waiter = typename topology_registry_t::sync_waiter_type;
  using topology_async_waiter = typename topology_registry_t::async_waiter_type;
  using round_resolution = typename topology_registry_t::round_resolution;

  struct round_outcome {
    std::optional<std::size_t> winner_position{};
    std::optional<status_type> winner_status{};
    bool stopped_without_winner{false};
  };

  class round_tracker {
  public:
    auto reset(const std::size_t count) -> void {
      if (count > capacity_) {
        slots_ = std::make_unique<slot[]>(count);
        capacity_ = count;
      }
      for (std::size_t index = 0U; index < count; ++index) {
        slots_[index].status.reset();
      }
      pending_.store(count, std::memory_order_release);
      finish_sequence_.store(0U, std::memory_order_release);
      winner_key_.store(0U, std::memory_order_release);
      winner_position_.store(no_winner_position, std::memory_order_release);
      stopped_without_winner_.store(false, std::memory_order_release);
    }

    [[nodiscard]] auto note_status(const std::size_t position,
                                   status_type status) noexcept -> bool {
      const auto candidate_rank = classify_winner(status);
      if (candidate_rank == 0U) {
        return false;
      }

      slots_[position].status.emplace(std::move(status));
      const auto candidate_key = make_winner_key(
          candidate_rank,
          finish_sequence_.fetch_add(1U, std::memory_order_acq_rel));
      auto winner_key = winner_key_.load(std::memory_order_acquire);
      while (candidate_key > winner_key) {
        if (winner_key_.compare_exchange_weak(winner_key, candidate_key,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          winner_position_.store(position, std::memory_order_release);
          return candidate_rank >= 2U;
        }
      }
      return false;
    }

    auto note_stop() noexcept -> void {
      stopped_without_winner_.store(true, std::memory_order_release);
    }

    [[nodiscard]] auto finish_one() noexcept -> bool {
      return pending_.fetch_sub(1U, std::memory_order_acq_rel) == 1U;
    }

    [[nodiscard]] auto take_outcome() noexcept -> round_outcome {
      round_outcome outcome{};
      const auto winner_position =
          winner_position_.load(std::memory_order_acquire);
      if (winner_position != no_winner_position) {
        outcome.winner_position = winner_position;
        outcome.winner_status.emplace(
            std::move(*slots_[winner_position].status));
        return outcome;
      }
      outcome.stopped_without_winner =
          stopped_without_winner_.load(std::memory_order_acquire);
      return outcome;
    }

  private:
    struct slot {
      std::optional<status_type> status{};
    };

    [[nodiscard]] static auto
    classify_winner(const status_type &status) noexcept -> std::uint8_t {
      if (status.has_error()) {
        return 3U;
      }
      return status.value().eof ? 1U : 2U;
    }

    [[nodiscard]] static auto
    make_winner_key(const std::uint8_t rank,
                    const std::uint64_t sequence) noexcept -> std::uint64_t {
      constexpr auto sequence_mask =
          std::numeric_limits<std::uint64_t>::max() >> 8U;
      return (std::uint64_t{rank} << 56U) | (sequence_mask - sequence);
    }

    static constexpr auto no_winner_position =
        std::numeric_limits<std::size_t>::max();

    std::unique_ptr<slot[]> slots_{};
    std::size_t capacity_{0U};
    std::atomic<std::size_t> pending_{0U};
    std::atomic<std::uint64_t> finish_sequence_{0U};
    std::atomic<std::uint64_t> winner_key_{0U};
    std::atomic<std::size_t> winner_position_{no_winner_position};
    std::atomic<bool> stopped_without_winner_{false};
  };

  struct shared_state {
    using lane_list = typename topology_registry_t::lane_list;
    using round_spec = typename topology_registry_t::round_plan;

    struct blocking_read_op {
      using scheduler_t = stdexec::inline_scheduler;
      using child_sender_t = decltype(std::declval<reader_t &>().read_async());
      friend class wh::core::detail::callback_guard<blocking_read_op>;

      struct child_completion {
        std::size_t position{0U};
        status_type status{};
        bool stopped_signal{false};
      };

      struct child_env {
        stdexec::inplace_stop_token stop_token{};

        [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
            -> stdexec::inplace_stop_token {
          return stop_token;
        }

        [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
            -> scheduler_t {
          return {};
        }

        [[nodiscard]] auto
        query(stdexec::get_delegation_scheduler_t) const noexcept
            -> scheduler_t {
          return {};
        }

        template <typename cpo_t>
        [[nodiscard]] auto
        query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
            -> scheduler_t {
          return {};
        }
      };

      struct child_receiver {
        using receiver_concept = stdexec::receiver_t;

        blocking_read_op *round{nullptr};
        std::uint32_t slot_id{0U};
        std::size_t position{0U};
        std::shared_ptr<stdexec::inplace_stop_source> stop_source{};

        auto set_value(stream_result<chunk_type> status) noexcept -> void {
          auto scope = round->callbacks_.enter(round);
          round->enqueue_completion(slot_id, child_completion{
                                                 .position = position,
                                                 .status = std::move(status),
                                                 .stopped_signal = false,
                                             });
        }

        template <typename error_t>
        auto set_error(error_t &&error) noexcept -> void {
          auto scope = round->callbacks_.enter(round);
          round->enqueue_completion(
              slot_id,
              child_completion{
                  .position = position,
                  .status = status_type::failure(detail::map_merge_async_error(
                      std::forward<error_t>(error))),
                  .stopped_signal = false,
              });
        }

        auto set_stopped() noexcept -> void {
          auto scope = round->callbacks_.enter(round);
          round->enqueue_completion(slot_id, child_completion{
                                                 .position = position,
                                                 .status = status_type{},
                                                 .stopped_signal = true,
                                             });
        }

        [[nodiscard]] auto get_env() const noexcept -> child_env {
          return child_env{stop_source == nullptr
                               ? stdexec::inplace_stop_token{}
                               : stop_source->get_token()};
        }
      };

      using child_op_t =
          stdexec::connect_result_t<child_sender_t, child_receiver>;
      using child_set_t = wh::core::detail::child_set<child_op_t>;
      using mailbox_t =
          wh::core::detail::child_completion_mailbox<child_completion>;

      explicit blocking_read_op(shared_state &state) : state_(state) {}

      [[nodiscard]] auto run(round_spec spec) -> status_type {
        lane_indices_ = std::move(spec.lanes);
        result_.reset();
        round_finished_ = false;
        stop_source_ = std::make_shared<stdexec::inplace_stop_source>();
        wake_epoch_.store(0U, std::memory_order_release);
        if (lane_indices_.empty()) {
          state_.complete_empty_round(0U);
          stop_source_.reset();
          return status_type::failure(wh::core::errc::internal_error);
        }

        try {
          tracker_.reset(lane_indices_.size());
          children_.reset(state_.lane_count());
          completions_.reset(state_.lane_count());
        } catch (...) {
          children_.destroy_all();
          completions_.reset(0U);
          stop_source_.reset();
          state_.cancel_round();
          return status_type::failure(wh::core::errc::internal_error);
        }

        for (std::size_t position = 0U; position < lane_indices_.size();
             ++position) {
          const auto lane_index =
              static_cast<std::uint32_t>(lane_indices_[position]);
          if (stop_source_->stop_requested()) {
            record_completion(child_completion{
                .position = position,
                .status = status_type{},
                .stopped_signal = true,
            });
            continue;
          }

          auto started = children_.start_child(lane_index, [&](auto &slot) {
            slot.emplace_from(stdexec::connect,
                              state_.lane_read_async(lane_indices_[position]),
                              child_receiver{
                                  .round = this,
                                  .slot_id = lane_index,
                                  .position = position,
                                  .stop_source = stop_source_,
                              });
          });
          if (started.has_error()) {
            record_completion(child_completion{
                .position = position,
                .status = status_type::failure(started.error()),
                .stopped_signal = false,
            });
          }
        }

        while (true) {
          drain_completions();
          if (result_.has_value() && children_.active_count() == 0U &&
              !callbacks_.active()) {
            auto status = std::move(*result_);
            cleanup();
            return status;
          }
          if (completions_.has_ready()) {
            continue;
          }

          const auto observed = wake_epoch_.load(std::memory_order_acquire);
          if (completions_.has_ready() ||
              (result_.has_value() && children_.active_count() == 0U &&
               !callbacks_.active())) {
            continue;
          }
          wake_epoch_.wait(observed, std::memory_order_acquire);
        }
      }

    private:
      auto notify_owner() noexcept -> void {
        wake_epoch_.fetch_add(1U, std::memory_order_acq_rel);
        wake_epoch_.notify_one();
      }

      auto on_callback_exit() noexcept -> void { notify_owner(); }

      auto enqueue_completion(const std::uint32_t slot_id,
                              child_completion completion) noexcept -> void {
        if (!completions_.publish(slot_id, std::move(completion))) {
          std::terminate();
        }
        notify_owner();
      }

      auto record_completion(child_completion completion) noexcept -> void {
        if (completion.stopped_signal) {
          tracker_.note_stop();
        } else if (tracker_.note_status(completion.position,
                                        std::move(completion.status))) {
          if (stop_source_ != nullptr) {
            stop_source_->request_stop();
          }
        }

        if (tracker_.finish_one()) {
          round_finished_ = true;
        }
        if (round_finished_ && children_.active_count() == 0U) {
          finalize_round();
        }
      }

      auto drain_completions() noexcept -> void {
        completions_.drain(
            [this](const std::uint32_t slot_id, child_completion completion) {
              children_.reclaim_child(slot_id);
              record_completion(std::move(completion));
            });
        if (round_finished_ && children_.active_count() == 0U) {
          finalize_round();
        }
      }

      auto finalize_round() noexcept -> void {
        if (result_.has_value()) {
          return;
        }
        round_finished_ = false;
        auto outcome = tracker_.take_outcome();

        if (outcome.winner_position.has_value() &&
            outcome.winner_status.has_value()) {
          auto resolution = state_.complete_round_winner(
              lane_indices_[*outcome.winner_position], *outcome.winner_position,
              std::move(*outcome.winner_status));
          state_.close_lane_if_needed(resolution.close_lane);
          result_.emplace(std::move(resolution.status));
          notify_owner();
          return;
        }

        if (outcome.stopped_without_winner) {
          state_.cancel_round();
          result_.emplace(status_type::failure(wh::core::errc::internal_error));
          notify_owner();
          return;
        }

        state_.complete_empty_round(lane_indices_.size());
        result_.emplace(status_type::failure(wh::core::errc::internal_error));
        notify_owner();
      }

      auto cleanup() noexcept -> void {
        children_.destroy_all();
        stop_source_.reset();
        lane_indices_.clear();
      }

      shared_state &state_;
      lane_list lane_indices_{};
      std::shared_ptr<stdexec::inplace_stop_source> stop_source_{};
      child_set_t children_{};
      mailbox_t completions_{};
      wh::core::detail::callback_guard<blocking_read_op> callbacks_{};
      std::atomic<std::uint64_t> wake_epoch_{0U};
      round_tracker tracker_{};
      std::optional<status_type> result_{};
      bool round_finished_{false};
    };

    shared_state() : blocking_read_(*this) {}

    explicit shared_state(std::vector<lane_type> &&readers)
        : blocking_read_(*this), topology_(std::move(readers)) {}

    explicit shared_state(std::vector<lane_source_type> &&sources)
        : blocking_read_(*this), topology_(std::move(sources)) {}

    [[nodiscard]] auto uses_fixed_poll_path() const noexcept -> bool {
      return topology_.uses_fixed_poll_path();
    }

    [[nodiscard]] auto try_read() -> try_status_type {
      return topology_.try_read_immediate();
    }

    [[nodiscard]] auto read_blocking() -> status_type {
      for (;;) {
        auto immediate = try_read();
        if (std::holds_alternative<status_type>(immediate)) {
          return std::move(std::get<status_type>(immediate));
        }

        auto round = begin_round();
        if (round.immediate_status.has_value()) {
          return std::move(*round.immediate_status);
        }
        if (round.wait_for_topology) {
          topology_sync_waiter waiter{};
          if (register_sync_topology_waiter(&waiter, round.topology_epoch)) {
            waiter.wait();
          }
          continue;
        }

        return blocking_read_.run(std::move(round));
      }
    }

    [[nodiscard]] auto begin_round() -> round_spec {
      return topology_.prepare_round();
    }

    [[nodiscard]] auto lane_read_async(const std::size_t lane_index) {
      return topology_.lane_read_async(lane_index);
    }

    [[nodiscard]] auto complete_round_winner(const std::size_t lane_index,
                                             const std::size_t winner_offset,
                                             status_type status)
        -> round_resolution {
      return topology_.complete_round_winner(lane_index, winner_offset,
                                             std::move(status));
    }

    auto complete_empty_round(const std::size_t lane_count) -> void {
      topology_.complete_empty_round(lane_count);
    }

    auto cancel_round() -> void { topology_.cancel_round(); }

    auto close_lane_if_needed(const std::optional<std::size_t> lane_index)
        -> void {
      topology_.close_lane_if_needed(lane_index);
    }

    auto close_all() -> wh::core::result<void> { return topology_.close_all(); }

    [[nodiscard]] auto is_closed() const noexcept -> bool {
      return topology_.is_closed();
    }

    [[nodiscard]] auto is_source_closed() const noexcept -> bool {
      return topology_.is_source_closed();
    }

    auto set_automatic_close(const auto_close_options &options) -> void {
      topology_.set_automatic_close(options);
    }

    auto attach(std::string_view source, reader_t reader)
        -> wh::core::result<void> {
      return topology_.attach(source, std::move(reader));
    }

    auto disable(std::string_view source) -> wh::core::result<void> {
      return topology_.disable(source);
    }

    [[nodiscard]] auto
    register_sync_topology_waiter(topology_sync_waiter *waiter,
                                  const std::uint64_t expected_epoch) -> bool {
      return topology_.register_sync_topology_waiter(waiter, expected_epoch);
    }

    [[nodiscard]] auto
    register_async_topology_waiter(topology_async_waiter *waiter,
                                   const std::uint64_t expected_epoch) -> bool {
      return topology_.register_async_topology_waiter(waiter, expected_epoch);
    }

    [[nodiscard]] auto
    remove_async_topology_waiter(topology_async_waiter *waiter) -> bool {
      return topology_.remove_async_topology_waiter(waiter);
    }

    [[nodiscard]] auto topology_epoch() const noexcept -> std::uint64_t {
      return topology_.topology_epoch();
    }

    [[nodiscard]] auto has_pending_lanes() const noexcept -> bool {
      return topology_.has_pending_lanes();
    }

    [[nodiscard]] auto lane_count() const noexcept -> std::size_t {
      return topology_.lane_count();
    }

  private:
    blocking_read_op blocking_read_;
    topology_registry_t topology_{};
  };

  template <typename receiver_t>
  class read_operation
      : public std::enable_shared_from_this<read_operation<receiver_t>>,
        private wh::core::detail::scheduled_drive_loop<
            read_operation<receiver_t>, wh::core::detail::resume_scheduler_t<
                                            stdexec::env_of_t<receiver_t>>> {
    using drive_loop_t = wh::core::detail::scheduled_drive_loop<
        read_operation<receiver_t>,
        wh::core::detail::resume_scheduler_t<stdexec::env_of_t<receiver_t>>>;
    friend drive_loop_t;
    friend class wh::core::detail::callback_guard<read_operation>;

    using receiver_env_t = stdexec::env_of_t<receiver_t>;
    using child_sender_t = decltype(std::declval<reader_t &>().read_async());
    using outer_stop_token_t = stdexec::stop_token_of_t<receiver_env_t>;
    using resume_scheduler_t =
        wh::core::detail::resume_scheduler_t<receiver_env_t>;

    struct final_delivery {
      receiver_t receiver;
      std::optional<status_type> value{};
      std::exception_ptr error{};
      bool stopped{false};

      explicit final_delivery(receiver_t next_receiver) noexcept(
          std::is_nothrow_move_constructible_v<receiver_t>)
          : receiver(std::move(next_receiver)) {}

      auto complete() && noexcept -> void {
        if (error != nullptr) {
          stdexec::set_error(std::move(receiver), std::move(error));
          return;
        }
        if (stopped) {
          stdexec::set_stopped(std::move(receiver));
          return;
        }
        stdexec::set_value(std::move(receiver), std::move(*value));
      }
    };

    struct child_completion {
      std::size_t position{0U};
      status_type status{};
      bool stopped_signal{false};
    };

    struct child_env {
      receiver_env_t base_env_{};
      resume_scheduler_t scheduler_{};
      stdexec::inplace_stop_token stop_token_{};

      [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
          -> stdexec::inplace_stop_token {
        return stop_token_;
      }

      [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
          -> resume_scheduler_t {
        return scheduler_;
      }

      [[nodiscard]] auto
      query(stdexec::get_delegation_scheduler_t) const noexcept
          -> resume_scheduler_t {
        return scheduler_;
      }

      template <typename cpo_t>
      [[nodiscard]] auto
      query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
          -> resume_scheduler_t {
        return scheduler_;
      }
    };

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      read_operation *op{nullptr};
      std::uint32_t slot_id{0U};
      std::size_t position{0U};
      std::shared_ptr<stdexec::inplace_stop_source> stop_source{};

      auto set_value(stream_result<chunk_type> status) noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(slot_id, child_completion{
                                      .position = position,
                                      .status = std::move(status),
                                      .stopped_signal = false,
                                  });
      }

      template <typename error_t>
      auto set_error(error_t &&error) noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(slot_id, child_completion{
                                      .position = position,
                                      .status = status_type::failure(
                                          detail::map_merge_async_error(
                                              std::forward<error_t>(error))),
                                      .stopped_signal = false,
                                  });
      }

      auto set_stopped() noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(slot_id, child_completion{
                                      .position = position,
                                      .status = status_type{},
                                      .stopped_signal = true,
                                  });
      }

      [[nodiscard]] auto get_env() const noexcept -> child_env {
        return child_env{
            .base_env_ = op->env_,
            .scheduler_ = op->drive_loop_t::scheduler(),
            .stop_token_ = stop_source == nullptr
                               ? stdexec::inplace_stop_token{}
                               : stop_source->get_token(),
        };
      }
    };

    using child_op_t =
        stdexec::connect_result_t<child_sender_t, child_receiver>;
    using child_set_t = wh::core::detail::child_set<child_op_t>;
    using mailbox_t =
        wh::core::detail::child_completion_mailbox<child_completion>;

    struct topology_waiter final : topology_async_waiter {
      read_operation *op{nullptr};
    };

    struct stop_callback {
      read_operation *self{nullptr};

      auto operator()() const noexcept -> void {
        self->parent_stop_requested_.store(true, std::memory_order_release);
        if (self->stop_source_ != nullptr) {
          self->stop_source_->request_stop();
        }
        if (self->publish_completion(self->cancel_wait())) {
          self->request_drive();
          return;
        }
        self->request_drive();
      }
    };

    using stop_callback_t =
        stdexec::stop_callback_for_t<outer_stop_token_t, stop_callback>;

  public:
    using operation_state_concept = stdexec::operation_state_t;

    explicit read_operation(std::shared_ptr<shared_state> state,
                            receiver_t receiver)
        : drive_loop_t(
              wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(
                  stdexec::get_env(receiver))),
          state_(std::move(state)), receiver_(std::move(receiver)),
          env_(stdexec::get_env(receiver_)),
          lane_capacity_(state_ == nullptr ? 0U : state_->lane_count()),
          children_(lane_capacity_), completions_(lane_capacity_),
          stop_source_(std::make_shared<stdexec::inplace_stop_source>()) {
      static constexpr typename topology_async_waiter::ops_type ops{
          [](topology_async_waiter *base) noexcept {
            static_cast<topology_waiter *>(base)->op->deliver_topology_ready();
          }};
      topology_waiter_.op = this;
      topology_waiter_.ops = &ops;
    }

    auto start() noexcept -> void {
      if (!state_) {
        publish_completion(make_value_delivery(
            stream_result<chunk_type>::failure(wh::core::errc::not_found)));
        request_drive();
        return;
      }

      auto stop_token = stdexec::get_stop_token(env_);
      if (stop_token.stop_requested()) {
        publish_completion(make_stopped_delivery());
        request_drive();
        return;
      }

      if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
        try {
          stop_callback_.emplace(stop_token, stop_callback{this});
        } catch (...) {
          publish_completion(make_error_delivery(std::current_exception()));
          request_drive();
          return;
        }
      }

      request_drive();
    }

  private:
    [[nodiscard]] auto finished() const noexcept -> bool {
      return delivered_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto completion_pending() const noexcept -> bool {
      return pending_completion_.has_value();
    }

    [[nodiscard]] auto take_completion() noexcept
        -> std::optional<final_delivery> {
      if (!pending_completion_.has_value()) {
        return std::nullopt;
      }
      auto completion = std::move(pending_completion_);
      pending_completion_.reset();
      return completion;
    }

    [[nodiscard]] auto acquire_owner_lifetime_guard() noexcept
        -> std::shared_ptr<read_operation> {
      auto keepalive = this->weak_from_this().lock();
      if (!keepalive) {
        std::terminate();
      }
      return keepalive;
    }

    [[nodiscard]] auto make_value_delivery(status_type status) noexcept
        -> std::optional<final_delivery> {
      if (delivered_.exchange(true, std::memory_order_acq_rel)) {
        return std::nullopt;
      }
      reset_round_state();
      auto delivery = final_delivery{std::move(receiver_)};
      delivery.value = std::move(status);
      return delivery;
    }

    [[nodiscard]] auto make_error_delivery(std::exception_ptr error) noexcept
        -> std::optional<final_delivery> {
      if (delivered_.exchange(true, std::memory_order_acq_rel)) {
        return std::nullopt;
      }
      reset_round_state();
      auto delivery = final_delivery{std::move(receiver_)};
      delivery.error = std::move(error);
      return delivery;
    }

    [[nodiscard]] auto make_stopped_delivery() noexcept
        -> std::optional<final_delivery> {
      if (delivered_.exchange(true, std::memory_order_acq_rel)) {
        return std::nullopt;
      }
      reset_round_state();
      auto delivery = final_delivery{std::move(receiver_)};
      delivery.stopped = true;
      return delivery;
    }

    auto publish_completion(std::optional<final_delivery> delivery) noexcept
        -> bool {
      if (!delivery.has_value()) {
        return false;
      }
      if (pending_completion_.has_value()) {
        std::terminate();
      }
      pending_completion_.emplace(std::move(*delivery));
      return true;
    }

    auto request_drive() noexcept -> void { drive_loop_t::request_drive(); }

    auto on_callback_exit() noexcept -> void {
      if (completion_pending() || completions_.has_ready() ||
          topology_resume_pending_.load(std::memory_order_acquire)) {
        request_drive();
      }
    }

    auto drive() noexcept -> void {
      while (!finished()) {
        if (callbacks_.active()) {
          return;
        }

        if (topology_resume_pending_.exchange(false,
                                              std::memory_order_acq_rel)) {
          topology_waiting_registered_.store(false, std::memory_order_release);
        }

        if (round_active_) {
          drain_completions();
          if (finished()) {
            return;
          }
          if (round_finished_ && children_.active_count() == 0U) {
            if (publish_completion(finalize_round())) {
              return;
            }
            continue;
          }
          if (completions_.has_ready()) {
            continue;
          }
          return;
        }

        if (topology_waiting_registered_.load(std::memory_order_acquire)) {
          return;
        }

        if (parent_stop_requested_.load(std::memory_order_acquire)) {
          if (publish_completion(make_stopped_delivery())) {
            return;
          }
          return;
        }

        if (!state_) {
          if (publish_completion(
                  make_value_delivery(stream_result<chunk_type>::failure(
                      wh::core::errc::not_found)))) {
            return;
          }
          return;
        }

        auto immediate = state_->try_read();
        if (std::holds_alternative<status_type>(immediate)) {
          if (publish_completion(make_value_delivery(
                  std::move(std::get<status_type>(immediate))))) {
            return;
          }
          return;
        }

        auto round = state_->begin_round();
        round_topology_epoch_ = round.topology_epoch;
        if (round.immediate_status.has_value()) {
          if (publish_completion(
                  make_value_delivery(std::move(*round.immediate_status)))) {
            return;
          }
          return;
        }

        if (round.wait_for_topology) {
          if (!wait_for_topology()) {
            continue;
          }
          if (parent_stop_requested_.load(std::memory_order_acquire)) {
            if (publish_completion(cancel_wait())) {
              return;
            }
          }
          return;
        }

        if (publish_completion(start_round(std::move(round.lanes)))) {
          return;
        }
      }
    }

    [[nodiscard]] auto start_round(shared_state::lane_list lanes) noexcept
        -> std::optional<final_delivery> {
      lane_indices_ = std::move(lanes);
      round_active_ = true;
      round_finished_ = false;
      stop_source_ = std::make_shared<stdexec::inplace_stop_source>();

      try {
        tracker_.reset(lane_indices_.size());
        children_.destroy_all();
        completions_.reset(lane_capacity_);
      } catch (...) {
        state_->cancel_round();
        round_active_ = false;
        lane_indices_.clear();
        stop_source_.reset();
        return make_error_delivery(std::current_exception());
      }

      if (parent_stop_requested_.load(std::memory_order_acquire) ||
          stdexec::get_stop_token(env_).stop_requested()) {
        parent_stop_requested_.store(true, std::memory_order_release);
        stop_source_->request_stop();
      }

      for (std::size_t position = 0U; position < lane_indices_.size();
           ++position) {
        const auto lane_index =
            static_cast<std::uint32_t>(lane_indices_[position]);
        if (stop_source_->stop_requested()) {
          consume_completion(child_completion{
              .position = position,
              .status = status_type{},
              .stopped_signal = true,
          });
          continue;
        }

        auto started = children_.start_child(lane_index, [&](auto &slot) {
          slot.emplace_from(stdexec::connect,
                            state_->lane_read_async(lane_indices_[position]),
                            child_receiver{
                                .op = this,
                                .slot_id = lane_index,
                                .position = position,
                                .stop_source = stop_source_,
                            });
        });
        if (started.has_error()) {
          consume_completion(child_completion{
              .position = position,
              .status = status_type::failure(started.error()),
              .stopped_signal = false,
          });
        }
      }

      if (round_finished_ && children_.active_count() == 0U) {
        return finalize_round();
      }
      return std::nullopt;
    }

    auto finish_child(const std::uint32_t slot_id,
                      child_completion completion) noexcept -> void {
      if (finished()) {
        return;
      }
      if (!completions_.publish(slot_id, std::move(completion))) {
        std::terminate();
      }
      request_drive();
    }

    auto consume_completion(child_completion completion) noexcept -> void {
      if (completion.stopped_signal) {
        tracker_.note_stop();
      } else if (tracker_.note_status(completion.position,
                                      std::move(completion.status))) {
        if (stop_source_ != nullptr) {
          stop_source_->request_stop();
        }
      }

      if (tracker_.finish_one()) {
        round_finished_ = true;
      }
    }

    auto drain_completions() noexcept -> void {
      completions_.drain(
          [this](const std::uint32_t slot_id, child_completion completion) {
            children_.reclaim_child(slot_id);
            consume_completion(std::move(completion));
          });
    }

    [[nodiscard]] auto finalize_round() noexcept
        -> std::optional<final_delivery> {
      auto outcome = tracker_.take_outcome();

      if (outcome.winner_position.has_value() &&
          outcome.winner_status.has_value()) {
        auto resolution = state_->complete_round_winner(
            lane_indices_[*outcome.winner_position], *outcome.winner_position,
            std::move(*outcome.winner_status));
        state_->close_lane_if_needed(resolution.close_lane);
        return make_value_delivery(std::move(resolution.status));
      }

      if (outcome.stopped_without_winner) {
        state_->cancel_round();
        if (parent_stop_requested_.load(std::memory_order_acquire)) {
          return make_stopped_delivery();
        }
        return make_value_delivery(
            stream_result<chunk_type>::failure(wh::core::errc::internal_error));
      }

      state_->complete_empty_round(lane_indices_.size());
      round_active_ = false;
      round_finished_ = false;
      children_.destroy_all();
      completions_.reset(lane_capacity_);
      lane_indices_.clear();
      stop_source_.reset();
      if (state_->topology_epoch() != round_topology_epoch_ ||
          state_->has_pending_lanes()) {
        if (!wait_for_topology()) {
          return std::nullopt;
        }
        return std::nullopt;
      }
      return make_value_delivery(
          stream_result<chunk_type>::failure(wh::core::errc::internal_error));
    }

    [[nodiscard]] auto wait_for_topology() noexcept -> bool {
      if (!state_) {
        return false;
      }
      if (!state_->register_async_topology_waiter(&topology_waiter_,
                                                  round_topology_epoch_)) {
        return false;
      }
      topology_waiting_registered_.store(true, std::memory_order_release);
      return true;
    }

    [[nodiscard]] auto cancel_wait() noexcept -> std::optional<final_delivery> {
      if (state_ &&
          topology_waiting_registered_.load(std::memory_order_acquire)) {
        if (!state_->remove_async_topology_waiter(&topology_waiter_)) {
          return std::nullopt;
        }
        topology_waiting_registered_.store(false, std::memory_order_release);
      }
      if (topology_resume_pending_.exchange(false, std::memory_order_acq_rel)) {
        return std::nullopt;
      }
      return make_stopped_delivery();
    }

    auto deliver_topology_ready() noexcept -> void {
      topology_resume_pending_.store(true, std::memory_order_release);
      request_drive();
    }

    auto reset_round_state() noexcept -> void {
      children_.destroy_all();
      completions_.reset(lane_capacity_);
      lane_indices_.clear();
      stop_source_.reset();
      round_active_ = false;
      round_finished_ = false;
      topology_waiting_registered_.store(false, std::memory_order_release);
      topology_resume_pending_.store(false, std::memory_order_release);
    }

    auto drive_error(const wh::core::error_code error) noexcept -> void {
      publish_completion(
          make_value_delivery(stream_result<chunk_type>::failure(error)));
    }

    std::shared_ptr<shared_state> state_{};
    receiver_t receiver_;
    receiver_env_t env_{};
    std::size_t lane_capacity_{0U};
    shared_state::lane_list lane_indices_{};
    child_set_t children_{};
    mailbox_t completions_{};
    std::optional<stop_callback_t> stop_callback_{};
    std::shared_ptr<stdexec::inplace_stop_source> stop_source_{};
    wh::core::detail::callback_guard<read_operation> callbacks_{};
    std::optional<final_delivery> pending_completion_{};
    std::atomic<bool> parent_stop_requested_{false};
    std::atomic<bool> topology_waiting_registered_{false};
    std::atomic<bool> topology_resume_pending_{false};
    std::atomic<bool> delivered_{false};
    round_tracker tracker_{};
    std::uint64_t round_topology_epoch_{0U};
    bool round_active_{false};
    bool round_finished_{false};
    topology_waiter topology_waiter_{};
  };

  class read_sender {
  public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(stream_result<chunk_type>),
        stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

    explicit read_sender(std::shared_ptr<shared_state> state)
        : state_(std::move(state)) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) && -> wh::core::detail::shared_operation_state<
        read_operation<std::remove_cvref_t<receiver_t>>> {
      using operation_t = read_operation<std::remove_cvref_t<receiver_t>>;
      return wh::core::detail::shared_operation_state<operation_t>{
          std::make_shared<operation_t>(std::move(state_),
                                        std::move(receiver))};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver)
        const & -> wh::core::detail::shared_operation_state<
            read_operation<std::remove_cvref_t<receiver_t>>> {
      using operation_t = read_operation<std::remove_cvref_t<receiver_t>>;
      return wh::core::detail::shared_operation_state<operation_t>{
          std::make_shared<operation_t>(state_, std::move(receiver))};
    }

    [[nodiscard]] auto get_env() const noexcept
        -> wh::core::detail::async_completion_env {
      return {};
    }

  private:
    std::shared_ptr<shared_state> state_{};
  };

public:
  merge_stream_reader() = default;

  explicit merge_stream_reader(std::vector<lane_type> &&readers)
      : state_(std::make_shared<shared_state>(std::move(readers))) {}

  explicit merge_stream_reader(std::vector<lane_source_type> &&sources)
      : state_(std::make_shared<shared_state>(std::move(sources))) {}

  merge_stream_reader(const merge_stream_reader &) = delete;
  auto operator=(const merge_stream_reader &) -> merge_stream_reader & = delete;
  merge_stream_reader(merge_stream_reader &&) noexcept = default;
  auto operator=(merge_stream_reader &&) noexcept
      -> merge_stream_reader & = default;
  ~merge_stream_reader() = default;

  /// Returns true when fixed-width polling path is used.
  [[nodiscard]] auto uses_fixed_poll_path() const noexcept -> bool {
    return state_ && state_->uses_fixed_poll_path();
  }

  /// Tries to pull the next merged chunk.
  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    if (!state_) {
      return status_type{chunk_type::make_eof()};
    }
    return state_->try_read();
  }

  /// Reads the next merged chunk, blocking until one lane produces a value,
  /// source EOF, terminal EOF, or error.
  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    if (!state_) {
      return stream_result<chunk_type>{chunk_type::make_eof()};
    }
    return state_->read_blocking();
  }

  /// Async read API that waits all active lanes and resolves one merged chunk.
  [[nodiscard]] auto read_async() const
    requires requires(reader_t &reader) { reader.read_async(); }
  {
    return read_sender{state_};
  }

  /// Closes all underlying readers.
  auto close_impl() -> wh::core::result<void> {
    if (!state_) {
      return {};
    }
    return state_->close_all();
  }

  /// Returns stream closed status.
  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return !state_ || state_->is_closed();
  }

  /// Returns true only when no lane is still pending and every attached lane
  /// has reached source-closed.
  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    return !state_ || state_->is_source_closed();
  }

  /// Sets automatic close behavior for underlying readers.
  auto set_automatic_close(const auto_close_options &options) -> void {
    if (!state_) {
      return;
    }
    state_->set_automatic_close(options);
  }

  /// Attaches one concrete reader to a pre-registered pending lane.
  auto attach(std::string_view source, reader_t reader)
      -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return state_->attach(source, std::move(reader));
  }

  /// Disables one pre-registered pending lane.
  auto disable(std::string_view source) -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return state_->disable(source);
  }

  /// Creates one additional handle sharing the same live merge state.
  [[nodiscard]] auto share() const noexcept -> merge_stream_reader {
    merge_stream_reader shared{};
    shared.state_ = state_;
    return shared;
  }

private:
  std::shared_ptr<shared_state> state_{};
};

/// Builds a merged stream from explicitly named readers.
template <stream_reader reader_t>
[[nodiscard]] inline auto
make_merge_stream_reader(std::vector<named_stream_reader<reader_t>> &&readers)
    -> merge_stream_reader<reader_t> {
  detail::sort_named_stream_readers(readers);
  return merge_stream_reader<reader_t>{std::move(readers)};
}

/// Builds a live merged stream shell from pending lane source names.
template <stream_reader reader_t>
[[nodiscard]] inline auto
make_merge_stream_reader(std::vector<std::string> &&sources)
    -> merge_stream_reader<reader_t> {
  std::ranges::sort(sources);
  return merge_stream_reader<reader_t>{std::move(sources)};
}

/// Builds a merged stream from anonymous readers named by index.
template <stream_reader reader_t>
[[nodiscard]] inline auto
make_merge_stream_reader(std::vector<reader_t> &&readers)
    -> merge_stream_reader<reader_t> {
  return make_merge_stream_reader(
      detail::make_named_stream_readers(std::move(readers)));
}

} // namespace wh::schema::stream
