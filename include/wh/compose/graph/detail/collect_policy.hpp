// Defines graph reader-to-value collection policy used by input lowering.
#pragma once

#include "wh/compose/graph/detail/child_pump.hpp"

namespace wh::compose::detail {

struct collect_policy {
  using child_sender_type = decltype(std::declval<graph_stream_reader &>().read_async());
  using completion_type = graph_stream_reader::chunk_result_type;

  graph_stream_reader reader{};
  edge_limits limits{};
  std::vector<graph_value> collected{};

  auto start() noexcept -> void {
    if (limits.max_items > 0U && limits.max_items <= 64U) {
      collected.reserve(limits.max_items);
    }
  }

  [[nodiscard]] auto next_step() -> wh::core::result<child_pump_step<child_sender_type>> {
    return child_pump_step<child_sender_type>::launch(reader.read_async());
  }

  [[nodiscard]] auto handle_completion(completion_type next)
      -> std::optional<wh::core::result<graph_value>> {
    if (next.has_error()) {
      return wh::core::result<graph_value>::failure(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.is_terminal_eof()) {
      auto closed = reader.close();
      if (closed.has_error()) {
        return wh::core::result<graph_value>::failure(closed.error());
      }
      return wh::core::result<graph_value>{wh::core::any(std::move(collected))};
    }
    if (chunk.is_source_eof()) {
      return std::nullopt;
    }
    if (chunk.error != wh::core::errc::ok) {
      return wh::core::result<graph_value>::failure(chunk.error);
    }

    if (chunk.value.has_value()) {
      collected.push_back(std::move(*chunk.value));
      if (limits.max_items > 0U && collected.size() > limits.max_items) {
        return wh::core::result<graph_value>::failure(wh::core::errc::resource_exhausted);
      }
    }
    return std::nullopt;
  }
};

} // namespace wh::compose::detail

namespace wh::compose {

inline auto
graph::collect_reader_value(graph_stream_reader reader, const edge_limits limits,
                            const wh::core::detail::any_resume_scheduler_t &graph_scheduler)
    -> graph_sender {
  return detail::bridge_graph_sender(detail::make_child_pump_sender(
      detail::collect_policy{
          .reader = std::move(reader),
          .limits = limits,
      },
      graph_scheduler));
}

} // namespace wh::compose
