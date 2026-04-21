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
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/intrusive_ptr.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/detail/scheduled_resume_turn.hpp"
#include "wh/core/stdexec/detail/slot_ready_list.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::schema::stream {

enum class merge_topology_mode : std::uint8_t {
  static_attached = 0U,
  dynamic_injection,
};

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

template <typename index_t = std::uint32_t>
[[nodiscard]] inline constexpr auto indexed_capacity_limit() noexcept -> std::size_t {
  return static_cast<std::size_t>(std::numeric_limits<index_t>::max());
}

template <typename slot_t, typename index_t = std::uint32_t>
[[nodiscard]] inline constexpr auto slot_array_capacity_limit() noexcept -> std::size_t {
  return std::min(indexed_capacity_limit<index_t>(),
                  static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max() /
                                           static_cast<std::ptrdiff_t>(sizeof(slot_t))));
}

template <typename index_t = std::uint32_t>
[[nodiscard]] inline auto validate_indexed_capacity(const std::size_t count, const char *context)
    -> std::size_t {
  if (count > indexed_capacity_limit<index_t>()) {
    throw std::length_error{context};
  }
  return count;
}

template <typename slot_t, typename index_t = std::uint32_t>
[[nodiscard]] inline auto validate_slot_array_capacity(const std::size_t count, const char *context)
    -> std::size_t {
  if (count > slot_array_capacity_limit<slot_t, index_t>()) {
    throw std::length_error{context};
  }
  return count;
}

template <typename slot_t, typename index_t = std::uint32_t>
[[nodiscard]] inline auto allocate_slot_array(const std::size_t count, const char *context)
    -> std::unique_ptr<slot_t[]> {
  if (count == 0U) {
    return {};
  }
  return std::make_unique<slot_t[]>(validate_slot_array_capacity<slot_t, index_t>(count, context));
}

template <typename error_t>
[[nodiscard]] inline auto map_merge_async_error(error_t &&error) noexcept -> wh::core::error_code {
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
inline auto sort_named_stream_readers(std::vector<named_stream_reader<reader_t>> &readers) -> void {
  std::ranges::sort(readers, [](const named_stream_reader<reader_t> &left,
                                const named_stream_reader<reader_t> &right) {
    return left.source < right.source;
  });
}

template <stream_reader reader_t>
[[nodiscard]] inline auto make_named_stream_readers(std::vector<reader_t> &&readers)
    -> std::vector<named_stream_reader<reader_t>> {
  std::vector<named_stream_reader<reader_t>> named_readers{};
  static_cast<void>(validate_indexed_capacity<>(
      readers.size(), "merge_stream_reader lane count exceeds uint32_t slot capacity"));
  named_readers.reserve(readers.size());
  for (std::size_t index = 0U; index < readers.size(); ++index) {
    named_readers.push_back(
        named_stream_reader<reader_t>{std::to_string(index), std::move(readers[index]), false});
  }
  return named_readers;
}

} // namespace detail

#include "wh/schema/stream/reader/detail/dynamic_topology_registry.hpp"
#include "wh/schema/stream/reader/detail/static_topology_registry.hpp"

/// Merges multiple named readers into one interleaved output stream.
template <stream_reader reader_t,
          merge_topology_mode topology_mode = merge_topology_mode::dynamic_injection>
