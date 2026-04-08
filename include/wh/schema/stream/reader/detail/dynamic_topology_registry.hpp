// Defines the merge topology registry that owns lane state, poll order, and
// topology waiters independently from any read-round controller.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/types.hpp"

namespace detail {

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

template <typename waiter_t> struct waiter_ops {
  void (*complete)(waiter_t *) noexcept {nullptr};
};

template <typename waiter_t> class waiter_ready_list {
public:
  auto push_back(waiter_t *waiter) noexcept -> void {
    waiter->next = nullptr;
    waiter->prev = nullptr;
    if (tail_ != nullptr) {
      tail_->next = waiter;
    } else {
      head_ = waiter;
    }
    tail_ = waiter;
  }

  auto complete_all() noexcept -> void {
    while (head_ != nullptr) {
      auto *current = head_;
      head_ = head_->next;
      current->next = nullptr;
      current->prev = nullptr;
      current->ops->complete(current);
    }
    tail_ = nullptr;
  }

private:
  waiter_t *head_{nullptr};
  waiter_t *tail_{nullptr};
};

struct topology_sync_waiter {
  topology_sync_waiter *next{nullptr};
  topology_sync_waiter *prev{nullptr};
  std::atomic_flag ready = ATOMIC_FLAG_INIT;

  auto notify() noexcept -> void {
    ready.test_and_set(std::memory_order_release);
    ready.notify_one();
  }

  auto wait() noexcept -> void {
    while (!ready.test(std::memory_order_acquire)) {
      ready.wait(false, std::memory_order_acquire);
    }
  }
};

struct topology_async_waiter {
  using ops_type = waiter_ops<topology_async_waiter>;

  topology_async_waiter *next{nullptr};
  topology_async_waiter *prev{nullptr};
  const ops_type *ops{nullptr};
};

enum class topology_poll_mode : std::uint8_t {
  fixed = 0U,
  dynamic,
};

template <typename reader_t> class dynamic_topology_registry {
public:
  using value_type = typename reader_t::value_type;
  using chunk_type = stream_chunk<value_type>;
  using status_type = stream_result<chunk_type>;
  using try_status_type = stream_try_result<chunk_type>;
  using lane_type = wh::schema::stream::named_stream_reader<reader_t>;
  using lane_source_type = std::string;
  using lane_list = wh::core::small_vector<std::size_t, 8U>;
  using sync_waiter_type = topology_sync_waiter;
  using async_waiter_type = topology_async_waiter;
  using topology_sync_ready_buffer =
      wh::core::small_vector<sync_waiter_type *, 8U>;
  using topology_async_ready_list = waiter_ready_list<async_waiter_type>;

  struct round_plan {
    lane_list lanes{};
    std::optional<status_type> immediate_status{};
    std::uint64_t topology_epoch{0U};
    bool wait_for_topology{false};
  };

  struct round_resolution {
    status_type status{};
    std::optional<std::size_t> close_lane{};
  };

  dynamic_topology_registry() = default;

  explicit dynamic_topology_registry(std::vector<lane_type> &&readers)
      : lanes_(build_lanes(std::move(readers))),
        mode_(lanes_.size() <= 5U ? topology_poll_mode::fixed
                                  : topology_poll_mode::dynamic),
        attached_lanes_(count_attached_lanes(lanes_)),
        pending_lanes_(count_pending_lanes(lanes_)) {}

  explicit dynamic_topology_registry(std::vector<lane_source_type> &&sources)
      : lanes_(build_pending_lanes(std::move(sources))),
        mode_(lanes_.size() <= 5U ? topology_poll_mode::fixed
                                  : topology_poll_mode::dynamic),
        pending_lanes_(lanes_.size()) {}

  [[nodiscard]] auto uses_fixed_poll_path() const noexcept -> bool {
    std::scoped_lock lock(lock_);
    return mode_ == topology_poll_mode::fixed;
  }

  [[nodiscard]] auto try_read_immediate() -> try_status_type {
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
      return pending_lanes_ > 0U
                 ? try_status_type{stream_pending}
                 : try_status_type{status_type{chunk_type::make_eof()}};
    }
    return mode_ == topology_poll_mode::fixed ? next_fixed_path_locked()
                                              : next_dynamic_path_locked();
  }

