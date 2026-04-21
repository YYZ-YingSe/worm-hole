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
#include "wh/schema/stream/reader/detail/topology_waiters.hpp"

namespace detail {

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
  using topology_sync_ready_buffer = wh::core::small_vector<sync_waiter_type *, 8U>;
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
        mode_(lanes_.size() <= 5U ? topology_poll_mode::fixed : topology_poll_mode::dynamic),
        attached_lanes_(count_attached_lanes(lanes_)), pending_lanes_(count_pending_lanes(lanes_)) {
    seed_attached_lanes();
  }

  explicit dynamic_topology_registry(std::vector<lane_source_type> &&sources)
      : lanes_(build_pending_lanes(std::move(sources))),
        mode_(lanes_.size() <= 5U ? topology_poll_mode::fixed : topology_poll_mode::dynamic),
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
      return pending_lanes_ > 0U ? try_status_type{stream_pending}
                                 : try_status_type{status_type{chunk_type::make_eof()}};
    }

    while (const auto lane_index = pop_ready_lane_locked()) {
      status_type output{};
      if (poll_ready_lane_locked(*lane_index, output)) {
        return try_status_type{std::move(output)};
      }
      enqueue_probe_locked(*lane_index);
    }
    return stream_pending;
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
          .immediate_status = status_type::failure(wh::core::errc::unavailable),
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

    auto lanes = take_round_lanes_locked();
    if (lanes.empty()) {
      if (pending_lanes_ > 0U) {
        return round_plan{
            .topology_epoch = topology_epoch_,
            .wait_for_topology = true,
        };
      }
      return round_plan{
          .immediate_status = status_type::failure(wh::core::errc::internal_error),
      };
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

  [[nodiscard]] auto complete_round_winner(const lane_list &round_lanes,
                                           const std::size_t winner_position, status_type status)
      -> round_resolution {
    std::optional<std::size_t> close_lane{};
    status_type resolved{};
    {
      std::scoped_lock lock(lock_);
      read_in_flight_ = false;
      complete_round_lanes_locked(round_lanes, winner_position);
      if (winner_position >= round_lanes.size()) {
        return round_resolution{
            .status = status_type::failure(wh::core::errc::internal_error),
        };
      }

      const auto lane_index = round_lanes[winner_position];
      auto &lane = lanes_[lane_index];
      if (status.has_error()) {
        resolved = status_type::failure(status.error());
        enqueue_ready_locked(lane_index);
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
          enqueue_ready_locked(lane_index);
        }
      }
    }
    return round_resolution{
        .status = std::move(resolved),
        .close_lane = close_lane,
    };
  }

  auto complete_round_without_winner(const lane_list &round_lanes) -> void {
    std::scoped_lock lock(lock_);
    read_in_flight_ = false;
    complete_round_lanes_locked(round_lanes, std::nullopt);
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
          if (closed.has_error() && closed.error() != wh::core::errc::channel_closed &&
              !close_status.has_error()) {
            close_status = closed;
          }
        }
        lane.reader.reset();
        lane.status = lane_phase::disabled;
        lane.ready_queued = false;
        lane.probe_queued = false;
        lane.in_round = false;
      }
      attached_lanes_ = 0U;
      pending_lanes_ = 0U;
      closed_ = true;
      ready_queue_.clear();
      ready_head_ = 0U;
      probe_queue_.clear();
      probe_head_ = 0U;
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
      if constexpr (requires(reader_t &value, const auto_close_options &value_options) {
                      value.set_automatic_close(value_options);
                    }) {
        lane.reader->set_automatic_close(options);
      }
    }
  }

  auto attach(std::string_view source, reader_t reader) -> wh::core::result<void> {
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
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      if constexpr (requires(reader_t &value, const auto_close_options &value_options) {
                      value.set_automatic_close(value_options);
                    }) {
        reader.set_automatic_close(auto_close_options{automatic_close_});
      }
      lane->reader.emplace(std::move(reader));
      lane->status = lane_phase::attached;
      lane->in_round = false;
      lane->probe_queued = false;
      lane->ready_queued = false;
      ++attached_lanes_;
      --pending_lanes_;
      enqueue_ready_locked(static_cast<std::size_t>(lane - lanes_.data()));
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
      if (lane->status == lane_phase::disabled || lane->status == lane_phase::finished) {
        return {};
      }
      if (lane->status == lane_phase::attached) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      lane->status = lane_phase::disabled;
      lane->ready_queued = false;
      lane->probe_queued = false;
      lane->in_round = false;
      if (pending_lanes_ > 0U) {
        --pending_lanes_;
      }
      ++topology_epoch_;
      detach_topology_waiters_locked(sync_ready, async_ready);
    }
    notify_topology_waiters(sync_ready, async_ready);
    return {};
  }

  [[nodiscard]] auto register_sync_topology_waiter(sync_waiter_type *waiter,
                                                   const std::uint64_t expected_epoch) -> bool {
    std::scoped_lock lock(lock_);
    if (closed_ || topology_epoch_ != expected_epoch || attached_lanes_ > 0U ||
        pending_lanes_ == 0U) {
      return false;
    }
    waiter->ready.clear(std::memory_order_release);
    sync_topology_waiters_.push_back(waiter);
    return true;
  }

  [[nodiscard]] auto register_async_topology_waiter(async_waiter_type *waiter,
                                                    const std::uint64_t expected_epoch) -> bool {
    std::scoped_lock lock(lock_);
    if (closed_ || topology_epoch_ != expected_epoch || attached_lanes_ > 0U ||
        pending_lanes_ == 0U) {
      return false;
    }
    async_topology_waiters_.push_back(waiter);
    return true;
  }

  [[nodiscard]] auto remove_async_topology_waiter(async_waiter_type *waiter) -> bool {
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
    bool ready_queued{false};
    bool probe_queued{false};
    bool in_round{false};
  };

  [[nodiscard]] static auto build_lanes(std::vector<lane_type> &&readers)
      -> std::vector<lane_state> {
    std::vector<lane_state> lanes{};
    static_cast<void>(validate_indexed_capacity<>(
        readers.size(), "merge_stream_reader lane count exceeds uint32_t slot capacity"));
    lanes.reserve(readers.size());
    for (auto &reader : readers) {
      lanes.push_back(lane_state{
          .source = std::move(reader.source),
          .reader = std::move(reader.reader),
          .status = reader.finished ? lane_phase::finished : lane_phase::attached,
      });
    }
    return lanes;
  }

  [[nodiscard]] static auto build_pending_lanes(const std::vector<lane_source_type> &sources)
      -> std::vector<lane_state> {
    std::vector<lane_state> lanes{};
    static_cast<void>(validate_indexed_capacity<>(
        sources.size(), "merge_stream_reader lane count exceeds uint32_t slot capacity"));
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

  [[nodiscard]] static auto build_pending_lanes(std::vector<lane_source_type> &&sources)
      -> std::vector<lane_state> {
    std::vector<lane_state> lanes{};
    static_cast<void>(validate_indexed_capacity<>(
        sources.size(), "merge_stream_reader lane count exceeds uint32_t slot capacity"));
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

  [[nodiscard]] static auto count_attached_lanes(const std::vector<lane_state> &lanes)
      -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        lanes, [](const lane_state &lane) -> bool { return lane.status == lane_phase::attached; }));
  }

  [[nodiscard]] static auto count_pending_lanes(const std::vector<lane_state> &lanes)
      -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        lanes, [](const lane_state &lane) -> bool { return lane.status == lane_phase::pending; }));
  }

  auto seed_attached_lanes() -> void {
    for (std::size_t index = 0U; index < lanes_.size(); ++index) {
      if (lanes_[index].status == lane_phase::attached) {
        enqueue_ready_locked(index);
      }
    }
  }

  auto compact_queue_locked(std::vector<std::size_t> &queue, std::size_t &head) -> void {
    if (head == 0U) {
      return;
    }
    if (head >= queue.size()) {
      queue.clear();
      head = 0U;
      return;
    }
    if (head < 64U && head * 2U < queue.size()) {
      return;
    }
    queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(head));
    head = 0U;
  }

  auto enqueue_ready_locked(const std::size_t index) -> void {
    auto &lane = lanes_[index];
    if (lane.status != lane_phase::attached || lane.in_round || lane.ready_queued) {
      return;
    }
    lane.ready_queued = true;
    lane.probe_queued = false;
    ready_queue_.push_back(index);
  }

  auto enqueue_probe_locked(const std::size_t index) -> void {
    auto &lane = lanes_[index];
    if (lane.status != lane_phase::attached || lane.in_round || lane.ready_queued ||
        lane.probe_queued) {
      return;
    }
    lane.probe_queued = true;
    probe_queue_.push_back(index);
  }

  [[nodiscard]] auto pop_ready_lane_locked() -> std::optional<std::size_t> {
    while (ready_head_ < ready_queue_.size()) {
      const auto index = ready_queue_[ready_head_++];
      auto &lane = lanes_[index];
      if (!lane.ready_queued || lane.status != lane_phase::attached || lane.in_round) {
        continue;
      }
      lane.ready_queued = false;
      compact_queue_locked(ready_queue_, ready_head_);
      return index;
    }
    compact_queue_locked(ready_queue_, ready_head_);
    return std::nullopt;
  }

  [[nodiscard]] auto take_probe_lanes_locked() -> lane_list {
    lane_list lanes{};
    while (probe_head_ < probe_queue_.size()) {
      const auto index = probe_queue_[probe_head_++];
      auto &lane = lanes_[index];
      if (!lane.probe_queued || lane.status != lane_phase::attached || lane.in_round ||
          lane.ready_queued) {
        continue;
      }
      lane.probe_queued = false;
      lane.in_round = true;
      lanes.push_back(index);
    }
    compact_queue_locked(probe_queue_, probe_head_);
    return lanes;
  }

  [[nodiscard]] auto take_ready_lanes_locked() -> lane_list {
    lane_list lanes{};
    while (ready_head_ < ready_queue_.size()) {
      const auto index = ready_queue_[ready_head_++];
      auto &lane = lanes_[index];
      if (!lane.ready_queued || lane.status != lane_phase::attached || lane.in_round) {
        continue;
      }
      lane.ready_queued = false;
      lane.in_round = true;
      lanes.push_back(index);
    }
    compact_queue_locked(ready_queue_, ready_head_);
    return lanes;
  }

  [[nodiscard]] auto take_round_lanes_locked() -> lane_list {
    auto lanes = take_probe_lanes_locked();
    if (!lanes.empty()) {
      return lanes;
    }

    lanes = take_ready_lanes_locked();
    if (!lanes.empty()) {
      return lanes;
    }

    rebuild_probe_queue_locked();
    lanes = take_probe_lanes_locked();
    if (!lanes.empty()) {
      return lanes;
    }

    return take_ready_lanes_locked();
  }

  auto rebuild_probe_queue_locked() -> void {
    for (std::size_t index = 0U; index < lanes_.size(); ++index) {
      enqueue_probe_locked(index);
    }
  }

  auto complete_round_lanes_locked(const lane_list &round_lanes,
                                   const std::optional<std::size_t> winner_position) -> void {
    for (std::size_t position = 0U; position < round_lanes.size(); ++position) {
      const auto lane_index = round_lanes[position];
      auto &lane = lanes_[lane_index];
      lane.in_round = false;
      if (winner_position.has_value() && position == *winner_position) {
        continue;
      }
      enqueue_probe_locked(lane_index);
    }
  }

  auto poll_ready_lane_locked(const std::size_t index, status_type &output) -> bool {
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
    enqueue_ready_locked(index);
    return true;
  }

  [[nodiscard]] auto find_lane_locked(const std::string_view source) noexcept -> lane_state * {
    auto iter =
        std::ranges::find_if(lanes_, [&](const lane_state &lane) { return lane.source == source; });
    return iter == lanes_.end() ? nullptr : std::addressof(*iter);
  }

  auto detach_topology_waiters_locked(topology_sync_ready_buffer &sync_ready,
                                      topology_async_ready_list &async_ready) -> void {
    while (auto *waiter = sync_topology_waiters_.try_pop_front()) {
      sync_ready.push_back(waiter);
    }
    while (auto *waiter = async_topology_waiters_.try_pop_front()) {
      async_ready.push_back(waiter);
    }
  }

  static auto notify_topology_waiters(const topology_sync_ready_buffer &sync_ready,
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
  std::vector<std::size_t> ready_queue_{};
  std::size_t ready_head_{0U};
  std::vector<std::size_t> probe_queue_{};
  std::size_t probe_head_{0U};
  bool closed_{false};
  bool automatic_close_{true};
  bool read_in_flight_{false};
  std::uint64_t topology_epoch_{0U};
  intrusive_waiter_list<sync_waiter_type> sync_topology_waiters_{};
  intrusive_waiter_list<async_waiter_type> async_topology_waiters_{};
};

} // namespace detail
