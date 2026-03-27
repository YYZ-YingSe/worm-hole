// Defines compose graph input/runtime data types shared by runtime helpers.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/bitset.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/any.hpp"

namespace wh::compose::detail::input_runtime {

enum class node_state : std::uint8_t { pending, running, executed, skipped };

enum class edge_status : std::uint8_t { waiting, active, disabled };

enum class ready_state : std::uint8_t { waiting, ready, skipped };

struct input_lane {
  std::uint32_t edge_id{0U};
  std::uint32_t source_id{0U};
  edge_status status{edge_status::disabled};
  bool output_ready{false};
};

struct branch_state {
  bool decided{false};
  std::vector<std::uint32_t> selected_end_nodes_sorted{};
};

enum class reader_lane_state : std::uint8_t { unseen = 0U, attached, disabled };

struct reader_lowering {
  edge_limits limits{};
  const stream_to_value_adapter *project{nullptr};
};

struct scratch_buffer {
  std::vector<graph_value> node_values{};
  std::vector<graph_stream_reader> node_readers{};
  wh::compose::detail::dynamic_bitset output_valid{};
  wh::compose::detail::dynamic_bitset source_eof_visible{};
  std::vector<graph_value> edge_values{};
  wh::compose::detail::dynamic_bitset edge_value_valid{};
  std::vector<graph_stream_reader> edge_readers{};
  wh::compose::detail::dynamic_bitset edge_reader_valid{};
  wh::compose::detail::dynamic_bitset reader_copy_ready{};
  std::vector<graph_stream_reader> merged_readers{};
  wh::compose::detail::dynamic_bitset merged_reader_valid{};
  std::vector<reader_lane_state> merged_reader_lane_states{};
  std::vector<node_state> node_states{};
  std::vector<branch_state> branch_states{};
  std::vector<std::uint32_t> decided_branch_nodes{};
  std::vector<std::uint32_t> ready_queue{};
  std::size_t ready_head{0U};
  wh::compose::detail::dynamic_bitset queued{};

  auto reset(const std::size_t node_count, const std::size_t edge_count) -> void {
    if (node_values.size() < node_count) {
      node_values.resize(node_count);
    }
    if (node_readers.size() < node_count) {
      node_readers.resize(node_count);
    }
    output_valid.reset(node_count, false);
    source_eof_visible.reset(node_count, false);
    if (edge_values.size() < edge_count) {
      edge_values.resize(edge_count);
    }
    edge_value_valid.reset(edge_count, false);
    if (edge_readers.size() < edge_count) {
      edge_readers.resize(edge_count);
    }
    edge_reader_valid.reset(edge_count, false);
    merged_reader_lane_states.resize(edge_count, reader_lane_state::unseen);
    std::fill_n(merged_reader_lane_states.begin(), edge_count,
                reader_lane_state::unseen);
    if (node_states.size() < node_count) {
      node_states.resize(node_count);
    }
    std::fill_n(node_states.begin(), node_count, node_state::pending);
    reader_copy_ready.reset(node_count, false);
    if (merged_readers.size() < node_count) {
      merged_readers.resize(node_count);
    }
    merged_reader_valid.reset(node_count, false);
    if (branch_states.size() < node_count) {
      branch_states.resize(node_count);
    }
    for (const auto node_id : decided_branch_nodes) {
      auto &state = branch_states[node_id];
      state.decided = false;
      state.selected_end_nodes_sorted.clear();
    }
    decided_branch_nodes.clear();
    ready_queue.clear();
    ready_head = 0U;
    queued.reset(node_count, false);
  }

  auto mark_value_output(const std::uint32_t node_id, graph_value value) -> void {
    node_values[node_id] = std::move(value);
    output_valid.set(node_id);
    source_eof_visible.clear(node_id);
  }

  auto mark_reader_output(const std::uint32_t node_id, graph_stream_reader reader,
                          const bool source_closed) -> void {
    node_readers[node_id] = std::move(reader);
    output_valid.set(node_id);
    if (source_closed) {
      source_eof_visible.set(node_id);
    } else {
      source_eof_visible.clear(node_id);
    }
  }

  auto mark_branch_decided(const std::uint32_t node_id,
                           std::vector<std::uint32_t> &&selected) -> void {
    auto &state = branch_states[node_id];
    if (!state.decided) {
      decided_branch_nodes.push_back(node_id);
    }
    state.decided = true;
    state.selected_end_nodes_sorted = std::move(selected);
  }
};

struct resolved_input {
  graph_value *borrowed_value{nullptr};
  graph_stream_reader *borrowed_reader{nullptr};
  std::optional<graph_value> owned_value{};
  std::optional<graph_stream_reader> owned_reader{};

  [[nodiscard]] static auto borrow_value(graph_value &value) -> resolved_input {
    return resolved_input{.borrowed_value = std::addressof(value)};
  }

  [[nodiscard]] static auto own_value(graph_value value) -> resolved_input {
    return resolved_input{.owned_value = std::move(value)};
  }

  [[nodiscard]] static auto borrow_reader(graph_stream_reader &reader)
      -> resolved_input {
    return resolved_input{.borrowed_reader = std::addressof(reader)};
  }

  [[nodiscard]] static auto own_reader(graph_stream_reader reader)
      -> resolved_input {
    return resolved_input{.owned_reader = std::move(reader)};
  }

  [[nodiscard]] auto materialize() && -> graph_value {
    if (owned_value.has_value()) {
      return std::move(*owned_value);
    }
    if (owned_reader.has_value()) {
      return graph_value{std::move(*owned_reader)};
    }
    if (borrowed_reader != nullptr) {
      if (auto *merged = borrowed_reader->template target_if<
              wh::schema::stream::merge_stream_reader<graph_stream_reader>>();
          merged != nullptr) {
        return graph_value{graph_stream_reader{merged->share()}};
      }
      return wh::core::any::ref(*borrowed_reader);
    }
    if (borrowed_value == nullptr) {
      return {};
    }
    if (borrowed_value->copyable()) {
      return graph_value{*borrowed_value};
    }
    return std::move(*borrowed_value);
  }
};

struct value_input {
  std::uint32_t source_id{0U};
  std::uint32_t edge_id{0U};
  graph_value *borrowed{nullptr};
  std::optional<graph_value> owned{};

  value_input() = default;
  value_input(const value_input &) = delete;
  auto operator=(const value_input &) -> value_input & = delete;

  value_input(value_input &&other) noexcept
      : source_id(other.source_id), edge_id(other.edge_id),
        borrowed(other.borrowed), owned(std::move(other.owned)) {
    if (owned.has_value()) {
      borrowed = std::addressof(*owned);
    }
  }

  auto operator=(value_input &&other) noexcept -> value_input & {
    if (this == &other) {
      return *this;
    }
    source_id = other.source_id;
    edge_id = other.edge_id;
    borrowed = other.borrowed;
    owned = std::move(other.owned);
    if (owned.has_value()) {
      borrowed = std::addressof(*owned);
    }
    return *this;
  }

  [[nodiscard]] auto value() noexcept -> graph_value * {
    return owned.has_value() ? std::addressof(*owned) : borrowed;
  }

  [[nodiscard]] auto value() const noexcept -> const graph_value * {
    return owned.has_value() ? std::addressof(*owned) : borrowed;
  }
};

struct value_batch {
  bool has_data_edge{false};
  bool has_static_fan_in{false};
  std::optional<value_input> single{};
  std::vector<value_input> active{};
};

} // namespace wh::compose::detail::input_runtime