  [[nodiscard]] auto prepare_round() -> round_plan {
    std::scoped_lock lock(lock_);
    if (closed_) {
      return round_plan{
          .immediate_status = status_type{chunk_type::make_eof()},
      };
    }
    if (read_in_flight_) {
      return round_plan{
          .immediate_status =
              status_type::failure(wh::core::errc::unavailable),
      };
    }
    if (lanes_.empty()) {
      return round_plan{.immediate_status = chunk_type::make_eof()};
    }
    if (attached_lanes_ == 0U) {
      if (pending_lanes_ > 0U) {
        return round_plan{
            .topology_epoch = topology_epoch_,
            .wait_for_topology = true,
        };
      }
      return round_plan{.immediate_status = chunk_type::make_eof()};
    }

    auto lanes = current_poll_order_locked();
    if (lanes.empty()) {
      if (pending_lanes_ > 0U) {
        return round_plan{
            .topology_epoch = topology_epoch_,
            .wait_for_topology = true,
        };
      }
      return round_plan{.immediate_status = chunk_type::make_eof()};
    }

    read_in_flight_ = true;
    return round_plan{
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
          if (lane.status == lane_phase::attached) {
            if (automatic_close_) {
              close_lane = lane_index;
            }
            lane.reader.reset();
            lane.status = lane_phase::finished;
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
    return round_resolution{
        .status = std::move(resolved),
        .close_lane = close_lane,
    };
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

  auto close_lane_if_needed(const std::optional<std::size_t> lane_index)
      -> void {
    if (!lane_index.has_value()) {
      return;
    }
    auto &lane = lanes_[*lane_index];
    if (lane.reader.has_value()) {
      [[maybe_unused]] const auto close_status = lane.reader->close();
    }
  }

  auto close_all() -> wh::core::result<void> {
    topology_sync_ready_buffer sync_ready{};
    topology_async_ready_list async_ready{};
    wh::core::result<void> close_status{};
    {
      std::scoped_lock lock(lock_);
      if (read_in_flight_) {
        return wh::core::result<void>::failure(wh::core::errc::unavailable);
      }
      for (auto &lane : lanes_) {
        if (lane.status == lane_phase::attached && lane.reader.has_value()) {
          auto closed = lane.reader->close();
          if (closed.has_error() &&
              closed.error() != wh::core::errc::channel_closed &&
              !close_status.has_error()) {
            close_status = closed;
          }
        }
        lane.reader.reset();
        lane.status = lane_phase::disabled;
      }
      attached_lanes_ = 0U;
      pending_lanes_ = 0U;
      closed_ = true;
      ++topology_epoch_;
      detach_topology_waiters_locked(sync_ready, async_ready);
    }
    notify_topology_waiters(sync_ready, async_ready);
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
      if (lane.status == lane_phase::attached && lane.reader.has_value() &&
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

  auto attach(std::string_view source, reader_t reader)
      -> wh::core::result<void> {
    topology_sync_ready_buffer sync_ready{};
    topology_async_ready_list async_ready{};
    {
      std::scoped_lock lock(lock_);
      if (closed_) {
        return wh::core::result<void>::failure(wh::core::errc::channel_closed);
      }
      auto *lane = find_lane_locked(source);
      if (lane == nullptr) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
      if (lane->status != lane_phase::pending) {
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
      lane->status = lane_phase::attached;
      ++attached_lanes_;
      --pending_lanes_;
      ++topology_epoch_;
      detach_topology_waiters_locked(sync_ready, async_ready);
    }
    notify_topology_waiters(sync_ready, async_ready);
    return {};
  }

  auto disable(std::string_view source) -> wh::core::result<void> {
    topology_sync_ready_buffer sync_ready{};
    topology_async_ready_list async_ready{};
    {
      std::scoped_lock lock(lock_);
      auto *lane = find_lane_locked(source);
      if (lane == nullptr) {
        return wh::core::result<void>::failure(wh::core::errc::not_found);
      }
      if (lane->status == lane_phase::disabled ||
          lane->status == lane_phase::finished) {
        return {};
      }
      if (lane->status == lane_phase::attached) {
        return wh::core::result<void>::failure(
            wh::core::errc::invalid_argument);
      }
      lane->status = lane_phase::disabled;
      if (pending_lanes_ > 0U) {
        --pending_lanes_;
      }
      ++topology_epoch_;
      detach_topology_waiters_locked(sync_ready, async_ready);
    }
    notify_topology_waiters(sync_ready, async_ready);
    return {};
  }

  [[nodiscard]] auto register_sync_topology_waiter(
      sync_waiter_type *waiter, const std::uint64_t expected_epoch) -> bool {
    std::scoped_lock lock(lock_);
    if (closed_ || topology_epoch_ != expected_epoch || attached_lanes_ > 0U ||
        pending_lanes_ == 0U) {
      return false;
    }
    waiter->ready.clear(std::memory_order_release);
    sync_topology_waiters_.push_back(waiter);
    return true;
  }

  [[nodiscard]] auto register_async_topology_waiter(
      async_waiter_type *waiter, const std::uint64_t expected_epoch) -> bool {
    std::scoped_lock lock(lock_);
    if (closed_ || topology_epoch_ != expected_epoch || attached_lanes_ > 0U ||
        pending_lanes_ == 0U) {
      return false;
    }
    async_topology_waiters_.push_back(waiter);
    return true;
  }

  [[nodiscard]] auto remove_async_topology_waiter(async_waiter_type *waiter)
      -> bool {
    std::scoped_lock lock(lock_);
    return async_topology_waiters_.try_remove(waiter);
  }

  [[nodiscard]] auto topology_epoch() const noexcept -> std::uint64_t {
    std::scoped_lock lock(lock_);
    return topology_epoch_;
  }

  [[nodiscard]] auto has_pending_lanes() const noexcept -> bool {
    std::scoped_lock lock(lock_);
    return pending_lanes_ > 0U;
  }

  [[nodiscard]] auto lane_count() const noexcept -> std::size_t {
    std::scoped_lock lock(lock_);
    return lanes_.size();
  }

private:
  enum class lane_phase : std::uint8_t {
    pending = 0U,
    attached,
    finished,
    disabled,
  };

  struct lane_state {
    std::string source{};
    std::optional<reader_t> reader{};
    lane_phase status{lane_phase::pending};
  };

  [[nodiscard]] static auto build_lanes(std::vector<lane_type> &&readers)
      -> std::vector<lane_state> {
    std::vector<lane_state> lanes{};
    lanes.reserve(readers.size());
    for (auto &reader : readers) {
      lanes.push_back(lane_state{
          .source = std::move(reader.source),
          .reader = std::move(reader.reader),
          .status = reader.finished ? lane_phase::finished
                                    : lane_phase::attached,
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
          .status = lane_phase::pending,
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
          .status = lane_phase::pending,
      });
    }
    return lanes;
  }

  [[nodiscard]] static auto
  count_attached_lanes(const std::vector<lane_state> &lanes) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        lanes, [](const lane_state &lane) -> bool {
          return lane.status == lane_phase::attached;
        }));
  }

  [[nodiscard]] static auto
  count_pending_lanes(const std::vector<lane_state> &lanes) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        lanes, [](const lane_state &lane) -> bool {
          return lane.status == lane_phase::pending;
        }));
  }

