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

template <typename error_t>
[[nodiscard]] inline auto merge_exception_ptr(error_t &&error) noexcept
    -> std::exception_ptr {
  if constexpr (std::same_as<std::remove_cvref_t<error_t>, std::exception_ptr>) {
    return std::forward<error_t>(error);
  } else {
    return std::make_exception_ptr(std::forward<error_t>(error));
  }
}

template <stream_reader reader_t>
inline auto sort_named_stream_readers(
    std::vector<named_stream_reader<reader_t>> &readers) -> void {
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
  enum class lane_status : std::uint8_t {
    pending = 0U,
    attached,
    finished,
    disabled,
  };

  struct lane_state {
    std::string source{};
    std::optional<reader_t> reader{};
    lane_status status{lane_status::pending};
  };

  template <typename waiter_t> class intrusive_waiter_list {
  public:
    auto push_back(waiter_t *waiter) noexcept -> void {
      waiter->prev = tail_;
      waiter->next = nullptr;
      if (tail_ != nullptr) {
        tail_->next = waiter;
      } else {
        head_ = waiter;
      }
      tail_ = waiter;
    }

    [[nodiscard]] auto try_pop_front() noexcept -> waiter_t * {
      if (head_ == nullptr) {
        return nullptr;
      }
      auto *waiter = head_;
      head_ = head_->next;
      if (head_ != nullptr) {
        head_->prev = nullptr;
      } else {
        tail_ = nullptr;
      }
      waiter->prev = nullptr;
      waiter->next = nullptr;
      return waiter;
    }

    [[nodiscard]] auto try_remove(waiter_t *waiter) noexcept -> bool {
      if (waiter == nullptr) {
        return false;
      }
      if (waiter->prev == nullptr && waiter->next == nullptr &&
          head_ != waiter) {
        return false;
      }

      auto *previous = waiter->prev;
      auto *next = waiter->next;
      if (previous != nullptr) {
        previous->next = next;
      } else {
        head_ = next;
      }
      if (next != nullptr) {
        next->prev = previous;
      } else {
        tail_ = previous;
      }
      waiter->prev = nullptr;
      waiter->next = nullptr;
      return true;
    }

  private:
    waiter_t *head_{nullptr};
    waiter_t *tail_{nullptr};
  };

  struct topology_waiter_base {
    using deliver_fn = void (*)(topology_waiter_base *) noexcept;

    topology_waiter_base *next{nullptr};
    topology_waiter_base *prev{nullptr};
    deliver_fn deliver{nullptr};
  };

  struct round_resolution {
    status_type status{};
    std::optional<std::size_t> close_lane{};
  };

  struct sync_topology_waiter final : topology_waiter_base {
    std::atomic_flag ready = ATOMIC_FLAG_INIT;

    auto notify() noexcept -> void {
      ready.test_and_set(std::memory_order_release);
      ready.notify_one();
    }

    auto wait() noexcept -> void {
      ready.wait(false, std::memory_order_acquire);
    }
  };

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
                                   status_type status) noexcept
        -> bool {
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
        outcome.winner_status.emplace(std::move(*slots_[winner_position].status));
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
    classify_winner(const status_type &status) noexcept
        -> std::uint8_t {
      if (status.has_error()) {
        return 3U;
      }
      return status.value().eof ? 1U : 2U;
    }

    [[nodiscard]] static auto make_winner_key(const std::uint8_t rank,
                                              const std::uint64_t sequence) noexcept
        -> std::uint64_t {
      constexpr auto sequence_mask = std::numeric_limits<std::uint64_t>::max() >>
                                     8U;
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
    using lane_list = wh::core::small_vector<std::size_t, 8U>;

    struct round_spec {
      lane_list lanes{};
      std::optional<status_type> immediate_status{};
      std::uint64_t topology_epoch{0U};
      bool wait_for_topology{false};
    };

    struct blocking_read_op {
      using scheduler_t = stdexec::inline_scheduler;
      using child_sender_t = decltype(std::declval<reader_t &>().read_async());

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

        [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const
            noexcept -> scheduler_t {
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
        std::size_t position{0U};

        auto set_value(stream_result<chunk_type> status) noexcept -> void {
          round->finish(position, std::move(status), false);
        }

        template <typename error_t>
        auto set_error(error_t &&error) noexcept -> void {
          round->finish(
              position,
              status_type::failure(
                  detail::map_merge_async_error(std::forward<error_t>(error))),
              false);
        }

        auto set_stopped() noexcept -> void {
          round->finish(position, status_type{}, true);
        }

        [[nodiscard]] auto get_env() const noexcept -> child_env {
          return child_env{round->stop_source_->get_token()};
        }
      };

      using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

      explicit blocking_read_op(shared_state &state) : state_(state) {}

      [[nodiscard]] auto run(round_spec spec) -> status_type {
        lane_indices_ = std::move(spec.lanes);
        result_.reset();
        ready_.clear(std::memory_order_release);
        stop_source_.emplace();
        if (lane_indices_.empty()) {
          state_.complete_empty_round(0U);
          return status_type::failure(wh::core::errc::internal_error);
        }

        try {
          tracker_.reset(lane_indices_.size());
          child_ops_.reset();
          child_ops_.ensure(lane_indices_.size());
        } catch (...) {
          state_.cancel_round();
          return status_type::failure(wh::core::errc::internal_error);
        }
        for (std::size_t position = 0U; position < lane_indices_.size(); ++position) {
          if (stop_source_->stop_requested()) {
            finish(position, status_type{}, true);
            continue;
          }

          try {
            child_ops_[position].emplace_from(
                stdexec::connect, state_.lane_read_async(lane_indices_[position]),
                child_receiver{this, position});
            stdexec::start(child_ops_[position].get());
          } catch (...) {
            finish(position,
                   status_type::failure(wh::core::errc::internal_error), false);
          }
        }

        ready_.wait(false, std::memory_order_acquire);
        return std::move(*result_);
      }

    private:
      auto finish(const std::size_t position,
                  status_type status,
                  const bool stopped_signal) noexcept -> void {
        if (stopped_signal) {
          tracker_.note_stop();
        } else if (tracker_.note_status(position, std::move(status))) {
          stop_source_->request_stop();
        }

        if (tracker_.finish_one()) {
          finalize();
        }
      }

      auto complete(status_type status) noexcept -> void {
        result_.emplace(std::move(status));
        ready_.test_and_set(std::memory_order_release);
        ready_.notify_one();
      }

      auto finalize() noexcept -> void {
        auto outcome = tracker_.take_outcome();

        if (outcome.winner_position.has_value() && outcome.winner_status.has_value()) {
          auto resolution = state_.complete_round_winner(
              lane_indices_[*outcome.winner_position], *outcome.winner_position,
              std::move(*outcome.winner_status));
          state_.close_lane_if_needed(resolution.close_lane);
          complete(std::move(resolution.status));
          return;
        }

        if (outcome.stopped_without_winner) {
          state_.cancel_round();
          complete(status_type::failure(wh::core::errc::internal_error));
          return;
        }

        state_.complete_empty_round(lane_indices_.size());
        complete(status_type::failure(wh::core::errc::internal_error));
      }

      shared_state &state_;
      lane_list lane_indices_{};
      wh::core::detail::op_buffer<child_op_t> child_ops_{};
      std::optional<stdexec::inplace_stop_source> stop_source_{};
      std::atomic_flag ready_ = ATOMIC_FLAG_INIT;
      round_tracker tracker_{};
      std::optional<status_type> result_{};
    };

    shared_state() : blocking_read_(*this) {}

    explicit shared_state(std::vector<lane_type> &&readers)
        : blocking_read_(*this), lanes_(build_lanes(std::move(readers))),
          mode_(lanes_.size() <= 5U ? poll_mode::fixed : poll_mode::dynamic),
          attached_lanes_(count_attached_lanes(lanes_)),
          pending_lanes_(count_pending_lanes(lanes_)) {}

    explicit shared_state(std::vector<lane_source_type> &&sources)
        : blocking_read_(*this), lanes_(build_pending_lanes(std::move(sources))),
          mode_(lanes_.size() <= 5U ? poll_mode::fixed : poll_mode::dynamic),
          pending_lanes_(lanes_.size()) {}

    [[nodiscard]] auto uses_fixed_poll_path() const noexcept -> bool {
      std::scoped_lock lock(lock_);
      return mode_ == poll_mode::fixed;
    }

    [[nodiscard]] auto try_read() -> try_status_type {
      std::scoped_lock lock(lock_);
      if (closed_) {
        return status_type{chunk_type::make_eof()};
      }
      if (read_in_flight_) {
        return stream_pending;
      }
      if (lanes_.empty()) {
        return status_type{chunk_type::make_eof()};
      }
      if (attached_lanes_ == 0U) {
        return pending_lanes_ > 0U ? try_status_type{stream_pending}
                                   : try_status_type{status_type{chunk_type::make_eof()}};
      }
      return mode_ == poll_mode::fixed ? next_fixed_path_locked()
                                       : next_dynamic_path_locked();
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
          sync_topology_waiter waiter{};
          waiter.deliver = [](topology_waiter_base *base) noexcept {
            static_cast<sync_topology_waiter *>(base)->notify();
          };
          if (register_topology_waiter(&waiter, round.topology_epoch)) {
            waiter.wait();
          }
          continue;
        }

        auto status = blocking_read_.run(std::move(round));
        return status;
      }
    }

    [[nodiscard]] auto begin_round() -> round_spec {
      std::scoped_lock lock(lock_);
      if (closed_) {
        return round_spec{
            .immediate_status = status_type{chunk_type::make_eof()},
        };
      }
      if (read_in_flight_) {
        return round_spec{
            .immediate_status =
                status_type::failure(wh::core::errc::unavailable),
        };
      }
      if (lanes_.empty()) {
        return round_spec{.immediate_status = chunk_type::make_eof()};
      }
      if (attached_lanes_ == 0U) {
        if (pending_lanes_ > 0U) {
          return round_spec{
              .topology_epoch = topology_epoch_,
              .wait_for_topology = true,
          };
        }
        return round_spec{.immediate_status = chunk_type::make_eof()};
      }

      auto lanes = current_poll_order_locked();
      if (lanes.empty()) {
        if (pending_lanes_ > 0U) {
          return round_spec{
              .topology_epoch = topology_epoch_,
              .wait_for_topology = true,
          };
        }
        return round_spec{.immediate_status = chunk_type::make_eof()};
      }

      read_in_flight_ = true;
      return round_spec{
          .lanes = std::move(lanes),
          .topology_epoch = topology_epoch_,
      };
    }

    [[nodiscard]] auto lane_read_async(const std::size_t lane_index) {
      return lanes_[lane_index].reader->read_async();
    }

    [[nodiscard]] auto complete_round_winner(const std::size_t lane_index,
                                            const std::size_t winner_offset,
                                            status_type status)
        -> round_resolution {
      std::optional<std::size_t> close_lane{};
      status_type resolved{};
      {
        std::scoped_lock lock(lock_);
        read_in_flight_ = false;
        cursor_ += winner_offset + 1U;

        auto &lane = lanes_[lane_index];
        if (status.has_error()) {
          resolved = status_type::failure(status.error());
        } else {
          auto chunk = std::move(status).value();
          if (chunk.eof) {
            if (lane.status == lane_status::attached) {
              if (automatic_close_) {
                close_lane = lane_index;
              }
              lane.reader.reset();
              lane.status = lane_status::finished;
              if (attached_lanes_ > 0U) {
                --attached_lanes_;
              }
            }
            resolved = chunk_type::make_source_eof(lane.source);
          } else {
            chunk.source = lane.source;
            resolved = std::move(chunk);
          }
        }
      }
      return round_resolution{.status = std::move(resolved),
                              .close_lane = close_lane};
    }

    auto complete_empty_round(const std::size_t lane_count) -> void {
      std::scoped_lock lock(lock_);
      read_in_flight_ = false;
      cursor_ += lane_count;
    }

    auto cancel_round() -> void {
      std::scoped_lock lock(lock_);
      read_in_flight_ = false;
    }

    auto close_lane_if_needed(const std::optional<std::size_t> lane_index) -> void {
      if (!lane_index.has_value()) {
        return;
      }
      auto &lane = lanes_[*lane_index];
      if (lane.reader.has_value()) {
        [[maybe_unused]] const auto close_status = lane.reader->close();
      }
    }

    auto close_all() -> wh::core::result<void> {
      intrusive_waiter_list<topology_waiter_base> ready_waiters{};
      wh::core::result<void> close_status{};
      {
        std::scoped_lock lock(lock_);
        if (read_in_flight_) {
          return wh::core::result<void>::failure(wh::core::errc::unavailable);
        }
        for (auto &lane : lanes_) {
          if (lane.status == lane_status::attached && lane.reader.has_value()) {
            auto closed = lane.reader->close();
            if (closed.has_error() &&
                closed.error() != wh::core::errc::channel_closed &&
                !close_status.has_error()) {
              close_status = closed;
            }
          }
          lane.reader.reset();
          lane.status = lane_status::disabled;
        }
        attached_lanes_ = 0U;
        pending_lanes_ = 0U;
        closed_ = true;
        ++topology_epoch_;
        detach_topology_waiters_locked(ready_waiters);
      }
      notify_topology_waiters(ready_waiters);
      return close_status;
    }

    [[nodiscard]] auto is_closed() const noexcept -> bool {
      std::scoped_lock lock(lock_);
      return closed_;
    }

    [[nodiscard]] auto is_source_closed() const noexcept -> bool {
      std::scoped_lock lock(lock_);
      if (pending_lanes_ > 0U) {
        return false;
      }
      for (const auto &lane : lanes_) {
        if (lane.status == lane_status::attached && lane.reader.has_value() &&
            !lane.reader->is_source_closed()) {
          return false;
        }
      }
      return true;
    }

    auto set_automatic_close(const auto_close_options &options) -> void {
      std::scoped_lock lock(lock_);
      automatic_close_ = options.enabled;
      for (auto &lane : lanes_) {
        if (!lane.reader.has_value()) {
          continue;
        }
        if constexpr (requires(reader_t &value,
                               const auto_close_options &value_options) {
                        value.set_automatic_close(value_options);
                      }) {
          lane.reader->set_automatic_close(options);
        }
      }
    }

    auto attach(std::string_view source, reader_t reader) -> wh::core::result<void> {
      intrusive_waiter_list<topology_waiter_base> ready_waiters{};
      {
        std::scoped_lock lock(lock_);
        if (closed_) {
          return wh::core::result<void>::failure(wh::core::errc::channel_closed);
        }
        auto *lane = find_lane_locked(source);
        if (lane == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }
        if (lane->status != lane_status::pending) {
          return wh::core::result<void>::failure(
              wh::core::errc::invalid_argument);
        }
        if constexpr (requires(reader_t &value,
                               const auto_close_options &value_options) {
                        value.set_automatic_close(value_options);
                      }) {
          reader.set_automatic_close(auto_close_options{automatic_close_});
        }
        lane->reader.emplace(std::move(reader));
        lane->status = lane_status::attached;
        ++attached_lanes_;
        --pending_lanes_;
        ++topology_epoch_;
        detach_topology_waiters_locked(ready_waiters);
      }
      notify_topology_waiters(ready_waiters);
      return {};
    }

    auto disable(std::string_view source) -> wh::core::result<void> {
      intrusive_waiter_list<topology_waiter_base> ready_waiters{};
      {
        std::scoped_lock lock(lock_);
        auto *lane = find_lane_locked(source);
        if (lane == nullptr) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }
        if (lane->status == lane_status::disabled ||
            lane->status == lane_status::finished) {
          return {};
        }
        if (lane->status == lane_status::attached) {
          return wh::core::result<void>::failure(
              wh::core::errc::invalid_argument);
        }
        lane->status = lane_status::disabled;
        if (pending_lanes_ > 0U) {
          --pending_lanes_;
        }
        ++topology_epoch_;
        detach_topology_waiters_locked(ready_waiters);
      }
      notify_topology_waiters(ready_waiters);
      return {};
    }

    [[nodiscard]] auto register_topology_waiter(
        topology_waiter_base *waiter, const std::uint64_t expected_epoch) -> bool {
      std::scoped_lock lock(lock_);
      if (closed_ || topology_epoch_ != expected_epoch || attached_lanes_ > 0U ||
          pending_lanes_ == 0U) {
        return false;
      }
      topology_waiters_.push_back(waiter);
      return true;
    }

    [[nodiscard]] auto remove_topology_waiter(topology_waiter_base *waiter) -> bool {
      std::scoped_lock lock(lock_);
      return topology_waiters_.try_remove(waiter);
    }

    [[nodiscard]] auto topology_epoch() const noexcept -> std::uint64_t {
      std::scoped_lock lock(lock_);
      return topology_epoch_;
    }

    [[nodiscard]] auto has_pending_lanes() const noexcept -> bool {
      std::scoped_lock lock(lock_);
      return pending_lanes_ > 0U;
    }

  private:
    [[nodiscard]] static auto build_lanes(std::vector<lane_type> &&readers)
        -> std::vector<lane_state> {
      std::vector<lane_state> lanes{};
      lanes.reserve(readers.size());
      for (auto &reader : readers) {
        lanes.push_back(lane_state{
            .source = std::move(reader.source),
            .reader = std::move(reader.reader),
            .status = reader.finished ? lane_status::finished
                                      : lane_status::attached,
        });
      }
      return lanes;
    }

    [[nodiscard]] static auto
    build_pending_lanes(const std::vector<lane_source_type> &sources)
        -> std::vector<lane_state> {
      std::vector<lane_state> lanes{};
      lanes.reserve(sources.size());
      for (const auto &source : sources) {
        lanes.push_back(lane_state{
            .source = source,
            .reader = std::nullopt,
            .status = lane_status::pending,
        });
      }
      return lanes;
    }

    [[nodiscard]] static auto
    build_pending_lanes(std::vector<lane_source_type> &&sources)
        -> std::vector<lane_state> {
      std::vector<lane_state> lanes{};
      lanes.reserve(sources.size());
      for (auto &source : sources) {
        lanes.push_back(lane_state{
            .source = std::move(source),
            .reader = std::nullopt,
            .status = lane_status::pending,
        });
      }
      return lanes;
    }

    [[nodiscard]] static auto
    count_attached_lanes(const std::vector<lane_state> &lanes) -> std::size_t {
      return static_cast<std::size_t>(std::ranges::count_if(
          lanes, [](const lane_state &lane) -> bool {
            return lane.status == lane_status::attached;
          }));
    }

    [[nodiscard]] static auto
    count_pending_lanes(const std::vector<lane_state> &lanes) -> std::size_t {
      return static_cast<std::size_t>(std::ranges::count_if(
          lanes, [](const lane_state &lane) -> bool {
            return lane.status == lane_status::pending;
          }));
    }

    [[nodiscard]] auto current_poll_order_locked() const
        -> lane_list {
      lane_list lanes{};
      lanes.reserve(attached_lanes_);
      const auto lane_count = lanes_.size();
      for (std::size_t attempts = 0U; attempts < lane_count; ++attempts) {
        const auto index = (cursor_ + attempts) % lane_count;
        if (lanes_[index].status == lane_status::attached) {
          lanes.push_back(index);
        }
      }
      return lanes;
    }

    auto poll_one_reader_locked(const std::size_t index,
                                status_type &output) -> bool {
      auto &lane = lanes_[index];
      if (lane.status != lane_status::attached || !lane.reader.has_value()) {
        return false;
      }

      auto next_chunk = lane.reader->try_read();
      if (std::holds_alternative<stream_signal>(next_chunk)) {
        return false;
      }

      auto resolved = std::move(std::get<status_type>(next_chunk));
      if (resolved.has_error()) {
        output = std::move(resolved);
        return true;
      }

      auto chunk = std::move(resolved).value();
      if (chunk.eof) {
        if (automatic_close_) {
          [[maybe_unused]] const auto close_status = lane.reader->close();
        }
        lane.reader.reset();
        lane.status = lane_status::finished;
        if (attached_lanes_ > 0U) {
          --attached_lanes_;
        }
        output = chunk_type::make_source_eof(lane.source);
        return true;
      }
      chunk.source = lane.source;
      output = std::move(chunk);
      return true;
    }

    template <std::size_t width>
    [[nodiscard]] auto next_fixed_width_locked() -> try_status_type {
      static_assert(width >= 1U && width <= 5U);
      for (std::size_t attempts = 0U; attempts < width; ++attempts) {
        const auto index = cursor_ % width;
        ++cursor_;
        status_type output{};
        if (poll_one_reader_locked(index, output)) {
          return try_status_type{std::move(output)};
        }
      }
      return stream_pending;
    }

    [[nodiscard]] auto next_fixed_path_locked() -> try_status_type {
      switch (lanes_.size()) {
      case 0U:
        return status_type{chunk_type::make_eof()};
      case 1U:
        return next_fixed_width_locked<1U>();
      case 2U:
        return next_fixed_width_locked<2U>();
      case 3U:
        return next_fixed_width_locked<3U>();
      case 4U:
        return next_fixed_width_locked<4U>();
      case 5U:
        return next_fixed_width_locked<5U>();
      default:
        return next_dynamic_path_locked();
      }
    }

    [[nodiscard]] auto next_dynamic_path_locked() -> try_status_type {
      const auto lane_count = lanes_.size();
      for (std::size_t attempts = 0U; attempts < lane_count; ++attempts) {
        const auto index = cursor_ % lane_count;
        ++cursor_;
        status_type output{};
        if (poll_one_reader_locked(index, output)) {
          return try_status_type{std::move(output)};
        }
      }
      return stream_pending;
    }

    [[nodiscard]] auto find_lane_locked(const std::string_view source) noexcept
        -> lane_state * {
      auto iter = std::ranges::find_if(lanes_, [&](const lane_state &lane) {
        return lane.source == source;
      });
      return iter == lanes_.end() ? nullptr : std::addressof(*iter);
    }

    auto detach_topology_waiters_locked(
        intrusive_waiter_list<topology_waiter_base> &ready_waiters) -> void {
      while (auto *waiter = topology_waiters_.try_pop_front()) {
        ready_waiters.push_back(waiter);
      }
    }

    static auto notify_topology_waiters(
        intrusive_waiter_list<topology_waiter_base> &ready_waiters) -> void {
      while (auto *waiter = ready_waiters.try_pop_front()) {
        waiter->deliver(waiter);
      }
    }

    mutable std::mutex lock_{};
    blocking_read_op blocking_read_;
    std::vector<lane_state> lanes_{};
    poll_mode mode_{poll_mode::fixed};
    std::size_t attached_lanes_{0U};
    std::size_t pending_lanes_{0U};
    std::size_t cursor_{0U};
    bool closed_{false};
    bool automatic_close_{true};
    bool read_in_flight_{false};
    std::uint64_t topology_epoch_{0U};
    intrusive_waiter_list<topology_waiter_base> topology_waiters_{};
  };

  template <typename receiver_t> class read_operation {
    using receiver_env_t = stdexec::env_of_t<receiver_t>;
    using child_sender_t = decltype(std::declval<reader_t &>().read_async());
    using outer_stop_token_t = stdexec::stop_token_of_t<receiver_env_t>;
    using resume_scheduler_t =
        wh::core::detail::resume_scheduler_t<receiver_env_t>;
    using handoff_sender_t = stdexec::schedule_result_t<resume_scheduler_t>;

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

      [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const
          noexcept -> resume_scheduler_t {
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
      std::size_t position{0U};

      auto set_value(stream_result<chunk_type> status) noexcept -> void {
        op->finish_child(position, std::move(status), false);
      }

      template <typename error_t>
      auto set_error(error_t &&error) noexcept -> void {
        op->finish_child(
            position,
            stream_result<chunk_type>::failure(
                detail::map_merge_async_error(std::forward<error_t>(error))),
            false);
      }

      auto set_stopped() noexcept -> void {
        op->finish_child(position, stream_result<chunk_type>{}, true);
      }

      [[nodiscard]] auto get_env() const noexcept -> child_env {
        return child_env{
            op->env_,
            op->scheduler_,
            op->stop_source_.get_token()};
      }
    };

    using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

    struct handoff_receiver {
      using receiver_concept = stdexec::receiver_t;

      read_operation *self{nullptr};

      auto set_value() noexcept -> void { self->pump(); }

      template <typename error_t>
      auto set_error(error_t &&error) noexcept -> void {
        stdexec::set_error(std::move(self->receiver_),
                           detail::merge_exception_ptr(
                               std::forward<error_t>(error)));
      }

      auto set_stopped() noexcept -> void {
        stdexec::set_stopped(std::move(self->receiver_));
      }

      [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> {
        return {};
      }
    };

    using handoff_op_t = stdexec::connect_result_t<handoff_sender_t, handoff_receiver>;

    struct topology_waiter final : topology_waiter_base {
      read_operation *op{nullptr};
    };

    struct stop_callback {
      read_operation *self{nullptr};

      auto operator()() const noexcept -> void {
        self->parent_stop_requested_.store(true, std::memory_order_release);
        self->stop_source_.request_stop();
        self->cancel_wait();
      }
    };

    using stop_callback_t =
        stdexec::stop_callback_for_t<outer_stop_token_t, stop_callback>;

  public:
    using operation_state_concept = stdexec::operation_state_t;

    explicit read_operation(std::shared_ptr<shared_state> state,
                            receiver_t receiver)
        : state_(std::move(state)),
          receiver_(std::move(receiver)),
          env_(stdexec::get_env(receiver_)),
          scheduler_(
              wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(
                  env_)) {
      topology_waiter_.op = this;
      topology_waiter_.deliver = [](topology_waiter_base *base) noexcept {
        static_cast<topology_waiter *>(base)->op->deliver_topology_ready();
      };
    }

    auto start() & noexcept -> void {
      if (!state_) {
        stdexec::set_value(
            std::move(receiver_),
            stream_result<chunk_type>::failure(wh::core::errc::not_found));
        return;
      }

      auto stop_token = stdexec::get_stop_token(env_);
      if (stop_token.stop_requested()) {
        stdexec::set_stopped(std::move(receiver_));
        return;
      }

      if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
        try {
          stop_callback_.emplace(stop_token, stop_callback{this});
        } catch (...) {
          stdexec::set_error(std::move(receiver_), std::current_exception());
          return;
        }
      }

      pump();
    }

  private:
    auto pump() noexcept -> void {
      topology_resume_pending_.store(false, std::memory_order_release);

      if (!state_) {
        stdexec::set_value(
            std::move(receiver_),
            stream_result<chunk_type>::failure(wh::core::errc::not_found));
        return;
      }
      if (stop_source_.stop_requested() ||
          stdexec::get_stop_token(env_).stop_requested()) {
        stdexec::set_stopped(std::move(receiver_));
        return;
      }

      auto immediate = state_->try_read();
      if (std::holds_alternative<status_type>(immediate)) {
        stdexec::set_value(std::move(receiver_),
                           std::move(std::get<status_type>(immediate)));
        return;
      }

      auto round = state_->begin_round();
      round_topology_epoch_ = round.topology_epoch;
      if (round.immediate_status.has_value()) {
        stdexec::set_value(std::move(receiver_),
                           std::move(*round.immediate_status));
        return;
      }
      if (round.wait_for_topology) {
        wait_for_topology();
        return;
      }

      lane_indices_ = std::move(round.lanes);
      parent_stopped_without_winner_.store(false, std::memory_order_release);

      try {
        tracker_.reset(lane_indices_.size());
        child_ops_.reset();
        child_ops_.ensure(lane_indices_.size());
      } catch (...) {
        state_->cancel_round();
        stdexec::set_error(std::move(receiver_), std::current_exception());
        return;
      }

      if (stdexec::get_stop_token(env_).stop_requested()) {
        parent_stop_requested_.store(true, std::memory_order_release);
        stop_source_.request_stop();
      }

      for (std::size_t position = 0U; position < lane_indices_.size(); ++position) {
        if (stop_source_.stop_requested()) {
          finish_child(position, stream_result<chunk_type>{}, true);
          continue;
        }

        try {
          child_ops_[position].emplace_from(
              stdexec::connect, state_->lane_read_async(lane_indices_[position]),
              child_receiver{this, position});
          stdexec::start(child_ops_[position].get());
        } catch (...) {
          finish_child(position,
                       stream_result<chunk_type>::failure(
                           wh::core::errc::internal_error),
                       false);
        }
      }
    }

    auto wait_for_topology() noexcept -> void {
      if (!state_) {
        stdexec::set_value(
            std::move(receiver_),
            stream_result<chunk_type>::failure(wh::core::errc::not_found));
        return;
      }
      if (!state_->register_topology_waiter(&topology_waiter_, round_topology_epoch_)) {
        pump();
        return;
      }
      topology_waiting_registered_.store(true, std::memory_order_release);
      if (stdexec::get_stop_token(env_).stop_requested()) {
        cancel_wait();
      }
    }

    auto finish_child(const std::size_t position,
                      stream_result<chunk_type> status,
                      const bool stopped_signal) noexcept -> void {
      if (stopped_signal) {
        if (parent_stop_requested_.load(std::memory_order_acquire)) {
          parent_stopped_without_winner_.store(true, std::memory_order_release);
        }
      } else if (tracker_.note_status(position, std::move(status))) {
        stop_source_.request_stop();
      }

      if (tracker_.finish_one()) {
        finalize();
      }
    }

    auto finalize() noexcept -> void {
      auto outcome = tracker_.take_outcome();
      const auto stopped_without_winner =
          outcome.stopped_without_winner ||
          parent_stopped_without_winner_.load(std::memory_order_acquire);

      if (outcome.winner_position.has_value() && outcome.winner_status.has_value()) {
        auto resolution = state_->complete_round_winner(
            lane_indices_[*outcome.winner_position], *outcome.winner_position,
            std::move(*outcome.winner_status));
        state_->close_lane_if_needed(resolution.close_lane);
        stdexec::set_value(std::move(receiver_),
                           std::move(resolution.status));
        return;
      }

      if (stopped_without_winner) {
        state_->cancel_round();
        if (parent_stop_requested_.load(std::memory_order_acquire)) {
          stdexec::set_stopped(std::move(receiver_));
        } else {
          stdexec::set_value(
              std::move(receiver_),
              stream_result<chunk_type>::failure(wh::core::errc::internal_error));
        }
        return;
      }

      state_->complete_empty_round(lane_indices_.size());
      if (state_->topology_epoch() != round_topology_epoch_ ||
          state_->has_pending_lanes()) {
        wait_for_topology();
        return;
      }
      stdexec::set_value(std::move(receiver_),
                         stream_result<chunk_type>::failure(wh::core::errc::internal_error));
    }

    auto cancel_wait() noexcept -> void {
      if (state_ && topology_waiting_registered_.load(std::memory_order_acquire)) {
        if (state_->remove_topology_waiter(&topology_waiter_)) {
          topology_waiting_registered_.store(false, std::memory_order_release);
        }
      }
      if (topology_resume_pending_.exchange(false, std::memory_order_acq_rel)) {
        return;
      }
      stdexec::set_stopped(std::move(receiver_));
    }

    auto deliver_topology_ready() noexcept -> void {
      topology_waiting_registered_.store(false, std::memory_order_release);
      bool expected = false;
      if (!topology_resume_pending_.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return;
      }
      if (wh::core::detail::scheduler_handoff::same_scheduler(scheduler_)) {
        pump();
        return;
      }
      try {
        handoff_.emplace_from(stdexec::connect, stdexec::schedule(scheduler_),
                              handoff_receiver{this});
        stdexec::start(handoff_.get());
      } catch (...) {
        stdexec::set_error(std::move(receiver_), std::current_exception());
      }
    }

    std::shared_ptr<shared_state> state_{};
    receiver_t receiver_;
    receiver_env_t env_{};
    resume_scheduler_t scheduler_{};
    shared_state::lane_list lane_indices_{};
    wh::core::detail::op_buffer<child_op_t> child_ops_{};
    wh::core::detail::manual_lifetime_box<handoff_op_t> handoff_{};
    std::optional<stop_callback_t> stop_callback_{};
    stdexec::inplace_stop_source stop_source_{};
    std::atomic<bool> parent_stop_requested_{false};
    std::atomic<bool> parent_stopped_without_winner_{false};
    std::atomic<bool> topology_waiting_registered_{false};
    std::atomic<bool> topology_resume_pending_{false};
    round_tracker tracker_{};
    std::uint64_t round_topology_epoch_{0U};
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
    [[nodiscard]] auto connect(receiver_t receiver) &&
        -> read_operation<std::remove_cvref_t<receiver_t>> {
      return read_operation<std::remove_cvref_t<receiver_t>>{
          std::move(state_), std::move(receiver)};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto connect(receiver_t receiver) const &
        -> read_operation<std::remove_cvref_t<receiver_t>> {
      return read_operation<std::remove_cvref_t<receiver_t>>{state_,
                                                             std::move(receiver)};
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
  auto operator=(merge_stream_reader &&) noexcept -> merge_stream_reader & =
      default;
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
  auto attach(std::string_view source, reader_t reader) -> wh::core::result<void> {
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
[[nodiscard]] inline auto make_merge_stream_reader(std::vector<std::string> &&sources)
    -> merge_stream_reader<reader_t> {
  std::ranges::sort(sources);
  return merge_stream_reader<reader_t>{std::move(sources)};
}

/// Builds a merged stream from anonymous readers named by index.
template <stream_reader reader_t>
[[nodiscard]] inline auto make_merge_stream_reader(std::vector<reader_t> &&readers)
    -> merge_stream_reader<reader_t> {
  return make_merge_stream_reader(detail::make_named_stream_readers(
      std::move(readers)));
}

} // namespace wh::schema::stream
