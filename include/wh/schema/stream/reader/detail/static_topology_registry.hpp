// Defines the fixed-attached merge topology registry used when all reader
// lanes are known and attached at construction time.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/core/small_vector.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/types.hpp"
#include "wh/schema/stream/reader/detail/topology_waiters.hpp"

namespace detail {

template <typename reader_t> class static_topology_registry {
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

  static_topology_registry() = default;

  explicit static_topology_registry(std::vector<lane_type> &&readers)
      : lanes_(build_lanes(std::move(readers))),
        mode_(lanes_.size() <= 5U ? topology_poll_mode::fixed
                                  : topology_poll_mode::dynamic),
        attached_lanes_(count_attached_lanes(lanes_)) {
    seed_attached_lanes();
  }

  [[nodiscard]] auto uses_fixed_poll_path() const noexcept -> bool {
    return mode_ == topology_poll_mode::fixed;
  }

  [[nodiscard]] auto try_read_immediate() -> try_status_type {
    if (closed_) {
      return status_type{chunk_type::make_eof()};
    }
    if (read_in_flight_) {
      return stream_pending;
    }
    if (lanes_.empty() || attached_lanes_ == 0U) {
      return status_type{chunk_type::make_eof()};
    }

    while (const auto lane_index = pop_ready_lane()) {
      status_type output{};
      if (poll_ready_lane(*lane_index, output)) {
        return try_status_type{std::move(output)};
      }
      enqueue_probe(*lane_index);
    }
    return stream_pending;
  }

  [[nodiscard]] auto prepare_round() -> round_plan {
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
    if (lanes_.empty() || attached_lanes_ == 0U) {
      return round_plan{
          .immediate_status = status_type{chunk_type::make_eof()},
      };
    }

    auto lanes = take_round_lanes();
    if (lanes.empty()) {
      return round_plan{
          .immediate_status =
              status_type::failure(wh::core::errc::internal_error),
      };
    }

    read_in_flight_ = true;
    return round_plan{
        .lanes = std::move(lanes),
    };
  }

  [[nodiscard]] auto lane_read_async(const std::size_t lane_index) {
    return lanes_[lane_index].reader->read_async();
  }

  [[nodiscard]] auto complete_round_winner(const lane_list &round_lanes,
                                           const std::size_t winner_position,
                                           status_type status)
      -> round_resolution {
    read_in_flight_ = false;
    complete_round_lanes(round_lanes, winner_position);
    if (winner_position >= round_lanes.size()) {
      return round_resolution{
          .status = status_type::failure(wh::core::errc::internal_error),
      };
    }

    std::optional<std::size_t> close_lane{};
    const auto lane_index = round_lanes[winner_position];
    auto &lane = lanes_[lane_index];
    if (status.has_error()) {
      enqueue_ready(lane_index);
      return round_resolution{
          .status = status_type::failure(status.error()),
      };
    }

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
      return round_resolution{
          .status = status_type{chunk_type::make_source_eof(lane.source)},
          .close_lane = close_lane,
      };
    }

    chunk.source = lane.source;
    enqueue_ready(lane_index);
    return round_resolution{
        .status = status_type{std::move(chunk)},
    };
  }