  [[nodiscard]] auto current_poll_order_locked() const -> lane_list {
    lane_list lanes{};
    lanes.reserve(attached_lanes_);
    const auto lane_count = lanes_.size();
    for (std::size_t attempts = 0U; attempts < lane_count; ++attempts) {
      const auto index = (cursor_ + attempts) % lane_count;
      if (lanes_[index].status == lane_phase::attached) {
        lanes.push_back(index);
      }
    }
    return lanes;
  }

  auto poll_one_reader_locked(const std::size_t index, status_type &output)
      -> bool {
    auto &lane = lanes_[index];
    if (lane.status != lane_phase::attached || !lane.reader.has_value()) {
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
      lane.status = lane_phase::finished;
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

  auto detach_topology_waiters_locked(topology_sync_ready_buffer &sync_ready,
                                      topology_async_ready_list &async_ready)
      -> void {
    while (auto *waiter = sync_topology_waiters_.try_pop_front()) {
      sync_ready.push_back(waiter);
    }
    while (auto *waiter = async_topology_waiters_.try_pop_front()) {
      async_ready.push_back(waiter);
    }
  }

  static auto notify_topology_waiters(
      const topology_sync_ready_buffer &sync_ready,
      topology_async_ready_list &async_ready) -> void {
    for (auto *waiter : sync_ready) {
      waiter->notify();
    }
    async_ready.complete_all();
  }

  mutable std::mutex lock_{};
  std::vector<lane_state> lanes_{};
  topology_poll_mode mode_{topology_poll_mode::fixed};
  std::size_t attached_lanes_{0U};
  std::size_t pending_lanes_{0U};
  std::size_t cursor_{0U};
  bool closed_{false};
  bool automatic_close_{true};
  bool read_in_flight_{false};
  std::uint64_t topology_epoch_{0U};
  intrusive_waiter_list<sync_waiter_type> sync_topology_waiters_{};
  intrusive_waiter_list<async_waiter_type> async_topology_waiters_{};
};

} // namespace detail
