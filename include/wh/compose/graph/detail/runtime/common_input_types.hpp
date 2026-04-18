// Defines runtime input transport types shared by DAG and Pregel runtimes.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "wh/compose/graph/detail/bitset.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/any.hpp"
#include "wh/core/small_vector.hpp"

namespace wh::compose::detail::input_runtime {

enum class input_edge_status : std::uint8_t { waiting, active, disabled };

enum class dag_node_phase : std::uint8_t {
  pending,
  running,
  executed,
  skipped
};

struct input_lane {
  std::uint32_t edge_id{0U};
  std::uint32_t source_id{0U};
  input_edge_status status{input_edge_status::disabled};
  bool output_ready{false};
};

using input_lane_vector = wh::core::small_vector<input_lane, 8U>;
using input_lane_span = std::span<const input_lane>;

enum class reader_lane_state : std::uint8_t { unseen = 0U, attached, disabled };

struct reader_lowering {
  edge_limits limits{};
  const edge_to_value_adapter *project{nullptr};
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

  [[nodiscard]] static auto borrow_reader(graph_stream_reader &reader) -> resolved_input {
    return resolved_input{.borrowed_reader = std::addressof(reader)};
  }

  [[nodiscard]] static auto own_reader(graph_stream_reader reader) -> resolved_input {
    return resolved_input{.owned_reader = std::move(reader)};
  }

  [[nodiscard]] auto materialize() && -> wh::core::result<graph_value> {
    if (owned_value.has_value()) {
      return wh::compose::detail::materialize_value_payload(std::move(*owned_value));
    }
    if (owned_reader.has_value()) {
      return graph_value{std::move(*owned_reader)};
    }
    if (borrowed_reader != nullptr) {
      auto forked = wh::compose::detail::fork_graph_reader(*borrowed_reader);
      if (forked.has_error()) {
        return wh::core::result<graph_value>::failure(forked.error());
      }
      return graph_value{std::move(forked).value()};
    }
    if (borrowed_value == nullptr) {
      return {};
    }
    return wh::compose::detail::materialize_value_payload(*borrowed_value);
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
      : source_id(other.source_id), edge_id(other.edge_id), borrowed(other.borrowed),
        owned(std::move(other.owned)) {
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

enum class value_input_form : std::uint8_t {
  direct = 0U,
  fan_in,
};

struct value_batch {
  value_input_form form{value_input_form::direct};
  std::optional<value_input> single{};
  std::vector<value_input> fan_in{};
};

struct runtime_io_storage {
  std::vector<graph_value> node_values{};
  wh::compose::detail::dynamic_bitset output_valid{};
  std::optional<graph_stream_reader> final_output_reader{};
  std::vector<graph_value> edge_values{};
  wh::compose::detail::dynamic_bitset edge_value_valid{};
  std::vector<graph_stream_reader> edge_readers{};
  wh::compose::detail::dynamic_bitset edge_reader_valid{};
  std::vector<graph_stream_reader> merged_readers{};
  wh::compose::detail::dynamic_bitset merged_reader_valid{};
  std::vector<reader_lane_state> merged_reader_lane_states{};

  auto reset(const std::size_t node_count, const std::size_t edge_count) -> void {
    if (node_values.size() < node_count) {
      node_values.resize(node_count);
    }
    output_valid.reset(node_count, false);
    final_output_reader.reset();
    if (edge_values.size() < edge_count) {
      edge_values.resize(edge_count);
    }
    edge_value_valid.reset(edge_count, false);
    if (edge_readers.size() < edge_count) {
      edge_readers.resize(edge_count);
    }
    edge_reader_valid.reset(edge_count, false);
    merged_reader_lane_states.resize(edge_count, reader_lane_state::unseen);
    std::fill_n(merged_reader_lane_states.begin(), edge_count, reader_lane_state::unseen);
    if (merged_readers.size() < node_count) {
      merged_readers.resize(node_count);
    }
    merged_reader_valid.reset(node_count, false);
  }

  auto mark_value_output(const std::uint32_t node_id, graph_value value) -> void {
    node_values[node_id] = std::move(value);
    output_valid.set(node_id);
  }

  auto mark_stream_output(const std::uint32_t node_id) -> void {
    output_valid.set(node_id);
  }

  auto mark_final_output_reader(const std::uint32_t node_id, graph_stream_reader reader)
      -> void {
    final_output_reader.emplace(std::move(reader));
    output_valid.set(node_id);
  }
};

} // namespace wh::compose::detail::input_runtime