  auto complete_round_without_winner(const lane_list &round_lanes) -> void {
    read_in_flight_ = false;
    complete_round_lanes(round_lanes, std::nullopt);
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
    if (read_in_flight_) {
      return wh::core::result<void>::failure(wh::core::errc::unavailable);
    }

    wh::core::result<void> close_status{};
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
      lane.ready_queued = false;
      lane.probe_queued = false;
      lane.in_round = false;
    }
    attached_lanes_ = 0U;
    ready_queue_.clear();
    ready_head_ = 0U;
    probe_queue_.clear();
    probe_head_ = 0U;
    closed_ = true;
    return close_status;
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool { return closed_; }

  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    for (const auto &lane : lanes_) {
      if (lane.status == lane_phase::attached && lane.reader.has_value() &&
          !lane.reader->is_source_closed()) {
        return false;
      }
    }
    return true;
  }

  auto set_automatic_close(const auto_close_options &options) -> void {
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

  [[nodiscard]] auto register_sync_topology_waiter(
      sync_waiter_type *, const std::uint64_t) -> bool {
    return false;
  }

  [[nodiscard]] auto register_async_topology_waiter(
      async_waiter_type *, const std::uint64_t) -> bool {
    return false;
  }

  [[nodiscard]] auto remove_async_topology_waiter(async_waiter_type *) -> bool {
    return false;
  }

  [[nodiscard]] auto topology_epoch() const noexcept -> std::uint64_t {
    return 0U;
  }

  [[nodiscard]] auto has_pending_lanes() const noexcept -> bool {
    return false;
  }

  [[nodiscard]] auto lane_count() const noexcept -> std::size_t {
    return lanes_.size();
  }

private:
  enum class lane_phase : std::uint8_t {
    attached = 0U,
    finished,
    disabled,
  };

  struct lane_state {
    std::string source{};
    std::optional<reader_t> reader{};
    lane_phase status{lane_phase::attached};
    bool ready_queued{false};
    bool probe_queued{false};
    bool in_round{false};
  };

  [[nodiscard]] static auto build_lanes(std::vector<lane_type> &&readers)
      -> std::vector<lane_state> {
    std::vector<lane_state> lanes{};
    static_cast<void>(validate_indexed_capacity<>(
        readers.size(),
        "merge_stream_reader lane count exceeds uint32_t slot capacity"));
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
  count_attached_lanes(const std::vector<lane_state> &lanes) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        lanes, [](const lane_state &lane) -> bool {
          return lane.status == lane_phase::attached;
        }));
  }

  auto seed_attached_lanes() -> void {
    for (std::size_t index = 0U; index < lanes_.size(); ++index) {
      if (lanes_[index].status == lane_phase::attached) {
        enqueue_ready(index);
      }
    }
  }

  auto compact_queue(std::vector<std::size_t> &queue, std::size_t &head) -> void {
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
    queue.erase(queue.begin(),
                queue.begin() + static_cast<std::ptrdiff_t>(head));
    head = 0U;
  }

  auto enqueue_ready(const std::size_t index) -> void {
    auto &lane = lanes_[index];
    if (lane.status != lane_phase::attached || lane.in_round ||
        lane.ready_queued) {
      return;
    }
    lane.ready_queued = true;
    lane.probe_queued = false;
    ready_queue_.push_back(index);
  }

  auto enqueue_probe(const std::size_t index) -> void {
    auto &lane = lanes_[index];
    if (lane.status != lane_phase::attached || lane.in_round ||
        lane.ready_queued || lane.probe_queued) {
      return;
    }
    lane.probe_queued = true;
    probe_queue_.push_back(index);
  }

  [[nodiscard]] auto pop_ready_lane() -> std::optional<std::size_t> {
    while (ready_head_ < ready_queue_.size()) {
      const auto index = ready_queue_[ready_head_++];
      auto &lane = lanes_[index];
      if (!lane.ready_queued || lane.status != lane_phase::attached ||
          lane.in_round) {
        continue;
      }
      lane.ready_queued = false;
      compact_queue(ready_queue_, ready_head_);
      return index;
    }
    compact_queue(ready_queue_, ready_head_);
    return std::nullopt;
  }

  [[nodiscard]] auto take_probe_lanes() -> lane_list {
    lane_list lanes{};
    while (probe_head_ < probe_queue_.size()) {
      const auto index = probe_queue_[probe_head_++];
      auto &lane = lanes_[index];
      if (!lane.probe_queued || lane.status != lane_phase::attached ||
          lane.in_round || lane.ready_queued) {
        continue;
      }
      lane.probe_queued = false;
      lane.in_round = true;
      lanes.push_back(index);
    }
    compact_queue(probe_queue_, probe_head_);
    return lanes;
  }

  [[nodiscard]] auto take_ready_lanes() -> lane_list {
    lane_list lanes{};
    while (ready_head_ < ready_queue_.size()) {
      const auto index = ready_queue_[ready_head_++];
      auto &lane = lanes_[index];
      if (!lane.ready_queued || lane.status != lane_phase::attached ||
          lane.in_round) {
        continue;
      }
      lane.ready_queued = false;
      lane.in_round = true;
      lanes.push_back(index);
    }
    compact_queue(ready_queue_, ready_head_);
    return lanes;
  }

  [[nodiscard]] auto take_round_lanes() -> lane_list {
    auto lanes = take_probe_lanes();
    if (!lanes.empty()) {
      return lanes;
    }

    lanes = take_ready_lanes();
    if (!lanes.empty()) {
      return lanes;
    }

    rebuild_probe_queue();
    lanes = take_probe_lanes();
    if (!lanes.empty()) {
      return lanes;
    }

    return take_ready_lanes();
  }

  auto rebuild_probe_queue() -> void {
    for (std::size_t index = 0U; index < lanes_.size(); ++index) {
      enqueue_probe(index);
    }
  }

  auto complete_round_lanes(const lane_list &round_lanes,
                            const std::optional<std::size_t> winner_position)
      -> void {
    for (std::size_t position = 0U; position < round_lanes.size(); ++position) {
      const auto lane_index = round_lanes[position];
      auto &lane = lanes_[lane_index];
      lane.in_round = false;
      if (winner_position.has_value() && position == *winner_position) {
        continue;
      }
      enqueue_probe(lane_index);
    }
  }

  auto poll_ready_lane(const std::size_t index, status_type &output) -> bool {
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
    enqueue_ready(index);
    return true;
  }

  std::vector<lane_state> lanes_{};
  topology_poll_mode mode_{topology_poll_mode::fixed};
  std::size_t attached_lanes_{0U};
  std::vector<std::size_t> ready_queue_{};
  std::size_t ready_head_{0U};
  std::vector<std::size_t> probe_queue_{};
  std::size_t probe_head_{0U};
  bool closed_{false};
  bool automatic_close_{true};
  bool read_in_flight_{false};
};

} // namespace detail