class merge_stream_reader final : public stream_base<merge_stream_reader<reader_t, topology_mode>,
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
  static constexpr auto topology_mode_v = topology_mode;
  static constexpr bool supports_dynamic_injection =
      topology_mode_v == merge_topology_mode::dynamic_injection;

  using topology_registry_t =
      std::conditional_t<supports_dynamic_injection, detail::dynamic_topology_registry<reader_t>,
                         detail::static_topology_registry<reader_t>>;
  using topology_sync_waiter = typename topology_registry_t::sync_waiter_type;
  using topology_async_waiter = typename topology_registry_t::async_waiter_type;
  using round_resolution = typename topology_registry_t::round_resolution;

  struct round_outcome {
    std::optional<std::size_t> winner_position{};
    std::optional<status_type> winner_status{};
    bool stopped_without_winner{false};
  };

  struct round_child_completion {
    std::size_t position{0U};
    status_type status{};
    bool stopped_signal{false};
  };

  template <typename child_op_t, typename completion_t> class child_slots {
    struct slot {
      ~slot() { reset(); }

      template <typename factory_t> auto emplace(factory_t &&factory) -> void {
        [[maybe_unused]] auto &operation =
            op_.template construct_with<child_op_t>(std::forward<factory_t>(factory));
        engaged_ = true;
      }

      auto reset() noexcept -> void {
        if (!engaged_) {
          return;
        }
        op_.template destruct<child_op_t>();
        engaged_ = false;
      }

      wh::core::detail::manual_storage<sizeof(child_op_t), alignof(child_op_t)> op_{};
      std::optional<completion_t> completion{};
      bool engaged_{false};
    };

  public:
    child_slots() = default;

    explicit child_slots(const std::size_t size)
        : slots_(detail::allocate_slot_array<slot>(
              size, "merge_stream_reader child slot count exceeds supported capacity")),
          size_(size) {}

    child_slots(const child_slots &) = delete;
    auto operator=(const child_slots &) -> child_slots & = delete;
    child_slots(child_slots &&) = delete;
    auto operator=(child_slots &&) -> child_slots & = delete;

    ~child_slots() { destroy_all(); }

    template <typename factory_t, typename before_start_t = std::nullptr_t>
    auto start(const std::uint32_t slot_id, factory_t &&factory,
               before_start_t &&before_start = nullptr) -> wh::core::result<void> {
      wh_precondition(slot_id < size_);
      auto &child_slot = slots_[slot_id];
      wh_invariant(!child_slot.engaged_);

      try {
        child_slot.emplace(std::forward<factory_t>(factory));
        if constexpr (!std::same_as<std::remove_cvref_t<before_start_t>, std::nullptr_t>) {
          std::forward<before_start_t>(before_start)();
        }
        stdexec::start(child_slot.op_.template get<child_op_t>());
        return {};
      } catch (...) {
        child_slot.reset();
        return wh::core::result<void>::failure(wh::core::map_current_exception());
      }
    }

    auto destroy_all() noexcept -> void {
      if (slots_ == nullptr) {
        return;
      }
      for (std::size_t index = 0U; index < size_; ++index) {
        slots_[index].reset();
        slots_[index].completion.reset();
      }
    }

    auto store_completion(const std::uint32_t slot_id, completion_t completion) noexcept -> bool {
      wh_precondition(slot_id < size_);
      auto &child_slot = slots_[slot_id];
      if (child_slot.completion.has_value()) {
        return false;
      }
      child_slot.completion.emplace(std::move(completion));
      return true;
    }

    template <typename fn_t>
    auto drain_ready(wh::core::detail::slot_ready_list &ready, fn_t &&fn) noexcept -> void {
      auto &drain_fn = fn;
      ready.drain([&](const std::uint32_t slot_id) {
        wh_precondition(slot_id < size_);
        auto &child_slot = slots_[slot_id];
        if (!child_slot.completion.has_value()) {
          return;
        }
        auto completion = std::move(*child_slot.completion);
        child_slot.completion.reset();
        drain_fn(slot_id, std::move(completion));
      });
    }

  private:
    std::unique_ptr<slot[]> slots_{};
    std::size_t size_{0U};
  };

  class round_tracker {
  public:
    auto reset(const std::size_t count) -> void {
      if (count > capacity_) {
        slots_ = detail::allocate_slot_array<slot>(
            count, "merge_stream_reader round tracker slot count exceeds supported capacity");
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

    [[nodiscard]] auto note_status(const std::size_t position, status_type status) noexcept
        -> bool {
      const auto candidate_rank = classify_winner(status);
      if (candidate_rank == 0U) {
        return false;
      }

      slots_[position].status.emplace(std::move(status));
      const auto candidate_key = make_winner_key(
          candidate_rank, finish_sequence_.fetch_add(1U, std::memory_order_acq_rel));
      auto winner_key = winner_key_.load(std::memory_order_acquire);
      while (candidate_key > winner_key) {
        if (winner_key_.compare_exchange_weak(winner_key, candidate_key, std::memory_order_acq_rel,
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
      const auto winner_position = winner_position_.load(std::memory_order_acquire);
      if (winner_position != no_winner_position) {
        outcome.winner_position = winner_position;
        outcome.winner_status.emplace(std::move(*slots_[winner_position].status));
        return outcome;
      }
      outcome.stopped_without_winner = stopped_without_winner_.load(std::memory_order_acquire);
      return outcome;
    }

  private:
    struct slot {
      std::optional<status_type> status{};
    };

    [[nodiscard]] static auto classify_winner(const status_type &status) noexcept -> std::uint8_t {
      if (status.has_error()) {
        return 3U;
      }
      return status.value().eof ? 1U : 2U;
    }

    [[nodiscard]] static auto make_winner_key(const std::uint8_t rank,
                                              const std::uint64_t sequence) noexcept
        -> std::uint64_t {
      constexpr auto sequence_mask = std::numeric_limits<std::uint64_t>::max() >> 8U;
      return (std::uint64_t{rank} << 56U) | (sequence_mask - sequence);
    }

    static constexpr auto no_winner_position = std::numeric_limits<std::size_t>::max();

    std::unique_ptr<slot[]> slots_{};
    std::size_t capacity_{0U};
    std::atomic<std::size_t> pending_{0U};
    std::atomic<std::uint64_t> finish_sequence_{0U};
    std::atomic<std::uint64_t> winner_key_{0U};
    std::atomic<std::size_t> winner_position_{no_winner_position};
    std::atomic<bool> stopped_without_winner_{false};
  };

  template <typename lane_list_t, typename child_slots_t, typename completion_t>
  struct round_state_base {
    explicit round_state_base(const std::size_t lane_capacity)
        : children(lane_capacity), ready(lane_capacity) {}

    auto publish_completion(const std::uint32_t slot_id, completion_t completion) noexcept -> void {
      publication_count.fetch_add(1U, std::memory_order_acq_rel);
      if (!children.store_completion(slot_id, std::move(completion))) {
        std::terminate();
      }
      if (!ready.publish(slot_id)) {
        std::terminate();
      }
      publication_count.fetch_sub(1U, std::memory_order_acq_rel);
    }

    template <typename fn_t> auto drain_ready(fn_t &&fn) noexcept -> void {
      children.drain_ready(ready, std::forward<fn_t>(fn));
    }

    [[nodiscard]] auto publications_quiescent() const noexcept -> bool {
      return publication_count.load(std::memory_order_acquire) == 0U;
    }

    lane_list_t lane_indices{};
    child_slots_t children{};
    wh::core::detail::slot_ready_list ready{};
    round_tracker tracker{};
    stdexec::inplace_stop_source stop_source{};
    std::atomic<std::size_t> publication_count{0U};
    bool finished{false};
  };

  static auto observe_round_completion(round_tracker &tracker,
                                       stdexec::inplace_stop_source &stop_source, bool &finished,
                                       round_child_completion completion) noexcept -> void {
    if (completion.stopped_signal) {
      tracker.note_stop();
    } else if (tracker.note_status(completion.position, std::move(completion.status))) {
      stop_source.request_stop();
    }

    if (tracker.finish_one()) {
      finished = true;
    }
  }

  struct runtime : wh::core::detail::intrusive_enable_from_this<runtime> {
    using lane_list = typename topology_registry_t::lane_list;
    using round_spec = typename topology_registry_t::round_plan;

    struct blocking_read_op {
      using scheduler_t = stdexec::inline_scheduler;
      using child_sender_t = decltype(std::declval<reader_t &>().read_async());
      using child_completion = round_child_completion;

      struct child_env {
        stdexec::inplace_stop_token stop_token{};

        [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
            -> stdexec::inplace_stop_token {
          return stop_token;
        }

        [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> scheduler_t {
          return {};
        }

        [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
            -> scheduler_t {
          return {};
        }

        template <typename cpo_t>
        [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
            -> scheduler_t {
          return {};
        }
      };

      struct round_state;

      struct child_receiver {
        using receiver_concept = stdexec::receiver_t;

        round_state *round{nullptr};
        std::uint32_t slot_id{0U};
        std::size_t position{0U};

        auto set_value(stream_result<chunk_type> status) noexcept -> void {
          complete(child_completion{
              .position = position,
              .status = std::move(status),
              .stopped_signal = false,
          });
        }

        template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
          complete(child_completion{
              .position = position,
              .status =
                  status_type::failure(detail::map_merge_async_error(std::forward<error_t>(error))),
              .stopped_signal = false,
          });
        }

        auto set_stopped() noexcept -> void {
          complete(child_completion{
              .position = position,
              .status = status_type{},
              .stopped_signal = true,
          });
        }

        [[nodiscard]] auto get_env() const noexcept -> child_env {
          return child_env{round == nullptr ? stdexec::inplace_stop_token{}
                                            : round->stop_source.get_token()};
        }

      private:
        auto complete(child_completion completion) noexcept -> void {
          if (round == nullptr) {
            return;
          }
          round->publish_completion(slot_id, std::move(completion));
          round->wake_epoch.fetch_add(1U, std::memory_order_acq_rel);
          round->wake_epoch.notify_one();
        }
      };

      using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;
      using child_slots_t = child_slots<child_op_t, child_completion>;
      using round_state_base_t = round_state_base<lane_list, child_slots_t, child_completion>;

      struct round_state : round_state_base_t {
        using round_state_base_t::round_state_base_t;

        std::atomic<std::uint64_t> wake_epoch{0U};
      };

      explicit blocking_read_op(runtime &state) : state_(state) {}

      [[nodiscard]] auto run(round_spec spec) -> status_type {
        round_state round{state_.lane_count()};
        round.lane_indices = std::move(spec.lanes);
        if (round.lane_indices.empty()) {
          state_.complete_round_without_winner(round.lane_indices);
          return status_type::failure(wh::core::errc::internal_error);
        }

        try {
          round.tracker.reset(round.lane_indices.size());
        } catch (...) {
          state_.complete_round_without_winner(round.lane_indices);
          return status_type::failure(wh::core::errc::internal_error);
        }

        for (std::size_t position = 0U; position < round.lane_indices.size(); ++position) {
          const auto lane_index = static_cast<std::uint32_t>(round.lane_indices[position]);
          if (round.stop_source.stop_requested()) {
            observe_completion(round, child_completion{
                                          .position = position,
                                          .status = status_type{},
                                          .stopped_signal = true,
                                      });
            continue;
          }

          auto started = round.children.start(lane_index, [&]() -> child_op_t {
            return stdexec::connect(state_.lane_read_async(round.lane_indices[position]),
                                    child_receiver{
                                        .round = std::addressof(round),
                                        .slot_id = lane_index,
                                        .position = position,
                                    });
          });
          if (started.has_error()) {
            observe_completion(round, child_completion{
                                          .position = position,
                                          .status = status_type::failure(started.error()),
                                          .stopped_signal = false,
                                      });
          }
        }

        for (;;) {
          drain(round);
          if (round.finished && round.publications_quiescent()) {
            auto status = finalize(round);
            round.children.destroy_all();
            return status;
          }

          const auto observed = round.wake_epoch.load(std::memory_order_acquire);
          if (round.ready.has_ready() || (round.finished && round.publications_quiescent())) {
            continue;
          }
          round.wake_epoch.wait(observed, std::memory_order_acquire);
        }
      }

    private:
      auto observe_completion(round_state &round, child_completion completion) noexcept -> void {
        merge_stream_reader::observe_round_completion(round.tracker, round.stop_source,
                                                      round.finished, std::move(completion));
      }

      auto drain(round_state &round) noexcept -> void {
        round.drain_ready([&](const std::uint32_t, child_completion completion) {
          observe_completion(round, std::move(completion));
        });
      }

      [[nodiscard]] auto finalize(round_state &round) noexcept -> status_type {
        auto outcome = round.tracker.take_outcome();

        if (outcome.winner_position.has_value() && outcome.winner_status.has_value()) {
          auto resolution = state_.complete_round_winner(
              round.lane_indices, *outcome.winner_position, std::move(*outcome.winner_status));
          state_.close_lane_if_needed(resolution.close_lane);
          return std::move(resolution.status);
        }

        if (outcome.stopped_without_winner) {
          state_.complete_round_without_winner(round.lane_indices);
          return status_type::failure(wh::core::errc::internal_error);
        }

        state_.complete_round_without_winner(round.lane_indices);
        return status_type::failure(wh::core::errc::internal_error);
      }

      runtime &state_;
    };

    runtime() : blocking_read_(*this) {}

    explicit runtime(std::vector<lane_type> &&readers)
        : blocking_read_(*this), topology_(std::move(readers)) {}

    explicit runtime(std::vector<lane_source_type> &&sources)
      requires(supports_dynamic_injection)
        : blocking_read_(*this), topology_(std::move(sources)) {}

    [[nodiscard]] auto uses_fixed_poll_path() const noexcept -> bool {
      return topology_.uses_fixed_poll_path();
    }

    [[nodiscard]] auto try_read() -> try_status_type { return topology_.try_read_immediate(); }

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

    [[nodiscard]] auto begin_round() -> round_spec { return topology_.prepare_round(); }

    [[nodiscard]] auto lane_read_async(const std::size_t lane_index) {
      return topology_.lane_read_async(lane_index);
    }

    [[nodiscard]] auto complete_round_winner(const runtime::lane_list &round_lanes,
                                             const std::size_t winner_position, status_type status)
        -> round_resolution {
      return topology_.complete_round_winner(round_lanes, winner_position, std::move(status));
    }

    auto complete_round_without_winner(const runtime::lane_list &round_lanes) -> void {
      topology_.complete_round_without_winner(round_lanes);
    }

    auto close_lane_if_needed(const std::optional<std::size_t> lane_index) -> void {
      topology_.close_lane_if_needed(lane_index);
    }

    auto close_all() -> wh::core::result<void> { return topology_.close_all(); }

    [[nodiscard]] auto is_closed() const noexcept -> bool { return topology_.is_closed(); }

    [[nodiscard]] auto is_source_closed() const noexcept -> bool {
      return topology_.is_source_closed();
    }

    auto set_automatic_close(const auto_close_options &options) -> void {
      topology_.set_automatic_close(options);
    }

    auto attach(std::string_view source, reader_t reader) -> wh::core::result<void>
      requires(supports_dynamic_injection)
    {
      return topology_.attach(source, std::move(reader));
    }

    auto disable(std::string_view source) -> wh::core::result<void>
      requires(supports_dynamic_injection)
    {
      return topology_.disable(source);
    }

    [[nodiscard]] auto register_sync_topology_waiter(topology_sync_waiter *waiter,
                                                     const std::uint64_t expected_epoch) -> bool {
      return topology_.register_sync_topology_waiter(waiter, expected_epoch);
    }

    [[nodiscard]] auto register_async_topology_waiter(topology_async_waiter *waiter,
                                                      const std::uint64_t expected_epoch) -> bool {
      return topology_.register_async_topology_waiter(waiter, expected_epoch);
    }

    [[nodiscard]] auto remove_async_topology_waiter(topology_async_waiter *waiter) -> bool {
      return topology_.remove_async_topology_waiter(waiter);
    }

    [[nodiscard]] auto topology_epoch() const noexcept -> std::uint64_t {
      return topology_.topology_epoch();
    }

    [[nodiscard]] auto has_pending_lanes() const noexcept -> bool {
      return topology_.has_pending_lanes();
    }

    [[nodiscard]] auto lane_count() const noexcept -> std::size_t { return topology_.lane_count(); }

  private:
    blocking_read_op blocking_read_;
    topology_registry_t topology_{};
  };

  template <typename receiver_t> class read_operation {
    using receiver_env_t = stdexec::env_of_t<receiver_t>;
    using child_sender_t = decltype(std::declval<reader_t &>().read_async());
    using outer_stop_token_t = stdexec::stop_token_of_t<receiver_env_t>;
    using resume_scheduler_t = wh::core::detail::resume_scheduler_t<receiver_env_t>;

    friend class wh::core::detail::scheduled_resume_turn<read_operation, resume_scheduler_t>;

    struct final_completion {
      std::optional<status_type> value{};
      std::exception_ptr error{};
      bool stopped{false};
    };
    using child_completion = round_child_completion;

    struct child_env {
      resume_scheduler_t scheduler_{};
      stdexec::inplace_stop_token stop_token_{};

      [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
          -> stdexec::inplace_stop_token {
        return stop_token_;
      }

      [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> resume_scheduler_t {
        return scheduler_;
      }

      [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
          -> resume_scheduler_t {
        return scheduler_;
      }

      template <typename cpo_t>
      [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
          -> resume_scheduler_t {
        return scheduler_;
      }
    };

    struct round_state;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      read_operation *op{nullptr};
      round_state *round{nullptr};
      std::uint32_t slot_id{0U};
      std::size_t position{0U};

      auto set_value(stream_result<chunk_type> status) noexcept -> void {
        complete(child_completion{
            .position = position,
            .status = std::move(status),
            .stopped_signal = false,
        });
      }

      template <typename error_t> auto set_error(error_t &&error) noexcept -> void {
        complete(child_completion{
            .position = position,
            .status =
                status_type::failure(detail::map_merge_async_error(std::forward<error_t>(error))),
            .stopped_signal = false,
        });
      }

      auto set_stopped() noexcept -> void {
        complete(child_completion{
            .position = position,
            .status = status_type{},
            .stopped_signal = true,
        });
      }

      [[nodiscard]] auto get_env() const noexcept -> child_env {
        return child_env{
            .scheduler_ = op->scheduler_,
            .stop_token_ =
                round == nullptr ? stdexec::inplace_stop_token{} : round->stop_source.get_token(),
        };
      }

    private:
      auto complete(child_completion completion) noexcept -> void {
        auto *op_ptr = op;
        auto *round_ptr = round;
        if (op_ptr == nullptr || round_ptr == nullptr) {
          return;
        }

        round_ptr->publish_completion(slot_id, std::move(completion));
        op_ptr->request_resume();
        op_ptr->arrive();
      }
    };

    using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;
    using child_slots_t = child_slots<child_op_t, child_completion>;
    using round_state_base_t =
        round_state_base<typename runtime::lane_list, child_slots_t, child_completion>;

    struct round_state : round_state_base_t {
      using round_state_base_t::round_state_base_t;
    };

    struct topology_waiter final : topology_async_waiter {
      read_operation *op{nullptr};
    };

    struct stop_callback {
      read_operation *self{nullptr};

      auto operator()() const noexcept -> void {
        if (self == nullptr) {
          return;
        }
        auto *self_ptr = self;
        self_ptr->count_.fetch_add(1U, std::memory_order_relaxed);
        self_ptr->stop_requested_.store(true, std::memory_order_release);
        self_ptr->request_resume();
        self_ptr->arrive();
      }
    };

    using stop_callback_t = stdexec::stop_callback_for_t<outer_stop_token_t, stop_callback>;

  public:
    using operation_state_concept = stdexec::operation_state_t;

    explicit read_operation(wh::core::detail::intrusive_ptr<runtime> state, receiver_t receiver)
        : state_(std::move(state)), receiver_(std::move(receiver)),
          env_(stdexec::get_env(receiver_)),
          scheduler_(wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(env_)),
          lane_capacity_(state_ ? state_->lane_count() : 0U), resume_turn_(scheduler_) {
      static constexpr
          typename topology_async_waiter::ops_type ops{[](topology_async_waiter *base) noexcept {
            auto *waiter = static_cast<topology_waiter *>(base);
            auto *op_ptr = waiter->op;
            op_ptr->topology_ready_.store(true, std::memory_order_release);
            op_ptr->request_resume();
            op_ptr->arrive();
          }};
      topology_waiter_.op = this;
      topology_waiter_.ops = &ops;
    }

    read_operation(const read_operation &) = delete;
    auto operator=(const read_operation &) -> read_operation & = delete;
    read_operation(read_operation &&) = delete;
    auto operator=(read_operation &&) -> read_operation & = delete;

    ~read_operation() {
      release_stop();
      resume_turn_.destroy();
      destroy_round();
    }

    auto start() & noexcept -> void {
      if (!state_) {
        set_terminal_value(stream_result<chunk_type>::failure(wh::core::errc::not_found));
        request_resume();
        arrive();
        return;
      }

      auto stop_token = stdexec::get_stop_token(env_);
      if (stop_token.stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        set_terminal_stopped();
        request_resume();
        arrive();
        return;
      }

      if constexpr (!stdexec::unstoppable_token<outer_stop_token_t>) {
        try {
          on_stop_.emplace(stop_token, stop_callback{this});
        } catch (...) {
          set_terminal_error(std::current_exception());
          request_resume();
          arrive();
          return;
        }
        if (stop_token.stop_requested()) {
          stop_requested_.store(true, std::memory_order_release);
          set_terminal_stopped();
          request_resume();
          arrive();
          return;
        }
      }

      request_resume();
      arrive();
    }

  private:
    [[nodiscard]] auto completed() const noexcept -> bool {
      return completed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto resume_turn_completed() const noexcept -> bool { return completed(); }

    [[nodiscard]] auto terminal_pending() const noexcept -> bool { return terminal_.has_value(); }

    auto release_stop() noexcept -> void { on_stop_.reset(); }

    auto request_resume() noexcept -> void { resume_turn_.request(this); }

    auto arrive() noexcept -> void {
      if (count_.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
        maybe_complete();
      }
    }

    auto resume_turn_arrive() noexcept -> void { arrive(); }

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
      return terminal_pending() && !round_engaged_ && !topology_waiting_registered_ &&
             !resume_turn_running();
    }

    auto resume_turn_add_ref() noexcept -> void { count_.fetch_add(1U, std::memory_order_relaxed); }

    auto resume_turn_schedule_error(const wh::core::error_code error) noexcept -> void {
      override_terminal_failure(error);
    }

    auto resume_turn_run() noexcept -> void {
      resume();
      maybe_complete();
    }

    auto resume_turn_idle() noexcept -> void { maybe_complete(); }

    [[nodiscard]] auto resume_turn_running() const noexcept -> bool {
      return resume_turn_.running();
    }

    auto set_terminal(final_completion completion) noexcept -> void {
      if (terminal_pending()) {
        return;
      }
      terminal_.emplace(std::move(completion));
      maybe_complete();
    }

    auto override_terminal(final_completion completion) noexcept -> void {
      if (!terminal_pending()) {
        set_terminal(std::move(completion));
        return;
      }
      terminal_ = std::move(completion);
      maybe_complete();
    }

    auto set_terminal_value(status_type status) noexcept -> void {
      set_terminal(final_completion{.value = std::move(status)});
    }

    auto set_terminal_error(std::exception_ptr error) noexcept -> void {
      set_terminal(final_completion{.error = std::move(error)});
    }

    auto set_terminal_stopped() noexcept -> void {
      set_terminal(final_completion{.stopped = true});
    }

    auto override_terminal_failure(const wh::core::error_code error) noexcept -> void {
      override_terminal(final_completion{.value = status_type::failure(error)});
    }

    [[nodiscard]] auto active_round() noexcept -> round_state & {
      wh_invariant(round_engaged_);
      return round_.template get<round_state>();
    }

    auto construct_round() -> void {
      [[maybe_unused]] auto &round = round_.template construct<round_state>(lane_capacity_);
      round_engaged_ = true;
    }

    auto destroy_round() noexcept -> void {
      if (!round_engaged_) {
        return;
      }
      round_.template get<round_state>().children.destroy_all();
      round_.template destruct<round_state>();
      round_engaged_ = false;
    }

    auto observe_completion(child_completion completion) noexcept -> void {
      auto &round = active_round();
      merge_stream_reader::observe_round_completion(round.tracker, round.stop_source,
                                                    round.finished, std::move(completion));
    }

    auto drain_round() noexcept -> void {
      if (!round_engaged_) {
        return;
      }
      auto &round = active_round();
      round.drain_ready([this](const std::uint32_t, child_completion completion) {
        observe_completion(std::move(completion));
      });
    }

    [[nodiscard]] auto wait_for_topology() noexcept -> bool {
      if (!state_) {
        return false;
      }

      count_.fetch_add(1U, std::memory_order_relaxed);
      if (!state_->register_async_topology_waiter(&topology_waiter_, round_topology_epoch_)) {
        arrive();
        return false;
      }
      topology_waiting_registered_ = true;
      return true;
    }

    [[nodiscard]] auto cancel_topology_wait() noexcept -> bool {
      if (!topology_waiting_registered_) {
        return true;
      }
      if (state_ && state_->remove_async_topology_waiter(&topology_waiter_)) {
        topology_waiting_registered_ = false;
        arrive();
        return true;
      }
      return false;
    }

    [[nodiscard]] auto start_round(runtime::lane_list lanes) noexcept -> bool {
      if (lanes.empty()) {
        state_->complete_round_without_winner(lanes);
        set_terminal_value(stream_result<chunk_type>::failure(wh::core::errc::internal_error));
        return true;
      }

      try {
        construct_round();
      } catch (...) {
        state_->complete_round_without_winner(lanes);
        set_terminal_error(std::current_exception());
        return true;
      }

      auto &round = active_round();
      round.lane_indices = std::move(lanes);
      try {
        round.tracker.reset(round.lane_indices.size());
      } catch (...) {
        state_->complete_round_without_winner(round.lane_indices);
        destroy_round();
        set_terminal_error(std::current_exception());
        return true;
      }

      if (stop_requested_.load(std::memory_order_acquire) ||
          stdexec::get_stop_token(env_).stop_requested()) {
        stop_requested_.store(true, std::memory_order_release);
        round.stop_source.request_stop();
      }

      for (std::size_t position = 0U; position < round.lane_indices.size(); ++position) {
        const auto lane_index = static_cast<std::uint32_t>(round.lane_indices[position]);
        if (round.stop_source.stop_requested()) {
          observe_completion(child_completion{
              .position = position,
              .status = status_type{},
              .stopped_signal = true,
          });
          continue;
        }

        auto started = round.children.start(
            lane_index,
            [&]() -> child_op_t {
              return stdexec::connect(state_->lane_read_async(round.lane_indices[position]),
                                      child_receiver{
                                          .op = this,
                                          .round = std::addressof(round),
                                          .slot_id = lane_index,
                                          .position = position,
                                      });
            },
            [this]() noexcept { count_.fetch_add(1U, std::memory_order_relaxed); });
        if (started.has_error()) {
          observe_completion(child_completion{
              .position = position,
              .status = status_type::failure(started.error()),
              .stopped_signal = false,
          });
        }
      }

      drain_round();
      if (round.finished && round.publications_quiescent()) {
        return finalize_round();
      }
      return false;
    }

    [[nodiscard]] auto finalize_round() noexcept -> bool {
      auto &round = active_round();
      auto outcome = round.tracker.take_outcome();
      auto lane_indices = std::move(round.lane_indices);

      if (outcome.winner_position.has_value() && outcome.winner_status.has_value()) {
        auto resolution = state_->complete_round_winner(lane_indices, *outcome.winner_position,
                                                        std::move(*outcome.winner_status));
        state_->close_lane_if_needed(resolution.close_lane);
        destroy_round();
        set_terminal_value(std::move(resolution.status));
        return true;
      }

      if (outcome.stopped_without_winner) {
        state_->complete_round_without_winner(lane_indices);
        destroy_round();
        if (stop_requested_.load(std::memory_order_acquire)) {
          set_terminal_stopped();
        } else {
          set_terminal_value(stream_result<chunk_type>::failure(wh::core::errc::internal_error));
        }
        return true;
      }

      state_->complete_round_without_winner(lane_indices);
      destroy_round();
      if (state_->topology_epoch() != round_topology_epoch_ || state_->has_pending_lanes()) {
        return false;
      }
      set_terminal_value(stream_result<chunk_type>::failure(wh::core::errc::internal_error));
      return true;
    }

    [[nodiscard]] auto handle_terminal() noexcept -> bool {
      if (!terminal_pending()) {
        return false;
      }

      if (round_engaged_) {
        auto &round = active_round();
        round.stop_source.request_stop();
        drain_round();
        if (!round.finished || !round.publications_quiescent()) {
          return false;
        }
        state_->complete_round_without_winner(round.lane_indices);
        destroy_round();
      }

      if (topology_waiting_registered_ && !cancel_topology_wait()) {
        return false;
      }

      return true;
    }

    auto resume() noexcept -> void {
      while (!completed()) {
        if (topology_ready_.exchange(false, std::memory_order_acq_rel)) {
          topology_waiting_registered_ = false;
        }

        if (terminal_pending()) {
          if (!handle_terminal()) {
            return;
          }
          return;
        }

        if (round_engaged_) {
          auto &round = active_round();
          if (stop_requested_.load(std::memory_order_acquire)) {
            round.stop_source.request_stop();
          }
          drain_round();
          if (!round.finished || !round.publications_quiescent()) {
            return;
          }
          if (finalize_round()) {
            continue;
          }
          if (round_engaged_) {
            return;
          }
          if (state_->has_pending_lanes() && wait_for_topology()) {
            return;
          }
          continue;
        }

        if (topology_waiting_registered_) {
          if (!stop_requested_.load(std::memory_order_acquire)) {
            return;
          }
          if (!cancel_topology_wait()) {
            return;
          }
          set_terminal_stopped();
          continue;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
          set_terminal_stopped();
          continue;
        }

        if (!state_) {
          set_terminal_value(stream_result<chunk_type>::failure(wh::core::errc::not_found));
          continue;
        }

        auto immediate = state_->try_read();
        if (std::holds_alternative<status_type>(immediate)) {
          set_terminal_value(std::move(std::get<status_type>(immediate)));
          continue;
        }

        auto round_spec = state_->begin_round();
        round_topology_epoch_ = round_spec.topology_epoch;
        if (round_spec.immediate_status.has_value()) {
          set_terminal_value(std::move(*round_spec.immediate_status));
          continue;
        }

        if (round_spec.wait_for_topology) {
          if (wait_for_topology()) {
            return;
          }
          continue;
        }

        if (start_round(std::move(round_spec.lanes))) {
          continue;
        }
        if (round_engaged_ || topology_waiting_registered_) {
          return;
        }
      }
    }

    auto release_runtime_state() noexcept -> void {
      release_stop();
      terminal_.reset();
      state_.reset();
      topology_waiting_registered_ = false;
      topology_ready_.store(false, std::memory_order_release);
    }

    auto complete() noexcept -> void {
      if (!terminal_pending() || completed_.exchange(true, std::memory_order_acq_rel)) {
        return;
      }

      auto completion = std::move(*terminal_);
      release_runtime_state();
      if (completion.error != nullptr) {
        stdexec::set_error(std::move(receiver_), std::move(completion.error));
        return;
      }
      if (completion.stopped) {
        stdexec::set_stopped(std::move(receiver_));
        return;
      }
      wh_invariant(completion.value.has_value());
      stdexec::set_value(std::move(receiver_), std::move(*completion.value));
    }

    wh::core::detail::intrusive_ptr<runtime> state_{};
    receiver_t receiver_;
    receiver_env_t env_{};
    resume_scheduler_t scheduler_;
    std::size_t lane_capacity_{0U};
    wh::core::detail::manual_storage<sizeof(round_state), alignof(round_state)> round_{};
    bool round_engaged_{false};
    wh::core::detail::scheduled_resume_turn<read_operation, resume_scheduler_t> resume_turn_;
    std::optional<stop_callback_t> on_stop_{};
    std::optional<final_completion> terminal_{};
    std::atomic<std::size_t> count_{1U};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> topology_ready_{false};
    std::atomic<bool> completed_{false};
    bool topology_waiting_registered_{false};
    std::uint64_t round_topology_epoch_{0U};
    topology_waiter topology_waiter_{};
  };

  class read_sender {
  public:
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(stream_result<chunk_type>),
                                       stdexec::set_error_t(std::exception_ptr),
                                       stdexec::set_stopped_t()>;

    explicit read_sender(wh::core::detail::intrusive_ptr<runtime> state)
        : state_(std::move(state)) {}

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) && -> read_operation<std::remove_cvref_t<receiver_t>> {
      using operation_t = read_operation<std::remove_cvref_t<receiver_t>>;
      return operation_t{std::move(state_), std::move(receiver)};
    }

    template <stdexec::receiver_of<completion_signatures> receiver_t>
      requires wh::core::detail::receiver_with_resume_scheduler<receiver_t>
    [[nodiscard]] auto
    connect(receiver_t receiver) const & -> read_operation<std::remove_cvref_t<receiver_t>> {
      using operation_t = read_operation<std::remove_cvref_t<receiver_t>>;
      return operation_t{state_, std::move(receiver)};
    }

    [[nodiscard]] auto get_env() const noexcept -> wh::core::detail::async_completion_env {
      return {};
    }

  private:
    wh::core::detail::intrusive_ptr<runtime> state_{};
  };

public:
  merge_stream_reader() = default;

  explicit merge_stream_reader(std::vector<lane_type> &&readers)
      : state_(wh::core::detail::make_intrusive<runtime>(std::move(readers))) {}

  explicit merge_stream_reader(std::vector<lane_source_type> &&sources)
    requires(supports_dynamic_injection)
      : state_(wh::core::detail::make_intrusive<runtime>(std::move(sources))) {}

  merge_stream_reader(const merge_stream_reader &) = delete;
  auto operator=(const merge_stream_reader &) -> merge_stream_reader & = delete;
  merge_stream_reader(merge_stream_reader &&) noexcept = default;
  auto operator=(merge_stream_reader &&) noexcept -> merge_stream_reader & = default;
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
  auto attach(std::string_view source, reader_t reader) -> wh::core::result<void>
    requires(supports_dynamic_injection)
  {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    return state_->attach(source, std::move(reader));
  }

  /// Disables one pre-registered pending lane.
  auto disable(std::string_view source) -> wh::core::result<void>
    requires(supports_dynamic_injection)
  {
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
  explicit merge_stream_reader(wh::core::detail::intrusive_ptr<runtime> state)
      : state_(std::move(state)) {}

  wh::core::detail::intrusive_ptr<runtime> state_{};
};

/// Builds a merged stream from explicitly named readers.
template <stream_reader reader_t>
[[nodiscard]] inline auto
make_merge_stream_reader(std::vector<named_stream_reader<reader_t>> &&readers)
    -> merge_stream_reader<reader_t, merge_topology_mode::static_attached> {
  detail::sort_named_stream_readers(readers);
  return merge_stream_reader<reader_t, merge_topology_mode::static_attached>{std::move(readers)};
}

/// Builds a live merged stream shell from pending lane source names.
template <stream_reader reader_t>
[[nodiscard]] inline auto make_merge_stream_reader(std::vector<std::string> &&sources)
    -> merge_stream_reader<reader_t, merge_topology_mode::dynamic_injection> {
  std::ranges::sort(sources);
  return merge_stream_reader<reader_t, merge_topology_mode::dynamic_injection>{std::move(sources)};
}

/// Builds a merged stream from anonymous readers named by index.
template <stream_reader reader_t>
[[nodiscard]] inline auto make_merge_stream_reader(std::vector<reader_t> &&readers)
    -> merge_stream_reader<reader_t, merge_topology_mode::static_attached> {
  return make_merge_stream_reader(detail::make_named_stream_readers(std::move(readers)));
}

} // namespace wh::schema::stream
